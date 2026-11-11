#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Runtime QML tests of catalog pages (the real QML over fake models): a click on a channel row
# opens the video with the row's hints after tapIntent.opened, and a playlist row pushes the
# preview. Silica from the SDK container, minimal platform; skipped where it is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Gui Qt5Qml Qt5Quick 2>/dev/null; then echo "catalog qml tests: SKIPPED (no Qt5Quick)"; exit 0; fi
if [ ! -d /usr/lib64/qt5/qml/Sailfish/Silica ] && [ ! -d /usr/lib/qt5/qml/Sailfish/Silica ]; then
    echo "catalog qml tests: SKIPPED (no Sailfish.Silica)"; exit 0
fi
out=${TMPDIR:-/tmp}/sailtube-catalog-qml.$$
mkdir -p "$out"
trap 'rm -rf "$out"' EXIT
moc=$(pkg-config --variable=host_bins Qt5Core 2>/dev/null)/moc
[ -x "$moc" ] || moc=moc
cp "$root/tests/catalog_qml/ChannelPageClickTest.cpp" "$out/"
"$moc" "$out/ChannelPageClickTest.cpp" -o "$out/ChannelPageClickTest.moc"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/TapIntent.h" -o "$out/moc_tap.cpp"
"$moc" -I"$root" "$root/Clients/Sailfish/Presentation/CatalogText.h" -o "$out/moc_text.cpp"
"${CXX:-c++}" -std=c++11 -fPIC -I"$root" -I"$out" $(pkg-config --cflags Qt5Gui Qt5Qml Qt5Quick | sed "s/-I/-isystem /g") -o "$out/t" \
    "$out/ChannelPageClickTest.cpp" "$root/Clients/Sailfish/Presentation/TapIntent.cpp" "$root/Clients/Sailfish/Presentation/CatalogText.cpp" \
    "$root/Core/Catalog/Domain/LocalizedText.cpp" "$root/Core/Catalog/Domain/TextParse.cpp" "$out/moc_tap.cpp" "$out/moc_text.cpp" \
    $(pkg-config --libs Qt5Gui Qt5Qml Qt5Quick)
QT_QPA_PLATFORM=minimal "$out/t" "$root/Clients/Sailfish/qml"
