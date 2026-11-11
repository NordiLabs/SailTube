#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt tests of the player's QML-facing pieces (overlay controller timing, frame
# mailbox, surface layout, hardware node choice, GL resource rules and the cover video item). Runs in the SDK/native container; skipped where Qt5Quick is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Quick 2>/dev/null; then echo "player qt tests: SKIPPED (no Qt5Quick)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-player-qt.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
inc=$(pkg-config --cflags-only-I Qt5Quick | sed 's/-I/-isystem /g')
"$moc" -I"$root" "$root/Core/Player/Infrastructure/OverlayController.h" -o "$out/moc_overlay.cpp"
"$moc" -I"$root" "$root/Core/Player/Infrastructure/VideoSurfaceItem.h" -o "$out/moc_surface.cpp"
"$moc" -I"$root" "$root/Core/Player/Infrastructure/CoverVideoItem.h" -o "$out/moc_cover.cpp"
"$moc" -I"$root" "$root/tests/player_qt/PlayerQtTest.cpp" -o "$out/PlayerQtTest.moc"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" -I"$out" $inc -o "$out/t" \
    "$root/tests/player_qt/PlayerQtTest.cpp" \
    "$root/Core/Player/Infrastructure/OverlayController.cpp" \
    "$root/Core/Player/Infrastructure/VideoFrameMailbox.cpp" \
    "$root/Core/Player/Infrastructure/VideoSurfaceItem.cpp" \
    "$root/Core/Player/Infrastructure/CoverVideoItem.cpp" \
    "$root/Core/Player/Infrastructure/GlResources.cpp" \
    "$root/Core/Player/Infrastructure/HardwareVideoNode.cpp" \
    "$root/Core/Player/Domain/TapGestures.cpp" \
    "$out/moc_overlay.cpp" "$out/moc_surface.cpp" "$out/moc_cover.cpp" \
    $(pkg-config --libs Qt5Quick) $(pkg-config --libs glesv2 2>/dev/null || echo -lGLESv2) \
    $(pkg-config --libs egl 2>/dev/null || echo -lEGL)
QT_QPA_PLATFORM=minimal "$out/t"
rm -rf "$out"
