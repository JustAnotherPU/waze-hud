// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 JustAnotherPU
'use strict';
/* The other "Waze on cluster": robertnisipeanu/idrive-apps' tool
 * waze-on-cluster and its app robertn_waze_cluster patch the same carplay
 * and aapd binaries this tool patches, so only one of the two may run.
 *
 * blocked() is a reason while that tool is installed (its component.json
 * under /var/opt/waze-on-cluster/current, which mgu-deploy removes with the
 * whole folder on uninstall), else null. While blocked this tool applies
 * nothing; it reverts only a patch of its own (from its own backup, as when
 * the other one was installed after it), and its boot steps (boot,
 * aa-boot) keep guarding such a patch: idle without our backup. Its units
 * with ConditionPathExists=!<that file> do not start (the map and its
 * socket, capture, hudguide, and the crash watcher, which would also start
 * carplay again after a crash beside the other one's).
 *
 * takeover(log, cb) is the owner's choice in the app: the other app, then
 * the other tool (the app requires the tool), each removed with
 * `mgu-deploy.sh uninstall <id> --yes`, as the Store does; its removal runs
 * its own deactivate, which reverts its patch. cb(err) once it is gone, or
 * with what failed (the rest is left as it is).
 *
 * Node 8.16.2 on the unit. No dependencies.
 */
var fs = require('fs');
var cp = require('child_process');

var OTHER_TOOL = 'waze-on-cluster';
var OTHER_APP = 'robertn_waze_cluster';
var MARK = process.env.WOC_OTHER_TOOL_FILE || '/var/opt/waze-on-cluster/current/component.json';
var DEPLOY = process.env.WOC_DEPLOY || '/var/opt/mgu-deploy/mgu-deploy.sh';
var UNINSTALL_MS = parseInt(process.env.WOC_UNINSTALL_MS, 10) || 10 * 60 * 1000;
var REASON = "Robert Nisipeanu's Waze on cluster is installed: both patch CarPlay, so this one stays off until it is removed";

function blocked() { return fs.existsSync(MARK) ? REASON : null; }

/* The installed component ids, from the deploy tool's status (empty when it
 * cannot say). */
function installed(cb) {
    cp.execFile(DEPLOY, ['status', '--json'], { timeout: 60000, maxBuffer: 8 << 20 }, function (err, out) {
        var ids = [];
        try { ids = JSON.parse(out).components.map(function (c) { return c.id; }); } catch (e) {}
        cb(ids);
    });
}

function takeover(log, cb) {
    installed(function (ids) {
        var steps = [OTHER_APP, OTHER_TOOL].filter(function (id) {
            return ids.indexOf(id) >= 0 || (id === OTHER_TOOL && blocked());
        });
        (function next() {
            var id = steps.shift();
            if (!id) { cb(blocked() ? new Error(OTHER_TOOL + ' is still installed') : null); return; }
            log('takeover: removing ' + id);
            cp.execFile(DEPLOY, ['uninstall', id, '--yes'], { timeout: UNINSTALL_MS, maxBuffer: 8 << 20 }, function (err, out, errOut) {
                if (err) {
                    var tail = String(errOut || out || err.message).trim().split('\n').slice(-1)[0];
                    cb(new Error('removing ' + id + ' failed: ' + tail));
                    return;
                }
                log('takeover: ' + id + ' removed');
                next();
            });
        })();
    });
}

module.exports = { blocked: blocked, takeover: takeover, OTHER_TOOL: OTHER_TOOL, OTHER_APP: OTHER_APP };
