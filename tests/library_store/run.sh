#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QtCore + QtSql tests of the SQLite library store. Runs in the SDK/native
# container; skipped where Qt5Core or Qt5Sql development files are absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core 2>/dev/null; then echo "library store tests: SKIPPED (no Qt5Core)"; exit 0; fi
if ! pkg-config --exists Qt5Sql 2>/dev/null; then echo "library store tests: SKIPPED (no Qt5Sql)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-library-store-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" \
    $(pkg-config --cflags Qt5Core Qt5Sql) -o "$out/t" \
    "$root/tests/library_store/LibraryStoreTest.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_store.cpp" \
    $(pkg-config --libs Qt5Core Qt5Sql)
LIBRARY_STORE_TMP="$out/data" "$out/t"
rm -rf "$out"
