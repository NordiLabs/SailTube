#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# The catalog side of the library hooks over a real library store: feed subscriptions and
# write-back (DSC-05/LIB-01), hide watched (LIB-07), remote playlist mirrors (LIB-03) and
# import handles (IMP-01).
SUITE=catalog_library
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/ShortsFeedService.h Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/FeedService.h Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h Core/Catalog/Infrastructure/PlaylistService.h \
Core/Library/Infrastructure/LibraryStore.h Clients/Sailfish/Presentation/LibraryModels.h \
Clients/Sailfish/Presentation/WatchedRows.h Clients/Sailfish/Presentation/CatalogLibraryBridge.h Clients/Sailfish/Presentation/ChannelModel.h \
Core/Catalog/Infrastructure/ShortDetailService.h Core/Catalog/Infrastructure/DislikeService.h Clients/Sailfish/Presentation/DislikeInfo.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp Core/Catalog/Infrastructure/ShortsFeedService.cpp Core/Catalog/Infrastructure/SearchService.cpp Core/Catalog/Domain/SearchFilter.cpp tests/catalog_library/LibraryBridgeTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp Core/Catalog/Domain/LocalizedText.cpp \
Core/Catalog/Domain/FeedSchedule.cpp Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/ChannelReference.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/RssParser.cpp Core/Catalog/Infrastructure/FeedCacheStore.cpp Core/Catalog/Infrastructure/FeedService.cpp \
Core/Catalog/Infrastructure/ChannelService.cpp Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/PlaylistService.cpp \
Core/Library/Infrastructure/LibraryStore.cpp Core/Library/Domain/LibraryPolicy.cpp Clients/Sailfish/Presentation/LibraryModels.cpp \
Clients/Sailfish/Presentation/WatchedRows.cpp Clients/Sailfish/Presentation/CatalogLibraryBridge.cpp Clients/Sailfish/Presentation/ChannelModel.cpp \
Core/Catalog/Infrastructure/ShortDetailService.cpp Core/Catalog/Domain/ShortDetail.cpp Core/Catalog/Infrastructure/DislikeService.cpp Clients/Sailfish/Presentation/DislikeInfo.cpp Clients/Sailfish/Presentation/SettingsFile.cpp"
QT_MODULES="Qt5Core Qt5Network Qt5Sql Qt5Qml"
. "$(dirname "$0")/../catalog_common/compile.sh"
