#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# PLY-20/PLY-19 host tests, no droid elements and no GL:
#   1. EglSinkBridge against fake sinks that implement the installed
#      nemo-gstreamer-interfaces API (HardwareSinkTest.cpp);
#   2. the per-device hardware-decode probe (HardwareDecodeGate + GstHardwareProbeRunner) and the
#      player's hardware render path and 3 s watchdog, with fake droidvdec / droideglsink
#      factories (HardwareProbeTest.cpp).
# Runs in the SailTube build container; skipped where the headers are absent (e.g. the Mac).
# Real rendering is device-only (§6 D3, D7).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core"
for p in $pkgs; do
    if ! pkg-config --exists "$p" 2>/dev/null; then echo "player hw tests: SKIPPED (no $p)"; exit 0; fi
done
out=${TMPDIR:-/tmp}/sailtube-player-hw.$$
mkdir -p "$out"
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core 2>/dev/null); do qtinc="$qtinc -isystem ${d#-I}"; done
inf="$root/Core/Player/Infrastructure"
dom="$root/Core/Player/Domain"
gstcflags=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl)
libs=$(pkg-config --libs $pkgs)
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtinc $gstcflags -o "$out/t" \
    "$root/tests/player_hw/HardwareSinkTest.cpp" "$inf/EglSinkBridge.cpp" "$inf/VideoFrameMailbox.cpp" $libs
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
"$moc" -I"$root" "$inf/HardwareDecodeGate.h" -o "$out/moc_gate.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtinc $gstcflags -o "$out/probe" \
    "$root/tests/player_hw/HardwareProbeTest.cpp" \
    "$inf/HardwareDecodeGate.cpp" "$inf/HardwareProbeRunner.cpp" "$inf/HardwareProbeClip.cpp" \
    "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
    "$dom/HardwareProbe.cpp" "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" \
    "$dom/SplitSeekGuard.cpp" "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" \
    "$dom/PositionPolicy.cpp" "$dom/SwitchPlan.cpp" \
    "$out/moc_engine.cpp" "$out/moc_player.cpp" "$out/moc_gate.cpp" $libs
status=0
"$out/t" || status=$?
"$out/probe" || status=$?
rm -rf "$out"
exit $status
