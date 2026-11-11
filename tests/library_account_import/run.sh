#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# IMP-02 account import: page parsing, paging limits, identity headers, and the
# batch through ImportController/ImportJob into a real library store.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Sql 2>/dev/null; then echo "library account import tests: SKIPPED (no Qt5Sql)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-account-import-test.$$
rm -rf "$out"
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"$moc" -I"$root" "$root/Core/Library/Infrastructure/ImportJob.h" -o "$out/moc_job.cpp"
"$moc" -I"$root" "$root/Core/Library/Infrastructure/AccountImport.h" -o "$out/moc_account.cpp"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/ImportController.h" -o "$out/moc_controller.cpp"
qtflags=$(pkg-config --cflags Qt5Core Qt5Sql | sed 's/-I/-isystem /g')
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtflags -o "$out/t" \
    "$root/tests/library_account_import/AccountImportTest.cpp" \
    "$root/Core/Library/Infrastructure/AccountImport.cpp" \
    "$root/Clients/Sailfish/Presentation/ImportController.cpp" \
    "$root/Core/Library/Infrastructure/ZipReader.cpp" \
    "$root/Core/Library/Infrastructure/NewPipeImporter.cpp" \
    "$root/Core/Library/Infrastructure/TakeoutImporter.cpp" \
    "$root/Core/Library/Infrastructure/SailTubeBackup.cpp" \
    "$root/Core/Library/Infrastructure/ImportJob.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_store.cpp" "$out/moc_job.cpp" "$out/moc_account.cpp" "$out/moc_controller.cpp" \
    $(pkg-config --libs Qt5Core Qt5Sql) -lz
TEST_DIR="$out" "$out/t"
rm -rf "$out"
