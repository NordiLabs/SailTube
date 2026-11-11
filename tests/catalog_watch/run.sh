#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog watch-page tests (related paging, video info, description runs, chapters, structured
# player fields, cache/single-flight, valid-empty vs drift, presentation models, upcoming streams,
# Hide Shorts on related, the dislike estimate on the watch info).
SUITE=catalog_watch
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/WatchNextService.h \
Core/Catalog/Infrastructure/DislikeService.h Clients/Sailfish/Presentation/DislikeInfo.h Clients/Sailfish/Presentation/WatchDislikes.h Clients/Sailfish/Presentation/ContentFilters.h \
Clients/Sailfish/Presentation/WatchInfo.h Clients/Sailfish/Presentation/RelatedModel.h Clients/Sailfish/Presentation/ChapterModel.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp tests/catalog_watch/WatchTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Domain/Chapters.cpp Core/Catalog/Domain/Description.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/WatchNextService.cpp \
Core/Catalog/Infrastructure/DislikeService.cpp Core/Catalog/Domain/LocalizedText.cpp Clients/Sailfish/Presentation/DislikeInfo.cpp Clients/Sailfish/Presentation/SettingsFile.cpp \
Clients/Sailfish/Presentation/WatchDislikes.cpp Clients/Sailfish/Presentation/ContentFilters.cpp \
Clients/Sailfish/Presentation/WatchInfo.cpp Clients/Sailfish/Presentation/RelatedModel.cpp Clients/Sailfish/Presentation/ChapterModel.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
