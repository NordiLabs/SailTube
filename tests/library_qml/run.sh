#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# QML compile check of the library component's pages and components (Silica from the
# SDK container, minimal platform). Skipped where Qt5Qml or Sailfish.Silica is absent.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
if ! pkg-config --exists Qt5Gui Qt5Qml 2>/dev/null; then echo "library qml tests: SKIPPED (no Qt5Qml)"; exit 0; fi
if [ ! -d /usr/lib64/qt5/qml/Sailfish/Silica ] && [ ! -d /usr/lib/qt5/qml/Sailfish/Silica ]; then
    echo "library qml tests: SKIPPED (no Sailfish.Silica)"; exit 0
fi
out=${TMPDIR:-/tmp}/sailtube-qml-test.$$
mkdir -p "$out"
"${CXX:-c++}" -std=c++11 -fPIC -I"$root" $(pkg-config --cflags Qt5Gui Qt5Qml) -o "$out/check" \
    "$root/tests/library_qml/QmlCompileCheck.cpp" $(pkg-config --libs Qt5Gui Qt5Qml)
cd "$root/Clients/Sailfish/qml"
set +e
QT_QPA_PLATFORM=minimal "$out/check" \
    pages/LibraryPage.qml pages/SubscriptionsPage.qml pages/PlaylistsPage.qml pages/PlaylistPage.qml \
    pages/PlaylistNameDialog.qml pages/AddToPlaylistDialog.qml pages/HistoryPage.qml \
    pages/ImportExportPage.qml pages/DownloadFolderPage.qml pages/DownloadsPage.qml \
    pages/DownloadQualityDialog.qml pages/DownloadSettingsSection.qml sections/SearchSection.qml sections/LibrarySection.qml \
    pages/SettingsPage.qml pages/AccountPage.qml pages/PasteSessionDialog.qml components/WatchOverlay.qml components/DownloadBadge.qml \
    components/ResumeOffer.qml components/VideoMenuItems.qml components/VideoListItem.qml \
    components/DownloadKeepAlive.qml components/DownloadNotifier.qml harbour-sailtube.qml > "$out/log" 2>&1
rc=$?
set -e
grep -v '^OK ' "$out/log" | grep -v 'QML debugging' || true
rm -rf "$out"
if [ $rc -ne 0 ]; then echo "library qml tests: FAIL ($rc files)"; exit 1; fi
echo "library qml tests: PASS (0 failures)"
