# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "watch": owned by the catalog component. Paths relative to the repository root.
# Watch page data: related (DSC-13), video info and description (DSC-14), chapters (DSC-15),
# structured player fields (I18N-02).
HEADERS += $$PWD/../Domain/Chapters.h \
           $$PWD/../Domain/Description.h \
           $$PWD/../Domain/WatchCache.h \
           $$PWD/../Infrastructure/WatchNextService.h \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchInfo.h \
           $$PWD/../../../Clients/Sailfish/Presentation/RelatedModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/ChapterModel.h
SOURCES += $$PWD/../Domain/Chapters.cpp \
           $$PWD/../Domain/Description.cpp \
           $$PWD/../Infrastructure/WatchNextService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/WatchInfo.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/RelatedModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/ChapterModel.cpp
