#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-PERF-05: sample the app's PSS (and optionally the spool size) as sailtube-sample lines.
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See tools/experiments/perf_timing.md.
#
#   pss.sh [-p PID] [-i INTERVAL_S] [-d DURATION_S] [-c CONTEXT] [-s SPOOL_DIR]
#
# Defaults: the oldest harbour-sailtube process, 5 s, 600 s, context "unspecified", no spool.
# Contexts the summary knows: playback-1080p-sw (<= 300 MB), feed (<= 180 MB).
# Output (stdout and $EXP_DIR/pss.log), t = ms since boot from /proc/uptime:
#   sailtube-sample: pss t=<ms> kb=<Pss KiB> context=<c> pid=<pid>
#   sailtube-sample: spool t=<ms> kb=<du KiB> context=<c>
set -u
. "$(dirname "$0")/common.sh"

PID='' INTERVAL=5 DURATION=600 CONTEXT=unspecified SPOOL=''
while getopts p:i:d:c:s: opt; do
    case $opt in
        p) PID=$OPTARG ;; i) INTERVAL=$OPTARG ;; d) DURATION=$OPTARG ;;
        c) CONTEXT=$OPTARG ;; s) SPOOL=$OPTARG ;; *) exit 2 ;;
    esac
done
[ "$INTERVAL" -ge 1 ] 2>/dev/null || { echo "interval must be an integer >= 1 s (max 1 Hz)" >&2; exit 2; }
[ -n "$PID" ] || PID=$(pgrep -o -x harbour-sailtube)
[ -n "$PID" ] && [ -r "/proc/$PID/status" ] || { echo "no harbour-sailtube process (use -p)" >&2; exit 2; }

exp_begin perf-pss "$DURATION"
exp_log "sampling pid $PID every $INTERVAL s for $DURATION s, context $CONTEXT"

pss_kb() {
    if [ -r "/proc/$1/smaps_rollup" ]; then
        awk '$1 == "Pss:" { print $2; exit }' "/proc/$1/smaps_rollup"
    else
        awk '$1 == "Pss:" { s += $2 } END { print s + 0 }' "/proc/$1/smaps"
    fi
}

_end=$(( $(exp_now_ms) + DURATION * 1000 ))
while [ "$(exp_now_ms)" -lt "$_end" ]; do
    [ -r "/proc/$PID/status" ] || { exp_log "process $PID exited"; break; }
    echo "sailtube-sample: pss t=$(exp_now_ms) kb=$(pss_kb "$PID") context=$CONTEXT pid=$PID"
    if [ -n "$SPOOL" ] && [ -d "$SPOOL" ]; then
        # du counts allocated blocks, so punched holes in the sparse spool do not count.
        echo "sailtube-sample: spool t=$(exp_now_ms) kb=$(du -sk "$SPOOL" | awk '{ print $1 }') context=$CONTEXT"
    fi
    sleep "$INTERVAL"
done | tee "$EXP_DIR/pss.log"
