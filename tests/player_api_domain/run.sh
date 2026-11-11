#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-TST-06: Qt-free tests of the playback component's domain functions (Core/Media/Domain,
# Core/Player/Domain). Needs only a C++11 compiler. POSIX sh, run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-api-domain.$$
trap 'rm -f "$out"' EXIT
md="$root/Core/Media/Domain"
pd="$root/Core/Player/Domain"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/player_api_domain/PlayerApiDomainTest.cpp" \
    "$md/StreamSelection.cpp" "$md/AudioTrackPolicy.cpp" "$md/LivePolicy.cpp" \
    "$pd/AudioChain.cpp" "$pd/AutoplayPolicy.cpp" "$pd/BufferingPolicy.cpp" "$pd/PipelinePlan.cpp" \
    "$pd/PositionPolicy.cpp" "$pd/PrerollBarrier.cpp" "$pd/SeekPolicy.cpp" "$pd/SessionArbiter.cpp"
"$out"
