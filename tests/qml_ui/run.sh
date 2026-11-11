#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Headless Silica UI tests (PLT-06 page lifetime, shell, watch page, settings, RTL). Needs the
# Sailfish SDK target (Qt5Quick and Sailfish.Silica); skipped elsewhere.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Quick 2>/dev/null || [ ! -d /usr/lib64/qt5/qml/Sailfish/Silica ] && [ ! -d /usr/lib/qt5/qml/Sailfish/Silica ]; then
    echo "qml ui tests: SKIPPED (no Qt5Quick or Sailfish.Silica)"; exit 0
fi
out=${TMPDIR:-/tmp}/sailtube-qmlui.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
"$moc" "$root/tests/qml_ui/harness.cpp" -o "$out/harness.moc"
# The platform interface header (to set a phone-sized screen) lives in Qt's versioned include dir.
qpa=""
guiver=$(pkg-config --modversion Qt5Gui 2>/dev/null)
guiinc=$(pkg-config --variable=includedir Qt5Gui 2>/dev/null)/QtGui/$guiver
[ -f "$guiinc/QtGui/qpa/qwindowsysteminterface.h" ] && qpa="-DHARNESS_QPA -I$guiinc -I$guiinc/QtGui"
# Real text widths (the minimal platform has no fonts): FreeType and fontconfig, when present.
fonts=""; fontlibs=""
if pkg-config --exists freetype2 fontconfig 2>/dev/null; then
    fonts="-DHARNESS_FONTS $(pkg-config --cflags freetype2 fontconfig)"; fontlibs=$(pkg-config --libs freetype2 fontconfig)
fi
"${CXX:-c++}" -std=c++11 -fPIC -Wall -I"$out" $qpa $fonts $(pkg-config --cflags Qt5Quick Qt5Qml Qt5Gui) -o "$out/harness" \
    "$root/tests/qml_ui/harness.cpp" $(pkg-config --libs Qt5Quick Qt5Qml Qt5Gui) $fontlibs
lrelease=$(command -v lrelease || echo /usr/lib64/qt5/bin/lrelease)
locales="en es pt fr de it ru ja zh_CN ar"
for l in $locales; do
    "$lrelease" -silent "$root/translations/harbour-sailtube-$l.ts" -qm "$out/$l.qm" >/dev/null 2>&1 || true
done
fail=0
# UI_TESTS="tst_a.qml tst_b.qml" runs only those files.
for t in "$root"/tests/qml_ui/tst_*.qml; do
    [ -z "${UI_TESTS:-}" ] || case " $UI_TESTS " in *" $(basename "$t") "*) ;; *) continue ;; esac
    # "harness: screen=540x960" gives the test a phone-sized screen.
    screen=$(head -5 "$t" | sed -n 's/.*harness:.*screen=\([0-9]*x[0-9]*\).*/--screen \1/p')
    if head -5 "$t" | grep -q 'harness:.*qm=all'; then
        # Once per catalog: every translation meets the real layout (I18N-03).
        for l in $locales; do
            QT_QPA_PLATFORM=minimal HOME="$out" XDG_RUNTIME_DIR="$out" timeout 120 "$out/harness" "$root" "$t" --qm "$out/$l.qm" --locale "$l" $screen || fail=1
        done
        continue
    fi
    args="$screen"
    head -5 "$t" | grep -q 'harness: rtl' && args="$args --rtl"
    head -5 "$t" | grep -q 'harness:.*qm=ar' && args="$args --qm $out/ar.qm --locale ar"
    QT_QPA_PLATFORM=minimal HOME="$out" XDG_RUNTIME_DIR="$out" timeout 120 "$out/harness" "$root" "$t" $args || fail=1
done
[ "$fail" = 0 ] && echo "qml ui tests: all passed" || { echo "qml ui tests: FAILED"; exit 1; }
