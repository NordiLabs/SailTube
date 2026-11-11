# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "captions": owned by the catalog component. Paths relative to the repository root.
# Captions (SEL-06), SponsorBlock fetch (SB-01) and Return YouTube Dislike (RYD-01).

HEADERS += $$PWD/../Domain/ExpiringCache.h \
           $$PWD/../Domain/Captions.h \
           $$PWD/../Domain/SponsorSegments.h \
           $$PWD/../Infrastructure/CaptionService.h \
           $$PWD/../Infrastructure/CaptionTrackService.h \
           $$PWD/../Infrastructure/SponsorBlockService.h \
           $$PWD/../Infrastructure/DislikeService.h \
           $$PWD/../../../Clients/Sailfish/Presentation/CaptionTracksModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/CaptionCues.h \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchCaptions.h \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchDislikes.h \
           $$PWD/../../../Clients/Sailfish/Presentation/SponsorSegmentsModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/DislikeInfo.h
SOURCES += $$PWD/../Domain/Captions.cpp \
           $$PWD/../Domain/SponsorSegments.cpp \
           $$PWD/../Infrastructure/CaptionService.cpp \
           $$PWD/../Infrastructure/CaptionTrackService.cpp \
           $$PWD/../Infrastructure/SponsorBlockService.cpp \
           $$PWD/../Infrastructure/DislikeService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/CaptionTracksModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/CaptionCues.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchCaptions.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchDislikes.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/SponsorSegmentsModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/DislikeInfo.cpp
