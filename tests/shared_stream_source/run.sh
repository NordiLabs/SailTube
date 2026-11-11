#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SharedStreamSource (adapter from SailTune's pinned component to Shared/Playback/StreamSource.h):
# the adapter and the threaded wrapper against SailTune's pinned FastStreamResolver / MediaProxy /
# HttpsConnection with local TLS servers (Qt5Core + Qt5Network, moc; skipped where they are
# absent). POSIX sh, run from anywhere.
set -e
dir=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$dir/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-shared-stream-source-test.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
if ! command -v pkg-config >/dev/null 2>&1 || ! pkg-config --exists Qt5Core Qt5Network 2>/dev/null; then
    echo "shared stream source tests: SKIPPED (no Qt5Core/Qt5Network)"; exit 0
fi
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=$(command -v moc || command -v moc-qt5)
st="$root/Shared/SailTune/Core/SharedPlayback/Infrastructure"
"$moc" "$st/HttpsConnection.h" -o "$out/moc_HttpsConnection.cpp"
"$moc" "$st/FastStreamResolver.h" -o "$out/moc_FastStreamResolver.cpp"
common="$root/Core/Player/Infrastructure/SharedStreamSource.cpp \
    $root/Core/Player/Infrastructure/SharedStreamSourceLadder.cpp \
    $root/Core/Player/Infrastructure/SharedStreamSourceThreaded.cpp \
    $st/FastStreamResolver.cpp $st/HttpsConnection.cpp $st/MediaProxy.cpp $st/PlayerResponseParser.cpp \
    $out/moc_HttpsConnection.cpp $out/moc_FastStreamResolver.cpp"
flags="-std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy -I$root $(pkg-config --cflags Qt5Core Qt5Network)"
fixtures="-DFIXTURE_DIR=\"$root/Shared/SailTune/tests/sharedplayback/fixtures\""
# shellcheck disable=SC2086
"${CXX:-c++}" $flags "$fixtures" -o "$out/adapter" "$dir/SharedStreamSourceTest.cpp" $common \
    $(pkg-config --libs Qt5Core Qt5Network)
QT_QPA_PLATFORM=minimal timeout -s KILL 240 "$out/adapter"
# shellcheck disable=SC2086
"${CXX:-c++}" $flags "$fixtures" -o "$out/threaded" "$dir/ThreadedStreamSourceTest.cpp" $common \
    $(pkg-config --libs Qt5Core Qt5Network)
QT_QPA_PLATFORM=minimal timeout -s KILL 120 "$out/threaded"
