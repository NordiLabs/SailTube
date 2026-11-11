#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free stall-watchdog policy tests (PLY-18 companion); needs only a C++11 compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-stall-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/player_stall/StallPolicyTest.cpp" \
    "$root/Core/Player/Domain/StallPolicy.cpp"
status=0
"$out" || status=$?
rm -f "$out"
exit $status
