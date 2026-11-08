// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* hudguide.js and pmg_schema.js on a small synthetic interface file shaped
 * like the unit's (no BMW data): the conversion, the events built for each
 * reading, and the calls made as readings come and go. */
'use strict';
var assert = require('assert'), fs = require('fs'), os = require('os'), path = require('path');
var conv = require('../pmg_schema.js'), hg = require('../hudguide.js');

var T = 'x.GuidingTypes.';
var all = {
    datatypes: {},
    instanceIDs: { 'x.Pmg': { Main_Instance: 128 } },
    interfaces: { 'x.Pmg': { name: 'Pmg', serviceId: 1, version: { major: 1, minor: 3 },
        methods: { updateEventsOnRoute: { methodId: 13, inputs: [{ name: 'eors', dataRef: T + 'U', dimension: 1 }], outputs: [] } },
        fields: { waypoints: { dataRef: 'UInt8', dimension: 1, getter: { id: 5 }, notifier: { id: 32771, eventgroups: [3] } } } } }
};
function def(n, d) { all.datatypes[T + n] = Object.assign({ name: n }, d); }
def('Unit', { type: 'enum', items: { NONE: 0, KM: 1, METER: 4 }, enumWidth: 1 });
def('Turn', { type: 'enum', items: { NONE: 0, STRAIGHT_ON: 1, LEFT: 2, RIGHT: 5 } });
def('Step', { type: 'enum', items: { GS0: 0, GS2: 2 } });
def('Comb', { type: 'enum', items: { FIRST: 1, SINGLE: 0 } });
def('Nil', { type: 'enum', items: { NO_VALUE: 0 } });
def('Opt', { type: 'union', members: [{ name: 'nil', dataRef: T + 'Nil' }, { name: 'value', dataRef: 'UInt8' }] });
def('Src', { type: 'union', members: [{ name: 'simpleId', dataRef: T + 'Sid' }, { name: 'trafficId', dataRef: 'UInt64' }] });
def('Sid', { type: 'struct', members: [{ name: 'id', dataRef: 'UInt64' }] });
def('Fd', { type: 'struct', members: [{ name: 'distance', dataRef: 'Utf8D' }, { name: 'unit', dataRef: T + 'Unit' }, { name: 'rawDistance', dataRef: 'Double' }] });
def('Ev', { type: 'struct', members: [{ name: 'uid', dataRef: 'UInt64' }, { name: 'distanceToDestination', dataRef: 'Double' }, { name: 'sourceId', dataRef: T + 'Src' }] });
def('Panel', { type: 'struct', extends: T + 'Ev', members: [{ name: 'formattedDistance', dataRef: T + 'Fd' }, { name: 'guidingStep', dataRef: T + 'Step' },
    { name: 'combinedManeuverType', dataRef: T + 'Comb' }, { name: 'tags', dataRef: 'Utf8D', dimension: 1, arrayMaxLength: 9 }] });
def('Inter', { type: 'struct', extends: T + 'Panel', members: [{ name: 'direction', dataRef: T + 'Turn' }, { name: 'roadlinks', dataRef: T + 'Opt' }] });
def('Round', { type: 'struct', extends: T + 'Panel', members: [{ name: 'exitAngle', dataRef: 'UInt16' }, { name: 'exitNumber', dataRef: 'UInt8' }] });
def('U', { type: 'union', members: [{ name: 'pointOfInterest', dataRef: T + 'Ev' }, { name: 'intersection', dataRef: T + 'Inter' }, { name: 'roundabout', dataRef: T + 'Round' }] });

var s = conv(all, 'x.Pmg');
var u = s.methods.updateEventsOnRoute.schema.e[0];
assert.strictEqual(u.t, 'ArrayD');
assert.strictEqual(u.e.t, 'Union');
var inter = u.e.e[1];
assert.deepStrictEqual(inter.e.map(function (m) { return m.n; }),
    ['uid', 'distanceToDestination', 'sourceId', 'formattedDistance', 'guidingStep', 'combinedManeuverType', 'tags', 'direction', 'roadlinks'], 'extends: the parents\' members first');
assert.strictEqual(inter.e[3].e[1].w, 1, 'an enum width other than 4 is kept');
assert.strictEqual(inter.e[7].w, undefined, 'the default width is left out');
assert.deepStrictEqual(inter.e[6], { n: 'tags', t: 'ArrayD', e: { t: 'Utf8D' }, xl: 9 });
assert.deepStrictEqual(s.fields.waypoints, { schema: { t: 'ArrayD', e: { t: 'UInt8' } }, getter: { id: 5 }, event: { id: 32771, groups: [3] } });

