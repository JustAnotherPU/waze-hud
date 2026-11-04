#!/usr/bin/env node
// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/* recovery.js — the safety net for the carplay and aapd patches (PATCH.md
 * "Boot recovery"). A bad interaction (e.g. a self-start throwing on a phone
 * connect, PATCH.md "Exception risk") can crash the patched service at every
 * session. A crash the crash handler or recmgr does not let pass reboots the
 * head unit with the patched binary still on disk: a boot loop with no HMI
 * to turn the patch off. Each service has its own counter and acts alone.
 *
 *   recovery.js boot [carplay|aapd]
 *                      jap-waze-on-cluster-boot.service (carplay) and
 *                      jap-waze-on-cluster-aa-boot.service (aapd), oneshots
 *                      ordered before their service: count the previous
 *                      boot and, at the threshold, restore the original
 *                      before the service starts.
 *   recovery.js        jap-waze-on-cluster-recovery.service: watches both for
 *                      the rest of the boot and records each crash the moment
 *                      it is seen; the boot step decides at the next start.
 *                      Where recmgr loaded a service's entry it also restores
 *                      at the threshold and starts the service again, for as
 *                      long as the head unit stays up.
 *
 * shutdown_mark.sh (a var-opt-ro.service drop-in) records at each head-unit
 * shutdown, before /var/opt goes read-only, whether each service was
 * running, failed or stopped. recmgr's reboot and a switch-off are both an
 * NSM fast shutdown that stops no service, so that record, not a SIGTERM,
 * tells a crash from a switch-off.
 *
 * A service's state holds fails, the crashes of its patched binary since
 * the last boot in which it ran healthy and did not crash; arm, this boot's
 * record ({boot, crashes, confirmed, clean}), kept while a backup of ours
 * exists; starts ({boot, n}), the starts made this boot; and ended ({boot,
 * run}), the last run whose end was handled. At the next boot a previous
 * boot adds its crashes (one when the mark says failed and none were seen);
 * one with none clears the counter if the service ran HEALTHY_MS
 * continuously, keeps it if it shut down with the service running or
 * stopped (or on SIGTERM while the system stops), and otherwise adds one (it
 * ended with no record, e.g. power lost).
 *
 * The restore is the patcher's revert Disable uses (a verified backup of the
 * target's own build), under the remount lock shared with patchd, with /
 * read-write only for the write. It acts when the patcher's status reads
 * `restorable: yes` (a revert would change a file, whatever the patcher's
 * site checks make of it); a stock or firmware-replaced binary clears the
 * counter. A state it cannot write loses the count, so a restorable patch is
 * then restored at once, whatever the count. Every command it runs is
 * bounded (STEP_MS), so a restore ends within RESTORE_BOUND_MS. It never
 * kills, restarts or reboots anything; the only start is `systemctl start`
 * of the service after a crash or a SIGKILL patchd did not send, never stop
 * or restart, bounded by START_DELAYS_MS per boot.
 *
 * Node 8.16.2 on the unit. No dependencies.
 */
'use strict';

var fs = require('fs');
var path = require('path');
var cp = require('child_process');

var HERE = __dirname;
var STATE = process.env.WOC_RECOVERY_STATE || '/var/opt/jap-waze-on-cluster/boot-recovery.json';
var SOCK = process.env.WOC_PATCH_SOCK || '/dev/shm/jap-waze-on-cluster/patch.sock';
/* ExecMainCode of a main process a signal ended without a core dump. */
var CLD_KILLED = '2';

/* What is watched and restored, per service. state: the counter;
 * mark: written by shutdown_mark.sh, "<boot id> running|failed|stopped";
 * pending: patchd's marker of a change waiting for the next start; killed:
 * the run (InvocationID) patchd kills to activate a change, whose end is not
 * a crash; running: the patched binary, kept hard-linked while restoring so
 * replacing the target by rename does not leave an unlinked-but-open file
 * that blocks the read-only remount (patchd.js keeps the same link);
 * killedRun: how a SIGKILL that is no crash ends the run. carplay's drop-in
 * makes SIGKILL a success; aapd.service has none, so SIGKILL ends it
 * failed with Result=signal. */
