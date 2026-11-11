#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# PLT-03: one settings file. A static scan (every QSettings in Core/ and Clients/ names its file)
# and the one-time move of the keys an older build wrote through a default QSettings.
python3 -I "$(dirname "$0")/scan.py" "$(cd "$(dirname "$0")/../.." && pwd)" || exit 1
SUITE=catalog_settings
MOC=""
SOURCES="tests/catalog_settings/SettingsTest.cpp Clients/Sailfish/Presentation/SettingsFile.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
