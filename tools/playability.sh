#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Playability matrix against live YouTube (docs/PLAYABILITY.md): the app's whole resolve chain
# (PlaybackController -> SharedStreamSource: SailTune fast path, then yt-dlp without and with
# QuickJS through the warm worker), the SEL-02 pick, time to first picture and the app's message.
#
#   tools/playability.sh [--local] [--videos id,id,...] [--pause MS] [--no-clients] [--no-warm]
#                        [--list FILE] [--build-only]
#
# Without --local: copies the sources it needs into the build container ($SAILTUBE_PLAYABILITY_CONTAINER,
# default sfos-m1-sailtube; Docker via $DOCKER_HOST or OrbStack's socket) under
# $SAILTUBE_PLAYABILITY_DIR (default /tmp/sailtube-playability), then builds and runs there. With
# --local (or inside the container): builds with pkg-config's GStreamer + Qt5 (Core, Network, Qml)
# and the bundled resolver runtime ($SAILTUBE_PYTHON_RUNTIME, default /builds/_vendor/python-runtime:
# runtime/yt-dlp.pyz, lib/libsailtune-quickjs.so.1; PyOtherSide and python3.11 installed).
# Needs network access to www.youtube.com and *.googlevideo.com. Never touches a phone.
# Request budget: one pass over the list, >= 3 s between videos (4 s default), at most 3 extra
# player_client runs per video that was expected to play and did not. Do not loop it.
set -e
root=$(cd "$(dirname "$0")/.." && pwd)

pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network Qt5Qml"
have_deps() {
    command -v pkg-config >/dev/null 2>&1 || return 1
    for p in $pkgs; do pkg-config --exists "$p" 2>/dev/null || return 1; done
    return 0
}

local_mode=0
if [ "${1:-}" = "--local" ]; then local_mode=1; shift; fi
if [ $local_mode -eq 0 ] && ! command -v docker >/dev/null 2>&1 && have_deps; then local_mode=1; fi

if [ $local_mode -eq 0 ]; then
    container=${SAILTUBE_PLAYABILITY_CONTAINER:-sfos-m1-sailtube}
    if [ -z "${DOCKER_HOST:-}" ] && [ -S "$HOME/.orbstack/run/docker.sock" ]; then
        DOCKER_HOST="unix://$HOME/.orbstack/run/docker.sock"; export DOCKER_HOST
    fi
    dest=${SAILTUBE_PLAYABILITY_DIR:-/tmp/sailtube-playability}
    echo "playability: copying sources into $container:$dest" >&2
    (cd "$root" && COPYFILE_DISABLE=1 tar -cf - Core Shared Clients/Sailfish/Presentation tools/playability.sh tools/playability) |
        docker exec -i -u sdk "$container" sh -c "rm -rf $dest/src && mkdir -p $dest/src && tar -xf - -C $dest/src 2>/dev/null"
    exec docker exec -u sdk -w "$dest/src" -e TMPDIR="$dest" "$container" sh tools/playability.sh --local "$@"
fi

if ! have_deps; then
    echo "playability: GStreamer/Qt5 development files missing (need: $pkgs)" >&2
    exit 2
fi
vendor=${SAILTUBE_PYTHON_RUNTIME:-/builds/_vendor/python-runtime}
if [ ! -s "$vendor/runtime/yt-dlp.pyz" ]; then
    echo "playability: no bundled resolver runtime at $vendor (runtime/yt-dlp.pyz)" >&2
    exit 2
fi

build_only=0
for a in "$@"; do [ "$a" = "--build-only" ] && build_only=1; done

out=${TMPDIR:-/tmp}/sailtube-playability-build
mkdir -p "$out/inc" "$out/root/runtime" "$out/root/lib"
# The runtime layout the app installs (NORDTILE_RUNTIME_ROOT): runtime/ and lib/.
cp "$root/Shared/Runtime/Python/resolver.py" "$root/Shared/Runtime/Python/Resolver.qml" \
   "$root/Core/Worker/Runtime/worker.py" "$root/Core/Worker/Runtime/Worker.qml" "$vendor/runtime/yt-dlp.pyz" "$out/root/runtime/"
[ -f "$vendor/lib/libsailtune-quickjs.so.1" ] && cp "$vendor/lib/libsailtune-quickjs.so.1" "$out/root/lib/"
printf '#pragma once\n#define NORDTILE_RUNTIME_ROOT "%s"\n' "$out/root" > "$out/inc/NordTileRuntimeLayout.h"

moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
cxx=${CXX:-c++}
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Network Qt5Qml); do qtinc="$qtinc -isystem ${d#-I}"; done
gstcflags=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl)
libs="$(pkg-config --libs $pkgs) -ldl -pthread"
flags="-std=c++11 -O2 -fPIC -Wall -Wextra -DNORDTILE_NO_AUDIORESOURCE -I$out/inc -I$root $qtinc $gstcflags"

