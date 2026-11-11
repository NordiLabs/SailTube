#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Watch-session tests. Part 1 (Qt-free, any C++11 compiler, also the Mac):
# PlaybackController with a fake port and injected time. Part 2 (SDK/native
# container): the real PlaybackSession on VideoPlayer + GStreamer behind fake
# shared StreamResolver / StreamFetcher implementations, with generated
# fixtures over HTTP; skipped where the GStreamer or Qt development files are
# absent. SESSION_TEST_ONLY=<substring> runs only the matching part 2 tests.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-session-test.$$
mkdir -p "$out"
dom="$root/Core/Player/Domain"
med="$root/Core/Media/Domain"
policies="$dom/SessionArbiter.cpp $dom/RecoveryPolicy.cpp $dom/AudioResourcePolicy.cpp $dom/AutoplayPolicy.cpp
          $dom/MediaControlsPolicy.cpp $dom/SponsorSkip.cpp $dom/StallPolicy.cpp $med/StreamSelection.cpp
          $med/AudioTrackPolicy.cpp $med/LivePolicy.cpp"
status=0
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out/controller" "$root/tests/session_qt/ControllerTest.cpp" \
    "$root/Clients/Sailfish/Presentation/PlaybackController.cpp" $policies
"$out/controller" || status=$?

pkgs="gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl Qt5Core Qt5Network"
missing=""
for p in $pkgs; do pkg-config --exists "$p" 2>/dev/null || missing="$p"; done
if [ -n "$missing" ]; then
    echo "session gst tests: SKIPPED (no $missing)"
    rm -rf "$out"
    exit $status
fi
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
inf="$root/Core/Player/Infrastructure"
pre="$root/Clients/Sailfish/Presentation"
"$moc" -I"$root" "$inf/GstEngine.h" -o "$out/moc_engine.cpp"
"$moc" -I"$root" "$inf/VideoPlayer.h" -o "$out/moc_player.cpp"
"$moc" -I"$root" "$inf/AudioResourceGate.h" -o "$out/moc_gate.cpp"
"$moc" -I"$root" "$inf/HardwareDecodeGate.h" -o "$out/moc_hwgate.cpp"
"$moc" -I"$root" "$pre/PlaybackSession.h" -o "$out/moc_session.cpp"
"$moc" -I"$root" "$pre/AppSettings.h" -o "$out/moc_settings.cpp"
# SEL-06 captions seam: the catalog component's caption models behind PlaybackCaptionSource.
cat="$root/Core/Catalog"
"$moc" -I"$root" "$pre/PlaybackCaptions.h" -o "$out/moc_captions.cpp"
"$moc" -I"$root" "$pre/CaptionTracksModel.h" -o "$out/moc_tracks.cpp"
"$moc" -I"$root" "$pre/CaptionCues.h" -o "$out/moc_cues.cpp"
"$moc" -I"$root" "$cat/Infrastructure/CaptionService.h" -o "$out/moc_captionservice.cpp"
"$moc" -I"$root" "$cat/Infrastructure/HttpTransport.h" -o "$out/moc_http.cpp"
"$moc" -I"$root" "$cat/Infrastructure/WorkLane.h" -o "$out/moc_lane.cpp"
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Network 2>/dev/null); do qtinc="$qtinc -isystem ${d#-I}"; done
# AppSettings' PO-token settings (RES-14) clamp through the resolver's policy when it exists.
potoken=""
[ -f "$root/Core/Resolver/Domain/PoTokenPolicy.cpp" ] && potoken="$root/Core/Resolver/Domain/PoTokenPolicy.cpp"
gstcflags=$(pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-video-1.0 nemo-gstreamer-interfaces-1.0 egl)
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -DNORDTILE_NO_AUDIORESOURCE -I"$root" $qtinc $gstcflags -o "$out/session" \
    "$root/tests/session_qt/SessionGstTest.cpp" \
    "$pre/PlaybackSession.cpp" "$pre/PlaybackController.cpp" "$pre/AppSettings.cpp" $potoken \
    "$pre/PlaybackCaptions.cpp" "$pre/CaptionTracksModel.cpp" "$pre/CaptionCues.cpp" "$pre/SettingsFile.cpp" \
    "$cat/Domain/Captions.cpp" "$cat/Domain/TextParse.cpp" "$cat/Infrastructure/CaptionService.cpp" \
    "$cat/Infrastructure/HttpTransport.cpp" "$cat/Infrastructure/Json.cpp" \
    "$root/Core/Settings/Domain/SettingsDefaults.cpp" \
    "$inf/GstEngine.cpp" "$inf/VideoPlayer.cpp" "$inf/VideoFrameMailbox.cpp" "$inf/EglSinkBridge.cpp" \
    "$inf/AudioResourceGate.cpp" "$inf/SharedStreamSourceThreaded.cpp" "$inf/MemoryTrim.cpp" \
    "$inf/HardwareDecodeGate.cpp" "$inf/HardwareProbeClip.cpp" "$dom/HardwareProbe.cpp" \
    "$dom/PipelinePlan.cpp" "$dom/AudioChain.cpp" "$dom/SeekPolicy.cpp" "$dom/SplitSeekGuard.cpp" \
    "$dom/PrerollBarrier.cpp" "$dom/BufferingPolicy.cpp" "$dom/FreezePolicy.cpp" "$dom/PositionPolicy.cpp" \
    "$dom/SwitchPlan.cpp" "$dom/TapGestures.cpp" $policies \
    "$out/moc_engine.cpp" "$out/moc_player.cpp" "$out/moc_gate.cpp" "$out/moc_session.cpp" "$out/moc_settings.cpp" \
    "$out/moc_hwgate.cpp" "$out/moc_captions.cpp" "$out/moc_tracks.cpp" "$out/moc_cues.cpp" \
    "$out/moc_captionservice.cpp" "$out/moc_http.cpp" "$out/moc_lane.cpp" \
    $(pkg-config --libs $pkgs)
"$out/session" || status=$?
rm -rf "$out"
exit $status
