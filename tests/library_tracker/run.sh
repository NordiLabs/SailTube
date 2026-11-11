#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free checks of the LIB-05 progress-save cadence. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-tracker-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/library_tracker/TrackerTest.cpp" \
    "$root/Core/Library/Domain/ProgressTracker.cpp"
"$out"
rm -f "$out"
