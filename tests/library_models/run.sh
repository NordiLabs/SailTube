#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Presentation models over a real library store (QtCore, QtSql, QtQml). Runs in
# the SDK/native container; skipped where the Qt development files are absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Sql Qt5Qml 2>/dev/null; then echo "library models tests: SKIPPED (no Qt5Sql/Qt5Qml)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-models-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Core/Library/Infrastructure/LibraryStore.h" -o "$out/moc_store.cpp"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/LibraryModels.h" -o "$out/moc_models.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags Qt5Core Qt5Sql Qt5Qml) -o "$out/t" \
    "$root/tests/library_models/ModelsTest.cpp" \
    "$root/Clients/Sailfish/Presentation/LibraryModels.cpp" \
    "$root/Core/Library/Infrastructure/LibraryStore.cpp" \
    "$root/Core/Library/Domain/LibraryPolicy.cpp" \
    "$out/moc_store.cpp" "$out/moc_models.cpp" \
    $(pkg-config --libs Qt5Core Qt5Sql Qt5Qml)
TEST_DIR="$out" "$out/t"
rm -rf "$out"
