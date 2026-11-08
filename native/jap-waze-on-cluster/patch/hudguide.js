// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* BMW's own HUD arrows for Waze (README.md "HUD arrows", hud_backend =
 * native). Waze sends CarPlay no iAP2 route guidance, so carplay never calls
 * the navigation's ProjectedModeGuiding for it, which is how Apple Maps' and
 * Google Maps' turns reach the HUD. jap-waze-on-cluster reads Waze's next turn
 * and writes it to /run/jap-waze-on-cluster/hud.json (every second while it
 * runs); this service makes carplay's calls with it:
 *   setGuidanceState(ACTIVE, CAR_PLAY), then updateEventsOnRoute([the turn])
 *   on each change; setGuidanceState(INACTIVE) and an empty list when the
 *   reading goes empty or stale (3 s), and on stop.
 * The event is built as carplay builds the next manoeuvre (carplay 22.28.3,
 * RouteGuidanceConverters): uid the manoeuvre's number, guidingStep GS2,
 * formattedDistance the displayed number and unit with rawDistance 1.0,
 * everything else empty or nil.
 *
 * It gives way: while the navigation's waypoints or current road are set
 * (carplay fills them for Apple's or Google's guidance; we never do) or
 * BMW's own guidance is active, it sends nothing, not even INACTIVE, which
 * would reset what the other source shows.
 *
 *   node hudguide.js          the service
 *   node hudguide.js --dry    print what it would send for each reading */
'use strict';

var fs = require('fs');
var pmgSchema = require('./pmg_schema.js');

var IFACES = '/etc/oap/someip/someip-interfaces.json';
var PMG = 'de.bmw.infotainment.navigation.projectedmodeguiding.ProjectedModeGuiding';
var GUIDANCE = '/usr/lib/oap-mainapp/src-gen/de_bmw_infotainment_navigation_guiding_Guidance.json';
var HUD_JSON = '/run/jap-waze-on-cluster/hud.json';
var STALE_MS = 3000, TICK_MS = 300, CALL_MS = 2000;
var LOCAL_CONF = '/var/opt/jap-waze-on-cluster/jap-waze-on-cluster.local.conf';
/* While CarPlay is in the background (one of our apps on the centre screen)
 * the card is gone and only Waze's cluster stream stays: the last card's
 * manoeuvre and street are kept, its distance counted down with the car's
 * speed (Vehicle2.statusCarSpeed2), until the route traced on the cluster
 * map (hud.json's geo) says the turn is passed, within hud_bg_hold_s (local
 * conf, 600 s: a safety cap; 0: off), and only while Waze's route line
 * shows on the cluster map (hud.json's route 1: calibrated on the car,
 * README.md "HUD arrows"). With hud_geo_next = 1 (local conf; 0, the
 * default, until the car's pairs show the map's side agrees with the
 * card's), the next turn's side is then taken from the map. Nothing rather
 * than wrong: see reckon(). */
var SPEED_JSON = '/run/jap-waze-on-cluster/speed.json', SPEED_POLL_MS = 1000, SPEED_FILE_MAX_MS = 3000;
var BG_HOLD_S = 600, CARD_FRESH_MS = 5000, SPEED_LIVE_MS = 3000, SPEED_FREEZE_MS = 5000, SPEED_GONE_MS = 8000, PASSED_M = -20;
/* with the map traced, the reckoned distance is only a backstop: Waze's
 * 10 m steps and the speed's error, well past the turn */
var PASSED_GEO_M = -150, GEO_MIN_CONF = 60, NEXT_BEATS = 3, NEXT_MISSES = 2;
/* Calibration pairs (README.md "HUD arrows"): while hud_dump_flag exists,
 * one line a second to <hud_dump_dir>/pairs.csv, at most PAIRS_MAX bytes. */
var DUMP_FLAG = '/tmp/jap-woc-hud-dump', DUMP_DIR = '/tmp/jap-woc-hud', PAIRS_MAX = 2 * 1024 * 1024;
var PAIRS_HEAD = 'at,source,maneuver,distance,street,kmh,route,map,geo_side,geo_angle,geo_x,geo_y,geo_arc,geo_conf,geo_passed,geo_label\n';

