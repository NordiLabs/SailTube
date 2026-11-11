#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-06: Qt / GStreamer side of the playback component's engine API that had no test yet.
# Five parts, each skipped cleanly where its development files are absent (e.g. the Mac):
#   core   (Qt5Core, Qt5Network)  AudioResourceGate host build, SharedStreamSource helpers and
#                                 mapSet over SailTune's component types, the ladder, timing labels
#   quick  (Qt5Quick, GLES, EGL)  VideoSurfaceItem fill mode, OverlayController gesture cancel,
#                                 GL name release, HardwareVideoNode without a context
#   engine (GStreamer, Qt5Network) GstEngine and VideoPlayer on generated WebM fixtures
#   init   (GStreamer, Qt5Network) start-up off the GUI thread and the decoder preload
#                                 (NFR-PERF-01/02), fresh child processes for the timings
#   store  (Qt5Core)              the STORE engine stub's start-up API (GstEngineStore.cpp)
# The last line sums every part: "player api qt tests: N checks, M failures". POSIX sh.
set -e
dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$dir/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-api-qt.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
CXX=${CXX:-c++}
total=0
failed=0
skipped=0
status=0

have() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    for p in "$@"; do pkg-config --exists "$p" 2>/dev/null || { missing=$p; return 1; }; done
    return 0
}

# Qt headers as system headers (Qt 5.6 warns under gcc 13).
sysinc() {
    r=""
    for d in $(pkg-config --cflags-only-I "$@" 2>/dev/null); do r="$r -isystem ${d#-I}"; done
    echo "$r"
}

mocbin() {
    m=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
    [ -x "$m" ] || m=$(command -v moc || command -v moc-qt5 || echo moc)
    echo "$m"
}

# run <part> <binary>: run it, print its output, add its "N checks, M failures" line.
run() {
    part=$1
    shift
    rc=0
    if command -v timeout >/dev/null 2>&1; then
        QT_QPA_PLATFORM=minimal timeout -s KILL 300 "$@" > "$out/$part.log" 2>&1 || rc=$?
    else
        QT_QPA_PLATFORM=minimal "$@" > "$out/$part.log" 2>&1 || rc=$?
    fi
    cat "$out/$part.log"
    counts=$(sed -n 's/.*: \([0-9][0-9]*\) checks, \([0-9][0-9]*\) failures*$/\1 \2/p' "$out/$part.log" | tail -n 1)
    if [ -z "$counts" ]; then
        if grep -q "SKIPPED" "$out/$part.log"; then skipped=$((skipped + 1)); return 0; fi
        echo "FAIL $part: no summary line (exit $rc)"
        failed=$((failed + 1))
        status=1
        return 0
    fi
    set -- $counts
    total=$((total + $1))
    failed=$((failed + $2))
    if [ "$rc" -ne 0 ] || [ "$2" -ne 0 ]; then status=1; fi
}

inf="$root/Core/Player/Infrastructure"
dom="$root/Core/Player/Domain"

# ---- core ------------------------------------------------------------------------------------
if have Qt5Core Qt5Network; then
    moc=$(mocbin)
    st="$root/Shared/SailTune/Core/SharedPlayback/Infrastructure"
    "$moc" -I"$root" "$inf/AudioResourceGate.h" -o "$out/moc_gate.cpp"
    "$moc" "$st/HttpsConnection.h" -o "$out/moc_HttpsConnection.cpp"
    "$moc" "$st/FastStreamResolver.h" -o "$out/moc_FastStreamResolver.cpp"
    # shellcheck disable=SC2046
    "$CXX" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy -DNORDTILE_NO_AUDIORESOURCE -I"$root" \
        $(sysinc Qt5Core Qt5Network) -o "$out/core" \
        "$dir/CoreApiTest.cpp" "$inf/AudioResourceGate.cpp" \
        "$inf/SharedStreamSource.cpp" "$inf/SharedStreamSourceLadder.cpp" "$inf/SharedStreamSourceThreaded.cpp" \
        "$st/FastStreamResolver.cpp" "$st/HttpsConnection.cpp" "$st/MediaProxy.cpp" "$st/PlayerResponseParser.cpp" \
        "$out/moc_gate.cpp" "$out/moc_HttpsConnection.cpp" "$out/moc_FastStreamResolver.cpp" \
        $(pkg-config --libs Qt5Core Qt5Network)
    run core "$out/core"
else
    echo "player api core tests: SKIPPED (no $missing)"
    skipped=$((skipped + 1))
