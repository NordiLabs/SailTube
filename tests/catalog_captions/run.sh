#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Captions (SEL-06), SponsorBlock fetch (SB-01) and Return YouTube Dislike (RYD-01).
SUITE=catalog_captions
MOC="Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h \
Core/Catalog/Infrastructure/CaptionService.h Core/Catalog/Infrastructure/SponsorBlockService.h Core/Catalog/Infrastructure/DislikeService.h \
Clients/Sailfish/Presentation/CaptionTracksModel.h Clients/Sailfish/Presentation/CaptionCues.h \
Clients/Sailfish/Presentation/SponsorSegmentsModel.h Clients/Sailfish/Presentation/DislikeInfo.h"
SOURCES="tests/catalog_captions/CaptionsTest.cpp Core/Catalog/Domain/Captions.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/SponsorSegments.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp \
Core/Catalog/Infrastructure/CaptionService.cpp Core/Catalog/Infrastructure/SponsorBlockService.cpp Core/Catalog/Infrastructure/DislikeService.cpp \
Clients/Sailfish/Presentation/CaptionTracksModel.cpp Clients/Sailfish/Presentation/SettingsFile.cpp Clients/Sailfish/Presentation/CaptionCues.cpp \
Clients/Sailfish/Presentation/SponsorSegmentsModel.cpp Clients/Sailfish/Presentation/DislikeInfo.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