st="$root/Shared/SailTune/Core/SharedPlayback/Infrastructure"
inf="$root/Core/Player/Infrastructure"
dom="$root/Core/Player/Domain"
med="$root/Core/Media/Domain"
res="$root/Core/Resolver"
pre="$root/Clients/Sailfish/Presentation"
for h in "$st/HttpsConnection.h" "$st/FastStreamResolver.h" "$inf/GstEngine.h" "$inf/VideoPlayer.h" \
         "$inf/AudioResourceGate.h" "$inf/HardwareDecodeGate.h" "$pre/PlaybackSession.h" "$pre/AppSettings.h" \
         "$res/Infrastructure/YtDlpRunner.h" "$res/Infrastructure/YtDlpVideoService.h" \
         "$root/Core/Worker/Infrastructure/WarmResolver.h" "$root/Core/Worker/Infrastructure/PythonWorkerChild.h" \
         "$root/Core/Process/Infrastructure/HelperProcess.h" "$root/Shared/Runtime/Child/PythonResolverChild.h"; do
    "$moc" -I"$root" "$h" -o "$out/moc_$(basename "$h" .h).cpp"
done

objs=""
pids=""
compile() {  # source, extra flags
    o="$out/$(basename "$(dirname "$1")")_$(basename "$1" .cpp).o"
    objs="$objs $o"
    # shellcheck disable=SC2086
    $cxx $flags $2 -c "$1" -o "$o" &
    pids="$pids $!"
}
# The pinned component with its own recipe's flag (as tools/perf_playback.sh); the rest warning-free.
for f in "$st/HttpsConnection.cpp" "$st/FastStreamResolver.cpp" "$st/PlayerResponseParser.cpp" "$st/MediaProxy.cpp" \
         "$out/moc_HttpsConnection.cpp" "$out/moc_FastStreamResolver.cpp"; do
    compile "$f" "-Wno-deprecated-copy"
done
potoken=""
[ -f "$res/Domain/PoTokenPolicy.cpp" ] && potoken="$res/Domain/PoTokenPolicy.cpp"
for f in "$inf/SharedStreamSource.cpp" "$inf/SharedStreamSourceLadder.cpp" "$inf/SharedStreamSourceThreaded.cpp" \
         "$inf/YtDlpStreamResolver.cpp" "$inf/YtDlpStreamMapping.cpp" \
         "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
         "$inf/AudioResourceGate.cpp" "$inf/HardwareDecodeGate.cpp" "$inf/HardwareProbeClip.cpp" \
         "$dom/ResolveCache.cpp" "$dom/HardwareProbe.cpp" "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" \
         "$dom/SplitSeekGuard.cpp" "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" \
         "$dom/PositionPolicy.cpp" "$dom/SwitchPlan.cpp" "$dom/TapGestures.cpp" "$dom/SessionArbiter.cpp" \
         "$dom/RecoveryPolicy.cpp" "$dom/AudioResourcePolicy.cpp" "$dom/AutoplayPolicy.cpp" "$dom/MediaControlsPolicy.cpp" \
         "$dom/SponsorSkip.cpp" "$dom/StallPolicy.cpp" "$med/StreamSelection.cpp" "$med/AudioTrackPolicy.cpp" "$med/LivePolicy.cpp" \
         "$res/Infrastructure/YtDlpJson.cpp" "$res/Infrastructure/YtDlpRunner.cpp" "$res/Infrastructure/YtDlpVideoService.cpp" \
         $potoken "$root/Core/Settings/Domain/SettingsDefaults.cpp" \
         "$pre/PlaybackSession.cpp" "$pre/PlaybackController.cpp" "$pre/AppSettings.cpp" \
         "$root/Core/Worker/Infrastructure/WarmResolver.cpp" "$root/Core/Worker/Infrastructure/PythonWorkerChild.cpp" \
         "$root/Core/Process/Infrastructure/HelperProcess.cpp" \
         "$root/Shared/Runtime/Child/PythonResolverChild.cpp" "$root/Shared/Runtime/Child/QuickJsChild.cpp" \
         "$out"/moc_GstEngine.cpp "$out"/moc_VideoPlayer.cpp "$out"/moc_AudioResourceGate.cpp "$out"/moc_HardwareDecodeGate.cpp \
         "$out"/moc_PlaybackSession.cpp "$out"/moc_AppSettings.cpp "$out"/moc_YtDlpRunner.cpp "$out"/moc_YtDlpVideoService.cpp \
         "$out"/moc_WarmResolver.cpp "$out"/moc_PythonWorkerChild.cpp "$out"/moc_HelperProcess.cpp \
         "$out"/moc_PythonResolverChild.cpp "$root/tools/playability/Playability.cpp"; do
    compile "$f" ""
done
failed=0
for p in $pids; do wait "$p" || failed=1; done
if [ $failed -ne 0 ]; then echo "playability: build FAILED" >&2; exit 2; fi
# shellcheck disable=SC2086
$cxx -o "$out/playability" $objs $libs
echo "playability: built $out/playability" >&2
[ $build_only -eq 1 ] && exit 0

args=""
for a in "$@"; do [ "$a" != "--build-only" ] && args="$args $a"; done
# Settings files (AppSettings is linked, not used) and caches stay in the build directory.
HOME="$out/home"; XDG_CONFIG_HOME="$out/home/.config"; XDG_CACHE_HOME="$out/home/.cache"; export HOME XDG_CONFIG_HOME XDG_CACHE_HOME
mkdir -p "$XDG_CONFIG_HOME" "$XDG_CACHE_HOME"
# A UTF-8 locale as on the phone (the container's POSIX one makes yt-dlp restrict file names).
locale -a 2>/dev/null | grep -qi '^c.utf-\?8$' && { LC_ALL=C.UTF-8; export LC_ALL; }
# shellcheck disable=SC2086
exec "$out/playability" --list "$root/tools/playability/videos.tsv" --work "$out/work" $args