fi

# ---- quick -----------------------------------------------------------------------------------
if have Qt5Quick; then
    moc=$(mocbin)
    "$moc" -I"$root" "$inf/OverlayController.h" -o "$out/moc_overlay.cpp"
    "$moc" -I"$root" "$inf/VideoSurfaceItem.h" -o "$out/moc_surface.cpp"
    "$moc" -I"$root" "$dir/QuickApiTest.cpp" -o "$out/QuickApiTest.moc"
    # shellcheck disable=SC2046
    "$CXX" -std=c++11 -fPIC -Wall -Wextra -I"$root" -I"$out" $(sysinc Qt5Quick) -o "$out/quick" \
        "$dir/QuickApiTest.cpp" "$inf/OverlayController.cpp" "$inf/VideoFrameMailbox.cpp" \
        "$inf/VideoSurfaceItem.cpp" "$inf/GlResources.cpp" "$inf/HardwareVideoNode.cpp" "$dom/TapGestures.cpp" \
        "$out/moc_overlay.cpp" "$out/moc_surface.cpp" \
        $(pkg-config --libs Qt5Quick) $(pkg-config --libs glesv2 2>/dev/null || echo -lGLESv2) \
        $(pkg-config --libs egl 2>/dev/null || echo -lEGL)
    run quick "$out/quick"
else
    echo "player api quick tests: SKIPPED (no ${missing:-Qt5Quick})"
    skipped=$((skipped + 1))
fi

# ---- engine ----------------------------------------------------------------------------------
gst="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl"
# shellcheck disable=SC2086
if have $gst Qt5Core Qt5Network; then
    moc=$(mocbin)
    "$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
    "$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
    # shellcheck disable=SC2046,SC2086
    "$CXX" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(sysinc Qt5Core Qt5Network) $(pkg-config --cflags $gst) \
        -o "$out/engine" "$dir/EngineApiTest.cpp" \
        "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
        "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" "$dom/SplitSeekGuard.cpp" \
        "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" "$dom/PositionPolicy.cpp" \
        "$dom/SwitchPlan.cpp" "$out/moc_engine.cpp" "$out/moc_player.cpp" \
        $(pkg-config --libs $gst Qt5Core Qt5Network)
    run engine "$out/engine"
else
    echo "player api engine tests: SKIPPED (no $missing)"
    skipped=$((skipped + 1))
fi

# ---- init ------------------------------------------------------------------------------------
# shellcheck disable=SC2086
if have $gst Qt5Core Qt5Network; then
    moc=$(mocbin)
    [ -f "$out/moc_engine.cpp" ] || "$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
    [ -f "$out/moc_player.cpp" ] || "$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
    # shellcheck disable=SC2046,SC2086
    "$CXX" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(sysinc Qt5Core Qt5Network) $(pkg-config --cflags $gst) \
        -o "$out/init" "$dir/EngineInitTest.cpp" \
        "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
        "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" "$dom/SplitSeekGuard.cpp" \
        "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" "$dom/PositionPolicy.cpp" \
        "$dom/SwitchPlan.cpp" "$out/moc_engine.cpp" "$out/moc_player.cpp" \
        $(pkg-config --libs $gst Qt5Core Qt5Network)
    # Live GObject instance counts for the "preload creates no element" check.
    GOBJECT_DEBUG=instance-count
    export GOBJECT_DEBUG
    run init "$out/init"
    unset GOBJECT_DEBUG
else
    echo "player api init tests: SKIPPED (no $missing)"
    skipped=$((skipped + 1))
fi

# ---- store -----------------------------------------------------------------------------------
if have Qt5Core; then
    moc=$(mocbin)
    "$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine_store.cpp"
    # shellcheck disable=SC2046
    "$CXX" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(sysinc Qt5Core) -o "$out/store" \
        "$dir/StoreInitTest.cpp" "$inf/GstEngineStore.cpp" "$inf/VideoFrameMailbox.cpp" "$out/moc_engine_store.cpp" \
        $(pkg-config --libs Qt5Core)
    run store "$out/store"
else
    echo "player api store tests: SKIPPED (no $missing)"
    skipped=$((skipped + 1))
fi

note=""
[ "$skipped" -gt 0 ] && note=" ($skipped of 5 parts skipped)"
echo "player api qt tests: $total checks, $failed failure$( [ "$failed" -eq 1 ] || echo s)$note"
exit $status
