// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* The vehicle speed for hudguide's reckoning (README.md "HUD arrows").
 * Vehicle2 answers hmi_5000 but refuses cia_13002, hudguide's user
 * (VERIFIED on the car 2026-11-07: subscribe accepted, no notify, every
 * getValue refused), so this runs as hmi_5000, in its own unit
 * (jap-waze-on-cluster-speedd.service), and writes
 * /run/jap-waze-on-cluster/speed.json {"kmh":12.5} every second, whole or not
 * at all. No sample (none yet, 65535, not a number): the file is not written,
 * so hudguide sees it go stale and reckons nothing.
 *
 *   node speedd.js */
'use strict';

var fs = require('fs');
var pmgSchema = require('./pmg_schema.js');

var IFACES = '/etc/oap/someip/someip-interfaces.json';
var VEHICLE2 = 'de.bmw.infotainment.vehicle.Vehicle2';
var OUT = process.env.WOC_SPEED_JSON || '/run/jap-waze-on-cluster/speed.json';
var POLL_MS = 2000, WRITE_MS = 1000, SAMPLE_MAX_MS = 3000;

function log(m) { console.log('speedd: ' + m); }

/* km/h from a statusCarSpeed2 value, or null (1/64 km/h per unit, 65535 invalid). */
function kmh(v) {
    var k = v && v.speedVehicleCentreOfGravity;
    if (typeof k !== 'number' || !isFinite(k) || k < 0 || k === 65535) return null;
    return k * 0.015625;
}

function write(path, value) {
    try {
        fs.writeFileSync(path + '.tmp', JSON.stringify({ kmh: Math.round(value * 10) / 10 }));
        fs.renameSync(path + '.tmp', path);
        return true;
    } catch (e) { return false; }
}

function main() {
    var someip = require('/usr/lib/someip.node');
    var cfg = pmgSchema(JSON.parse(fs.readFileSync(IFACES, 'utf8')), VEHICLE2);
    var last = null, lastAt = 0, first = true, failed = false, p;
    function got(v) {
        var s = kmh(v);
        if (s === null) return;
        last = s; lastAt = Date.now();
        if (first) { first = false; log('speed ' + s.toFixed(1) + ' km/h'); }
    }
    Promise.resolve(someip.buildProxy(cfg, Object.keys(cfg.instances || {})[0])).then(function (px) {
        p = px;
        return p.init();
    }).then(function () {
        return p.subscribe({ name: 'statusCarSpeed2', onReceive: got });
    }).then(function () {
        log('watching ' + VEHICLE2 + '.statusCarSpeed2 -> ' + OUT);
        setInterval(function () {
            try { p.getValue('statusCarSpeed2', 1500).then(got, function () {}); } catch (e) { /* next poll */ }
        }, POLL_MS);
        setInterval(function () {
            if (last === null || Date.now() - lastAt > SAMPLE_MAX_MS) return;   /* stale: let the file age */
            if (!write(OUT, last) && !failed) { failed = true; log('cannot write ' + OUT); }
        }, WRITE_MS);
    }).catch(function (e) { log('no speed: ' + e); process.exit(1); });
}

if (require.main === module) main();
module.exports = { kmh: kmh, write: write };
