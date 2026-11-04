#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
/* Proves the guard against robertnisipeanu's Waze on cluster (conflict.js):
 * while its tool is installed, `patchd.js activate` and `deactivate` exit 0,
 * say blocked, and write nothing (carplay, aapd, backups, pending marker),
 * running no systemctl, mount or reboot; the daemon's status carries the
 * reason and refuses apply and aa-apply; takeover removes the other app, then
 * the other tool, through the deploy tool, stops at a failed removal, and
 * leaves this tool unblocked; the dormant units carry the condition. Fakes
 * the deploy tool, systemctl, mount and reboot.
 *
 *   node conflict_test.js
 */
'use strict';

var fs = require('fs');
var os = require('os');
var net = require('net');
var path = require('path');
var cp = require('child_process');

var PATCHD = path.join(__dirname, '..', 'patchd.js');
var fails = 0;
function ok(name, cond, detail) {
    process.stdout.write((cond ? 'ok   ' : 'FAIL ') + name + (detail !== undefined && !cond ? '  — ' + detail : '') + '\n');
    if (!cond) fails++;
}

var dir = fs.mkdtempSync(path.join(os.tmpdir(), 'jap-conflict-'));
var children = [];
process.on('exit', function () {
    children.forEach(function (c) { try { c.kill('SIGKILL'); } catch (e) {} });
    try { cp.execFileSync('rm', ['-rf', dir]); } catch (e) {}
});

var OTHER = path.join(dir, 'other', 'current', 'component.json');
var CALLS = path.join(dir, 'calls');
var BIN = path.join(dir, 'bin');
var SOCKDIR = path.join(dir, 'shm');
var TGT = path.join(dir, 'carplay');
var AAPD = path.join(dir, 'aapd');
var AAPD_CONF = path.join(dir, 'aapd.conf');
fs.mkdirSync(BIN);
fs.mkdirSync(path.dirname(OTHER), { recursive: true });
fs.writeFileSync(CALLS, '');
['systemctl', 'kill', 'reboot', 'mount', 'journalctl'].forEach(function (n) {
    fs.writeFileSync(path.join(BIN, n), '#!/bin/sh\necho "' + n + ' $*" >> "' + CALLS + '"\n');
    fs.chmodSync(path.join(BIN, n), 0o755);
});
/* The deploy tool: status lists what is installed; uninstall of the other
 * tool removes its folder, as mgu-deploy does; FAIL_ID fails that removal. */
var INSTALLED = path.join(dir, 'installed.json');
var DEPLOY = path.join(dir, 'mgu-deploy.sh');
fs.writeFileSync(DEPLOY, '#!' + process.execPath + '\n' + [
    "var fs = require('fs'), a = process.argv.slice(2);",
    "fs.appendFileSync(" + JSON.stringify(CALLS) + ", 'deploy ' + a.join(' ') + '\\n');",
    "var ids = JSON.parse(fs.readFileSync(" + JSON.stringify(INSTALLED) + ", 'utf8'));",
    "if (a[0] === 'status') { process.stdout.write(JSON.stringify({ components: ids.map(function (id) { return { id: id }; }) })); process.exit(0); }",
    "if (a[0] === 'uninstall') {",
    "    if (process.env.FAIL_ID === a[1]) { process.stderr.write('waze-on-cluster deactivate: refused\\n'); process.exit(2); }",
    "    fs.writeFileSync(" + JSON.stringify(INSTALLED) + ", JSON.stringify(ids.filter(function (i) { return i !== a[1]; })));",
    "    if (a[1] === 'waze-on-cluster') require('child_process').execFileSync('rm', ['-rf', " + JSON.stringify(path.join(dir, 'other')) + "]);",
    "}",
].join('\n') + '\n');
fs.chmodSync(DEPLOY, 0o755);

var CARPLAY = Buffer.from('a carplay patched by the other tool');
fs.writeFileSync(TGT, CARPLAY, { mode: 0o755 });
fs.writeFileSync(AAPD, 'aapd');
fs.writeFileSync(AAPD_CONF, 'conf');
fs.writeFileSync(OTHER, '{"id":"waze-on-cluster"}');

