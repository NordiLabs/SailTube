# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "comments": owned by the catalog component. Paths relative to the repository root.
# DSC-16 comments (read-only) and the NFR-PERF-08 host timing.
HEADERS += $$PWD/../Domain/Comments.h \
           $$PWD/../Infrastructure/CommentsService.h \
           $$PWD/../../../Clients/Sailfish/Presentation/CommentsModel.h
SOURCES += $$PWD/../Domain/Comments.cpp \
           $$PWD/../Infrastructure/CommentsService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/CommentsModel.cpp
