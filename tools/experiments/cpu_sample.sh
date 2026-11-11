#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-PERF-06 / §6 P7: sample the app's CPU time over 10 min with the screen off, at most 1 Hz.
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See tools/experiments/perf_timing.md.
#
#   cpu_sample.sh [-p PID] [-i INTERVAL_S] [-d DURATION_S] [-c CONTEXT]
#
# Defaults: the oldest harbour-sailtube process, 10 s, 600 s, context "background-opus".
# The sampler itself wakes at most once per INTERVAL_S (>= 1), so it does not distort the
# "no app timer above 1 Hz" check. Output (stdout and $EXP_DIR/cpu.log):
#   sailtube-sample: cpu t=<ms since boot> ticks=<utime+stime> hz=<CLK_TCK> wakeups=<ctxt switches> threads=<n> context=<c> pid=<pid>
# perf_summary.py turns first/last samples into mean % of one core (budget <= 8 % over >= 600 s).
set -u
. "$(dirname "$0")/common.sh"

PID='' INTERVAL=10 DURATION=600 CONTEXT=background-opus
while getopts p:i:d:c: opt; do
    case $opt in
        p) PID=$OPTARG ;; i) INTERVAL=$OPTARG ;; d) DURATION=$OPTARG ;; c) CONTEXT=$OPTARG ;; *) exit 2 ;;
    esac
done
[ "$INTERVAL" -ge 1 ] 2>/dev/null || { echo "interval must be an integer >= 1 s (max 1 Hz)" >&2; exit 2; }
[ -n "$PID" ] || PID=$(pgrep -o -x harbour-sailtube)
[ -n "$PID" ] && [ -r "/proc/$PID/stat" ] || { echo "no harbour-sailtube process (use -p)" >&2; exit 2; }
HZ=$(getconf CLK_TCK 2>/dev/null || echo 100)

exp_begin perf-cpu $((DURATION + 30))
exp_log "sampling pid $PID every $INTERVAL s for $DURATION s, context $CONTEXT, CLK_TCK $HZ"

# utime and stime are fields 14 and 15; strip "pid (comm)" first because comm may hold spaces.
ticks() { sed 's/^.*) //' "/proc/$1/stat" | awk '{ print $12 + $13 }'; }
# Voluntary + involuntary context switches over all threads: a coarse wake-up count.
wakeups() {
    cat "/proc/$1"/task/*/status 2>/dev/null \
        | awk '$1 ~ /ctxt_switches:$/ { s += $2 } $1 == "Pid:" { n++ } END { printf "%d %d\n", s, n }'
}

_end=$(( $(exp_now_ms) + DURATION * 1000 ))
while :; do
    [ -r "/proc/$PID/stat" ] || { exp_log "process $PID exited"; break; }
    set -- $(wakeups "$PID")
    echo "sailtube-sample: cpu t=$(exp_now_ms) ticks=$(ticks "$PID") hz=$HZ wakeups=$1 threads=$2 context=$CONTEXT pid=$PID"
    [ "$(exp_now_ms)" -ge "$_end" ] && break
    sleep "$INTERVAL"
done | tee "$EXP_DIR/cpu.log"
