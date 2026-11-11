#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free pipeline-plan, audio-chain, decode-mode and hardware-probe verdict tests; needs only a C++11
# compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-plan-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/player_plan/PlayerPlanTest.cpp" \
    "$root/Core/Player/Domain/PipelinePlan.cpp" "$root/Core/Player/Domain/AudioChain.cpp" \
    "$root/Core/Player/Domain/HardwareProbe.cpp"
status=0
"$out" || status=$?
rm -f "$out"
exit $status
