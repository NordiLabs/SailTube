#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free player policy tests (seek, split-seek, preroll, speed, buffering,
# freeze, position, switch); needs only a C++11 compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-seek-test.$$
d="$root/Core/Player/Domain"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/player_seek/PlayerSeekTest.cpp" \
    "$d/SeekPolicy.cpp" "$d/SplitSeekGuard.cpp" "$d/PrerollBarrier.cpp" "$d/BufferingPolicy.cpp" \
    "$d/FreezePolicy.cpp" "$d/PositionPolicy.cpp" "$d/SwitchPlan.cpp"
"$out"
rm -f "$out"