var CARPLAY = process.env.WOC_CARPLAY || '/usr/bin/carplay';
var AAPD = process.env.WOC_AAPD || '/usr/bin/aapd';
var AAPD_CONF = process.env.WOC_AAPD_CONF || '/etc/aapd.conf';
var TARGETS = {
    carplay: {
        name: 'carplay', unit: 'carplay.service', target: CARPLAY, patcher: path.join(HERE, 'carplay_patch.js'), files: ['--target', CARPLAY],
        backups: [CARPLAY + '.jap-orig'], state: STATE, mark: path.join(path.dirname(STATE), 'shutdown'),
        pending: path.join(path.dirname(SOCK), 'activation-pending'), killed: path.join(path.dirname(SOCK), 'carplay-killed'),
        running: CARPLAY + '.jap-running',
        killedRun: function (i) { return i.state === 'inactive' && i.result === 'success' && i.code === CLD_KILLED && i.status === '9'; },
    },
    aapd: {
        name: 'aapd', unit: 'aapd.service', target: AAPD, patcher: path.join(HERE, 'aapd_patch.js'),
        files: ['--target', AAPD, '--conf', AAPD_CONF, '--coding', process.env.WOC_CODING || '/dev/shm/rawcodingdata'],
        backups: [AAPD + '.jap-orig', AAPD_CONF + '.jap-orig'], state: path.join(path.dirname(STATE), 'aa-boot-recovery.json'),
        mark: path.join(path.dirname(STATE), 'shutdown-aapd'),
        pending: path.join(path.dirname(SOCK), 'aa-pending'), killed: path.join(path.dirname(SOCK), 'aapd-killed'),
        running: AAPD + '.jap-running',
        killedRun: function (i) { return (i.state === 'failed' || i.state === 'inactive') && i.result === 'signal' && i.code === CLD_KILLED && i.status === '9'; },
    },
};
var MOUNT = process.env.WOC_MOUNT || '/bin/mount';
var PROC_MOUNTS = process.env.WOC_PROC_MOUNTS || '/proc/mounts';
var PROC = process.env.WOC_PROC || '/proc';
var BOOT_ID = process.env.WOC_BOOT_ID || '/proc/sys/kernel/random/boot_id';
var SYSTEMCTL = process.env.WOC_SYSTEMCTL || 'systemctl';

var THRESHOLD = parseInt(process.env.WOC_RECOVERY_THRESHOLD, 10) || 2;
var HEALTHY_MS = parseInt(process.env.WOC_RECOVERY_HEALTHY_MS, 10) || 60000;
var TICK_MS = parseInt(process.env.WOC_RECOVERY_TICK_MS, 10) || 1000;
/* systemctl is asked this often; between asks a tick only checks /proc. */
var PROBE_MS = 5000;
var START_DELAYS_MS = [5000, 15000, 60000];
/* The bound on each mount; the classify and the revert get three, since they
 * read the 3.5 MB binary on a head unit still busy starting. A restore
 * (classify, remount rw, revert, two read-only tries) ends within
 * RESTORE_BOUND_MS, which the units' start and stop timeouts cover. */
var STEP_MS = 10000;
var CLASSIFY_MS = 3 * STEP_MS;
var RO_TRIES = 2;
var RESTORE_BOUND_MS = CLASSIFY_MS + STEP_MS * (1 + 3 + RO_TRIES) + 500 * (RO_TRIES - 1);
/* The two boot steps run side by side and share the remount lock: a boot
 * step waits this long for the other's restore before its own, so the
 * boot units' timeouts cover BOOT_BOUND_MS. */
var BOOT_LOCK_WAIT_MS = 20000;
var BOOT_BOUND_MS = BOOT_LOCK_WAIT_MS + RESTORE_BOUND_MS;

var lock = require('./remount_lock.js');
var recmgr = require('./recmgr_list.js');

/* / is read-only per /proc/mounts (the last `/` entry is the effective root). */
function rootIsReadOnly() {
    try {
        var lines = fs.readFileSync(PROC_MOUNTS, 'utf8').split('\n');
        var opts = null;
        for (var i = 0; i < lines.length; i++) {
            var p = lines[i].split(' ');
            if (p[1] === '/') opts = p[3];
        }
        if (opts !== null) return /(^|,)ro(,|$)/.test(opts);
    } catch (e) {}
    return false;
}