function log(m) { console.log('hudguide: ' + m); }

/* ms on a monotonic clock, for intervals: a wall clock set by GPS or NTP
 * would jump the reckoning. process.hrtime(): the unit's Node may predate bigint. */
function mono() { var t = process.hrtime(); return t[0] * 1e3 + t[1] / 1e6; }

/* A zero value for a compact schema: the enum's 0 item (else its first), a
 * union's first member (nil for the optionals), empty arrays and strings. */
function zero(s) {
    switch (s.t) {
    case 'Struct': var o = {}; s.e.forEach(function (m) { o[m.n] = zero(m); }); return o;
    case 'Union': var u = {}; u[s.e[0].n] = zero(s.e[0]); return u;
    case 'Enum':
        var ks = Object.keys(s.e), z = ks.filter(function (k) { return s.e[k] === 0; });
        return z.length ? z[0] : ks[0];
    case 'ArrayD': return [];
    case 'Map': return {};
    case 'Bool': return false;
    case 'Utf8D': case 'Utf16D': return '';
    case 'ByteBuffer': return [];
    case 'UInt64': case 'Int64': return '0';   /* someip.node takes 64-bit integers as strings */
    default: return 0;
    }
}

/* "450 m" -> {distance: "450", unit: "METER"}; nothing for an unknown unit. */
function parseDistance(t) {
    var m = /^\s*([0-9]+(?:[.,][0-9]+)?)\s*(m|km|ft|mi|yd)\s*$/.exec(t || '');
    if (!m) return null;
    var unit = { m: 'METER', km: 'KM', ft: 'FEET', mi: 'MILE_US', yd: 'YARD' }[m[2]];
    return { distance: m[1], unit: unit };
}

var TURNS = {
    straight: 'STRAIGHT_ON', slight_left: 'SLIGHT_LEFT', left: 'LEFT', sharp_left: 'HARD_LEFT',
    slight_right: 'SLIGHT_RIGHT', right: 'RIGHT', sharp_right: 'HARD_RIGHT', uturn_left: 'UTURN_LEFT', uturn_right: 'UTURN_RIGHT'
};

function metres(d) {
    var v = parseFloat(d.distance.replace(',', '.'));
    return v * ({ METER: 1, KM: 1000, FEET: 0.3048, MILE_US: 1609.344, YARD: 0.9144 }[d.unit] || 1);
}

/* A key's value in the local conf, or null (absent, unreadable). */
function confValue(path, key) {
    try {
        var m = new RegExp('^[ \\t]*' + key + '[ \\t]*=[ \\t]*([^\\s#]*)', 'm').exec(fs.readFileSync(path, 'utf8'));
        return m ? m[1] : null;
    } catch (e) { return null; }
}

/* hud_geo_next from the local conf: on only when it says 1. */
function geoNextOn(path) { return confValue(path, 'hud_geo_next') === '1'; }

/* hud_bg_hold_s from the local conf: BG_HOLD_S unless it says a number. */
function bgHoldS(path) {
    var v = parseFloat(confValue(path, 'hud_bg_hold_s'));
    return isFinite(v) && v >= 0 ? v : BG_HOLD_S;
}

/* A statusCarSpeed2 value as {kmh, at}, or null when invalid (65535, not finite) or missing. */
function speedSample(v, now) {
    var k = v && v.speedVehicleCentreOfGravity;
    return typeof k === 'number' && isFinite(k) && k !== 65535 ? { kmh: k * 0.015625, at: now } : null;
}

/* metres as the card writes them: 10 m steps under 1 km, then 0.1 km
 * (whole km from 10 km), the card's decimal separator; '' under 10 m. */
function formatMetres(m, sep) {
    if (m < 10) return '';
    if (Math.round(m / 10) * 10 < 1000) return Math.round(m / 10) * 10 + ' m';
    if (m >= 10000) return Math.round(m / 1000) + ' km';
    return (Math.round(m / 100) / 10).toFixed(1).replace('.', sep || '.') + ' km';
}

