#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# RES-10 resolve cache (Qt-free, always runs), then the shared interface's key registry and
# RES-09 table, SailTube's ladder and the yt-dlp mapping (Qt Core; skipped where Qt5Core
# development files are absent). Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-resolve-cache-test.$$
mkdir -p "$out"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out/cache" "$root/tests/resolve_cache/ResolveCacheTest.cpp" \
    "$root/Core/Player/Domain/ResolveCache.cpp"
"$out/cache"
if ! pkg-config --exists Qt5Core 2>/dev/null; then
    echo "resolve cache mapping tests: SKIPPED (no Qt5Core)"; rm -rf "$out"; exit 0
fi
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/mapping" \
    "$root/tests/resolve_cache/MappingTest.cpp" \
    "$root/Core/Player/Infrastructure/SharedStreamSourceLadder.cpp" \
    "$root/Core/Player/Infrastructure/YtDlpStreamMapping.cpp" \
    $(pkg-config --libs Qt5Core)
"$out/mapping"
rm -rf "$out"
