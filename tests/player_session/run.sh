#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free playback-session policy tests; needs only a C++11 compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-player-session-test.$$
d="$root/Core/Player/Domain"
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/player_session/PlayerSessionTest.cpp" \
    "$d/SessionArbiter.cpp" "$d/RecoveryPolicy.cpp" "$d/TapGestures.cpp" "$d/AudioResourcePolicy.cpp" \
    "$d/AutoplayPolicy.cpp" "$d/MediaControlsPolicy.cpp" "$d/SponsorSkip.cpp" "$d/FreezePolicy.cpp"
status=0
"$out" || status=$?
rm -f "$out"
exit $status
