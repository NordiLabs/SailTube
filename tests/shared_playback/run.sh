#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QtCore tests of the yt-dlp fallback adapter (StreamSource.h interface) over core's yt-dlp path (fake extractor).
# Runs in the SDK/native container; skipped where Qt5Core development files are absent. The
# key registry, resolve cache and mapping are tests/resolve_cache/run.sh.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "shared playback tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-shared-playback-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpRunner.h" -o "$out/moc_runner.cpp"
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpVideoService.h" -o "$out/moc_service.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy -I"$root" -DSAILTUBE_FIXTURES="\"$root/tests/fixtures\"" \
    $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/shared_playback/SharedPlaybackTest.cpp" \
    "$root/Core/Player/Infrastructure/YtDlpStreamResolver.cpp" \
    "$root/Core/Player/Infrastructure/YtDlpStreamMapping.cpp" \
    "$root/Core/Player/Infrastructure/SharedStreamSourceLadder.cpp" \
    "$root/Core/Player/Domain/ResolveCache.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpJson.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpRunner.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpVideoService.cpp" \
    "$out/moc_runner.cpp" "$out/moc_service.cpp" \
    $(pkg-config --libs Qt5Core)
"$out/t"
rm -rf "$out"