var ENV = Object.assign({}, process.env, {
    PATH: BIN + path.delimiter + process.env.PATH,
    WOC_OTHER_TOOL_FILE: OTHER,
    WOC_DEPLOY: DEPLOY,
    WOC_PATCH_SOCK: path.join(SOCKDIR, 'patch.sock'),
    WOC_CARPLAY: TGT,
    WOC_AAPD: AAPD,
    WOC_AAPD_CONF: AAPD_CONF,
    WOC_MOUNT: path.join(BIN, 'mount'),
    WOC_PROC_MOUNTS: path.join(dir, 'no-mounts'),
    WOC_UNIT_DIRS: path.join(dir, 'no-units'),
    WOC_CAPTURE_SOCK: path.join(dir, 'no-capture.sock'),
    WOC_DLT_RECEIVE: path.join(dir, 'no-dlt-receive'),
    WOC_DLT_RETRY_MS: '600000',
    WOC_REBOOT_CMD: path.join(BIN, 'reboot'),
    WOC_STATE_DIR: path.join(dir, 'var'),
    WOC_REMOUNT_LOCK: path.join(SOCKDIR, 'remount.lock'),
});

function hook(which) {
    var r = cp.spawnSync(process.execPath, [PATCHD, which], { env: ENV, encoding: 'utf8', timeout: 30000 });
    var lines = (r.stdout || '').split('\n').filter(function (l) { return l.trim(); });
    var result = {};
    try { result = JSON.parse(lines[lines.length - 1]); } catch (e) {}
    return { code: r.status, result: result, out: (r.stdout || '') + (r.stderr || '') };
}
function untouched() {
    return fs.readFileSync(TGT).equals(CARPLAY) && fs.readFileSync(AAPD, 'utf8') === 'aapd' &&
        !fs.existsSync(TGT + '.jap-orig') && !fs.existsSync(AAPD + '.jap-orig') &&
        !fs.existsSync(path.join(SOCKDIR, 'activation-pending'));
}
function calls() { return fs.readFileSync(CALLS, 'utf8').split('\n').filter(Boolean); }

/* patchd as a daemon: send cmds in turn, resolve with every status seen. */
function daemon(cmds, until) {
    return new Promise(function (resolve, reject) {
        var d = cp.spawn(process.execPath, [PATCHD], { env: ENV, stdio: ['ignore', 'ignore', 'inherit'] });
        children.push(d);
        var tries = 0, seen = [];
        var timer = setTimeout(function () { d.kill('SIGKILL'); reject(new Error('timed out; saw ' + JSON.stringify(seen))); }, 20000);
        (function connect() {
            var c = net.connect(path.join(SOCKDIR, 'patch.sock')), buf = '', sent = false;
            c.setEncoding('utf8');
            c.on('error', function () { if (++tries > 200) { reject(new Error('no patchd socket')); } else setTimeout(connect, 25); });
            c.on('data', function (chunk) {
                buf += chunk;
                var i;
                while ((i = buf.indexOf('\n')) >= 0) {
                    var m = JSON.parse(buf.slice(0, i));
                    buf = buf.slice(i + 1);
                    if (m.ev !== 'status') continue;
                    seen.push(m);
                    if (!sent) { sent = true; cmds.forEach(function (x) { c.write(JSON.stringify(x) + '\n'); }); }
                    if (until(m, seen)) { clearTimeout(timer); c.destroy(); d.kill('SIGKILL'); resolve(seen); return; }
                }
            });
        })();
    });
}