assert.deepStrictEqual(hg.parseDistance('450 m'), { distance: '450', unit: 'METER' });
assert.deepStrictEqual(hg.parseDistance('1,5 km'), { distance: '1,5', unit: 'KM' });
assert.strictEqual(hg.parseDistance('?'), null);

var ev = hg.eventFor(u.e, { maneuver: 'right', distance: '450 m' }, 7);
assert.deepStrictEqual(ev, { intersection: { uid: '7', distanceToDestination: 0, sourceId: { simpleId: { id: '0' } },
    formattedDistance: { distance: '450', unit: 'METER', rawDistance: 1 }, guidingStep: 'GS2', combinedManeuverType: 'SINGLE', tags: [],
    direction: 'RIGHT', roadlinks: { nil: 'NO_VALUE' } } });
var rb = hg.eventFor(u.e, { maneuver: 'roundabout_2', distance: '' }, 1).roundabout;
assert.strictEqual(rb.exitNumber, 2);
assert.strictEqual(rb.exitAngle, 360);
assert.strictEqual(rb.formattedDistance.unit, 'NONE');
assert.strictEqual(hg.eventFor(u.e, { maneuver: 'merge', distance: '' }, 1), null, 'no event for merge');
var st = hg.eventFor(u.e, { maneuver: 'right', distance: '190 m', street: 'Kerkstraat' }, 2).intersection;
assert.deepStrictEqual(st.description, { elements: [{ text: 'Kerkstraat' }] }, 'the street in the description');
assert.strictEqual(ev.intersection.description, undefined, 'no street: the description is left as it was');
assert.strictEqual(hg.eventFor(u.e, { maneuver: 'keep_left', distance: '' }, 1), null, 'no branch in this schema: nothing, not a throw');

var st = { active: false, key: '', uid: 0, lastManeuver: '', lastMetres: -1 };
function names(c) { return c.map(function (x) { return x[0] + (x[1].guidanceState ? ':' + x[1].guidanceState : ':' + x[1].eors.length); }); }
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'right', distance: '450 m' }, false)), ['setGuidanceState:ACTIVE', 'updateEventsOnRoute:1']);
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'right', distance: '450 m' }, false)), [], 'unchanged: nothing sent');
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'right', distance: '400 m' }, false)), ['updateEventsOnRoute:1']);
assert.strictEqual(st.uid, 1, 'a distance tick keeps the uid');
hg.step(st, u.e, { maneuver: 'right', distance: '900 m' }, false);
assert.strictEqual(st.uid, 2, 'the distance went up: the next manoeuvre');
hg.step(st, u.e, { maneuver: 'left', distance: '800 m' }, false);
assert.strictEqual(st.uid, 3, 'another kind: the next manoeuvre');
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'merge', distance: '' }, false)), ['updateEventsOnRoute:0'], 'unmapped: no arrow, still ACTIVE');
assert.strictEqual(st.active, true);
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'merge', distance: '' }, false)), [], 'unmapped again: nothing');
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: '', distance: '' }, false)), ['updateEventsOnRoute:0', 'setGuidanceState:INACTIVE']);
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: '', distance: '' }, false)), [], 'already clear');
hg.step(st, u.e, { maneuver: 'left', distance: '80 m' }, false);
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'left', distance: '70 m' }, true)), [], 'giving way: nothing, not even INACTIVE');
assert.strictEqual(st.active, false);
assert.deepStrictEqual(names(hg.step(st, u.e, { maneuver: 'left', distance: '60 m' }, false)), ['setGuidanceState:ACTIVE', 'updateEventsOnRoute:1'], 'back once free');

