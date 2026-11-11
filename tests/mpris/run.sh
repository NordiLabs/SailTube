#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# MPR-01 / PLT-10: the values MediaControls.qml publishes on the session bus through Amber.Mpris
# (mpris:length and Position in microseconds) and the units Seek / SetPosition hand back.
# Needs Qt5Qml, Qt5DBus, the Amber.Mpris QML plugin and dbus-run-session (the Sailfish target
# has them); skipped elsewhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Core Qt5Qml Qt5DBus 2>/dev/null; then echo "mpris tests: SKIPPED (no Qt5Qml / Qt5DBus)"; exit 0; fi
plugin=""
for d in /usr/lib64/qt5/qml /usr/lib/qt5/qml; do [ -f "$d/Amber/Mpris/qmldir" ] && plugin="$d"; done
if [ -z "$plugin" ]; then echo "mpris tests: SKIPPED (no Amber.Mpris QML plugin)"; exit 0; fi
if ! command -v dbus-run-session >/dev/null 2>&1; then echo "mpris tests: SKIPPED (no dbus-run-session)"; exit 0; fi
out=${TMPDIR:-/tmp}/sailtube-mpris-test.$$
mkdir -p "$out"; trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" "$root/tests/mpris/MprisTest.cpp" -o "$out/MprisTest.moc"
qtinc=""
for d in $(pkg-config --cflags-only-I Qt5Core Qt5Qml Qt5DBus 2>/dev/null); do qtinc="$qtinc -isystem ${d#-I}"; done
"${CXX:-c++}" -std=c++11 -fPIC -Wall -Wextra -I"$out" $qtinc -o "$out/t" "$root/tests/mpris/MprisTest.cpp" \
    $(pkg-config --libs Qt5Core Qt5Qml Qt5DBus)
QT_QPA_PLATFORM=minimal dbus-run-session -- "$out/t" "$root/Clients/Sailfish/qml/components/MediaControls.qml"