function side(m) {
    var s = /^(?:slight_|sharp_|uturn_)?(left|right)$/.exec(m || '');
    return s ? s[1] : '';
}

/* The reading to give step(). A card reading passes through and is kept.
 * Without one, the last card's manoeuvre and street, with its distance less
 * what the car drove since that distance showed, only while: hold_s > 0,
 * Waze's cluster stream is live and the arrows on (stream 1), Waze's route
 * line shows (route 1 as the card goes; a route 0 two heartbeats running,
 * or none or -1, ends it), the card had a distance under CARD_FRESH_MS
 * before it went and a speed was read under SPEED_FREEZE_MS before; at most
 * hold_s after it went; until the traced route says passed on two
 * heartbeats (geo.passed, each a fresh heartbeat), or, as a backstop, 20 m
 * past the turn by the reckoning (150 m once the route was traced since the
 * card's manoeuvre began: geo with conf above 0); until the map's arrow
 * shows the other side twice (heartbeats apart, about 1 s). The street is
 * the last card read's, as hud.json had it. Speed older than SPEED_LIVE_MS: the
 * distance stands still; older than SPEED_FREEZE_MS: no distance; older than
 * SPEED_GONE_MS: nothing. Once ended it stays ended until the card is back,
 * except with geoNext after a passed turn: see nextFromMap().
 * rk: {} at first; speed: the last valid speedSample() or null; now: mono(). */
function reckon(rk, r, now, speed, holdS, geoNext) {
    var age = speed ? now - speed.at : Infinity;
    if (rk.t && age < SPEED_LIVE_MS) rk.driven = (rk.driven || 0) + speed.kmh / 3.6 * (now - rk.t) / 1000;
    rk.t = now;
    if (r.geo && r.geo.conf > 0) rk.geoSeen = true;
    if (r.maneuver) {
        rk.off = false; rk.checked = false; rk.mapN = 0; rk.routeN = 0; rk.passN = 0; rk.passed = false; rk.next = null;
        if (r.source === 'hold') {   /* the card's own repeat, the card already gone */
            if (!rk.goneAt) rk.goneAt = now;
            if (rk.card && rk.card.maneuver === r.maneuver) rk.card.street = r.street || '';
            return r;
        }
        rk.goneAt = 0;
        var d = parseDistance(r.distance), c = rk.card;
        if (!c || c.maneuver !== r.maneuver) {
            c = rk.card = { maneuver: r.maneuver, distance: '', metres: -1, distAt: 0 }; rk.driven = 0;
            rk.geoSeen = !!(r.geo && r.geo.conf > 0);
        }
        if (d && (d.unit === 'METER' || d.unit === 'KM')) {
            if (r.distance !== c.distance) { c.distance = r.distance; c.metres = metres(d); rk.driven = 0; }
            c.distAt = now;
        }
        c.street = r.street || '';   /* as hud.json has it: an old street never outlives its read */
        return r;
    }
    if (!rk.card) return r;
    if (rk.off) return rk.passed && geoNext && holdS > 0 && now - rk.goneAt <= holdS * 1000 ? nextFromMap(rk, r) : r;
    var c = rk.card;
    if (!rk.goneAt) rk.goneAt = now;
    if (!rk.checked) {
        rk.checked = true;
        if (!(holdS > 0) || c.metres < 0 || rk.goneAt - c.distAt >= CARD_FRESH_MS || age >= SPEED_FREEZE_MS || r.route !== 1) rk.off = true;
    }
    /* the route line: one 0 may be a frame it hides under something; unknown never passes */
    if (r.route === 0) {
        if (r.at === undefined || r.at !== rk.routeAt) { rk.routeAt = r.at; rk.routeN = (rk.routeN || 0) + 1; }
    } else if (r.route === 1) rk.routeN = 0;
    else rk.off = true;
    var left = c.metres - rk.driven, ms = side(c.maneuver), mapS = side(r.map);
    if (ms && mapS && mapS !== ms) {
        if (r.at === undefined || r.at !== rk.mapAt) { rk.mapAt = r.at; rk.mapN = (rk.mapN || 0) + 1; }
    } else rk.mapN = 0;
    if (r.geo && r.geo.passed) {
        var pat = geoAt(r);
        if (pat === undefined || pat !== rk.passAt) { rk.passAt = pat; rk.passN = (rk.passN || 0) + 1; }
    } else if (r.geo) rk.passN = 0;
    if (rk.passN >= 2 && !rk.off) { rk.off = true; rk.passed = true; }
    if (r.stream !== 1 || now - rk.goneAt > holdS * 1000 || left < (rk.geoSeen ? PASSED_GEO_M : PASSED_M) || rk.mapN >= 2 ||
        rk.routeN >= 2 || age >= SPEED_GONE_MS) rk.off = true;
    if (rk.off) return rk.passed && geoNext ? nextFromMap(rk, r) : r;
    var out = { maneuver: c.maneuver, distance: age < SPEED_FREEZE_MS ? formatMetres(left, /,/.test(c.distance) ? ',' : '.') : '', source: 'reckon' };
    if (c.street) out.street = c.street;
    return out;
}

