#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# E3 / PLY-20 / PLT-08 / §6 D3: does droidvdec decode H.264 and VP9 on this device?
# Runs ON THE DEVICE as defaultuser, started by the maintainer only. See docs/experiments/E3_droidvdec.md.
#
#   e3_droidvdec_probe.sh CLIP...      (.mp4 = H.264 video-only, .webm/.mkv = VP9 video-only)
#
# For each clip and decoder (droidvdec, then the software decoder) it runs
#   gst-launch-1.0 -v filesrc ! demux ! parse ! caps ! DECODER ! fakesink silent=false sync=false
# for E3_TIMEOUT_S (default 3) seconds, counting fakesink "chain" messages as frames, then a
# real-time run (sync=true, E3_REALTIME_S, default 10) to compare CPU. No audio element is used.
set -u
. "$(dirname "$0")/common.sh"

[ $# -ge 1 ] || { echo "usage: $0 CLIP..." >&2; exit 2; }
TIMEOUT_S=${E3_TIMEOUT_S:-3}
REALTIME_S=${E3_REALTIME_S:-10}
# Each decode run is checked against SailTune's test windows before it starts.
exp_begin E3 $((TIMEOUT_S + REALTIME_S + 30))

exp_have gst-launch-1.0 && exp_have gst-inspect-1.0 || exp_die "gstreamer1.0-tools missing"
exp_have python3 || exp_die "python3 missing (needed by gst_watch.py)"

# 1. Element inventory. Rank matters for PLT-08: droidvdec must not autoplug.
for element in droidvdec droideglsink droidvenc h264parse vp9parse qtdemux matroskademux \
               avdec_h264 openh264dec vp9dec avdec_vp9 decodebin uridecodebin; do
    if gst-inspect-1.0 "$element" > "$EXP_DIR/inspect.$element.txt" 2>&1; then
        rank=$(sed -n 's/^ *Rank *//p' "$EXP_DIR/inspect.$element.txt" | head -n 1)
        exp_log "element $element present rank=${rank:-?}"
    else
        exp_log "element $element MISSING"
    fi
done
gst-inspect-1.0 droid > "$EXP_DIR/inspect.plugin-droid.txt" 2>&1 || exp_log "plugin droid not listed"
# Device codec configuration, if readable (file names vary per port; copy what is there).
[ -d /etc/gst-droid ] && cp -R /etc/gst-droid "$EXP_DIR/etc-gst-droid" 2>/dev/null && exp_log "copied /etc/gst-droid"
[ -r /etc/media_codecs.xml ] && cp /etc/media_codecs.xml "$EXP_DIR/" && exp_log "copied /etc/media_codecs.xml"

have() { gst-inspect-1.0 "$1" >/dev/null 2>&1; }
first_of() { for e in "$@"; do have "$e" && { echo "$e"; return; }; done; }
SW_H264=$(first_of avdec_h264 openh264dec)
SW_VP9=$(first_of vp9dec avdec_vp9)
VP9PARSE=$(have vp9parse && echo "vp9parse !")

results="$EXP_DIR/results.tsv"
printf 'clip\tdecoder\tvariant\tmode\trc\ttimed_out\tfirst_ms\tframes\telapsed_ms\tcpu_ms\tverdict\n' > "$results"

run_one() { # clip decoder variant mode chain
    _tag="$(basename "$1").$2.$3.$4"
    if [ "$4" = probe ]; then _t=$TIMEOUT_S; _sync=false; else _t=$REALTIME_S; _sync=true; fi
    exp_wait_window $((_t + 10))
    exp_mute_and_verify "$_tag" || exp_die "mute lost before $_tag"
    # shellcheck disable=SC2086 # the chain is a gst-launch description, split on purpose
    _out=$($EXP_PY "$EXP_TOOLS/gst_watch.py" --timeout "$_t" --log "$EXP_DIR/raw.$_tag.txt" -- \
        gst-launch-1.0 -v filesrc location="$1" ! $5 ! fakesink silent=false sync=$_sync)
    exp_log "$_tag $_out"
    _v() { printf '%s\n' "$_out" | sed -n "s/.* $1=\([^ ]*\).*/\1/p"; }
    _first=$(_v first_ms); _frames=$(_v frames)
    if [ "$4" = probe ]; then
        # PLY-20 probe rule: frames within 2 s; no frame within 3 s means fallback.
        if [ "${_frames:-0}" -gt 0 ] && [ "${_first:--1}" -le 2000 ]; then _verdict=probe-pass
        elif [ "${_frames:-0}" -gt 0 ]; then _verdict=late
        else _verdict=no-frame; fi
    else
        _verdict=realtime
    fi
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(basename "$1")" "$2" "$3" "$4" \
        "$(_v rc)" "$(_v timed_out)" "$_first" "$_frames" "$(_v elapsed_ms)" "$(_v cpu_ms)" \
        "$_verdict" >> "$results"
}

for clip in "$@"; do
    [ -r "$clip" ] || { exp_log "clip $clip unreadable; skipped"; continue; }
    case "$clip" in
        *.mp4|*.m4v)
            # Spec PLY-20 parsed caps first; byte-stream is a fallback variant to learn from.
            set -- "avc" "qtdemux ! h264parse ! video/x-h264,stream-format=avc,alignment=au" \
                   "bytestream" "qtdemux ! h264parse ! video/x-h264,stream-format=byte-stream,alignment=au"
            sw=$SW_H264 ;;
        *.webm|*.mkv)
            set -- "vp9" "matroskademux ! $VP9PARSE video/x-vp9"
            sw=$SW_VP9 ;;
        *) exp_log "clip $clip: unknown container; skipped"; continue ;;
    esac
    while [ $# -ge 2 ]; do
        variant=$1; chain=$2; shift 2
        if have droidvdec; then
            run_one "$clip" droidvdec "$variant" probe "$chain ! droidvdec"
            run_one "$clip" droidvdec "$variant" realtime "$chain ! droidvdec"
        fi
        if [ -n "$sw" ]; then
            run_one "$clip" "$sw" "$variant" probe "$chain ! $sw"
            run_one "$clip" "$sw" "$variant" realtime "$chain ! $sw"
        fi
    done
done

exp_log "results: $results"
cat "$results" >&2
exp_log "manual part (D3/D7 in the app) is in docs/experiments/E3_droidvdec.md"
