# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "images": owned by the catalog component. Paths relative to the repository root.
# Artwork loading for QML (DSC-19): shared 64 MB disk cache, host allow-list, avatar policy.
QT += network qml
HEADERS += $$PWD/../../../Clients/Sailfish/Presentation/ImageCacheFactory.h
SOURCES += $$PWD/../../../Clients/Sailfish/Presentation/ImageCacheFactory.cpp
