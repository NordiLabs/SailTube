#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Memory trim policy and allocator calls (Core/Player/Domain/MemoryTrimPolicy.h, Infrastructure/MemoryTrim.cpp).
# Plain C++11, no Qt; the allocator calls are no-ops without glibc (the Mac host).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-memtrim.XXXXXX")
trap 'rm -rf "$out"' EXIT
"${CXX:-c++}" -std=c++11 -Wall -Wextra -I"$root" -o "$out/t" \
    "$root/tests/memtrim/MemTrimTest.cpp" "$root/Core/Player/Infrastructure/MemoryTrim.cpp"
"$out/t"
