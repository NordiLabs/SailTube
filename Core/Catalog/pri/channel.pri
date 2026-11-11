# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "channel": owned by the catalog component. Paths relative to the repository root.
# Channels (DSC-12), the Shorts rule and its stores (DSC-17), members-only rows (DSC-18) and the
# remote playlist preview (LIB-03 / LNK-04).
HEADERS += $$PWD/../Domain/LruStore.h \
           $$PWD/../Domain/ShortsKnowledge.h \
           $$PWD/../Domain/ChannelReference.h \
           $$PWD/../Infrastructure/ChannelService.h \
           $$PWD/../Infrastructure/ShortsStore.h \
           $$PWD/../Infrastructure/PlaylistService.h \
           $$PWD/../../../Clients/Sailfish/Presentation/ChannelModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/PlaylistModel.h
SOURCES += $$PWD/../Domain/ShortsKnowledge.cpp \
           $$PWD/../Domain/ChannelReference.cpp \
           $$PWD/../Infrastructure/ChannelService.cpp \
           $$PWD/../Infrastructure/ShortsStore.cpp \
           $$PWD/../Infrastructure/PlaylistService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/ChannelModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/PlaylistModel.cpp