/* After a passed turn (hud_geo_next = 1): the next turn's side from the
 * traced route, plain left or right (never slight, sharp, a U-turn or a
 * roundabout: the geometry cannot tell them), no distance and no street
 * (an arrow without a distance is what hud_reader = map sends), source
 * 'map'. Shown once NEXT_BEATS heartbeats running agree on the side (geo
 * not passed, conf at least GEO_MIN_CONF, stream 1); gone after NEXT_MISSES
 * heartbeats without it (another side, none, passed), and hudguide's 3 s
 * staleness clears the HUD after that as for any empty reading. */
/* What tells two traces apart: geo's own at (hud.json is rewritten on any
 * change, the same trace in it), else the heartbeat's. */
function geoAt(r) { return r.geo && r.geo.at !== undefined ? r.geo.at : r.at; }

function nextFromMap(rk, r) {
    var n = rk.next || (rk.next = { side: '', beats: 0, shown: '', misses: 0, at: undefined });
    var at = geoAt(r);
    if (at !== undefined && at === n.at) return n.shown ? { maneuver: n.shown, distance: '', source: 'map' } : r;
    n.at = at;
    var g = r.geo, s = r.stream === 1 && g && !g.passed && g.conf >= GEO_MIN_CONF && (g.side === 'left' || g.side === 'right') ? g.side : '';
    if (s && s === n.side) n.beats++;
    else { n.side = s; n.beats = s ? 1 : 0; }
    if (n.shown) {
        n.misses = s === n.shown ? 0 : n.misses + 1;
        if (n.misses >= NEXT_MISSES) { n.shown = ''; n.misses = 0; }
    }
    if (!n.shown && n.beats >= NEXT_BEATS) { n.shown = n.side; n.misses = 0; }
    return n.shown ? { maneuver: n.shown, distance: '', source: 'map' } : r;
}

/* One pairs.csv line: when (wall ms), what is sent (source, maneuver,
 * distance, street), the speed, the route line, the map's white arrow and
 * the traced route; quoted where a value may hold a comma. */
