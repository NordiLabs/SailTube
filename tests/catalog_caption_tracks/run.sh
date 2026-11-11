#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# The caption track list of a streamed video (SEL-06): the track-list request and its in-memory
# visitor id, the list service (manual + automatic tracks, none, refused, failing), and the
# hand-over to the playing video's models up to playback's caption seam.
SUITE=catalog_caption_tracks
MOC="Clients/Sailfish/Presentation/ChannelAvatars.h Clients/Sailfish/Presentation/LargeThumbnails.h Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/WatchNextService.h Core/Catalog/Infrastructure/CaptionService.h Core/Catalog/Infrastructure/CaptionTrackService.h \
Clients/Sailfish/Presentation/WatchInfo.h Clients/Sailfish/Presentation/RelatedModel.h Clients/Sailfish/Presentation/ChapterModel.h \
Clients/Sailfish/Presentation/CaptionTracksModel.h Clients/Sailfish/Presentation/CaptionCues.h Clients/Sailfish/Presentation/WatchCaptions.h \
Clients/Sailfish/Presentation/PlaybackCaptions.h"
SOURCES="Clients/Sailfish/Presentation/ChannelAvatars.cpp Clients/Sailfish/Presentation/LargeThumbnails.cpp tests/catalog_caption_tracks/CaptionTracksTest.cpp Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp \
Core/Catalog/Domain/Chapters.cpp Core/Catalog/Domain/Description.cpp Core/Catalog/Domain/Captions.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/WatchNextService.cpp Core/Catalog/Infrastructure/CaptionService.cpp Core/Catalog/Infrastructure/CaptionTrackService.cpp \
Clients/Sailfish/Presentation/WatchInfo.cpp Clients/Sailfish/Presentation/RelatedModel.cpp Clients/Sailfish/Presentation/ChapterModel.cpp \
Clients/Sailfish/Presentation/CaptionTracksModel.cpp Clients/Sailfish/Presentation/SettingsFile.cpp Clients/Sailfish/Presentation/CaptionCues.cpp \
Clients/Sailfish/Presentation/WatchCaptions.cpp Clients/Sailfish/Presentation/PlaybackCaptions.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
