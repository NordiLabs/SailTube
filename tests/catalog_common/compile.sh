#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Shared build-and-run step for the catalog suites (sourced by tests/<suite>/run.sh).
#
#   SUITE=name MOC="headers..." SOURCES="sources..." QT_MODULES="Qt5Core Qt5Network" . compile.sh
#
# Paths are relative to the repository root. Fixtures in tests/fixtures/catalog/*.gz are
# decompressed into a private directory exported as SAILTUBE_CATALOG_FIXTURES. Skips (exit 0)
# where the Qt development files are absent (outside the SDK/native container).
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
QT_MODULES=${QT_MODULES:-"Qt5Core Qt5Network"}
if ! pkg-config --exists $QT_MODULES 2>/dev/null; then echo "$SUITE: SKIPPED (no $QT_MODULES)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-$SUITE.$$
mkdir -p "$out/fixtures"
trap 'rm -rf "$out"' EXIT
for f in "$root"/tests/fixtures/catalog/*.gz; do
    [ -f "$f" ] || continue
    gzip -dc "$f" > "$out/fixtures/$(basename "$f" .gz)"
done
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
mocs=""
i=0
for h in $MOC; do
    i=$((i + 1))
    "$moc" -I"$root" "$root/$h" -o "$out/moc_$i.cpp"
    mocs="$mocs $out/moc_$i.cpp"
done
srcs=""
for s in $SOURCES; do srcs="$srcs $root/$s"; done
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$root" $(pkg-config --cflags $QT_MODULES | sed "s/-I/-isystem /g") -o "$out/t" $srcs $mocs \
    $(pkg-config --libs $QT_MODULES) -lpthread
SAILTUBE_CATALOG_FIXTURES="$out/fixtures" "$out/t"
