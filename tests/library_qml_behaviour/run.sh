#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QML behaviour checks of the library component (LIB-02 remorse, LIB-06 overlay, DL-04
# badges) with Silica from the SDK container, minimal platform, fake models.
# Skipped where Qt5Quick or Sailfish.Silica is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Gui Qt5Qml Qt5Quick 2>/dev/null; then echo "library qml behaviour tests: SKIPPED (no Qt5Quick)"; exit 0; fi
if [ ! -d /usr/lib64/qt5/qml/Sailfish/Silica ] && [ ! -d /usr/lib/qt5/qml/Sailfish/Silica ]; then
    echo "library qml behaviour tests: SKIPPED (no Sailfish.Silica)"; exit 0
fi
out=${TMPDIR:-/tmp}/sailtube-qml-behaviour-test.$$
mkdir -p "$out"
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" -I"$root" "$root/tests/library_qml_behaviour/Fakes.h" -o "$out/moc_fakes.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -I"$root" $(pkg-config --cflags Qt5Gui Qt5Qml Qt5Quick) -o "$out/t" \
    "$root/tests/library_qml_behaviour/BehaviourTest.cpp" "$out/moc_fakes.cpp" \
    $(pkg-config --libs Qt5Gui Qt5Qml Qt5Quick)
set +e
QT_QPA_PLATFORM=minimal "$out/t" "$root/tests/library_qml_behaviour" > "$out/log" 2>&1
rc=$?
set -e
# The container has no session bus, so Silica's hint counters cannot save to dconf.
grep -v -e 'QML debugging' -e 'dconf-WARNING' -e '^$' "$out/log" || true
rm -rf "$out"
exit $rc
