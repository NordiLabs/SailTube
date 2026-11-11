#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# DIA-01/02 (plain C++11) and DIA-03 (Qt5Core; skipped where Qt5Core is absent).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-diagnostics.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -o "$out/domain" "$root/tests/diagnostics/DiagnosticsTest.cpp"
"$out/domain"
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "diagnostics: timing log SKIPPED (no Qt5Core)"; echo "diagnostics: PASS"; exit 0; fi
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra $(pkg-config --cflags Qt5Core) -o "$out/timing" \
    "$root/tests/diagnostics/TimingLogTest.cpp" "$root/Core/Diagnostics/Infrastructure/TimingLog.cpp" $(pkg-config --libs Qt5Core)
"$out/timing"
echo "diagnostics: PASS"
