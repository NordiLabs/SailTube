#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# The Shorts section's feed (ShortsFeedService, ShortsModel): parsers on recorded answers,
# subscriptions then popular fill, paging, the 10-minute cache, the signed sequence, setCurrent.
SUITE=catalog_shorts
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h Core/Catalog/Infrastructure/SearchService.h \
Core/Catalog/Infrastructure/ShortsFeedService.h Clients/Sailfish/Presentation/ShortsModel.h Clients/Sailfish/Presentation/TapIntent.h \
Core/Catalog/Infrastructure/ShortDetailService.h Core/Catalog/Infrastructure/DislikeService.h Clients/Sailfish/Presentation/DislikeInfo.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp tests/catalog_shorts/ShortsTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Domain/LocalizedText.cpp Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/ChannelReference.cpp Core/Catalog/Domain/SearchFilter.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/ChannelService.cpp Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/SearchService.cpp \
Core/Catalog/Infrastructure/ShortsFeedService.cpp Clients/Sailfish/Presentation/ShortsModel.cpp Clients/Sailfish/Presentation/TapIntent.cpp \
Core/Catalog/Infrastructure/ShortDetailService.cpp Core/Catalog/Domain/ShortDetail.cpp Core/Catalog/Infrastructure/DislikeService.cpp Clients/Sailfish/Presentation/DislikeInfo.cpp Clients/Sailfish/Presentation/SettingsFile.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
