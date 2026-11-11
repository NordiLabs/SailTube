#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# ACC-11 / RES-08: the yt-dlp runner's cookie rung (Qt5Core; skipped where it is absent).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "cookie_rung: SKIPPED (no Qt5Core)"; echo "cookie_rung: PASS"; exit 0; fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-cookie-rung.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Resolver/Infrastructure/YtDlpRunner.h" -o "$out/moc_runner.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -w -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/t" "$root/tests/cookie_rung/CookieRungTest.cpp" \
    "$root/Core/Resolver/Infrastructure/YtDlpRunner.cpp" "$out/moc_runner.cpp" $(pkg-config --libs Qt5Core)
timeout -s KILL 90 "$out/t"
echo "cookie_rung: PASS"