async function main() {
    var conflict = require('../conflict.js');
    var r;

    fs.writeFileSync(INSTALLED, JSON.stringify(['cluster-render', 'waze-on-cluster', 'robertn_waze_cluster']));
    r = hook('activate');
    ok('blocked activate: exits 0, says blocked, asks no restart', r.code === 0 && r.result.blocked === true && !r.result.reboot &&
        /Robert Nisipeanu's Waze on cluster is installed/.test(r.result.status), r.out);
    ok('blocked activate: carplay, aapd, backups and the pending marker untouched', untouched());
    r = hook('deactivate');
    ok('blocked deactivate: exits 0 and leaves carplay and aapd as they are', r.code === 0 && r.result.blocked === true && untouched(), r.out);
    ok('blocked hooks: no systemctl, mount, kill or reboot', calls().length === 0, calls().join('; '));

    /* The other one installed after ours: our removal still undoes our own
     * patch (our backup is there), never short-cut as blocked. */
    fs.writeFileSync(TGT + '.jap-orig', 'our pristine carplay');
    r = hook('deactivate');
    ok('blocked deactivate with our backup: runs the normal revert of our patch (the patcher decides), not short-cut',
        r.result.blocked !== true && /undoing our own patch, from our backup/.test(r.out) && /carplay/.test(r.result.status || ''), r.out);
    r = hook('activate');
    ok('blocked activate with our backup: still dormant', r.code === 0 && r.result.blocked === true, r.out);
    [TGT + '.jap-orig', TGT + '.jap-orig.json', path.join(SOCKDIR, 'activation-pending')].forEach(function (f) { try { fs.unlinkSync(f); } catch (e) {} });
    fs.writeFileSync(TGT, CARPLAY, { mode: 0o755 });
    fs.writeFileSync(CALLS, '');

    var seen = await daemon([{ cmd: 'apply', panel: 'high', mode: 'avb' }, { cmd: 'aa-apply' }], function (m, all) {
        return all.length >= 3 && all.some(function (x) { return x.aa && x.aa.reason; });
    });
    ok('daemon: the status carries why it is blocked', seen.every(function (m) { return /Waze on cluster is installed/.test(m.blocked); }));
    ok('daemon: apply and aa-apply refused with that reason', seen.some(function (m) { return /installed/.test(m.reason); }) &&
        seen.some(function (m) { return m.aa && /installed/.test(m.aa.reason); }));
    ok('daemon: nothing written while blocked', untouched());

    /* takeover through conflict.js: the app first, then the tool. */
    process.env.WOC_OTHER_TOOL_FILE = OTHER;
    fs.writeFileSync(CALLS, '');
    var err = await new Promise(function (res) {
        var c2 = cp.spawnSync(process.execPath, ['-e', [
            "var c = require(" + JSON.stringify(path.join(__dirname, '..', 'conflict.js')) + ");",
            "c.takeover(function () {}, function (e) { process.stdout.write(e ? e.message : 'ok'); });"].join('\n')],
            { env: Object.assign({}, ENV, { FAIL_ID: 'waze-on-cluster' }), encoding: 'utf8' });
        res(c2.stdout);
    });
    ok('takeover: a refused removal is the error, the tool stays, this one stays blocked',
        /removing waze-on-cluster failed: waze-on-cluster deactivate: refused/.test(err) && fs.existsSync(OTHER), err);
    ok('takeover: the app is removed before the tool', calls().filter(function (l) { return /uninstall/.test(l); }).join(',') ===
        'deploy uninstall robertn_waze_cluster --yes,deploy uninstall waze-on-cluster --yes', calls().join('; '));

    fs.writeFileSync(CALLS, '');
    seen = await daemon([{ cmd: 'takeover' }], function (m) { return m.takeover && m.takeover !== 'removing' && m.takeover !== 'activating'; });
    var last = seen[seen.length - 1];
    ok('daemon takeover: removes the tool, then runs activate', /^(done|failed: activate)/.test(last.takeover) &&
        calls().filter(function (l) { return /uninstall/.test(l); }).join(',') === 'deploy uninstall waze-on-cluster --yes', last.takeover + ' ' + calls().join('; '));
    ok('daemon takeover: no longer blocked, cluster-render left installed', last.blocked === null && !conflict.blocked() &&
        JSON.parse(fs.readFileSync(INSTALLED, 'utf8')).indexOf('cluster-render') >= 0);
    ok('daemon takeover: activate ran unblocked (patched or a reported conflict, never "blocked")',
        !calls().some(function (l) { return /reboot/.test(l); }));

    var units = path.join(__dirname, '..', '..', 'systemd');
    var COND = 'ConditionPathExists=!/var/opt/waze-on-cluster/current/component.json';
    var dormant = ['jap-waze-on-cluster.socket', 'jap-waze-on-cluster.service', 'jap-waze-on-cluster-capture.service', 'jap-waze-on-cluster-hudguide.service',
        'jap-waze-on-cluster-recovery.service'];
    ok('units: the map and its socket, capture, hudguide and the crash watcher are dormant while the other tool is installed',
        dormant.every(function (u) { return fs.readFileSync(path.join(units, u), 'utf8').indexOf(COND) >= 0; }));
    ok('units: patchd and the boot steps are not (the app needs patchd; the boot steps guard a patch of ours)',
        ['jap-waze-on-cluster-patchd.service', 'jap-waze-on-cluster-boot.service', 'jap-waze-on-cluster-aa-boot.service']
            .every(function (u) { return fs.readFileSync(path.join(units, u), 'utf8').indexOf(COND) < 0; }));

    process.stdout.write(fails ? 'conflict_test: ' + fails + ' FAILED\n' : 'conflict_test: all passed\n');
    process.exit(fails ? 1 : 0);
}

main().catch(function (e) { process.stdout.write('FAIL ' + e.stack + '\n'); process.exit(1); });
