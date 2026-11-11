#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# HOST-side: synthetic test clips for E2, E3 and E7 (no downloaded media, no copyrighted content).
#
#   make_clips.sh OUT_DIR            env: CLIP_SECONDS (default 90), FFMPEG (default ffmpeg)
#
# Needs an ffmpeg with libx264, libvpx-vp9, libopus and the native AAC encoder (Homebrew's has
# them). Video-only files mimic DASH video formats: 2 s GOP, fragmented mp4 with a sidx-style
# layout, or WebM with cues. Audio-only files are for the SEL-04 push-seek comparison.
#   h264_720p.mp4  h264_1080p.mp4       fragmented (moof) H.264, like DASH video itags
#   h264_1080p_progressive.mp4          one moov up front (faststart), the non-DASH layout
#   vp9_720p.webm  vp9_1080p.webm       VP9 with cues
#   opus.webm  aac.m4a                  audio-only, 1 kHz tone (fragmented m4a)
set -eu
out=${1:?usage: make_clips.sh OUT_DIR}
secs=${CLIP_SECONDS:-90}
ff=${FFMPEG:-ffmpeg}
mkdir -p "$out"
q="-hide_banner -loglevel error -y"

video() { # size rate -> testsrc2 input args
    echo "-f lavfi -i testsrc2=size=$1:rate=$2:duration=$secs"
}
# shellcheck disable=SC2086 # option strings are split on purpose
{
    $ff $q $(video 1280x720 30) -an -c:v libx264 -profile:v high -pix_fmt yuv420p -g 60 -keyint_min 60 \
        -sc_threshold 0 -b:v 2500k -movflags +frag_keyframe+empty_moov+default_base_moof+global_sidx \
        "$out/h264_720p.mp4"
    $ff $q $(video 1920x1080 30) -an -c:v libx264 -profile:v high -pix_fmt yuv420p -g 60 -keyint_min 60 \
        -sc_threshold 0 -b:v 4500k -movflags +frag_keyframe+empty_moov+default_base_moof+global_sidx \
        "$out/h264_1080p.mp4"
    $ff $q $(video 1920x1080 30) -an -c:v libx264 -profile:v high -pix_fmt yuv420p -g 60 -keyint_min 60 \
        -sc_threshold 0 -b:v 4500k -movflags +faststart "$out/h264_1080p_progressive.mp4"
    $ff $q $(video 1280x720 30) -an -c:v libvpx-vp9 -b:v 1800k -g 60 -row-mt 1 -deadline realtime \
        -cpu-used 8 "$out/vp9_720p.webm"
    $ff $q $(video 1920x1080 30) -an -c:v libvpx-vp9 -b:v 3000k -g 60 -row-mt 1 -deadline realtime \
        -cpu-used 8 "$out/vp9_1080p.webm"
    $ff $q -f lavfi -i "sine=frequency=1000:sample_rate=48000:duration=$secs" -c:a libopus -b:a 128k \
        "$out/opus.webm"
    $ff $q -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=$secs" -c:a aac -b:a 128k \
        -movflags +frag_keyframe+empty_moov+default_base_moof "$out/aac.m4a"
}
ls -l "$out"
