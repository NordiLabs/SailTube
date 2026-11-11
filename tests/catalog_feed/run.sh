#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog feed and home tests (DSC-04/05/06/07): RSS, schedule, merge, cache, tiers, depth, live.
SUITE=catalog_feed
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/FeedService.h Core/Catalog/Infrastructure/HomeService.h Core/Catalog/Infrastructure/SearchService.h Clients/Sailfish/Presentation/FeedModel.h Clients/Sailfish/Presentation/HomeSettings.h Clients/Sailfish/Presentation/ContentFilters.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp tests/catalog_feed/FeedTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Domain/FeedSchedule.cpp Core/Catalog/Domain/LocalizedText.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/RssParser.cpp Core/Catalog/Infrastructure/FeedCacheStore.cpp Core/Catalog/Infrastructure/FeedService.cpp \
Core/Catalog/Infrastructure/HomeService.cpp Core/Catalog/Infrastructure/SearchService.cpp Core/Catalog/Domain/SearchFilter.cpp \
Clients/Sailfish/Presentation/FeedModel.cpp Clients/Sailfish/Presentation/HomeSettings.cpp Clients/Sailfish/Presentation/ContentFilters.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
