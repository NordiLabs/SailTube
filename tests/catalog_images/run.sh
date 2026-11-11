#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Artwork cache for QML (DSC-19): URL policy, host allow-list, avatar 7-day freshness, the
# shared 64 MB bound, no cookies; end to end against a loopback server (no outside network).
SUITE=catalog_images
MOC="Core/Catalog/Infrastructure/HttpTransport.h"
SOURCES="tests/catalog_images/ImagesTest.cpp Clients/Sailfish/Presentation/ImageCacheFactory.cpp Core/Catalog/Domain/Images.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp"
QT_MODULES="Qt5Core Qt5Network Qt5Qml"
. "$(dirname "$0")/../catalog_common/compile.sh"