var f = path.join(os.tmpdir(), 'hudguide-test.json');
fs.writeFileSync(f, '{"maneuver":"left","distance":"30 m","at":1}\n');
assert.deepStrictEqual(hg.readHud(f, Date.now()), { maneuver: 'left', distance: '30 m' });
assert.deepStrictEqual(hg.readHud(f, Date.now() + 5000), { maneuver: '', distance: '' }, 'stale');
fs.unlinkSync(f);
assert.deepStrictEqual(hg.readHud(f, Date.now()), { maneuver: '', distance: '' }, 'missing');

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'hudguide-'));
var lc = dir + '/local.conf';
/* the card hidden (CarPlay in the background): the distance reckoned with the speed */
function card(m, d, street) { var r = { maneuver: m, distance: d, source: 'card', stream: 1, map: '', route: 1, at: 0 }; if (street) r.street = street; return r; }
function none(at, extra) { return Object.assign({ maneuver: '', distance: '', source: 'none', stream: 1, map: '', route: 1, at: at }, extra || {}); }
function sp(kmh, at) { return { kmh: kmh, at: at }; }
assert.deepStrictEqual(hg.speedSample({ speedVehicleCentreOfGravity: 3200 }, 5), { kmh: 50, at: 5 });
assert.strictEqual(hg.speedSample({ speedVehicleCentreOfGravity: 65535 }, 5), null, '65535: invalid');
assert.strictEqual(hg.speedSample(null, 5), null);
assert.strictEqual(hg.speedSample({ speedVehicleCentreOfGravity: NaN }, 5), null, 'NaN: invalid');
assert.strictEqual(hg.speedSample({ speedVehicleCentreOfGravity: Infinity }, 5), null, 'Infinity: invalid');
assert.deepStrictEqual(['45 m', '190 m', '450 m', '995 m', '1234 m', '12345 m', '5 m'].map(function (t) { return hg.formatMetres(parseFloat(t)); }),
    ['50 m', '190 m', '450 m', '1.0 km', '1.2 km', '12 km', '']);
