#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Host A/B of the pinned media proxy's start pre-buffer and tail cache, interleaved on one network
# (docs/PERF_PLAYBACK.md). Run it where tools/perf_playback.sh --local runs (inside the build
# container, in a test slot):
#
#   tools/perf_playback_ab.sh [--videos id,id,...] [--rounds N] [--warm-rounds N] [--pause S] [--out DIR]
#
# cold: one fresh process per video and arm; the arm order alternates (on,off / off,on) from one
#       video to the next and from one round to the next;
# warm: one process per round and arm (a primer video, then every video), arm order alternating.
# The arm is SAILTUBE_PREBUFFER=1|0 (tools/perf_playback/PerfPlayback.cpp). Stops at the first sign
# of rate limiting. Request budget: videos x rounds x 2 cold starts + warm-rounds x 2 x (videos + 1).
# One pass; do not loop it. Summary: tools/experiments/prebuffer_ab.py over the logs in DIR.
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
videos="aqz-KE-bpKQ,eRsGyueVLvQ,R6MlUcmOul8,M7lc1UVf-VE,YE7VzlLtp-4"
rounds=2; warm=2; pause=3; out=${TMPDIR:-/tmp}/sailtube-prebuffer-ab
while [ $# -gt 0 ]; do
    case "$1" in
        --videos) videos=$2; shift 2;;
        --rounds) rounds=$2; shift 2;;
        --warm-rounds) warm=$2; shift 2;;
        --pause) pause=$2; shift 2;;
        --out) out=$2; shift 2;;
        *) echo "unknown argument: $1" >&2; exit 2;;
    esac
done
mkdir -p "$out"
sh "$root/tools/perf_playback.sh" --local --build-only > "$out/build.log" 2>&1 || { echo "perf_playback_ab: build failed (see $out/build.log)"; tail -5 "$out/build.log"; exit 2; }
bin=${TMPDIR:-/tmp}/sailtube-perf-build/perf_playback
[ -x "$bin" ] || { echo "perf_playback_ab: no harness at $bin"; exit 2; }

limited() {  # a log that shows rate limiting or a bot check ends the pass
    grep -q -i "429\|bot_wall\|rate.limit\|try_later" "$1"
}
run() {  # arm, log, harness arguments
    arm=$1; log=$2; shift 2
    SAILTUBE_PREBUFFER=$arm "$bin" "$@" > "$log" 2>&1
    if limited "$log"; then echo "perf_playback_ab: rate limiting in $log; stopping"; summary; exit 3; fi
    sleep "$pause"
}
summary() {
    on=$(ls "$out"/on.*.log 2>/dev/null | sort); off=$(ls "$out"/off.*.log 2>/dev/null | sort)
    [ -n "$on" ] && [ -n "$off" ] || { echo "perf_playback_ab: no logs to summarise"; return; }
    # shellcheck disable=SC2086
    python3 -I "$root/tools/experiments/prebuffer_ab.py" --keep-first --on $on --off $off
}

n=0
for round in $(seq 1 "$rounds"); do
    for v in $(echo "$videos" | tr ',' ' '); do
        n=$((n + 1))
        tag=$(printf "cold.%02d.%03d" "$round" "$n")
        if [ $((n % 2)) -eq 1 ]; then first=1; second=0; else first=0; second=1; fi
        for arm in $first $second; do
            name=$([ "$arm" = 1 ] && echo on || echo off)
            run "$arm" "$out/$name.$tag.log" --rounds 1 --videos "$v" --modes cold --pause 0
        done
    done
done
for round in $(seq 1 "$warm"); do
    tag=$(printf "warm.%02d" "$round")
    if [ $((round % 2)) -eq 1 ]; then first=1; second=0; else first=0; second=1; fi
    for arm in $first $second; do
        name=$([ "$arm" = 1 ] && echo on || echo off)
        run "$arm" "$out/$name.$tag.log" --rounds 1 --videos "$videos" --modes warm
    done
done
echo "perf_playback_ab: $(ls "$out"/on.*.log | wc -l) on logs, $(ls "$out"/off.*.log | wc -l) off logs in $out"
summary