function remount(rw, cb) {
    cp.execFile(MOUNT, ['-o', 'remount,' + (rw ? 'rw' : 'ro'), '/'], { timeout: STEP_MS }, function (err) { cb(err); });
}

function remountRo(tries, cb) {
    remount(false, function () {
        if (rootIsReadOnly()) { cb(null); return; }
        if (tries > 1) { setTimeout(function () { remountRo(tries - 1, cb); }, 500); return; }
        cb(new Error('/ is still read-write after remount'));
    });
}

function readText(f) { try { return fs.readFileSync(f, 'utf8'); } catch (e) { return ''; } }
function readBootId() { return readText(BOOT_ID).trim(); }
function monoMs() { var t = process.hrtime(); return t[0] * 1000 + t[1] / 1e6; }

/* ---- the real dependencies, injectable for host tests ---- */

/* The on-disk state of t's patch. ok is false when the status could not be
 * read (a transient spawn/timeout failure), so the counter is not disturbed.
 * The patch counts as active, and restorable, when the patcher's revert
 * would change a file (`restorable: yes`): a verified backup of the same
 * build differs from it, whatever the patcher's site checks make of it, so a
 * patch any version of this tool wrote is counted and restored. */
function classifyTarget(t, cb) {
    cp.execFile(process.execPath, [t.patcher, 'status'].concat(t.files), { timeout: CLASSIFY_MS }, function (err, out) {
        var text = (out || '') + '';
        if (err || !/^size:/m.test(text)) { cb({ ok: false }); return; }
        var restorable = /^restorable:\s+yes$/m.test(text);
        cb({ ok: true, patchActive: restorable, restorable: restorable });
    });
}

/* Restore the original with the vetted revert, under the shared remount lock,
 * / read-write only for the write and read-only again after. The patched
 * binary is hard-linked to t.running first so the rename does not leave an
 * unlinked-but-open file that would keep / writable: always in the watcher,
 * where the service can start between that check and the rename (patchd
 * removes the link once nothing runs it), and in the boot step, which the
 * service waits for, only when a process runs it. A link a process still
 * runs (a change pending this boot) is that link already and stays. With
 * both the link and the target run by processes there is no name left to
 * keep the target linked, so the restore waits for the next boot. Yields to
 * patchd if the lock is held (cb with an error; the counter stays, so a
 * later try restores). */
function restoreTarget(t, onlyIfRunning, cb, waitMs) {
    if (lock.holds(t.running, t.target) && lock.inUse(t.target)) { cb(new Error('processes run both ' + t.target + ' and ' + t.running + '; deferring')); return; }
    if (waitMs) {
        lock.acquire(waitMs, function (got) {
            if (got) locked(t, onlyIfRunning, cb);
            else cb(new Error('remount lock held for ' + waitMs + ' ms; deferring'));
        });
        return;
    }
    if (!lock.tryAcquire()) { cb(new Error('remount lock held; deferring')); return; }
    locked(t, onlyIfRunning, cb);
}
function locked(t, onlyIfRunning, cb) {
    var released = false;
    function finish(err) { if (!released) { released = true; lock.release(); } cb(err); }
    remount(true, function (mErr) {
        if (mErr) { finish(new Error('remount rw: ' + mErr.message)); return; }
        if (!lock.holds(t.running, t.target) && (!onlyIfRunning || lock.inUse(t.target))) {
            try { fs.unlinkSync(t.running); } catch (e) {}
            try { fs.linkSync(t.target, t.running); } catch (e) { log('keeping the running ' + t.name + ' linked failed: ' + e.message); }
        }
        cp.execFile(process.execPath, [t.patcher, 'revert', '--yes'].concat(t.files), { timeout: 3 * STEP_MS }, function (pErr, out, errOut) {
            var text = ((out || '') + '') + ((errOut || '') + '');
            try { fs.unlinkSync(t.pending); } catch (e) {}   /* the pending patch is gone */
            remountRo(RO_TRIES, function (roErr) {
                finish(pErr ? new Error('revert: ' + pErr.message + (text ? ' (' + text.trim().slice(-200) + ')' : '')) : roErr);
            });
        });
    });
}
function realRestore(onlyIfRunning, cb) { restoreTarget(TARGETS.carplay, onlyIfRunning, cb); }

