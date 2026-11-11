#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Qt-free domain tests; needs only a C++11 compiler. Run from anywhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${TMPDIR:-/tmp}/sailtube-domain-test.$$
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out" "$root/tests/domain/DomainTest.cpp" \
    "$root/Core/Media/Domain/StreamSelection.cpp" "$root/Core/Media/Domain/AudioTrackPolicy.cpp" \
    "$root/Core/Media/Domain/LivePolicy.cpp" "$root/Core/Media/Domain/DashManifest.cpp"
"$out"
rm -f "$out"
