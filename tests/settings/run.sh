#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Settings invariants (NFR-TST-03; SET-01, SET-02, SET-03). Needs Qt5Core and Qt5Qml.
# Second pass: the same test built against a table with one default changed must fail.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Qml 2>/dev/null; then echo "settings tests: SKIPPED (no Qt5Qml)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-settings.$$
mkdir -p "$out"; trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/AppSettings.h" -o "$out/moc_settings.cpp"
# AppSettings' PO-token settings (RES-14, FULL) clamp through the resolver's policy.
potoken=""
[ -f "$root/Core/Resolver/Domain/PoTokenPolicy.cpp" ] && potoken="$root/Core/Resolver/Domain/PoTokenPolicy.cpp"
build() {  # build <table.cpp> <binary>
    "${CXX:-c++}" -std=c++11 -fPIC -Wall -I"$root" -DSAILTUBE_ROOT="\"$root\"" $(pkg-config --cflags Qt5Qml) \
        -o "$2" "$root/tests/settings/SettingsTest.cpp" "$root/Clients/Sailfish/Presentation/AppSettings.cpp" \
        "$root/Core/Player/Domain/SponsorSkip.cpp" \
        $potoken \
        "$1" "$out/moc_settings.cpp" $(pkg-config --libs Qt5Qml)
}
build "$root/Core/Settings/Domain/SettingsDefaults.cpp" "$out/t"
"$out/t"
# SET-03: changing one table value in a scratch build makes the invariant test fail.
mkdir -p "$out/Core/Settings/Domain"
cp "$root/Core/Settings/Domain/SettingsDefaults.h" "$out/Core/Settings/Domain/"
sed 's/NUMBER("history\/cap", "500"/NUMBER("history\/cap", "600"/' "$root/Core/Settings/Domain/SettingsDefaults.cpp" \
    > "$out/Core/Settings/Domain/SettingsDefaults.cpp"
cmp -s "$root/Core/Settings/Domain/SettingsDefaults.cpp" "$out/Core/Settings/Domain/SettingsDefaults.cpp" && { echo "FAIL mutation did not apply"; exit 1; }
build "$out/Core/Settings/Domain/SettingsDefaults.cpp" "$out/mutant"
if "$out/mutant" >"$out/mutant.log" 2>&1; then echo "FAIL a changed table default went unnoticed"; exit 1; fi
grep -q 'history/cap: UI literal 500, table 600' "$out/mutant.log" || { echo "FAIL mutant failed for another reason"; cat "$out/mutant.log"; exit 1; }
echo "settings: mutated table default detected (SET-03)"
