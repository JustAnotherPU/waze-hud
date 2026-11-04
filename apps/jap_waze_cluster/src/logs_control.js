// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Robert Nisipeanu
/**
 * logs_control - the Logs page's client of jap-waze-on-cluster's logs socket
 * (LOGS_PATH, group tm_oap_app). Connecting starts the logs service, which
 * serves the saved archives to a phone on the car's hotspot while the app
 * stays connected and for 15 minutes after a save; the app holds the
 * connection while the Logs page is visible.
 *
 *   app  -> logs  {"cmd":"save"}
 *   logs -> app   {"ev":"state","url":"http://<address>:8475/<token>/"|null,"saving",
 *                  "error":null|"no-space"|"too-large"|"failed","saved","archives":[{"name","size","mtime"}]}
 */
'use strict';

const EventEmitter = require('events');
const { LineSocket } = require('./waze_cluster_control.js');

const LOGS_PATH = '/dev/shm/jap-waze-on-cluster/logs.sock';
const ROWS = 5;
const ERRORS = { 'no-space': 'Not enough space', 'too-large': 'Logs too large', failed: 'Save failed' };

function size(bytes) {
    return bytes >= 1048576 ? `${(bytes / 1048576).toFixed(1)} MB` : `${Math.max(1, Math.round(bytes / 1024))} KB`;
}

class LogsControl extends EventEmitter {
    /* options: path, retryMs, log. Emits 'change'. */
    constructor(options) {
        const o = options || {};
        super();
        this.state = null;
        this.socket = new LineSocket({
            path: o.path || LOGS_PATH,
            retryMs: o.retryMs,
            log: o.log,
            onMessage: (msg) => {
                if (msg.ev === 'state') {
                    this.state = msg;
                    this.emit('change');
                }
            }
        });
        this.socket.on('connect', () => this.emit('change'));
        this.socket.on('close', () => {
            this.state = null;
            this.emit('change');
        });
    }

    open() {
        this.socket.open();
        this.emit('change');
    }

    close() {
        this.socket.close();
        this.state = null;
        this.emit('change');
    }

    save() {
        this.socket.send({ cmd: 'save' });
    }

    canSave() {
        return !!(this.state && !this.state.saving);
    }

    url() {
        return (this.state && this.state.url) || null;
    }

    statusText() {
        const s = this.state;
        if (!s) {
            return this.socket.link === 'absent' ? 'Logs unavailable — restart head unit' : 'Connecting…';
        }
        if (s.saving) {
            return 'Saving…';
        }
        if (s.error) {
            return ERRORS[s.error] || 'Save failed';
        }
        if (s.saved) {
            return `Saved ${s.saved}`;
        }
        return s.archives && s.archives.length ? `${s.archives.length} saved` : 'No logs saved';
    }

    urlText() {
        return this.state ? this.url() || 'Wi-Fi hotspot off' : '';
    }

    /* ROWS rows: the archives, newest first, then empty ones. */
    rows() {
        const list = (this.state && this.state.archives) || [];
        return Array.from({ length: ROWS }, (_, i) => (list[i] ? `${list[i].name} · ${size(list[i].size)}` : ''));
    }
}

module.exports = { LogsControl, LOGS_PATH, ROWS };
