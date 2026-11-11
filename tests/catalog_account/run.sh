#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# ACC-11: signed-in recommended home, Watch later and Liked through SignedCatalog over a stand-in
# for the account component's SignedCatalogTransport (derived signed answers; no account is used).
SUITE=catalog_account
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/ShortsFeedService.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/HomeService.h Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/PlaylistService.h \
Clients/Sailfish/Presentation/PlaylistModel.h Clients/Sailfish/Presentation/SignedCatalog.h \
Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h Clients/Sailfish/Presentation/ChannelModel.h Clients/Sailfish/Presentation/WatchedRows.h \
Core/Catalog/Infrastructure/ShortDetailService.h Core/Catalog/Infrastructure/DislikeService.h Clients/Sailfish/Presentation/DislikeInfo.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp Core/Catalog/Infrastructure/ShortsFeedService.cpp tests/catalog_account/AccountTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/HomeService.cpp Core/Catalog/Infrastructure/SearchService.cpp Core/Catalog/Domain/SearchFilter.cpp Core/Catalog/Infrastructure/PlaylistService.cpp \
Clients/Sailfish/Presentation/PlaylistModel.cpp Clients/Sailfish/Presentation/SignedCatalog.cpp \
Core/Catalog/Domain/ChannelReference.cpp Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/LocalizedText.cpp \
Core/Catalog/Infrastructure/ChannelService.cpp Core/Catalog/Infrastructure/ShortsStore.cpp \
Clients/Sailfish/Presentation/ChannelModel.cpp Clients/Sailfish/Presentation/WatchedRows.cpp \
Core/Catalog/Infrastructure/ShortDetailService.cpp Core/Catalog/Domain/ShortDetail.cpp Core/Catalog/Infrastructure/DislikeService.cpp Clients/Sailfish/Presentation/DislikeInfo.cpp Clients/Sailfish/Presentation/SettingsFile.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
