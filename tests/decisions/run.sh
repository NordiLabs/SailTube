#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Decision library (NFR-TST-04) and ES5 gate (PLT-05). In the build container the harness runs
# in Qt 5.6's QJSEngine; where Node is installed (the Mac) it also runs in a bare Node vm.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
ran=0
if command -v node >/dev/null 2>&1; then
    node "$root/tests/decisions/node_host.js" "$root"
    ran=1
fi
if pkg-config --exists Qt5Qml 2>/dev/null; then
    out=${TMPDIR:-/tmp}/sailtube-decisions.$$
    mkdir -p "$out"
    moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
    [ -x "$moc" ] || moc=moc
    "$moc" "$root/tests/decisions/jsrun.cpp" -o "$out/jsrun.moc"
    "${CXX:-c++}" -std=c++11 -fPIC -Wall -I"$out" $(pkg-config --cflags Qt5Qml) -o "$out/jsrun" \
        "$root/tests/decisions/jsrun.cpp" $(pkg-config --libs Qt5Qml)
    "$out/jsrun" "$root"
    rm -rf "$out"
    ran=1
fi
[ "$ran" = 1 ] || { echo "decisions: SKIPPED (neither node nor Qt5Qml found)"; exit 0; }
