#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Start-up and tap-to-picture timing points (NFR-PERF-01/07). Needs Qt5Core.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "startup timing tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-startup.$$
mkdir -p "$out"; trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/StartupTiming.h" -o "$out/moc_timing.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/startup/StartupTimingTest.cpp" "$root/Clients/Sailfish/Presentation/StartupTiming.cpp" \
    "$out/moc_timing.cpp" $(pkg-config --libs Qt5Core)
"$out/t"
