#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-05 host player tests: real GStreamer engine + VideoPlayer on
# generated fixtures. Runs in the SailTube build container; skipped where the
# GStreamer or Qt development files are absent (e.g. the Mac).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network"
for p in $pkgs; do
    if ! pkg-config --exists "$p" 2>/dev/null; then echo "player gst tests: SKIPPED (no $p)"; exit 0; fi
done
out=${TMPDIR:-/tmp}/sailtube-player-gst-build.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
inf="$root/Core/Player/Infrastructure"
"$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
# Qt headers as system headers (Qt 5.6 warns under gcc 13).
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Network 2>/dev/null); do qtinc="$qtinc -isystem ${d#-I}"; done
gstcflags=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl)
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtinc $gstcflags -o "$out/t" \
    "$root/tests/player_gst/PlayerGstTest.cpp" \
    "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
    "$root/Core/Player/Domain/PipelinePlan.cpp" "$root/Core/Player/Domain/AudioChain.cpp" \
    "$root/Core/Player/Domain/SeekPolicy.cpp" "$root/Core/Player/Domain/SplitSeekGuard.cpp" \
    "$root/Core/Player/Domain/PrerollBarrier.cpp" "$root/Core/Player/Domain/BufferingPolicy.cpp" \
    "$root/Core/Player/Domain/FreezePolicy.cpp" "$root/Core/Player/Domain/PositionPolicy.cpp" \
    "$root/Core/Player/Domain/SwitchPlan.cpp" \
    "$out/moc_engine.cpp" "$out/moc_player.cpp" \
    $(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network)
status=0
"$out/t" || status=$?
rm -rf "$out"
exit $status
