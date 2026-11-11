#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QtCore tests (parsers, arguments, probe mapping). Runs in the SDK/native container;
# skipped where Qt5Core development files are absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "resolver tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-resolver-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpRunner.h" -o "$out/moc_runner.cpp"
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpVideoService.h" -o "$out/moc_service.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" -DSAILTUBE_FIXTURES="\"$root/tests/fixtures\"" \
    $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/resolver_qt/ResolverTest.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpJson.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpRunner.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpVideoService.cpp" \
    "$root/Core/Platform/Infrastructure/GstCapabilityProbe.cpp" \
    "$root/Core/Media/Domain/StreamSelection.cpp" \
    "$root/Core/Media/Domain/AudioTrackPolicy.cpp" \
    "$root/Core/Media/Domain/LivePolicy.cpp" \
    "$out/moc_runner.cpp" "$out/moc_service.cpp" \
    $(pkg-config --libs Qt5Core)
"$out/t"
rm -rf "$out"
