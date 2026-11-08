// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
'use strict';
var assert = require('assert'), fs = require('fs'), os = require('os'), path = require('path');
var sd = require('../speedd.js');

assert.strictEqual(sd.kmh({ speedVehicleCentreOfGravity: 0 }), 0);
assert.strictEqual(sd.kmh({ speedVehicleCentreOfGravity: 3200 }), 50);
assert.strictEqual(sd.kmh({ speedVehicleCentreOfGravity: 65535 }), null, '65535 is invalid');
assert.strictEqual(sd.kmh({ speedVehicleCentreOfGravity: NaN }), null);
assert.strictEqual(sd.kmh({}), null);
assert.strictEqual(sd.kmh(null), null);

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'speedd-')), out = path.join(dir, 'speed.json');
assert.ok(sd.write(out, 49.97));
assert.deepStrictEqual(JSON.parse(fs.readFileSync(out, 'utf8')), { kmh: 50 });
assert.ok(!fs.existsSync(out + '.tmp'), 'written whole');
assert.ok(!sd.write(path.join(dir, 'missing', 'speed.json'), 1), 'an unwritable path is false, not a throw');
console.log('speedd_test: all passed');