assert.strictEqual(hg.formatMetres(1250, ','), '1,3 km');
/* 36 km/h = 10 m/s */
var rk = {}, T0 = 1000000;
var r0 = hg.reckon(rk, card('right', '450 m', 'Kerkstraat'), T0, sp(36, T0), 90);
assert.strictEqual(r0.source, 'card', 'a card reading passes through');
var r1 = hg.reckon(rk, none(1), T0 + 1000, sp(36, T0 + 1000), 90);
assert.deepStrictEqual(r1, { maneuver: 'right', distance: '440 m', source: 'reckon', street: 'Kerkstraat' }, 'reckoned, street kept');
assert.strictEqual(hg.reckon(rk, none(2), T0 + 20000, sp(36, T0 + 20000), 90).distance, '250 m');
/* the card back: no new uid */
var gs = { active: false, key: '', uid: 0, lastManeuver: '', lastMetres: -1 };
hg.step(gs, u.e, card('right', '900 m'), false);
assert.strictEqual(gs.uid, 1);
assert.deepStrictEqual(names(hg.step(gs, u.e, { maneuver: 'right', distance: '300 m', source: 'reckon' }, false)), ['updateEventsOnRoute:1']);
hg.step(gs, u.e, card('right', '350 m'), false);
assert.strictEqual(gs.uid, 1, 'card back after reckoning (350 m > reckoned 300 m + 30): same manoeuvre');
hg.step(gs, u.e, card('right', '950 m'), false);
assert.strictEqual(gs.uid, 2, 'above the last card distance: the next one');
/* card back mid-reckoning resumes from the card */
assert.strictEqual(hg.reckon(rk, card('right', '200 m'), T0 + 21000, sp(36, T0 + 21000), 90).source, 'card');
assert.strictEqual(hg.reckon(rk, none(3), T0 + 22000, sp(36, T0 + 22000), 90).distance, '190 m');
/* 20 m past the turn: dropped, and stays dropped */
assert.strictEqual(hg.reckon(rk, none(4), T0 + 41000, sp(36, T0 + 41000), 90).distance, '', 'past the turn: no distance');
assert.strictEqual(hg.reckon(rk, none(5), T0 + 42500, sp(36, T0 + 42500), 90).maneuver, 'right', '15 m past: kept');
assert.strictEqual(hg.reckon(rk, none(6), T0 + 43500, sp(36, T0 + 43500), 90).maneuver, '', 'more than 20 m past: nothing');
assert.strictEqual(hg.reckon(rk, none(7), T0 + 47000, sp(36, T0 + 47000), 90).maneuver, '', 'and stays so');
assert.strictEqual(hg.reckon(rk, none(6), T0 + 48000, sp(0, T0 + 48000), 90).maneuver, '', 'ended until the card is back');
/* the 90 s cap, and hud_bg_hold_s = 0 */
rk = {};
hg.reckon(rk, card('left', '1,2 km'), T0, sp(1, T0), 90);
hg.reckon(rk, none(1), T0 + 1000, sp(1, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(2), T0 + 90000, sp(1, T0 + 90000), 90).distance, '1,2 km');
assert.strictEqual(hg.reckon(rk, none(3), T0 + 92000, sp(1, T0 + 92000), 90).maneuver, '', '90 s after the card went: nothing');
rk = {};
hg.reckon(rk, card('left', '450 m'), T0, sp(36, T0), 0);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 1000, sp(36, T0 + 1000), 0).maneuver, '', 'hold 0: off');
/* stream 0 clears */
rk = {};
hg.reckon(rk, card('left', '450 m'), T0, sp(36, T0), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 1000, sp(36, T0 + 1000), 90).maneuver, 'left');
assert.strictEqual(hg.reckon(rk, none(2, { stream: 0 }), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, '', 'stream 0: nothing');
assert.strictEqual(hg.reckon(rk, none(3), T0 + 3000, sp(36, T0 + 3000), 90).maneuver, '', 'not again once the stream is back');
/* stale speed: frozen, then no distance, then nothing; no speed at all: never starts */
rk = {};
hg.reckon(rk, card('left', '450 m', 'Rue Haute'), T0, sp(36, T0), 90);
var stale = sp(36, T0);   /* only 65535 since: the last valid sample stays */
assert.strictEqual(hg.reckon(rk, none(1), T0 + 2000, stale, 90).distance, '430 m');
assert.strictEqual(hg.reckon(rk, none(2), T0 + 4000, stale, 90).distance, '430 m', 'over 3 s: frozen');
assert.deepStrictEqual(hg.reckon(rk, none(3), T0 + 6000, stale, 90), { maneuver: 'left', distance: '', source: 'reckon', street: 'Rue Haute' }, 'over 5 s: arrow and street only');
assert.strictEqual(hg.reckon(rk, none(4), T0 + 8000, stale, 90).maneuver, '', 'over 8 s: nothing');
rk = {};
hg.reckon(rk, card('left', '450 m'), T0, null, 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 1000, null, 90).maneuver, '', 'no speed: never reckoned');
rk = {};
hg.reckon(rk, card('left', '450 m'), T0, sp(36, T0), 90);
hg.reckon(rk, card('left', ''), T0 + 4000, sp(36, T0 + 4000), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 6000, sp(36, T0 + 6000), 90).maneuver, '', 'the card\'s last distance over 5 s old: nothing');
/* the real sequence: the card's 3 s hold first; the card went at the first hold */
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
[1, 2, 3].forEach(function (i) { assert.strictEqual(hg.reckon(rk, Object.assign(card('right', '450 m'), { source: 'hold' }), T0 + i * 1000, sp(36, T0 + i * 1000), 90).source, 'hold'); });
assert.deepStrictEqual(hg.reckon(rk, none(4), T0 + 4000, sp(36, T0 + 4000), 90), { maneuver: 'right', distance: '410 m', source: 'reckon' }, 'after the hold: reckoned from the card');
assert.strictEqual(hg.reckon(rk, none(5), T0 + 30000, sp(1, T0 + 30000), 90).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(6), T0 + 92000, sp(1, T0 + 92000), 90).maneuver, '', '90 s counted from the first hold');
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
hg.reckon(rk, Object.assign(card('right', ''), { source: 'hold' }), T0 + 5000, sp(36, T0 + 5000), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 6000, sp(36, T0 + 6000), 90).maneuver, '', 'the distance 5 s old when the card went: nothing');
/* the map's arrow: the other side twice ends it, never starts one */
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
assert.strictEqual(hg.reckon(rk, none(1, { map: 'left' }), T0 + 1000, sp(36, T0 + 1000), 90).maneuver, 'right', 'one read: kept');
assert.strictEqual(hg.reckon(rk, none(1, { map: 'left' }), T0 + 1300, sp(36, T0 + 1300), 90).maneuver, 'right', 'the same read again: not a second one');
assert.strictEqual(hg.reckon(rk, none(2, { map: 'right' }), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, 'right', 'same side: reset');
assert.strictEqual(hg.reckon(rk, none(3, { map: 'slight_left' }), T0 + 3000, sp(36, T0 + 3000), 90).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(4, { map: 'left' }), T0 + 4000, sp(36, T0 + 4000), 90).maneuver, '', 'the other side twice: ended');
rk = {};
hg.reckon(rk, card('roundabout_2', '450 m'), T0, sp(36, T0), 90);
hg.reckon(rk, none(1, { map: 'left' }), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(2, { map: 'left' }), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, 'roundabout_2', 'roundabouts never compared');
assert.strictEqual(hg.reckon({}, none(1, { map: 'left' }), T0, sp(36, T0), 90).maneuver, '', 'the map never starts one');
/* the route line: route 1 to start; unknown (-1, missing) or 0 never starts; one 0 kept, two end it, -1 ends it */
[{ route: 0 }, { route: -1 }, { route: undefined }].forEach(function (x) {
    rk = {};
    hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
    assert.strictEqual(hg.reckon(rk, none(1, x), T0 + 1000, sp(36, T0 + 1000), 90).maneuver, '', 'route ' + x.route + ' as the card goes: never');
    assert.strictEqual(hg.reckon(rk, none(2), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, '', 'and not once it shows');
});
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 1000, sp(36, T0 + 1000), 90).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(2, { route: 0 }), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, 'right', 'one route 0: kept');
assert.strictEqual(hg.reckon(rk, none(2, { route: 0 }), T0 + 2300, sp(36, T0 + 2300), 90).maneuver, 'right', 'the same heartbeat again: not a second');
assert.strictEqual(hg.reckon(rk, none(3), T0 + 3000, sp(36, T0 + 3000), 90).maneuver, 'right', 'route back: reset');
assert.strictEqual(hg.reckon(rk, none(4, { route: 0 }), T0 + 4000, sp(36, T0 + 4000), 90).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(5, { route: 0 }), T0 + 5000, sp(36, T0 + 5000), 90).maneuver, '', 'route 0 two heartbeats running: ended');
assert.strictEqual(hg.reckon(rk, none(6), T0 + 6000, sp(36, T0 + 6000), 90).maneuver, '', 'ended until the card is back');
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
hg.reckon(rk, none(1), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(2, { route: -1 }), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, '', 'route -1 mid-reckoning: ended at once');
/* the street mirrors hud.json: a card read without one clears it, so it never reaches a reckoned event */
rk = {};
hg.reckon(rk, card('right', '450 m', 'Rue Haute'), T0, sp(36, T0), 90);
hg.reckon(rk, card('right', '440 m'), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 2000, sp(36, T0 + 2000), 90).street, undefined, 'the last card read had no street: none');
rk = {};
hg.reckon(rk, card('right', '450 m', 'Rue Haute'), T0, sp(36, T0), 90);
hg.reckon(rk, Object.assign(card('right', '450 m'), { source: 'hold' }), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 2000, sp(36, T0 + 2000), 90).street, undefined, 'the hold without a street: none');
rk = {};
hg.reckon(rk, card('right', '450 m', 'Rue Haute'), T0, sp(36, T0), 90);
hg.reckon(rk, card('left', '300 m'), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 2000, sp(36, T0 + 2000), 90).street, undefined, 'a new manoeuvre: the old street gone');
/* only 'hold' is the card's hold: a map or banner reading is a reading of its own */
rk = {};
hg.reckon(rk, card('right', '450 m'), T0, sp(36, T0), 90);
hg.reckon(rk, Object.assign(card('left', ''), { source: 'map' }), T0 + 1000, sp(36, T0 + 1000), 90);
assert.strictEqual(rk.goneAt, 0, 'map: not counted as the card gone');
assert.strictEqual(hg.reckon(rk, none(1), T0 + 2000, sp(36, T0 + 2000), 90).maneuver, '', 'map reading (no distance) then none: never reckoned');
/* old hud.json (no source, no stream): passes through, never reckoned */
rk = {};
assert.deepStrictEqual(hg.reckon(rk, { maneuver: 'left', distance: '30 m' }, T0, sp(36, T0), 90), { maneuver: 'left', distance: '30 m' });
assert.deepStrictEqual(hg.reckon(rk, { maneuver: '', distance: '' }, T0 + 1000, sp(36, T0 + 1000), 90), { maneuver: '', distance: '' });
fs.writeFileSync(f, '{"maneuver":"left","distance":"30 m","street":"A12","source":"card","stream":1,"map":"right","route":1,"at":7}\n');
assert.deepStrictEqual(hg.readHud(f, Date.now()), { maneuver: 'left', distance: '30 m', street: 'A12', source: 'card', map: 'right', stream: 1, at: 7, route: 1 });
fs.unlinkSync(f);
fs.writeFileSync(lc, 'hud_bg_hold_s = 45\n');
assert.strictEqual(hg.bgHoldS(lc), 45);
fs.writeFileSync(lc, 'hud_bg_hold_s = 0\n');
assert.strictEqual(hg.bgHoldS(lc), 0);
assert.strictEqual(hg.bgHoldS(dir + '/none'), 600, 'absent: 600, a safety cap');
fs.writeFileSync(lc, 'hud_bg_hold_s = 4000\n');
assert.strictEqual(hg.bgHoldS(lc), 600, 'over 3600: the default');
var sdir = fs.mkdtempSync(require('path').join(require('os').tmpdir(), 'hgspeed-')), sp = sdir + '/speed.json';
assert.strictEqual(hg.readSpeed(sp, Date.now(), 5), null, 'no file: no speed');
fs.writeFileSync(sp, '{"kmh":50}');
assert.deepStrictEqual(hg.readSpeed(sp, Date.now(), 5), { kmh: 50, at: 5 });
assert.strictEqual(hg.readSpeed(sp, Date.now() + 10000, 5), null, 'a stale file: no speed');
fs.writeFileSync(sp, '{"kmh":"x"}');
assert.strictEqual(hg.readSpeed(sp, Date.now(), 5), null, 'not a number: no speed');
/* the route traced on the cluster map (hud.json's geo) */
function spd(k, a) { return { kmh: k, at: a }; }   /* sp is the speed file's path by now */
function G(x) { return Object.assign({ x: 536, y: 150, side: '', angle: 0, arc: 100, label: null, conf: 70, passed: 0 }, x || {}); }
fs.writeFileSync(f, '{"maneuver":"","distance":"","source":"none","stream":1,"map":"","route":1,"at":9,"geo":{"x":532,"y":148,"side":"right","angle":73,"arc":104,"label":[452,76,180,68],"conf":70,"passed":0,"at":8}}\n');
assert.deepStrictEqual(hg.readHud(f, Date.now()).geo, { x: 532, y: 148, side: 'right', angle: 73, arc: 104, label: [452, 76, 180, 68], conf: 70, passed: 0, at: 8 });
fs.writeFileSync(f, '{"maneuver":"","distance":"","stream":1,"at":9,"geo":{"side":"uturn","conf":"x","passed":true,"label":[1,2]}}\n');
assert.deepStrictEqual(hg.readHud(f, Date.now()).geo, { x: 0, y: 0, side: '', angle: 0, arc: 0, label: null, conf: 0, passed: 0 }, 'a bad geo: nothing in it counts');
fs.unlinkSync(f);
/* passed on two heartbeats ends the reckoned turn; one, or the same heartbeat again, does not */
rk = {};
hg.reckon(rk, Object.assign(card('right', '450 m'), { geo: G({ side: 'right' }) }), T0, spd(36, T0), 600);
assert.strictEqual(hg.reckon(rk, none(1, { geo: G({ side: 'right' }) }), T0 + 1000, spd(36, T0 + 1000), 600).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(2, { geo: G({ passed: 1 }) }), T0 + 2000, spd(36, T0 + 2000), 600).maneuver, 'right', 'passed once: kept');
assert.strictEqual(hg.reckon(rk, none(2, { geo: G({ passed: 1 }) }), T0 + 2300, spd(36, T0 + 2300), 600).maneuver, 'right', 'the same heartbeat again');
assert.strictEqual(hg.reckon(rk, none(3, { geo: G({ passed: 1, at: 2 }) }), T0 + 2600, spd(36, T0 + 2600), 600).maneuver, 'right',
    'another heartbeat (a context change), the same trace (geo.at): not a second');
