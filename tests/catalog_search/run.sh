#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog search tests (sp encoder, search/suggest caches, paging, tiers, models, timing).
SUITE=catalog_search
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Clients/Sailfish/Presentation/WatchedRows.h Clients/Sailfish/Presentation/TapIntent.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/SuggestService.h \
Clients/Sailfish/Presentation/DiscoverySearchModel.h Clients/Sailfish/Presentation/SuggestionModel.h \
Core/Resolver/Infrastructure/YtDlpRunner.h Core/Resolver/Infrastructure/YtDlpVideoService.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp Clients/Sailfish/Presentation/WatchedRows.cpp Clients/Sailfish/Presentation/TapIntent.cpp tests/catalog_search/SearchTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Domain/SearchFilter.cpp Core/Catalog/Domain/SearchCache.cpp Core/Catalog/Domain/FeedSchedule.cpp Core/Catalog/Infrastructure/FeedCacheStore.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/SearchService.cpp Core/Catalog/Infrastructure/SuggestService.cpp Core/Catalog/Infrastructure/YtDlpSearchFallback.cpp \
Clients/Sailfish/Presentation/DiscoverySearchModel.cpp Clients/Sailfish/Presentation/SuggestionModel.cpp \
Core/Resolver/Infrastructure/YtDlpJson.cpp Core/Resolver/Infrastructure/YtDlpRunner.cpp Core/Resolver/Infrastructure/YtDlpVideoService.cpp \
Core/Media/Domain/StreamSelection.cpp Core/Media/Domain/AudioTrackPolicy.cpp Core/Media/Domain/LivePolicy.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