/* t.unit's current run: its id (InvocationID, else the main process's start
 * time), main PID, ActiveState, Result, and how its main process ended
 * (ExecMainCode, a CLD_* code, and ExecMainStatus). */
function probeTarget(t, cb) {
    cp.execFile(SYSTEMCTL, ['show', t.unit, '-p', 'InvocationID', '-p', 'ExecMainStartTimestampMonotonic',
        '-p', 'MainPID', '-p', 'ActiveState', '-p', 'Result', '-p', 'ExecMainCode', '-p', 'ExecMainStatus'], { timeout: STEP_MS }, function (err, out) {
        cb(parseShow((out || '') + ''));
    });
}

function parseShow(text) {
    function get(k) { var m = new RegExp('^' + k + '=(.*)$', 'm').exec(text); return m ? m[1].trim() : ''; }
    var start = get('ExecMainStartTimestampMonotonic');
    return {
        run: get('InvocationID') || (start && start !== '0' ? 't' + start : ''),
        pid: parseInt(get('MainPID'), 10) || 0,
        state: get('ActiveState'),
        result: get('Result'),
        code: get('ExecMainCode'),
        status: get('ExecMainStatus'),
    };
}

function realSystemState(cb) {
    cp.execFile(SYSTEMCTL, ['is-system-running'], { timeout: STEP_MS }, function (err, out) { cb(((out || '') + '').trim()); });
}

function startTarget(t, cb) {
    cp.execFile(SYSTEMCTL, ['start', t.unit], { timeout: 30000 }, function (err) { cb(err); });
}

function markOf(t, boot) {
    var m = /^(\S+) (running|failed|stopped)\s*$/.exec(readText(t.mark));
    return m && m[1] === boot ? m[2] : null;
}

function log(line) { process.stdout.write('[' + new Date().toISOString() + '] recovery: ' + line + '\n'); }

/* ---- the counter ---- */

function newArm(boot) { return { boot: boot, crashes: 0, confirmed: false, clean: false }; }

/* jap-waze-on-cluster 0.7.5 and earlier wrote the arm as {boot, state:
 * unconfirmed|confirmed|clean}; a unit updated mid-boot still holds one. */
function normArm(a) {
    if (!a || typeof a !== 'object') return null;
    return { boot: a.boot, crashes: a.crashes || 0, confirmed: !!(a.confirmed || a.state === 'confirmed'), clean: !!(a.clean || a.state === 'clean') };
}

/* fails after the previous boot, from its arm, its shutdown mark, and
 * whether the other service crashed in it (explained): a boot that ended
 * with no record of its own after the other service's crash was ended by
 * that crash, and counts for that service only. */
function tally(fails, arm, mark, explained) {
    var crashes = arm.crashes || (mark === 'failed' ? 1 : 0);
    if (crashes) return fails + crashes;
    if (arm.confirmed) return 0;
    if (arm.clean || mark === 'running' || mark === 'stopped' || explained) return fails;
    return fails + 1;
}

/* Whether t's other service recorded a crash in boot: its arm of that boot,
 * or the crashed one its boot step counted (prev). */
function otherCrashed(t, boot) {
    var other = t === TARGETS.carplay ? TARGETS.aapd : TARGETS.carplay, st = null;
    try { st = JSON.parse(fs.readFileSync(other.state, 'utf8')); } catch (e) { return false; }
    return [st && st.arm, st && st.prev].some(function (a) { return !!a && a.boot === boot && a.crashes > 0; });
}

