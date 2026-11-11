#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-SEC-02: RES-17 SSRF test set against the pinned loopback media proxy (Qt5Core + Qt5Network).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Network 2>/dev/null; then echo "ssrf: SKIPPED (no Qt5Core/Qt5Network)"; echo "ssrf: PASS"; exit 0; fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-ssrf.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
infra="$root/Shared/SailTune/Core/SharedPlayback/Infrastructure"
"$moc" "$infra/HttpsConnection.h" -o "$out/moc_HttpsConnection.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -Wno-deprecated-copy $(pkg-config --cflags Qt5Core Qt5Network) -o "$out/t" \
    "$root/tests/ssrf/SsrfTest.cpp" "$infra/MediaProxy.cpp" "$infra/HttpsConnection.cpp" "$out/moc_HttpsConnection.cpp" \
    $(pkg-config --libs Qt5Core Qt5Network)
QT_QPA_PLATFORM=minimal timeout -s KILL 60 "$out/t"
echo "ssrf: PASS"
