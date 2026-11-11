#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# E7 / PLY-04 / SEL-04 / §6 P3: is qtdemux push-mode seeking still broken on this SFOS GStreamer?
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See docs/experiments/E7_qtdemux_push_seek.md.
#
#   e7_qtdemux_push_seek.sh CLIP...   (.mp4/.m4a -> qtdemux, .webm/.mkv -> matroskademux; >= 60 s each)
#
# Upstream modes per clip (fakesink only, no audio output):
#   pull-file    filesrc ! DEMUX                       baseline, demuxer in pull mode
#   push-file    filesrc ! queue ! DEMUX               push mode, byte-seekable upstream
#   push-http    souphttpsrc(127.0.0.1, Range) ! queue ! DEMUX
#   push-norange souphttpsrc(127.0.0.1, no Range) ! queue ! DEMUX   the "no byte-seekable upstream" case
# Seek flag sets: key (FLUSH|KEY_UNIT), snap (+SNAP_BEFORE), accurate (FLUSH|ACCURATE).
# Env: E7_SEEKS (20), E7_SEEK_TIMEOUT_S (60), E7_RATE_KBPS (20000, the spec's Wi-Fi floor), E7_RTT_MS (20).
set -u
. "$(dirname "$0")/common.sh"

[ $# -ge 1 ] || { echo "usage: $0 CLIP..." >&2; exit 2; }
SEEKS=${E7_SEEKS:-20}
SEEK_TIMEOUT=${E7_SEEK_TIMEOUT_S:-60}
RATE=${E7_RATE_KBPS:-20000}
RTT=${E7_RTT_MS:-20}
# Each run is checked against SailTune's windows before it starts (exp_wait_window waits).
STEP_S=$(( SEEKS * 4 + 3 * SEEK_TIMEOUT + 15 ))
exp_begin E7 "$STEP_S"

exp_have python3 || exp_die "python3 missing"
if ! $EXP_PY -c 'import gi; gi.require_version("Gst", "1.0"); from gi.repository import Gst' 2>/dev/null; then
    exp_log "python3-gobject (Gst 1.0 typelib) missing: cannot issue seeks from a script."
    exp_log "Manual alternative: see 'Without python3-gobject' in docs/experiments/E7_qtdemux_push_seek.md"
    exit 4
fi

serve_dir="$EXP_DIR/serve"
mkdir -p "$serve_dir"
for clip in "$@"; do
    [ -r "$clip" ] && ln -sf "$(cd "$(dirname "$clip")" && pwd)/$(basename "$clip")" "$serve_dir/"
done

start_server() { # port-file extra-args...
    _pf=$1; shift
    rm -f "$_pf"
    $EXP_PY "$EXP_TOOLS/range_server.py" --root "$serve_dir" --rate-kbps "$RATE" --rtt-ms "$RTT" \
        --port-file "$_pf" "$@" >> "$EXP_DIR/server.log" 2>&1 &
    exp_register_pid $!
    _i=0
    while [ ! -s "$_pf" ] && [ $_i -lt 50 ]; do sleep 0.1; _i=$((_i + 1)); done
    [ -s "$_pf" ] || exp_die "range server did not start (see server.log)"
}
start_server "$EXP_DIR/ranged.port"
read -r RPORT RTOKEN < "$EXP_DIR/ranged.port"
start_server "$EXP_DIR/norange.port" --no-range
read -r NPORT NTOKEN < "$EXP_DIR/norange.port"
exp_log "servers: ranged=127.0.0.1:$RPORT norange=127.0.0.1:$NPORT rate=${RATE}kbps rtt=${RTT}ms"

results="$EXP_DIR/results.tsv"
printf 'label\tflags\tseek\ttarget_ms\taccepted\toutcome\tms\tlanded_ms\terror_ms\n' > "$results"

for clip in "$@"; do
    [ -r "$clip" ] || { exp_log "clip $clip unreadable; skipped"; continue; }
    name=$(basename "$clip")
    case "$clip" in
        *.mp4|*.m4a|*.m4v) demux=qtdemux ;;
        *.webm|*.mkv|*.mka) demux=matroskademux ;;
        *) exp_log "clip $clip: unknown container; skipped"; continue ;;
    esac
    tail="$demux name=d d. ! fakesink name=sink sync=true"
    abs="$serve_dir/$name"
    for mode in pull-file push-file push-http push-norange; do
        case $mode in
            pull-file)    src="filesrc location=$abs ! $tail" ;;
            push-file)    src="filesrc location=$abs ! queue ! $tail" ;;
            push-http)    src="souphttpsrc location=http://127.0.0.1:$RPORT/$RTOKEN/$name ! queue ! $tail" ;;
            push-norange) src="souphttpsrc location=http://127.0.0.1:$NPORT/$NTOKEN/$name ! queue ! $tail" ;;
        esac
        for flags in key snap accurate; do
            exp_wait_window "$STEP_S"
            exp_mute_and_verify "$name.$mode.$flags" || exp_die "mute lost"
            exp_log "run $name $mode $flags"
            $EXP_PY "$EXP_TOOLS/e7_seek.py" --pipeline "$src" --flags "$flags" --seeks "$SEEKS" \
                --seek-timeout "$SEEK_TIMEOUT" --label "$name:$mode" --out "$results" \
                >> "$EXP_DIR/seeks.log" 2>&1 || exp_log "e7_seek.py exit $? ($name $mode $flags)"
        done
    done
done

# Per (label, flags): accepted count, async-done count, median and max ms, max |landed - target|.
awk -F '\t' 'NR > 1 && $3 > 0 {
        k = $1 " " $2; n[k]++; if ($5 == 1) acc[k]++
        if ($6 == "async-done") { done[k]++; v[k, done[k]] = $7; if ($7 > mx[k]) mx[k] = $7
            e = $9 < 0 ? -$9 : $9; if (e > me[k]) me[k] = e }
    }
    END {
        printf "%-40s %-9s %4s %4s %4s %8s %8s %10s\n", "label", "flags", "n", "acc", "done", "p50_ms", "max_ms", "max_err_ms"
        for (k in n) {
            c = done[k] + 0; for (i = 1; i <= c; i++) s[i] = v[k, i]
            for (i = 1; i <= c; i++) for (j = i + 1; j <= c; j++) if (s[j] < s[i]) { t = s[i]; s[i] = s[j]; s[j] = t }
            p = c ? s[int((c + 1) / 2)] : -1
            split(k, kk, " "); printf "%-40s %-9s %4d %4d %4d %8d %8d %10d\n", kk[1], kk[2], n[k], acc[k], c, p, mx[k], me[k]
        }
    }' "$results" | tee "$EXP_DIR/summary.txt" >&2
exp_log "pass criterion (E7): push-http + accurate on mp4: every seek accepted and done in < 2000 ms"