assert.strictEqual(hg.reckon(rk, none(4, { geo: G() }), T0 + 3000, spd(36, T0 + 3000), 600).maneuver, 'right', 'not passed: reset');
assert.strictEqual(hg.reckon(rk, none(4, { geo: G({ passed: 1 }) }), T0 + 4000, spd(36, T0 + 4000), 600).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, none(5, { geo: G({ passed: 1 }) }), T0 + 5000, spd(36, T0 + 5000), 600).maneuver, '', 'passed twice running: ended');
assert.strictEqual(hg.reckon(rk, none(6, { geo: G({ side: 'left' }) }), T0 + 6000, spd(36, T0 + 6000), 600).maneuver, '', 'and stays so (hud_geo_next off)');
/* traced since the card: past the turn by the reckoning is only a backstop at 150 m; never traced: 20 m as before */
rk = {};
hg.reckon(rk, Object.assign(card('right', '100 m'), { geo: G() }), T0, spd(36, T0), 600);
hg.reckon(rk, none(0, { geo: G() }), T0 + 1000, spd(36, T0 + 1000), 600);
assert.deepStrictEqual(hg.reckon(rk, none(1, { geo: G() }), T0 + 13000, spd(36, T0 + 13000), 600), { maneuver: 'right', distance: '', source: 'reckon' }, '30 m past, traced: kept, no distance');
assert.strictEqual(hg.reckon(rk, none(2, { geo: G() }), T0 + 26000, spd(36, T0 + 26000), 600).maneuver, '', '160 m past: ended');
rk = {};
hg.reckon(rk, Object.assign(card('right', '100 m'), { geo: G({ conf: 0 }) }), T0, spd(36, T0), 600);
hg.reckon(rk, none(0, { geo: G({ conf: 0 }) }), T0 + 1000, spd(36, T0 + 1000), 600);
assert.strictEqual(hg.reckon(rk, none(1, { geo: G({ conf: 0 }) }), T0 + 13000, spd(36, T0 + 13000), 600).maneuver, '', 'nothing traced: 20 m past ends it');
/* the 600 s cap */
rk = {};
hg.reckon(rk, card('left', '1,2 km'), T0, spd(0.5, T0), 600);
assert.strictEqual(hg.reckon(rk, none(1), T0 + 1000, spd(0.5, T0 + 1000), 600).maneuver, 'left');
assert.strictEqual(hg.reckon(rk, none(2), T0 + 599000, spd(0.5, T0 + 599000), 600).maneuver, 'left');
assert.strictEqual(hg.reckon(rk, none(3), T0 + 602000, spd(0.5, T0 + 602000), 600).maneuver, '', '600 s after the card went: nothing');
/* hud_geo_next on: after the passed turn, the next side once three heartbeats agree */
function passedHold() {
    var k = {};
    hg.reckon(k, Object.assign(card('right', '450 m'), { geo: G({ side: 'right' }) }), T0, spd(36, T0), 600, true);
    hg.reckon(k, none(1, { geo: G({ passed: 1 }) }), T0 + 1000, spd(36, T0 + 1000), 600, true);
    assert.strictEqual(hg.reckon(k, none(2, { geo: G({ passed: 1 }) }), T0 + 2000, spd(36, T0 + 2000), 600, true).maneuver, '', 'passed: the turn gone');
    return k;
}
rk = passedHold();
var t = T0 + 3000;
function beat(at, geo, extra) { t += 1000; return hg.reckon(rk, none(at, Object.assign({ geo: geo }, extra || {})), t, spd(36, t), 600, true); }
assert.strictEqual(beat(3, G({ side: 'left', passed: 1 })).maneuver, '', 'still passed: not counted');
assert.strictEqual(beat(4, G({ side: 'left' })).maneuver, '');
assert.strictEqual(beat(5, G({ side: 'left' })).maneuver, '');
t += 300;
assert.strictEqual(hg.reckon(rk, none(5, { geo: G({ side: 'left' }) }), t, spd(36, t), 600, true).maneuver, '', 'the same heartbeat again: not a third');
assert.deepStrictEqual(beat(6, G({ side: 'left' })), { maneuver: 'left', distance: '', source: 'map' }, 'three heartbeats agree: left, no distance, no street');
assert.strictEqual(beat(7, G({ side: 'right' })).maneuver, 'left', 'one miss: kept');
assert.strictEqual(beat(8, G({ side: 'left' })).maneuver, 'left', 'back: reset');
assert.strictEqual(beat(9, G({ side: '' })).maneuver, 'left');
assert.strictEqual(beat(10, G({ side: 'left', passed: 1 })).maneuver, '', 'two misses (none, passed): cleared');
rk = passedHold();
t = T0 + 3000;
[3, 4, 5, 6].forEach(function (i) { assert.strictEqual(beat(i, G({ side: 'right', conf: 40 })).maneuver, '', 'conf under 60: never'); });
[7, 8, 9].forEach(function (i) { assert.strictEqual(beat(i, G({ side: 'right' }), { stream: 0 }).maneuver, '', 'stream 0: never'); });
assert.strictEqual(beat(10, null).maneuver, '', 'no geo: never');
rk = passedHold();
t = T0 + 3000;
[3, 4].forEach(function (i) { beat(i, G({ side: 'right' })); });
assert.strictEqual(beat(5, G({ side: 'right' })).maneuver, 'right');
assert.strictEqual(hg.reckon(rk, card('right', '300 m'), t + 500, spd(36, t + 500), 600, true).source, 'card', 'the card back: it wins');
assert.strictEqual(rk.next, null, 'and the map\'s next is forgotten');
rk = {};
hg.reckon(rk, Object.assign(card('right', '450 m'), { geo: G() }), T0, spd(36, T0), 600, true);
hg.reckon(rk, none(1, { stream: 0 }), T0 + 1000, spd(36, T0 + 1000), 600, true);
assert.strictEqual(hg.reckon(rk, none(2, { geo: G({ side: 'left' }) }), T0 + 2000, spd(36, T0 + 2000), 600, true).maneuver, '', 'ended otherwise (stream 0): no next from the map');
/* the map's turn, then the card: the same side keeps the uid (no second chime); another side is a new one */
var ms = { active: false, key: '', uid: 0, lastManeuver: '', lastMetres: -1 };
hg.step(ms, u.e, { maneuver: 'left', distance: '', source: 'map' }, false);
assert.strictEqual(ms.uid, 1);
hg.step(ms, u.e, card('slight_left', '300 m'), false);
assert.strictEqual(ms.uid, 1, 'the card back, the same side: the same manoeuvre');
hg.step(ms, u.e, card('left', '290 m'), false);
assert.strictEqual(ms.uid, 2, 'card to card, another kind: a new one as before');
hg.step(ms, u.e, { maneuver: 'left', distance: '', source: 'map' }, false);
hg.step(ms, u.e, card('right', '200 m'), false);
assert.strictEqual(ms.uid, 3, 'the card on another side: it wins with a new uid');
/* calibration pairs */
assert.strictEqual(hg.pairsLine(1794148219000, none(1, { map: 'left', geo: G({ side: 'right', label: [452, 76, 180, 68] }) }),
    { maneuver: 'right', distance: '190 m', street: 'Rue, Haute', source: 'reckon' }, 36),
'1794148219000,reckon,right,190 m,"Rue, Haute",36.0,1,left,right,0,536,150,100,70,0,452 76 180 68\n');
assert.strictEqual(hg.pairsLine(5, { maneuver: '', distance: '' }, { maneuver: '', distance: '' }), '5,none,,,,,,,,,,,,,,\n', 'nothing known: empty fields');
var pdir = dir + '/pairs';
assert.strictEqual(hg.appendPairs(pdir, '1,a\n'), true);
assert.strictEqual(hg.appendPairs(pdir, '2,b\n'), true);
var csv = fs.readFileSync(pdir + '/pairs.csv', 'utf8');
assert.strictEqual(csv.split('\n')[0].indexOf('at,source,maneuver,distance,street,kmh,route,map,geo_side'), 0, 'the header first');
assert.strictEqual(csv.split('\n').slice(1).join('\n'), '1,a\n2,b\n');
assert.strictEqual(fs.statSync(pdir).mode & parseInt('7777', 8), parseInt('755', 8), 'the folder 0755, not writable by others');
fs.chmodSync(pdir, parseInt('1777', 8));
assert.strictEqual(hg.appendPairs(pdir, '2b,c\n'), true);
assert.strictEqual(fs.statSync(pdir).mode & parseInt('7777', 8), parseInt('755', 8), 'an older 1777 folder of ours made 0755');
fs.symlinkSync(pdir, dir + '/pairs-link');
assert.strictEqual(hg.appendPairs(dir + '/pairs-link', '9,z\n'), false, 'a symlinked folder refused');
var odir = dir + '/pairs-other';
fs.mkdirSync(odir);
fs.symlinkSync(dir + '/victim', odir + '/pairs.csv');
assert.strictEqual(hg.appendPairs(odir, '9,z\n'), false, 'a symlinked pairs.csv refused');
assert.strictEqual(fs.existsSync(dir + '/victim'), false, 'nothing written through it');
if (process.getuid() !== 0)
    assert.strictEqual(hg.appendPairs('/', '9,z\n'), false, 'a folder of another user (root) refused');
fs.writeFileSync(pdir + '/pairs.csv', Buffer.alloc(2 * 1024 * 1024, 0x41));
assert.strictEqual(hg.appendPairs(pdir, '3,c\n'), false, 'at 2 MB: no more');
assert.strictEqual(hg.geoNextOn(dir + '/none'), false, 'hud_geo_next absent: off');
fs.writeFileSync(lc, 'hud_geo_next = 1\n');
assert.strictEqual(hg.geoNextOn(lc), true);
console.log('hudguide_test: all passed');
