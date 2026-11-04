#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Robert Nisipeanu
# shutdown_mark.sh — run by var-opt-ro.service (drop-in
# 50-jap-waze-on-cluster.conf) at each head-unit shutdown, before /var/opt goes
# read-only. recmgr's reboot after a crash and a switch-off are both an NSM
# fast shutdown that stops no service, so each patched service's state at
# that moment tells them apart for boot recovery (recovery.js): it writes
# "<boot id> running|failed|stopped" for carplay.service to <state
# dir>/shutdown and for aapd.service to <state dir>/shutdown-aapd. failed is
# a Result other than success, except a SIGKILL of aapd (Result signal,
# ExecMainCode 2, status 9; an oom-kill stays failed), which its watcher takes for no crash either: aapd.service has no
# SuccessExitStatus=SIGKILL, as carplay.service's drop-in gives carplay. It
# writes nothing for a service whose state cannot be read within WAIT
# seconds, and always exits 0, so it never holds up the remount.
DIR=${WOC_STATE_DIR:-/var/opt/jap-waze-on-cluster}
BOOT_ID=${WOC_BOOT_ID:-/proc/sys/kernel/random/boot_id}
SYSTEMCTL=${WOC_SYSTEMCTL:-systemctl}
WAIT=${WOC_MARK_WAIT:-3}

[ -d "$DIR" ] || exit 0
boot=$(cat "$BOOT_ID" 2>/dev/null) || exit 0

# mark <unit> <file> [sigkill-is-stopped]
mark() {
  # The unit has no timeout(1) on its path: systemctl runs in the background
  # and is killed after WAIT seconds.
  out="$DIR/$2.show"
  "$SYSTEMCTL" show "$1" -p ActiveState -p Result -p ExecMainCode -p ExecMainStatus > "$out" 2>/dev/null &
  pid=$!
  ( sleep "$WAIT"; kill "$pid" 2>/dev/null ) &
  guard=$!
  wait "$pid"
  status=$?
  kill "$guard" 2>/dev/null
  [ "$status" -eq 0 ] || { rm -f "$out"; return; }
  show=$(cat "$out")
  rm -f "$out"
  case "$show" in
    *Result=success*)
      case "$show" in
        *ActiveState=active*|*ActiveState=activating*|*ActiveState=reloading*) state=running ;;
        *) state=stopped ;;
      esac ;;
    *Result=*)
      state=failed
      if [ -n "$3" ] && printf '%s\n' "$show" | grep -qx 'Result=signal' && printf '%s\n' "$show" | grep -qx 'ExecMainCode=2' &&
         printf '%s\n' "$show" | grep -qx 'ExecMainStatus=9'; then
        state=stopped
      fi ;;
    *) return ;;
  esac
  printf '%s %s\n' "$boot" "$state" > "$DIR/$2.tmp" && mv -f "$DIR/$2.tmp" "$DIR/$2"
}

mark carplay.service shutdown
mark aapd.service shutdown-aapd sigkill
exit 0
