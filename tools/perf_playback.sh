#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Host playback performance harness against live YouTube (NFR-PERF-02; docs/PERF_PLAYBACK.md).
#
#   tools/perf_playback.sh [--local] [--rounds N] [--videos id,id,...] [--primer id] [--height H]
#                          [--modes cold,warm,cached] [--round-timeout MS] [--pause MS] [--build-only]
#
# Without --local: copies the sources it needs into the build container ($SAILTUBE_PERF_CONTAINER,
# default sfos-m1-sailtube; Docker via $DOCKER_HOST or OrbStack's socket) under $SAILTUBE_PERF_DIR
# (default /tmp/sailtube-perf; the build goes to <that>-build), then builds and runs there. With
# --local (or inside the container): builds with pkg-config's GStreamer + Qt5 and runs here.
# --build-only stops after the build (no request is made). Needs network access to www.youtube.com
# and *.googlevideo.com. Never touches a phone.
# Exit 0 when at least 3 videos produced numbers (tools/perf_playback/PerfPlayback.cpp).
set -e
root=$(cd "$(dirname "$0")/.." && pwd)

pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network"
have_deps() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    for p in $pkgs; do pkg-config --exists "$p" 2>/dev/null || return 1; done
    return 0
}

local_mode=0
if [ "${1:-}" = "--local" ]; then local_mode=1; shift; fi
if [ $local_mode -eq 0 ] && ! command -v docker >/dev/null 2>&1 && have_deps; then local_mode=1; fi

if [ $local_mode -eq 0 ]; then
    container=${SAILTUBE_PERF_CONTAINER:-sfos-m1-sailtube}
    if [ -z "${DOCKER_HOST:-}" ] && [ -S "$HOME/.orbstack/run/docker.sock" ]; then
        DOCKER_HOST="unix://$HOME/.orbstack/run/docker.sock"; export DOCKER_HOST
    fi
    # Own directory (not /tmp/lane, which other lane tools recreate while they run); a caller that
    # shares the container with other runs of this tool names its own.
    dest=${SAILTUBE_PERF_DIR:-/tmp/sailtube-perf}
    echo "perf_playback: copying sources into $container:$dest" >&2
    # Shared/SailTune/Core/Beta: the pinned HttpsConnection.cpp includes ../../Beta/ServerDate.h (5611210).
    (cd "$root" && COPYFILE_DISABLE=1 tar -cf - Core/Player Shared/SailTune/Core/SharedPlayback Shared/SailTune/Core/Beta \
        tools/perf_playback.sh tools/perf_playback) |
        docker exec -i -u sdk "$container" sh -c "rm -rf $dest && mkdir -p $dest $dest-build && tar -xf - -C $dest 2>/dev/null"
    exec docker exec -u sdk -w "$dest" -e TMPDIR="$dest-build" "$container" sh tools/perf_playback.sh --local "$@"
fi

if ! have_deps; then
    echo "perf_playback: GStreamer/Qt5 development files missing (need: $pkgs)" >&2
    exit 2
fi

build_only=0
for a in "$@"; do [ "$a" = "--build-only" ] && build_only=1; done

out=${TMPDIR:-/tmp}/sailtube-perf-build
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
cxx=${CXX:-c++}
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Network); do qtinc="$qtinc -isystem ${d#-I}"; done
gstcflags=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl)
libs=$(pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network)
flags="-std=c++11 -O2 -fPIC -Wall -Wextra -I$root $qtinc $gstcflags"

shared="$root/Shared/SailTune/Core/SharedPlayback/Infrastructure"
inf="$root/Core/Player/Infrastructure"
dom="$root/Core/Player/Domain"
"$moc" "$shared/HttpsConnection.h" -o "$out/moc_HttpsConnection.cpp"
"$moc" "$shared/FastStreamResolver.h" -o "$out/moc_FastStreamResolver.cpp"
"$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_GstEngine.cpp"
"$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_VideoPlayer.cpp"

# Compile in parallel. The pinned component is built with its own recipe's flag
# (Shared/SailTune/tests/sharedplayback/run.sh: -Wno-deprecated-copy); everything else warning-free.
objs=""
pids=""
compile() {  # source, extra flags
    o="$out/$(basename "$1" .cpp).o"
    objs="$objs $o"
    # shellcheck disable=SC2086
    $cxx $flags $2 -c "$1" -o "$o" &
    pids="$pids $!"
}
for f in "$shared/HttpsConnection.cpp" "$shared/FastStreamResolver.cpp" "$shared/PlayerResponseParser.cpp" \
         "$shared/MediaProxy.cpp" "$out/moc_HttpsConnection.cpp" "$out/moc_FastStreamResolver.cpp"; do
    compile "$f" "-Wno-deprecated-copy"
done
for f in "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
         "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" "$dom/SplitSeekGuard.cpp" \
         "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" "$dom/PositionPolicy.cpp" \
         "$dom/SwitchPlan.cpp" "$out/moc_GstEngine.cpp" "$out/moc_VideoPlayer.cpp" \
         "$root/tools/perf_playback/PerfPlayback.cpp"; do
    compile "$f" ""
done
failed=0
for p in $pids; do wait "$p" || failed=1; done
if [ $failed -ne 0 ]; then echo "perf_playback: build FAILED" >&2; exit 2; fi
# shellcheck disable=SC2086
$cxx -o "$out/perf_playback" $objs $libs
echo "perf_playback: built $out/perf_playback" >&2
[ $build_only -eq 1 ] && exit 0

status=0
"$out/perf_playback" "$@" || status=$?
exit $status
