#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* Proves carplay_locate.js and carplay_patch.js on synthetic builds
 * (synthetic_carplay.js): every site found where the build put it, also with
 * other offsets, addresses and code order, and in its patched state; a
 * signature found twice refused as ambiguous; a damaged site, a broken
 * cross-check or a relocation over an edit refused with the reason; apply
 * (only with --unproven), verify, probe and revert on a build outside the
 * proven list, byte-exact; a four-edit patch of an earlier release (stock
 * fps) enabled, Enable adding only the fps edit, and --fps. With CARPLAY_BIN, the reference build's sites are
 * the documented offsets.
 *
 *   node locate_test.js
 */
'use strict';

var fs = require('fs');
var os = require('os');
var path = require('path');
var cp = require('child_process');
var locator = require('../carplay_locate.js');
var synthetic = require('./synthetic_carplay.js');

var SCRIPT = path.join(__dirname, '..', 'carplay_patch.js');

var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}
function run(args) {
    var r = cp.spawnSync(process.execPath, [SCRIPT].concat(args), { encoding: 'utf8' });
    return { code: r.status, out: (r.stdout || '') + (r.stderr || '') };
}
function json(x) { return JSON.stringify(x); }

/* The sites the locator reports, in the fixture's terms. */
function sites(loc) {
    return {
        buildId: loc.buildId, table: loc.variant.table, slot: loc.variant.slot,
        original: loc.variant.original, high: loc.variant.high, mid: loc.variant.mid, sink: loc.sink.offset,
        call: loc.selfstart.offset, setStreamStatus: loc.selfstart.setStreamStatus, requestStream: loc.selfstart.requestStream,
        onSuggestUi: loc.selfstart.onSuggestUi, callOriginal: loc.selfstart.original, callPatched: loc.selfstart.patched,
        builder: loc.res.builder, mm: loc.res.mm, px: loc.res.px, safe1: loc.res.safe1, safe2: loc.res.safe2,
        fps: loc.fps.offset, fpsGetter: loc.fps.getter, fpsMain: loc.fps.main, fpsBuilder: loc.fps.builder, fpsConst: loc.fps.constant,
        fpsOriginal: loc.fps.original,
    };
}
function expected(at) { var e = {}; Object.keys(sites({ variant: {}, sink: {}, selfstart: {}, res: {}, fps: {} })).forEach(function (k) { e[k] = at[k]; }); return e; }
function reasons(loc) { return ['variant', 'sink', 'selfstart', 'res', 'fps'].filter(function (e) { return !loc[e].ok; }).map(function (e) { return loc[e].reason; }); }

var SHIFTED = { pad: 0x123, codeVa: 0x7a000, dataVa: 0x1234000, dataAt: 0x2800, highViaJne: true, midFirst: true };

/* Found where the build put them. */
[['synthetic build', {}], ['shifted build (other offsets, addresses, code order, jne branch)', SHIFTED]].forEach(function (t) {
    var s = synthetic.build(t[1]), loc = locator.locate(s.buf);
    ok(t[0] + ': every site found where it is', loc.ok && json(sites(loc)) === json(expected(s.at)), json(loc));
});
var plain = synthetic.build({}), shifted = synthetic.build(SHIFTED);
ok('the shifted build moves every site', ['slot', 'sink', 'call', 'mm', 'fps'].every(function (k) { return plain.at[k] !== shifted.at[k]; }));

/* The patched state is found at the same sites. */
(function () {
    var s = synthetic.build(SHIFTED), b = Buffer.from(s.buf);
    b.writeUInt32LE(s.at.high, s.at.slot);
    Buffer.alloc(59).copy(b, s.at.sink); b.write('udp://127.0.0.1:20557?mime_type=video/H264', s.at.sink, 'latin1');
    Buffer.from(s.at.callPatched, 'hex').copy(b, s.at.call);
    b.writeUInt32LE(138, s.at.mm); b.writeUInt32LE(1088, s.at.px);
    Buffer.from('b80f0000006690', 'hex').copy(b, s.at.fps);
    var loc = locator.locate(b);
    ok('patched build: the same sites', loc.ok && json(sites(loc)) === json(expected(s.at)), json(loc));
    b.writeUInt32LE(s.at.mid, s.at.slot);
    ok('slot 7 set to slot 10\'s value: found', locator.locate(b).variant.ok);
})();

