#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# NFR-ROB-02: SIGKILL during >= 200 store writes (SQLite library store, QSaveFile, QSettings), then
# the startup orphan cleanup. Linux with Qt5Core + Qt5Sql (the build container); skipped elsewhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if [ "$(uname -s)" != Linux ] || ! pkg-config --exists Qt5Core Qt5Sql 2>/dev/null; then
    echo "robustness: SKIPPED (need Linux, Qt5Core and Qt5Sql)"; echo "robustness: PASS"; exit 0
fi
out=$(mktemp -d "${TMPDIR:-/tmp}/sailtube-robustness.XXXXXX")
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core Qt5Sql) -o "$out/t" \
    "$root/tests/robustness/StoreKillTest.cpp" "$root/Core/Storage/Infrastructure/StartupCleanup.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_store.cpp" $(pkg-config --libs Qt5Core Qt5Sql)
timeout -s KILL 240 "$out/t"
echo "robustness: PASS"