function pairsLine(at, r, out, kmh) {
    function q(v) { v = v === undefined || v === null ? '' : String(v); return /[",\n]/.test(v) ? '"' + v.replace(/"/g, '""').replace(/\n/g, ' ') + '"' : v; }
    var g = r.geo || {};
    return [at, out.source || (out.maneuver ? 'card' : 'none'), out.maneuver, out.distance, out.street,
        typeof kmh === 'number' ? kmh.toFixed(1) : '', r.route, r.map, g.side, g.angle, g.x, g.y, g.arc, g.conf, g.passed,
        Array.isArray(g.label) ? g.label.join(' ') : ''].map(q).join(',') + '\n';
}

/* The line appended to <dir>/pairs.csv (the folder made writable by all,
 * as jap-waze-on-cluster makes it; the header first), unless the file is at
 * PAIRS_MAX; errors ignored. */
function appendPairs(dir, line) {
    try {
        try { fs.mkdirSync(dir, parseInt('1777', 8)); fs.chmodSync(dir, parseInt('1777', 8)); } catch (e) { /* exists */ }
        var f = dir + '/pairs.csv', size = -1;
        try { size = fs.statSync(f).size; } catch (e) { /* new */ }
        if (size >= PAIRS_MAX) return false;
        fs.appendFileSync(f, (size <= 0 ? PAIRS_HEAD : '') + line);
        return true;
    } catch (e) { return false; }
}

/* The GuidanceEventUnion value for a reading, or null for a manoeuvre the
 * HUD has no event for (merge). */
function eventFor(unionSchema, r, uid) {
    var kind, extra = {}, mm = /^roundabout_([0-9]+)$/.exec(r.maneuver);
    if (TURNS[r.maneuver]) { kind = 'intersection'; extra.direction = TURNS[r.maneuver]; }
    else if (r.maneuver === 'keep_left' || r.maneuver === 'keep_right') {
        kind = 'branch'; extra.branchType = r.maneuver === 'keep_left' ? 'BRANCH_LEFT' : 'BRANCH_RIGHT';
    } else if (mm) {
        /* carplay without a junction angle: 90, 0, 270 for exits 1, 2, 3+ (right-hand traffic), 0 as 360 */
        var n = +mm[1];
        kind = 'roundabout'; extra.exitNumber = n; extra.exitAngle = n === 1 ? 90 : n === 2 ? 360 : 270; extra.driveDirection = 'RIGHT';
    } else if (r.maneuver === 'arrive') { kind = 'destinationManeuver'; extra.position = 'CENTER'; extra.isCheckeredFlag = true; }
    else return null;
    var member = unionSchema.e.filter(function (m) { return m.n === kind; })[0];
    if (!member) return null;
    var ev = zero(member);
    Object.keys(extra).forEach(function (k) { ev[k] = extra[k]; });
    ev.uid = String(uid);
    ev.sourceId = { simpleId: { id: '0' } };
    ev.guidingStep = 'GS2';
    ev.combinedManeuverType = 'SINGLE';
    var d = parseDistance(r.distance);
    ev.formattedDistance = d ? { distance: d.distance, unit: d.unit, rawDistance: 1.0 } : { distance: ' ', unit: 'NONE', rawDistance: 1.0 };
    /* the street or exit the card names, shown under the distance */
    if (r.street) ev.description = { elements: [{ text: r.street }] };
    var out = {};
    out[kind] = ev;
    return out;
}

/* The reading, or an empty one when the file is missing or stale. */
function readHud(path, now) {
    try {
        var st = fs.statSync(path);
        if (now - st.mtime.getTime() > STALE_MS) return { maneuver: '', distance: '' };
        if (st.size > 4096) return { maneuver: '', distance: '' };
        var j = JSON.parse(fs.readFileSync(path, 'utf8'));
        var r = { maneuver: String(j.maneuver || ''), distance: String(j.distance || '') };
        if (j.street) r.street = String(j.street).slice(0, 80);
        ['source', 'map'].forEach(function (k) { if (typeof j[k] === 'string') r[k] = j[k]; });
        if (typeof j.stream === 'number') { r.stream = j.stream; r.at = j.at; }   /* v2: at tells the heartbeats apart */
        if (typeof j.route === 'number') r.route = j.route;
        if (j.geo && typeof j.geo === 'object') {
            var g = j.geo, num = function (v) { return typeof v === 'number' && isFinite(v) ? v : 0; };
            r.geo = { x: num(g.x), y: num(g.y), side: g.side === 'left' || g.side === 'right' ? g.side : '', angle: num(g.angle),
                arc: num(g.arc), label: Array.isArray(g.label) && g.label.length === 4 ? g.label.map(num) : null,
                conf: num(g.conf), passed: g.passed === 1 ? 1 : 0 };
            if (typeof g.at === 'number') r.geo.at = g.at;   /* the trace's time: a heartbeat may repeat a trace */
        }
        return r;
    } catch (e) {
        return { maneuver: '', distance: '' };
    }
}

/* What to send for a reading: decided apart from SOME/IP, for the tests.
 * state: {active, key, uid, lastManeuver, lastMetres}; returns the calls in order. */
function step(state, unionSchema, r, foreign) {
    var calls = [];
    if (foreign) {
        if (state.active) log('another source guides the HUD: giving way');
        state.active = false; state.key = '';
        return calls;
    }
    var ev = null;
    if (r.maneuver) {
        /* a new manoeuvre: another kind, or the card's distance went up (the
         * next one, same kind); a reckoned distance is never compared or kept,
         * so the card coming back is the same manoeuvre (BMW would chime again) */
        var d = r.source !== 'reckon' && parseDistance(r.distance), m = d ? metres(d) : -1;
        /* the card back on the map's turn, the same side: the same manoeuvre (no second chime) */
        var same = r.maneuver === state.lastManeuver || (state.lastSource === 'map' && side(r.maneuver) !== '' && side(r.maneuver) === side(state.lastManeuver));
        if (!same || (m >= 0 && state.lastMetres >= 0 && m > state.lastMetres + 30)) state.uid++;
        if (r.source !== 'reckon') state.lastMetres = m;
        ev = eventFor(unionSchema, r, state.uid);
        state.lastManeuver = r.maneuver;
    } else {
        state.lastManeuver = '';
    }
    state.lastSource = r.source || '';
    if (ev) {
        if (!state.active) calls.push(['setGuidanceState', { guidanceState: 'ACTIVE', guidingProvider: 'CAR_PLAY' }]);
        var key = JSON.stringify(ev);
        if (!state.active || key !== state.key) calls.push(['updateEventsOnRoute', { eors: [ev] }]);
        state.active = true; state.key = key;
    } else if (state.active && r.maneuver) {
        /* a manoeuvre the HUD has no event for (merge): no arrow, still guiding */
        if (state.key !== '[]') calls.push(['updateEventsOnRoute', { eors: [] }]);
        state.key = '[]';
    } else if (state.active) {
        calls.push(['updateEventsOnRoute', { eors: [] }]);
        calls.push(['setGuidanceState', { guidanceState: 'INACTIVE', guidingProvider: 'CAR_PLAY' }]);
        state.active = false; state.key = '';
    }
    return calls;
}

function nonEmpty(v, f) {
    if (f === 'waypoints') return Array.isArray(v) && v.length > 0;
    return !!(v && v.elements && v.elements.length);
}

/* The car's speed from speed.json, as {kmh, at: mono()} when the file is
 * fresh (written within SPEED_FILE_MAX_MS), else null. speedd.js writes it
 * as hmi_5000: Vehicle2 refuses cia_13002 (VERIFIED on the car 2026-11-07),
 * so this user cannot read the speed itself. */
function readSpeed(path, nowWall, nowMono) {
    try {
        var st = fs.statSync(path);
        if (st.size > 256 || Math.abs(nowWall - st.mtime.getTime()) > SPEED_FILE_MAX_MS) return null;
        var k = JSON.parse(fs.readFileSync(path, 'utf8')).kmh;
        return typeof k === 'number' && isFinite(k) && k >= 0 && k < 400 ? { kmh: k, at: nowMono } : null;
    } catch (e) { return null; }
}

function main(dry) {
    var all = JSON.parse(fs.readFileSync(IFACES, 'utf8'));
    var schema = pmgSchema(all, PMG);
    var unionSchema = schema.methods.updateEventsOnRoute.schema.e[0].e;
    var state = { active: false, key: '', uid: 0, lastManeuver: '', lastMetres: -1, loggedUid: -1 };
    var rk = {}, speed = null, holdS = BG_HOLD_S, geoNext = false;
    var pairs = { at: -Infinity, confAt: -Infinity, flag: DUMP_FLAG, dir: DUMP_DIR };
    function reading(now) {   /* now: mono(); the file's age is wall clock */
        var r = readHud(HUD_JSON, Date.now());
        if (rk.card && !rk.checked && (!r.maneuver || r.source === 'hold')) { holdS = bgHoldS(LOCAL_CONF); geoNext = geoNextOn(LOCAL_CONF); }   /* read as the card goes */
        var out = reckon(rk, r, now, speed, holdS, geoNext);
        logPair(now, r, out);
        return out;
    }
    /* calibration pairs: the flag looked at once a second, the conf every 30 s */
    function logPair(now, r, out) {
        if (now - pairs.at < 1000) return;
        pairs.at = now;
        if (now - pairs.confAt >= 30000) {
            pairs.confAt = now;
            pairs.flag = confValue(LOCAL_CONF, 'hud_dump_flag') || DUMP_FLAG;
            pairs.dir = confValue(LOCAL_CONF, 'hud_dump_dir') || DUMP_DIR;
        }
        if (!fs.existsSync(pairs.flag)) return;
        var okp = appendPairs(pairs.dir, pairsLine(Date.now(), r, out, speed && now - speed.at < SPEED_LIVE_MS ? speed.kmh : undefined));
        if (!okp && !pairs.warned) log('cannot append to ' + pairs.dir + '/pairs.csv (full, or not writable by this user)');
        pairs.warned = !okp;
    }
    if (dry) {
        var last = '';
        setInterval(function () {
            var r = reading(mono()), k = JSON.stringify(r);
            if (k === last) return;
            last = k;
            step(state, unionSchema, r, false).forEach(function (c) { log(c[0] + ' ' + JSON.stringify(c[1])); });
        }, TICK_MS);
        return;
    }
    var someip = require('/usr/lib/someip.node');
    var pmg, g = null, other = { waypoints: false, currentRoad: false }, bmwGuiding = false, stopping = false;
    var inflight = Promise.resolve(), busy = false, fails = 0, quietUntil = 0;
    function foreign() { return other.waypoints || other.currentRoad || bmwGuiding; }
    var CLEAR = [['updateEventsOnRoute', { eors: [] }], ['setGuidanceState', { guidanceState: 'INACTIVE', guidingProvider: 'CAR_PLAY' }]];
    /* In order; stops before a call once another source guides (it would reset what that source shows). */
    function send(calls, evenIfForeign) {
        return calls.reduce(function (p, c) {
            return p.then(function () {
                if (foreign() && !evenIfForeign) throw new Error('another source started guiding');
                return pmg.callMethod(c[0], c[1], CALL_MS).then(function () {
                    if (c[0] === 'setGuidanceState') log('setGuidanceState ' + c[1].guidanceState);
                    else if (!c[1].eors.length) log('updateEventsOnRoute: none');
                    else if (c[1].eors[0] && JSON.stringify(c[1].eors[0]).indexOf('"uid":"' + state.uid + '"') >= 0 && state.loggedUid !== state.uid) {
                        state.loggedUid = state.uid;
                        log('updateEventsOnRoute ' + JSON.stringify(c[1].eors[0]).slice(0, 160));
                    }
                });
            });
        }, Promise.resolve());
    }
    function tick() {
        var now = mono();
        if (busy || stopping || now < quietUntil) return;
        var was = { active: state.active, key: state.key };
        var r = reading(now), calls = step(state, unionSchema, r, foreign());
        if (!calls.length) return;
        busy = true;
        inflight = send(calls).then(function () { fails = 0; }, function (e) {
            /* send it all again, from where it was, after a pause */
            state.active = was.active; state.key = '';
            if (/another source/.test(String(e))) { state.active = false; return; }
            fails++;
            quietUntil = mono() + 5000;
            log('call failed (' + fails + '): ' + e + '; again in 5 s');
            if (fails >= 5) {
                log('giving up after 5 failures');
                stopping = true;
                return send(CLEAR).catch(function () {}).then(function () { process.exit(1); });
            }
        }).then(function () { busy = false; });
    }
    function stop() {
        if (stopping) return;
        stopping = true;
        var t = setTimeout(function () { process.exit(0); }, 4000);
        inflight.then(function () {
            return state.active ? send(CLEAR) : null;
        }).catch(function (e) { log('clearing on stop: ' + e); })
            .then(function () { clearTimeout(t); process.exit(0); });
    }
    /* the giving-way flags: read now, and again whenever the navigation comes back */
    function readFlags() {
        var reads = ['waypoints', 'currentRoad'].map(function (f) {
            return pmg.getValue(f, CALL_MS).then(function (v) { other[f] = nonEmpty(v, f); });
        });
        if (g) reads.push(g.getValue('guidanceState', CALL_MS).then(setBmw));
        return Promise.all(reads);
    }
    function setBmw(v) { bmwGuiding = v === 'ACTIVE' || v === 'CALCULATING_ROUTE'; }
    function retry(what, fn, n) {
        return fn().catch(function (e) {
            if (n <= 1) throw e;
            log(what + ': ' + e + '; again in 2 s');
            return new Promise(function (r) { setTimeout(r, 2000); }).then(function () { return retry(what, fn, n - 1); });
        });
    }
    process.on('SIGTERM', stop);
    process.on('SIGINT', stop);
    Promise.resolve(someip.buildProxy(schema, 'Main_Instance')).then(function (p) {
        pmg = p;
        return pmg.init();
    }).then(function () {
        return Promise.resolve(someip.buildProxy(JSON.parse(fs.readFileSync(GUIDANCE, 'utf8')), 'Main_Instance')).then(function (gp) {
            return gp.init().then(function () { g = gp; });
        });
    }).then(function () {
        return Promise.all(['waypoints', 'currentRoad'].map(function (f) {
            return pmg.subscribe({ name: f, onReceive: function (v) { other[f] = nonEmpty(v, f); } });
        }).concat([g.subscribe({ name: 'guidanceState', onReceive: setBmw })]));
    }).then(function () {
        /* the navigation is offered a while after boot: wait for it, up to 2 min */
        return retry('reading the navigation', readFlags, 60);
    }).then(function () {
        pmg.subscribeToAvailability(function (up) {
            if (!up) return;
            /* a restarted navigation has none of ours: start over */
            state.active = false; state.key = '';
            readFlags().catch(function (e) { log('re-reading after the navigation came back: ' + e); });
        });
        /* a killed predecessor may have left an arrow: clear it, unless another source guides */
        var clear = fs.existsSync(HUD_JSON) && !foreign() ? send(CLEAR) : Promise.resolve();
        return clear.then(function () {
            log('ready: ' + HUD_JSON + ' -> ProjectedModeGuiding');
            setInterval(tick, TICK_MS);
            /* after the PMG side is up, on its own: it never holds that up */
            /* a stale or missing file keeps the last sample, which ages: reckon freezes, then drops */
            var spoke = false;
            setInterval(function () {
                var s = readSpeed(SPEED_JSON, Date.now(), mono());
                if (!s) return;
                if (!spoke) { spoke = true; log('speed: ' + s.kmh.toFixed(1) + ' km/h from ' + SPEED_JSON); }
                speed = s;
            }, SPEED_POLL_MS);
        });
    }).catch(function (e) {
        log('cannot reach the navigation: ' + e);
        process.exit(1);   /* systemd starts it again, within its start limit */
    });
}

module.exports = { zero: zero, parseDistance: parseDistance, eventFor: eventFor, step: step, readHud: readHud,
    reckon: reckon, speedSample: speedSample, readSpeed: readSpeed, formatMetres: formatMetres, bgHoldS: bgHoldS,
    geoNextOn: geoNextOn, pairsLine: pairsLine, appendPairs: appendPairs };

if (require.main === module) main(process.argv.indexOf('--dry') > 0);
