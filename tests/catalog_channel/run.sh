#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Channels (DSC-12), the one Shorts rule and its stores (DSC-17), members-only rows (DSC-18),
# channel headers (DSC-03) and the remote playlist preview (LIB-03 / LNK-04).
SUITE=catalog_channel
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Clients/Sailfish/Presentation/WatchedRows.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h Core/Catalog/Infrastructure/PlaylistService.h \
Clients/Sailfish/Presentation/ChannelModel.h Clients/Sailfish/Presentation/PlaylistModel.h Clients/Sailfish/Presentation/QueueSeeder.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp Clients/Sailfish/Presentation/WatchedRows.cpp tests/catalog_channel/ChannelTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp Core/Catalog/Domain/LocalizedText.cpp \
Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/ChannelReference.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/ChannelService.cpp Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/PlaylistService.cpp \
Clients/Sailfish/Presentation/ChannelModel.cpp Clients/Sailfish/Presentation/PlaylistModel.cpp Clients/Sailfish/Presentation/QueueSeeder.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