function Recovery(opts) {
    opts = opts || {};
    var t = this.target = opts.target || TARGETS.carplay;
    this.statePath = opts.statePath || t.state;
    this.threshold = opts.threshold || THRESHOLD;
    this.healthyMs = opts.healthyMs || HEALTHY_MS;
    this.bootId = opts.bootId || readBootId();
    this._classify = opts.classify || function (cb) { classifyTarget(t, cb); };
    this._restore = opts.restore || function (cb) { restoreTarget(t, false, cb); };
    this._probe = opts.probe || function (cb) { probeTarget(t, cb); };
    this._systemState = opts.systemState || realSystemState;
    this._start = opts.start || function (cb) { startTarget(t, cb); };
    this._mark = opts.mark || function (boot) { return markOf(t, boot); };
    this._hasBackup = opts.hasBackup || function () { return t.backups.some(function (f) { return fs.existsSync(f); }); };
    this._alive = opts.alive || function (pid) { return fs.existsSync(path.join(PROC, String(pid))); };
    this._killed = opts.killed || function () { return readText(t.killed).trim(); };
    this._explained = opts.explained || function (boot) { return otherCrashed(t, boot); };
    this._recmgrLoaded = opts.recmgrLoaded || recmgr.recmgrLoaded;
    this._lock = opts.lock || lock;
    this._now = opts.now || monoMs;
    this._log = opts.log || log;

    this.restoring = false;
    this._restored = [];        /* callbacks waiting for the restore to end */
    this._busy = false;         /* a probe, crash or start is in flight */
    this._pid = 0;              /* the main PID of the active run */
    this._run = '';             /* the active run, for the healthy timer */
    this._lastProbe = -Infinity;
    this._activeSince = null;   /* start of the current continuous-active run */
    this._startAt = null;       /* when the scheduled carplay start is due */
    /* This boot's ended run, crashes and starts, also kept here so a state
     * file that cannot be written neither repeats a crash nor lifts the start
     * limit; the larger of memory and disk counts. */
    this._ended = '';
    this._crashes = 0;
    this._starts = 0;
}

Recovery.prototype._load = function () {
    try {
        var st = JSON.parse(fs.readFileSync(this.statePath, 'utf8'));
        if (st && typeof st === 'object') return { fails: st.fails || 0, arm: normArm(st.arm), prev: normArm(st.prev), starts: st.starts || null, ended: st.ended || null };
    } catch (e) {}
    return { fails: 0, arm: null, prev: null, starts: null, ended: null };
};

/* Persist durably: the state must survive the reboot a crash can cause the
 * moment after, so it is on disk, renamed into place, before this returns. A
 * write that fails leaves the previous state whole. false when it failed. */
Recovery.prototype._save = function (st) {
    var dir = path.dirname(this.statePath), tmp = this.statePath + '.tmp', fd;
    try {
        try { fs.mkdirSync(dir, 0o755); } catch (e) { if (e.code !== 'EEXIST') throw e; }
        fd = fs.openSync(tmp, 'w', 0o644);
        try {
            var out = { fails: st.fails || 0, arm: st.arm || null, starts: st.starts || null, ended: st.ended || null };
            if (st.prev) out.prev = st.prev;
            fs.writeFileSync(fd, JSON.stringify(out) + '\n');
            fs.fsyncSync(fd);
        } finally { fs.closeSync(fd); }
        fs.renameSync(tmp, this.statePath);
    } catch (e) {
        try { fs.unlinkSync(tmp); } catch (e2) {}
        this._log('could not persist state: ' + e.message);
        return false;
    }
    /* The state is written; a folder that cannot be synced (a file system
     * without it) leaves only the rename's timing open. */
    try {
        fd = fs.openSync(dir, 'r');
        try { fs.fsyncSync(fd); } finally { fs.closeSync(fd); }
    } catch (e) { this._log('could not sync ' + dir + ': ' + e.message); }
    return true;
};

Recovery.prototype._armed = function (st) { return !!st.arm && st.arm.boot === this.bootId; };
Recovery.prototype._thisBoot = function (rec) { return !!rec && rec.boot === this.bootId; };

/* The boot-time decision: count the previous boot, arm this one, and restore
 * at the threshold, or when this boot's arm could not be saved. Only the
 * restore path classifies the binary. Also run by
 * the watcher at its start, where it counts nothing twice and retries a
 * restore the boot step did not finish. cb(result) for tests; result.action is
 * 'idle' | 'armed' | 'unreadable' | 'restored' | 'restore-failed' |
 * 'restore-unavailable'. */