/* A signature found twice is ambiguous. */
[['two sink URL slots', { twoSinks: true }, 'sink: URL slot found 2 times'],
 ['a second converter switch outside codingToInternal', { twoConverters: true }, 'variant: converter switch also outside codingToInternal'],
 ['a second maxFPS load in the IC display builder', { twoIcFps: true }, 'fps: IC display maxFPS load found 2 times']].forEach(function (t) {
    var loc = locator.locate(synthetic.build(t[1]).buf);
    ok(t[0] + ': refused as ambiguous, the other edits found', !loc.ok && json(reasons(loc)) === json([t[2]]), json(reasons(loc)));
});

/* A damaged site is refused with what is wrong. */
[
    ['the IC_HIGH handler stores OTHER', function (b, at) { b.writeUInt32LE(2, at.highHandler + 2); }, 'variant: slot 9 is not IC_HIGH'],
    ['slot 7 points at the NOT_ACTIVE handler', function (b, at) { b.writeUInt32LE(b.readUInt32LE(at.table), at.slot); }, 'variant: slot 7 not OTHER/IC_HIGH/IC_MID'],
    ['another sink URL', function (b, at) { b.write('ws://', at.sink, 'latin1'); }, 'sink: URL slot not found'],
    ['no AVAILABLE argument', function (b, at) { b[at.call - 12] = 2; }, 'self-start: call has no AVAILABLE argument'],
    ['onSuggestUi calls something else', function (b, at) { b.writeInt32LE(b.readInt32LE(at.call + 1) + 0x40, at.call + 1); }, 'self-start: onSuggestUi call not found'],
    ['no ForceKeyframe check', function (b) { var i = b.indexOf('ForceKeyframe'); b.write('ForceKeyFrame', i, 'latin1'); }, 'self-start: ForceKeyframe string not found'],
    ['a geometry store broken', function (b, at) { b[at.px + 8] = 0x90; }, 'resolution: 1 geometry builders (want 2)'],
    ['requestStream only reached through onSuggestUi', function (b, at) { b.fill(0x90, at.cmdCall, at.cmdCall + 5); }, 'self-start: requestStream not called directly'],
    ['a getter reads another field', function (b, at) { b[at.getter2c + 3] = 0x2a; }, 'resolution: geometry getters not found', 'fps: IC display builder not found'],
    ['the IC display load stores another register', function (b, at) { b[at.fps + 6] = 0x01; }, 'fps: IC display maxFPS load not found'],
    ['the main display load gone', function (b, at) { b.fill(0x90, at.fpsMain, at.fpsMain + 7); }, 'fps: main display maxFPS load not found'],
    ['the IC display load patched, the main one gone', function (b, at) {
        Buffer.from('b80f0000006690', 'hex').copy(b, at.fps); b.fill(0x90, at.fpsMain, at.fpsMain + 7); }, 'fps: maxFPS getter not found'],
    ['the maxFPS getter also called elsewhere', function (b, at) { b.writeInt32LE(at.fpsGetter - (at.fpsMain + 18), at.fpsMain + 14); },
        'fps: maxFPS getter called elsewhere'],
    ['the maxFPS constant 0', function (b, at) { b.writeUInt32LE(0, at.fpsConst); }, 'fps: maxFPS constant 0 out of range'],
    ['no uuid getter after the IC display load', function (b, at) { b.fill(0x90, at.fps + 13, at.fps + 18); }, 'fps: IC display load not followed by its uuid getter'],
].forEach(function (t) {
    var s = synthetic.build({}), b = Buffer.from(s.buf);
    t[1](b, s.at);
    var loc = locator.locate(b);
    ok('damaged (' + t[0] + '): ' + t.slice(2).join('; '), json(reasons(loc)) === json(t.slice(2)), json(reasons(loc)));
});
(function () {
    var loc = locator.locate(synthetic.build({ noEhFrame: true }).buf);
    ok('no function table: variant, self-start, resolution and fps say so',
        json(reasons(loc)) === json(['variant: no function table', 'self-start: no function table', 'resolution: no function table', 'fps: no function table']), json(reasons(loc)));
    loc = locator.locate(synthetic.build({ relocSink: true }).buf);
    ok('a dynamic relocation over the sink slot: refused', json(reasons(loc)) === json(['sink: a dynamic relocation over the edit']), json(reasons(loc)));
    var rel = synthetic.build({ relocSink: true });
    rel.buf.writeUInt32LE(17, rel.at.dyn);   /* DT_REL in place of DT_RELA */
    loc = locator.locate(rel.buf);
    ok('relocations it cannot read: every edit refused', json(reasons(loc)) ===
        json(['variant: relocations not readable', 'sink: relocations not readable', 'self-start: relocations not readable', 'resolution: relocations not readable', 'fps: relocations not readable']), json(reasons(loc)));
    rel = synthetic.build({ relocSink: true });
    for (var ph = 64; rel.buf.readUInt32LE(ph) !== 2; ph += 56);
    rel.buf.writeUInt32LE(rel.buf.length - 8, ph + 8);   /* PT_DYNAMIC running past the end of the file */
    loc = locator.locate(rel.buf);
    ok('a dynamic segment outside the file: every edit refused, no throw', json(reasons(loc)) ===
        json(['variant: relocations not readable', 'sink: relocations not readable', 'self-start: relocations not readable', 'resolution: relocations not readable', 'fps: relocations not readable']), json(reasons(loc)));
    loc = locator.locate(Buffer.from('#!/bin/sh\nexec carplay "$@"\n'));
    ok('not an ELF', loc.elf === false && loc.reason === 'not an ELF file' && !loc.ok, json(loc));
})();

