#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# PLT-09 host test: TMPDIR prepared by prepareFlashTempDir (emptied, owner-only, exported before
# GLib), then dual mp4 (download buffer) and dual WebM played over HTTP while /proc/self/fd is
# watched: nothing open in /tmp, /run or /dev/shm, the mp4 download buffer under TMPDIR. Runs in
# the SailTube build container; skipped where GStreamer or Qt development files (or /proc) are
# absent (e.g. the Mac). The 2-hour "/tmp usage < 1 MiB" check stays a device step.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network"
for p in $pkgs; do
    if ! pkg-config --exists "$p" 2>/dev/null; then echo "tmpdir tests: SKIPPED (no $p)"; exit 0; fi
done
out=${TMPDIR:-/tmp}/sailtube-tmpdir-build.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
inf="$root/Core/Player/Infrastructure"
dom="$root/Core/Player/Domain"
"$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Network 2>/dev/null); do qtinc="$qtinc -isystem ${d#-I}"; done
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtinc \
    $(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl) -o "$out/t" \
    "$root/tests/tmpdir/TmpDirTest.cpp" "$inf/TempDirSetup.cpp" \
    "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
    "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" "$dom/SplitSeekGuard.cpp" \
    "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" "$dom/PositionPolicy.cpp" \
    "$dom/SwitchPlan.cpp" "$out/moc_engine.cpp" "$out/moc_player.cpp" \
    $(pkg-config --libs $pkgs)
status=0
"$out/t" || status=$?
rm -rf "$out"
exit $status
