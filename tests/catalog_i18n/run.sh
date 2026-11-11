#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Locale tables and per-locale fixtures (I18N-02, I18N-04): every recorded locale parses with
# no dropped rows, and results do not depend on the process locale.
SUITE=catalog_i18n
MOC="Core/Catalog/Infrastructure/HttpTransport.h"
SOURCES="tests/catalog_i18n/I18nTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/LocalizedText.cpp Core/Catalog/Domain/Images.cpp \
Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/RendererParser.cpp Core/Catalog/Infrastructure/HttpTransport.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
