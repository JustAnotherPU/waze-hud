// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
'use strict';
/* What keeps the journal and logs units from restarting the head unit or
 * holding up its boot: each service's name in both recmgr lists; no ordering
 * against a unit not ours, no unit of the head unit's waiting for them (the
 * journal service comes from a timer, the logs service from its socket);
 * a core limit on every service, root only in node (on the crash handler's
 * list) or in the firewall unit, whose limit its children inherit; bounded
 * restarts and resources; and an update restarting only the journal
 * service. */
var assert = require('assert'), fs = require('fs'), path = require('path');

var root = path.join(__dirname, '..', '..');
var c = JSON.parse(fs.readFileSync(path.join(root, 'component.json'), 'utf8'));
var SERVICES = ['jap-waze-on-cluster-journal.service', 'jap-waze-on-cluster-logs.service', 'jap-waze-on-cluster-logs-fw.service'];
var NEW = SERVICES.concat(['jap-waze-on-cluster-journal.timer', 'jap-waze-on-cluster-logs.socket']);
var OURS = /^jap-waze-on-cluster-(journal|logs|logs-fw)\.(service|socket|timer)$/;

function unit(name) {
    var text = fs.readFileSync(path.join(root, 'systemd', name), 'utf8'), keys = {};
    text.split('\n').forEach(function (l) {
        var m = /^([A-Za-z]+)=(.*)$/.exec(l);
        if (m) (keys[m[1]] = keys[m[1]] || []).push(m[2]);
    });
    return keys;
}
function list(u, k) { return (u[k] || []).join(' ').split(/\s+/).filter(Boolean); }

NEW.forEach(function (name) {
    var u = unit(name);
    assert(c.units.some(function (x) { return x.name === name; }), name + ' installed');
    assert(!u.Before, name + ' orders itself before nothing');
    ['After', 'Wants', 'Requires', 'BindsTo', 'PartOf', 'Requisite'].forEach(function (k) {
        list(u, k).forEach(function (other) { assert(OURS.test(other), name + ' ' + k + '=' + other + ': only our own units'); });
    });
    assert(!u.DefaultDependencies, name + ' keeps systemd\'s default dependencies');
    list(u, 'WantedBy').forEach(function (t) { assert(t === 'timers.target' || t === 'sockets.target', name + ' WantedBy=' + t); });
});
SERVICES.forEach(function (name) {
    var u = unit(name);
    assert(c.recmgr.indexOf(name.replace(/\.service$/, '')) >= 0, name + ' in recmgr');
    assert(!u.WantedBy, name + ' is started by its timer, socket or service, nothing else');
    assert.deepStrictEqual(u.LimitCORE, ['1'], name);
    assert(u.Restart && u.RestartSec && Number(u.StartLimitBurst) > 0, name + ' restarts, bounded');
    assert(u.MemoryLimit && u.CPUQuota && u.TasksMax, name + ' bounded');
    ['ExecStartPre', 'ExecStart', 'ExecStopPost'].forEach(function (k) {
        (u[k] || []).forEach(function (cmd) {
            assert(!/ulimit/.test(cmd), cmd);
            if (/^-?\+/.test(cmd)) assert(/^-?\+\/usr\/bin\/node \S+\.js \w+$/.test(cmd), 'a root command runs node: ' + cmd);
        });
    });
});
['jap-waze-on-cluster-journal.service', 'jap-waze-on-cluster-logs.service'].forEach(function (name) {
    var u = unit(name);
    assert.deepStrictEqual(u.DynamicUser, ['yes'], name);
    assert.deepStrictEqual(u.Nice, ['19'], name);
    assert.deepStrictEqual(u.IOSchedulingClass, ['idle'], name);
});
var fw = unit('jap-waze-on-cluster-logs-fw.service');
assert(!fw.User, 'the firewall unit runs as root, in its own service so LimitCORE covers nft');
assert.deepStrictEqual(fw.BindsTo, ['jap-waze-on-cluster-logs.service']);
assert.deepStrictEqual(fw.ExecStopPost, ['/usr/bin/node /var/opt/jap-waze-on-cluster/current/logs_fw.js close']);
assert.deepStrictEqual(fw.ExecStart, ['/usr/bin/node /var/opt/jap-waze-on-cluster/current/logs_fw.js watch']);
var logs = unit('jap-waze-on-cluster-logs.service');
assert(list(logs, 'Wants').indexOf('jap-waze-on-cluster-logs-fw.service') >= 0 && list(logs, 'After').indexOf('jap-waze-on-cluster-logs-fw.service') >= 0);
assert(!(logs.ExecStartPre || []).length && !(logs.ExecStopPost || []).length, 'nothing in the logs service runs as root');
assert(/^\d+$/.test(unit('jap-waze-on-cluster-journal.timer').OnBootSec[0]) && Number(unit('jap-waze-on-cluster-journal.timer').OnBootSec[0]) >= 60);
assert(c.enable.some(function (e) { return e.unit === 'jap-waze-on-cluster-journal.timer' && e.target === 'timers.target'; }));
assert(c.enable.some(function (e) { return e.unit === 'jap-waze-on-cluster-logs.socket' && e.target === 'sockets.target'; }));
assert(!c.enable.some(function (e) { return /journal\.service|logs(-fw)?\.service/.test(e.unit); }), 'no service enabled on its own');
/* The deploy tool restarts the journal service on an update and checks it is
 * up 10 s later; the logs services, which their socket starts and which exit
 * by themselves, are left to run out their window. */
assert(c.restart.indexOf('jap-waze-on-cluster-journal.service') >= 0);
assert(c.restart.indexOf('jap-waze-on-cluster-logs.service') < 0 && c.restart.indexOf('jap-waze-on-cluster-logs-fw.service') < 0);
console.log('ok   journal and logs units: in recmgr, ordered against none of the head unit\'s, started late or on demand, no core dumps, root only in node and the firewall unit');