/* The patcher on a build outside the proven list. */
var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'woc-locate-'));
try {
    var tgt = path.join(dir, 'carplay'), orig = shifted.buf, at = shifted.at, r;
    fs.writeFileSync(tgt, orig, { mode: 0o755 });
    function hx(n) { return '0x' + n.toString(16); }

    r = run(['status', '--target', tgt]);
    ok('status: pristine by its stock sites, every edit ORIGINAL', /known:\s+yes, pristine build [0-9a-f]{40}/.test(r.out) &&
        /variant:\s+ORIGINAL/.test(r.out) && /sink:\s+ORIGINAL/.test(r.out) && /selfstart:\s+ORIGINAL/.test(r.out) &&
        /res:\s+ORIGINAL \(IC_HIGH advertises 1600x800\)/.test(r.out) && /fps:\s+ORIGINAL \(the phone may send the cluster up to 60 fps\)/.test(r.out), r.out);
    r = run(['probe', tgt]);
    ok('probe: the sites, not proven, patchable', r.code === 0 && /proven:\s+no/.test(r.out) && /patchable:\s+yes/.test(r.out) &&
        r.out.indexOf('slot ' + hx(at.slot) + ' (table ' + hx(at.table) + ', 12 slots)') >= 0 && r.out.indexOf('slot ' + hx(at.sink) + ' (59 bytes)') >= 0 &&
        r.out.indexOf('call ' + hx(at.call) + ' in onSuggestUi ' + hx(at.onSuggestUi)) >= 0 &&
        r.out.indexOf('immediates ' + [at.mm, at.px, at.safe1, at.safe2].map(hx).join(' ')) >= 0 &&
        r.out.indexOf('load ' + hx(at.fps) + ' in the IC display builder ' + hx(at.fpsBuilder) + ' (getter ' + hx(at.fpsGetter) + ' -> ' + hx(at.fpsConst) +
            ', main display load ' + hx(at.fpsMain) + '): ORIGINAL') >= 0 &&
        r.out.indexOf('build-id:  ' + at.buildId) >= 0, r.out);

    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('apply without --unproven: refused, no write, no backup', r.code !== 0 && /is not a proven build; pass --unproven/.test(r.out) &&
        fs.readFileSync(tgt).equals(orig) && !fs.existsSync(tgt + '.jap-orig'), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--sink', 'udp://127.0.0.1:20557?mime_type=video/H264', '--unproven', '--yes']);
    var b = fs.readFileSync(tgt);
    ok('apply high: all five edits written', r.code === 0 && /patched OK/.test(r.out) && /fps -> 15/.test(r.out) &&
        b.toString('hex', at.fps, at.fps + 7) === 'b80f0000006690' && b.readUInt32LE(at.slot) === at.high &&
        b.toString('hex', at.call, at.call + 5) === at.callPatched && b.toString('latin1', at.sink, at.sink + 9) === 'udp://127' &&
        b.readUInt32LE(at.mm) === 138 && b.readUInt32LE(at.px) === 1088, r.out);
    ok('apply high: the backup is the original', fs.readFileSync(tgt + '.jap-orig').equals(orig));
    r = run(['verify', '--target', tgt]);
    ok('verify: enabled', r.code === 0 && /OK: variant IC_HIGH, sink udp:\/\/127\.0\.0\.1:20557\?mime_type=video\/H264, self-start on, res 1088x480, fps 15$/m.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('status: fps 15', /^fps:\s+FPS 15 \(the cluster display asks the phone for at most 15 fps\)$/m.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'mid', '--yes']);
    b = fs.readFileSync(tgt);
    ok('apply mid: the stock geometry and fps back from the backup, the stock sink', r.code === 0 && /resolution -> stock 1600x800/.test(r.out) &&
        /fps -> stock 60/.test(r.out) && b.slice(at.fps, at.fps + 7).equals(orig.slice(at.fps, at.fps + 7)) &&
        b.slice(at.mm, at.safe2 + 8).equals(orig.slice(at.mm, at.safe2 + 8)) && b.slice(at.sink, at.sink + 59).equals(orig.slice(at.sink, at.sink + 59)) &&
        b.readUInt32LE(at.slot) === at.mid, r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert: byte-exact original', r.code === 0 && fs.readFileSync(tgt).equals(orig), r.out);

    /* A patch an earlier release wrote (four edits, the stock fps load) is
     * enabled, and Enable adds only the fps edit. */
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    var four = fs.readFileSync(tgt);
    Buffer.from(at.fpsOriginal, 'hex').copy(four, at.fps);
    fs.writeFileSync(tgt, four);
    r = run(['verify', '--target', tgt]);
    ok('four-edit patch: verify enabled, fps stock', r.code === 0 && /self-start on, res 1088x480, fps stock$/m.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('four-edit patch: known, fps ORIGINAL', /known:\s+yes, modified build/.test(r.out) && /fps:\s+ORIGINAL/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    var fd = [], now = fs.readFileSync(tgt);
    for (var i = 0; i < now.length; i++) if (now[i] !== four[i]) fd.push(i);
    ok('four-edit patch: Enable adds only the fps edit', r.code === 0 && /About to patch [^\n]*\n  fps -> 15\npatched OK/.test(r.out) &&
        fd.length > 0 && fd.every(function (x) { return x >= at.fps && x < at.fps + 7; }), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--fps', '30', '--yes']);
    ok('--fps 30 applies and reads back', r.code === 0 && /fps -> 30/.test(r.out) && /fps:\s+FPS 30 /.test(run(['status', '--target', tgt]).out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--fps', 'stock', '--yes']);
    ok('--fps stock writes the backup\'s load back', r.code === 0 && /fps -> stock 60/.test(r.out) &&
        fs.readFileSync(tgt).slice(at.fps, at.fps + 7).equals(orig.slice(at.fps, at.fps + 7)), r.out);
    var cur = fs.readFileSync(tgt);
    [[['--fps', '0'], /out of range/], [['--fps', '61'], /out of range/], [['--fps', '15x'], /--fps needs/], [['--fps'], /--fps needs/]].forEach(function (t) {
        r = run(['apply', '--target', tgt, '--panel', 'high'].concat(t[0], ['--yes']));
        ok('apply ' + t[0].join(' ') + ': refused, no change', r.code !== 0 && t[1].test(r.out) && fs.readFileSync(tgt).equals(cur), r.out);
    });
    r = run(['apply', '--target', tgt, '--panel', 'mid', '--fps', '15', '--yes']);
    ok('--fps with --panel mid: refused, no change', r.code !== 0 && /--panel high/.test(r.out) && fs.readFileSync(tgt).equals(cur), r.out);
    /* The fps edit alone is a partial patch. */
    var fpsOnly = Buffer.from(orig);
    Buffer.from('b80f0000006690', 'hex').copy(fpsOnly, at.fps);
    fs.writeFileSync(tgt, fpsOnly);
    r = run(['verify', '--target', tgt]);
    ok('fps edit alone: verify FAILS as partially patched', r.code !== 0 && /partially patched — .*, fps patched/.test(r.out), r.out);
    var outOfRange = Buffer.from(orig);
    Buffer.from('b83d0000006690', 'hex').copy(outOfRange, at.fps);   /* 61 */
    fs.writeFileSync(tgt, outOfRange);
    r = run(['status', '--target', tgt]);
    ok('fps load of 61: UNKNOWN, not patchable', /fps:\s+UNKNOWN \(fps: load not recognised/.test(r.out) && /known:\s+NO/.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('revert: byte-exact original, the fps load too', r.code === 0 && fs.readFileSync(tgt).equals(orig), r.out);

    /* A build without a GNU build-id: the same build is the same size and at
     * most a few hundred bytes apart from the backup. */
    var noId = Buffer.from(orig);
    noId.writeUInt32LE(0, 0x208);   /* the build-id note's type */
    [tgt + '.jap-orig', tgt + '.jap-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    fs.writeFileSync(tgt + '.jap-orig.tmp', 'cut short');        /* an earlier apply's, cut short */
    fs.writeFileSync(tgt + '.jap-orig.json.tmp', '{"sha');
    fs.writeFileSync(tgt, noId);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    ok('no build-id: apply writes the backup and its record, an earlier temporary file gone', r.code === 0 && fs.existsSync(tgt + '.jap-orig') &&
        fs.existsSync(tgt + '.jap-orig.json') && !fs.existsSync(tgt + '.jap-orig.tmp') && !fs.existsSync(tgt + '.jap-orig.json.tmp'), r.out);
    var noIdPatched = fs.readFileSync(tgt), mangled = Buffer.from(noIdPatched);
    mangled[at.highHandler] = 0x90;   /* a site the locator now refuses */
    fs.writeFileSync(tgt, mangled);
    r = run(['status', '--target', tgt]);
    ok('no build-id: a patch the locator refuses is restorable', /known:\s+NO/.test(r.out) && /^restorable: yes$/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('no build-id: revert writes the original whole', r.code === 0 && fs.readFileSync(tgt).equals(noId), r.out);
    fs.writeFileSync(tgt + '.jap-orig.tmp', 'cut short');
    fs.writeFileSync(tgt + '.jap-orig.json.tmp', '{"sha');
    r = run(['revert', '--target', tgt, '--yes', '--drop-backup']);
    ok('a removal drops the backup, its record and any temporary file', r.code === 0 && !fs.existsSync(tgt + '.jap-orig') &&
        !fs.existsSync(tgt + '.jap-orig.json') && !fs.existsSync(tgt + '.jap-orig.tmp') && !fs.existsSync(tgt + '.jap-orig.json.tmp'), r.out);
    run(['apply', '--target', tgt, '--panel', 'high', '--unproven', '--yes']);
    var far = Buffer.from(noId);
    for (var k = 0; k < 400; k++) far[0x300 + k] ^= 0xff;
    fs.writeFileSync(tgt, far);
    r = run(['status', '--target', tgt]);
    ok('no build-id: a binary hundreds of bytes away is another build', /^restorable: no \(another build: no build-id, and more than 214 bytes differ\)$/m.test(r.out), r.out);
    r = run(['revert', '--target', tgt, '--yes']);
    ok('no build-id: revert refuses it without --force, saying why', r.code !== 0 && /another build than the backup \(no build-id, and more than 214 bytes differ\)/.test(r.out) &&
        fs.readFileSync(tgt).equals(far), r.out);

    /* A backup an older release left without its record, cut short: never
     * written, also with --force. */
    fs.writeFileSync(tgt, noIdPatched);
    fs.writeFileSync(tgt + '.jap-orig', noId.slice(0, noId.length - 4096));
    fs.unlinkSync(tgt + '.jap-orig.json');
    r = run(['revert', '--target', tgt, '--yes', '--force']);
    ok('a short backup with no record: revert --force refuses, no write', r.code !== 0 && /no record of the original/.test(r.out) &&
        fs.readFileSync(tgt).equals(noIdPatched), r.out);

    /* A backup whose build-id note cannot be read: status says why and goes on. */
    var badNote = Buffer.from(orig);
    badNote.writeUInt32LE(0x7ffffffe, 0x200);   /* the note's name size */
    fs.writeFileSync(tgt + '.jap-orig', badNote);
    r = run(['status', '--target', tgt]);
    ok('a backup with a malformed note: not restorable, with the reason, status complete', r.code === 0 && /^restorable: no \(.+\)$/m.test(r.out) &&
        /^backup: /m.test(r.out), r.out);
    [tgt + '.jap-orig', tgt + '.jap-orig.json'].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });

    var amb = synthetic.build({ twoSinks: true }).buf;
    fs.writeFileSync(tgt, amb);
    r = run(['probe', tgt]);
    ok('probe of an ambiguous build: not patchable, with the reason', /patchable:\s+no — sink: URL slot found 2 times/.test(r.out), r.out);
    r = run(['status', '--target', tgt]);
    ok('status of an ambiguous build: known NO with the reason', /known:\s+NO — sink: URL slot found 2 times/.test(r.out), r.out);
    r = run(['apply', '--target', tgt, '--panel', 'high', '--yes']);
    ok('apply refuses it, no write', r.code !== 0 && /refusing: sink: URL slot found 2 times/.test(r.out) && fs.readFileSync(tgt).equals(amb), r.out);
} finally {
    cp.execFileSync('rm', ['-rf', dir]);
}

/* The reference build: the documented offsets. */
var SRC = process.env.CARPLAY_BIN || '';
if (fs.existsSync(SRC)) {
    var loc = locator.locate(fs.readFileSync(SRC));
    ok('reference build: the documented sites', loc.ok && loc.variant.table === 0x2ba4d4 && loc.variant.slot === 0x2ba4f0 &&
        loc.variant.original === 0xfff7f2ec && loc.variant.high === 0xfff7f2bc && loc.variant.mid === 0xfff7f30c &&
        loc.sink.offset === 0x2a20b0 && loc.sink.len === 59 &&
        loc.selfstart.offset === 0x18176c && loc.selfstart.original === 'e8effdffff' && loc.selfstart.patched === 'e89f000000' &&
        loc.selfstart.setStreamStatus === 0x181560 && loc.selfstart.requestStream === 0x181810 && loc.selfstart.onSuggestUi === 0x1816f0 &&
        loc.res.builder === 0x20ca30 && json([loc.res.mm, loc.res.px, loc.res.safe1, loc.res.safe2]) === json([0x20ca32, 0x20ca54, 0x20ca66, 0x20ca74]) &&
        loc.fps.offset === 0x207a24 && loc.fps.getter === 0x1fe5a0 && loc.fps.main === 0x2076e7 && loc.fps.builder === 0x2078b0 &&
        loc.fps.constant === 0x2a2c50 && loc.fps.value === 60 && loc.fps.original === 'e8776bffff8b00', json(loc));
} else {
    process.stdout.write('skip reference build: set CARPLAY_BIN to a copy of the unit\'s /usr/bin/carplay (carplay 22.28.3)\n');
}

process.stdout.write(fails ? ('\n' + fails + ' check(s) failed\n') : '\nall checks passed\n');
process.exit(fails ? 1 : 0);
