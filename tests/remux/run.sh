#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# DL-03 / E6 host side: GstRemuxer against the local GStreamer runtime (loaded at
# run time; no GStreamer headers needed). Skipped where Qt5Core is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "remux tests: SKIPPED (no Qt5Core)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-remux-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Downloads/Infrastructure/GstRemuxer.h" -o "$out/moc_remuxer.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core) -o "$out/t" \
    "$root/tests/remux/RemuxTest.cpp" \
    "$root/Core/Downloads/Infrastructure/GstRemuxer.cpp" \
    "$root/Core/Downloads/Domain/Mp4Layout.cpp" \
    "$out/moc_remuxer.cpp" \
    $(pkg-config --libs Qt5Core) -lpthread
"$out/t"
rm -rf "$out"