Recovery.prototype.startBoot = function (cb) {
    cb = cb || function () {};
    var self = this;
    var st = this._load();
    if (st.arm && !this._armed(st)) {
        var was = st.fails;
        st.fails = tally(st.fails, st.arm, this._mark(st.arm.boot), this._explained(st.arm.boot));
        st.prev = st.arm.crashes ? st.arm : null;
        st.arm = null;
        if (st.fails !== was) this._log('failed patched boots and crashes now ' + st.fails + ' (was ' + was + ')');
    }
    if (!this._hasBackup()) {
        /* No backup of ours: nothing of ours to protect or restore. */
        if (st.fails || st.arm) { st.fails = 0; st.arm = null; this._save(st); }
        cb({ action: 'idle' });
        return;
    }
    if (!st.arm) st.arm = newArm(this.bootId);
    var saved = this._save(st);
    if (saved && st.fails < this.threshold) { cb({ action: 'armed', fails: st.fails }); return; }
    this._classify(function (cls) { self._restoreWith(cls, st.fails, saved, cb); });
};

/* Restore a restorable patch, at the threshold or (saved false) because the
 * count could not be saved; the counter clears once carplay is not ours to
 * protect. */
Recovery.prototype._restoreWith = function (cls, fails, saved, cb) {
    var self = this;
    if (!cls.ok) {
        this._log('could not read ' + this.target.name + ' state; leaving the boot-recovery counter as is');
        cb({ action: 'unreadable', fails: fails });
        return;
    }
    if (!cls.patchActive) {
        /* Stock, or a binary we do not recognise (a firmware update):
         * nothing to restore. */
        this._clear();
        cb({ action: 'idle' });
        return;
    }
    if (!cls.restorable) {
        this._log('WARNING: ' + fails + ' ' + this.target.name + ' crashes but no restorable backup; leaving it as is');
        cb({ action: 'restore-unavailable', fails: fails });
        return;
    }
    var n = this.target.name;
    this._log(saved ? 'restoring the original ' + n + ' after ' + fails + ' ' + n + ' crashes or failed boots'
        : 'restoring the original ' + n + ': the boot-recovery state cannot be saved');
    this.restoring = true;
    this._restore(function (err) {
        self.restoring = false;
        if (err) self._log('boot recovery restore failed: ' + err.message);
        else { self._log('restored the original ' + n + '; its patch is off until Enable re-applies it'); self._clear(); }
        /* The counter stays after a failure, so a later try restores. */
        cb(err ? { action: 'restore-failed', fails: fails, error: err.message } : { action: 'restored', fails: fails });
        self._restored.splice(0).forEach(function (f) { f(); });
    });
};

Recovery.prototype._clear = function () {
    var st = this._load();
    st.fails = 0;
    st.arm = null;
    this._crashes = 0;
    this._save(st);
};

/* cb once no restore runs: a stop waits for it, so / is never left writable. */
Recovery.prototype.whenRestored = function (cb) {
    if (this.restoring) this._restored.push(cb);
    else cb();
};

/* One watcher poll. A run of carplay is told by its InvocationID. While a
 * run is active a tick only checks that its main PID is in /proc, and asks
 * systemctl every PROBE_MS or once the PID is gone; while carplay is down it
 * asks every PROBE_MS. A run that ended failed (Result not success) is a
 * crash, once per run, unless patchd killed it to activate a change or the
 * system is stopping. A run a SIGKILL ended as a success (the tool's
 * carplay.service drop-in, SuccessExitStatus=SIGKILL) is no crash; unless
 * patchd killed it, which starts it itself, carplay is started again like
 * after a crash. A run continuously active for HEALTHY_MS confirms the boot.
 * cb(result) for tests; result.action is 'skip' | 'watch' | 'confirmed' |
 * 'crash' | 'killed' | 'started' | 'start-skipped'. */
