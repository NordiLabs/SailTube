#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# DL-07 sidecar source over the catalog's services.
# Uses the catalog suites' build step, fake transport and recorded fixtures.
SUITE=library_catalog_glue
MOC="Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h \
Core/Catalog/Infrastructure/InnerTubeClient.h Core/Catalog/Infrastructure/SponsorBlockService.h Core/Catalog/Infrastructure/CaptionService.h \
Core/Catalog/Infrastructure/CaptionTrackService.h \
Clients/Sailfish/Presentation/CaptionTracksModel.h Clients/Sailfish/Composition/LibraryCatalogGlue.h"
SOURCES="tests/library_catalog_glue/GlueTest.cpp \
Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Captions.cpp Core/Catalog/Domain/SponsorSegments.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/SponsorBlockService.cpp Core/Catalog/Infrastructure/CaptionService.cpp \
Core/Catalog/Infrastructure/CaptionTrackService.cpp \
Clients/Sailfish/Presentation/CaptionTracksModel.cpp Clients/Sailfish/Presentation/SettingsFile.cpp Clients/Sailfish/Composition/LibraryCatalogGlue.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
