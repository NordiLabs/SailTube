#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Comments (DSC-16): parsers, key join, budgets, reveal window, auto-load, inline errors, sort, timing (NFR-PERF-08 host side).
SUITE=catalog_comments
MOC="Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/CommentsService.h Clients/Sailfish/Presentation/CommentsModel.h"
SOURCES="tests/catalog_comments/CommentsTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/Comments.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/CommentsService.cpp Clients/Sailfish/Presentation/CommentsModel.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