Recovery.prototype.tick = function (cb) {
    cb = cb || function () {};
    var self = this;
    if (this._busy || this.restoring) { cb({ action: 'skip' }); return; }
    var now = this._now();
    if (this._startAt !== null && now >= this._startAt) { this._startDue(cb); return; }
    var fresh = now - this._lastProbe < PROBE_MS;
    if (fresh && (!this._pid || this._alive(this._pid))) { cb({ action: this._healthy(now) }); return; }
    this._busy = true;
    this._probe(function (info) {
        self._lastProbe = now;
        if (info.state === 'active') {
            if (info.run !== self._run) { self._run = info.run; self._activeSince = now; }
            self._pid = info.pid;
        } else {
            self._run = '';
            self._pid = 0;
            self._activeSince = null;
        }
        var killed = self.target.killedRun(info);
        var ended = !killed && (info.state === 'failed' || (info.state === 'inactive' && !!info.result && info.result !== 'success'));
        if (!(ended || killed) || !info.run || self._handled(info.run)) { self._busy = false; cb({ action: self._healthy(now) }); return; }
        self._markHandled(info.run);
        if (info.run === self._killed()) { self._busy = false; cb({ action: 'watch' }); return; }
        self._systemState(function (sys) {
            if (sys === 'stopping') { self._busy = false; cb({ action: 'watch' }); return; }
            if (killed) { self._onKill(info, cb); return; }
            self._onCrash(info, cb);
        });
    });
};

Recovery.prototype._handled = function (run) {
    var e = this._load().ended;
    return run === this._ended || (this._thisBoot(e) && e.run === run);
};

Recovery.prototype._markHandled = function (run) {
    this._ended = run;
    var st = this._load();
    st.ended = { boot: this.bootId, run: run };
    this._save(st);
};

Recovery.prototype._healthy = function (now) {
    if (this._activeSince === null || now - this._activeSince < this.healthyMs) return 'watch';
    var st = this._load();
    if (this._armed(st) && !st.arm.confirmed) {
        st.arm.confirmed = true;
        this._save(st);
        this._log(this.target.name + ' healthy');
        return 'confirmed';
    }
    return 'watch';
};

/* A crash: recorded at once (the next moment can be a reboot), and a
 * restorable patch restored at once when the record could not be saved.
 * Without recmgr's carplay entry nothing more is done here: the boot step
 * counts it and restores at the next start. With it, a patched binary is
 * restored at the threshold, then carplay is started again. */
Recovery.prototype._onCrash = function (info, cb) {
    var self = this, saved = true, loaded = this._recmgrLoaded(this.target.name);
    function done(extra) { self._busy = false; cb(Object.assign({ action: 'crash' }, extra)); }
    function next(extra) { if (loaded) self._scheduleStart(); done(extra); }
    this._log(this.target.name + ' (run ' + info.run + ') ended: ' + info.result);
    this._crashes++;
    var st = this._load();
    if (this._armed(st)) { st.arm.crashes = Math.max(st.arm.crashes + 1, this._crashes); saved = this._save(st); }
    if (saved && !loaded) {
        this._log('recmgr did not load a ' + this.target.name + ' entry this boot: the boot step decides at the next start');
        done({ fails: st.fails + this._crashes });
        return;
    }
    this._classify(function (cls) {
        if (!cls.ok || !cls.patchActive) {
            if (cls.ok) self._clear();
            next();
            return;
        }
        if (!self._armed(st)) { st.arm = newArm(self.bootId); st.arm.crashes = self._crashes; saved = self._save(st); }
        var total = st.fails + Math.max(st.arm.crashes, self._crashes);
        self._log(self.target.name + ' crashes while patched: ' + total);
        if (saved && total < self.threshold) { next({ fails: total }); return; }
        self._restoreWith(cls, total, saved, function (r) { next({ fails: total, restore: r.action }); });
    });
};

/* A SIGKILL that was not patchd's (the OOM killer, a shell): carplay.service
 * has no Restart=, so it is started again where recmgr loaded its entry
 * (without it, recmgr restarts the head unit for the exit). */
Recovery.prototype._onKill = function (info, cb) {
    this._log(this.target.name + ' (run ' + info.run + ') was killed');
    if (this._recmgrLoaded(this.target.name)) this._scheduleStart();
    this._busy = false;
    cb({ action: 'killed' });
};

Recovery.prototype._startsThisBoot = function () {
    var s = this._load().starts;
    return Math.max(this._thisBoot(s) ? s.n : 0, this._starts);
};

Recovery.prototype._scheduleStart = function () {
    var n = this._startsThisBoot();
    if (n >= START_DELAYS_MS.length) {
        this._log(this.target.name + ' stays down: started ' + n + ' times this boot already');
        return;
    }
    this._startAt = this._now() + START_DELAYS_MS[n];
};

/* The scheduled start: only while carplay is down, the system is not
 * stopping, and patchd is not writing (it holds the remount lock; the start
 * waits for the next tick). The run it starts is watched like any other. */
