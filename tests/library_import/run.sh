#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Imports and export (IMP-01, IMP-03, IMP-04) over a real library store: ZipReader,
# NewPipe/PipePipe, Takeout/OPML, SailTube backup, ImportJob. Fixtures are built at
# test time by make_fixtures.py (python3 sqlite3 + zipfile). Needs QtCore, QtSql
# (QSQLITE) and zlib; skipped where they are absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Sql 2>/dev/null; then echo "library import tests: SKIPPED (no Qt5Sql)"; exit 0; fi
if ! command -v python3 >/dev/null 2>&1; then echo "library import tests: SKIPPED (no python3)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-import-test.$$
rm -rf "$out"
mkdir -p "$out/fixtures" "$out/scratch"
python3 -I "$root/tests/library_import/make_fixtures.py" "$out/fixtures" >/dev/null
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"$moc" -I"$root" "$root/Core/Library/Infrastructure/ImportJob.h" -o "$out/moc_job.cpp"
# Qt headers as system headers: newer compilers warn inside Qt 5.6 itself.
qtflags=$(pkg-config --cflags Qt5Core Qt5Sql | sed 's/-I/-isystem /g')
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $qtflags -o "$out/t" \
    "$root/tests/library_import/LibraryImportTest.cpp" \
    "$root/Core/Library/Infrastructure/ZipReader.cpp" \
    "$root/Core/Library/Infrastructure/NewPipeImporter.cpp" \
    "$root/Core/Library/Infrastructure/TakeoutImporter.cpp" \
    "$root/Core/Library/Infrastructure/SailTubeBackup.cpp" \
    "$root/Core/Library/Infrastructure/ImportJob.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_store.cpp" "$out/moc_job.cpp" \
    $(pkg-config --libs Qt5Core Qt5Sql) -lz
(cd "$out/scratch" && "$out/t" "$out/fixtures" "$out/scratch")
rm -rf "$out"