Recovery.prototype._startDue = function (cb) {
    var self = this;
    this._busy = true;
    function done(action) { self._busy = false; cb({ action: action }); }
    this._probe(function (info) {
        if (info.state === 'active' || info.state === 'activating') { self._startAt = null; done('start-skipped'); return; }
        self._systemState(function (sys) {
            if (sys === 'stopping') { self._startAt = null; done('start-skipped'); return; }
            if (!self._lock.tryAcquire()) { done('watch'); return; }
            var n = self._startsThisBoot() + 1;
            self._starts = n;
            var st = self._load();
            st.starts = { boot: self.bootId, n: n };
            self._save(st);
            self._startAt = null;
            self._log('starting ' + self.target.name + ' (start ' + n + ' this boot)');
            self._start(function (err) {
                self._lock.release();
                if (err) self._log('starting ' + self.target.name + ' failed: ' + err.message);
                self._lastProbe = -Infinity;   /* probe the new run at the next tick */
                done('started');
            });
        });
    });
};

/* SIGTERM while the system stops (an orderly reboot) exempts a boot with no
 * crash; a SIGTERM from a service restart does not. */
Recovery.prototype.onShutdown = function (cb) {
    var self = this;
    var st = this._load();
    if (!this._armed(st) || st.arm.crashes || this._crashes || st.arm.clean) { cb(); return; }
    this._systemState(function (sys) {
        if (sys === 'stopping') { st.arm.clean = true; self._save(st); }
        cb();
    });
};

module.exports = {
    Recovery: Recovery, tally: tally, parseShow: parseShow, RESTORE_BOUND_MS: RESTORE_BOUND_MS, BOOT_BOUND_MS: BOOT_BOUND_MS, TARGETS: TARGETS,
    realRestore: realRestore,
};

/* ---- run as the boot step or the watcher ---- */

/* The boot step for one service (`recovery.js boot [carplay|aapd]`) always
 * exits 0: the service's start never waits on its failure. Nothing runs the
 * service before it, so the restore links the patched binary only if a
 * process runs it. */
function runBoot(name) {
    var t = TARGETS[name || 'carplay'];
    if (!t) { log('unknown service ' + name); process.exit(0); }
    var rec = new Recovery({ target: t, restore: function (cb) { restoreTarget(t, true, cb, BOOT_LOCK_WAIT_MS); } });
    rec.startBoot(function (r) { log(t.name + ' boot check: ' + r.action); process.exit(0); });
    /* A stop mid-restore lets the restore finish (it exits then). */
    process.on('SIGTERM', function () { rec.whenRestored(function () { process.exit(0); }); });
}

/* The watcher follows carplay, and aapd where the unit has it; each has its
 * own counter and acts alone. */
function runWatcher() {
    var recs = [new Recovery({ target: TARGETS.carplay })];
    if (fs.existsSync(TARGETS.aapd.target)) recs.push(new Recovery({ target: TARGETS.aapd }));
    var timer = null;
    var starting = recs.length;
    var stopping = false;
    function each(fn, cb) {
        var left = recs.length;
        recs.forEach(function (r) { fn(r, function () { if (--left === 0) cb(); }); });
    }
    /* A stop waits for a restore in progress, so / is not left writable. */
    function finish() {
        if (timer) clearInterval(timer);
        each(function (r, cb) { r.whenRestored(function () { r.onShutdown(cb); }); }, function () { process.exit(0); });
    }
    recs.forEach(function (r) {
        r.startBoot(function () {
            if (--starting) return;
            if (stopping) { finish(); return; }
            /* Polled even with no patch to watch: the service stays up, since
             * Restart=always would otherwise start it again every few seconds
             * and the deploy tool's health check takes that for a crash loop. */
            timer = setInterval(function () { recs.forEach(function (x) { x.tick(); }); }, TICK_MS);
        });
    });
    function stop() { if (stopping) return; stopping = true; if (!starting) finish(); }   /* else finish() runs when startBoot returns */
    process.on('SIGTERM', stop);
    process.on('SIGINT', stop);
}

if (require.main === module) {
    if (process.argv[2] === 'boot') runBoot(process.argv[3]);
    else runWatcher();
}
