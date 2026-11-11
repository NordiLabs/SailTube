# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "feed": owned by the catalog component. Paths relative to the repository root.
# Subscriptions feed (DSC-05/06/07), its cache, and Home/Recommended with the landing page (DSC-04).
HEADERS += $$PWD/../Domain/Subscriptions.h \
           $$PWD/../Domain/FeedSchedule.h \
           $$PWD/../Infrastructure/RssParser.h \
           $$PWD/../Infrastructure/FeedCacheStore.h \
           $$PWD/../Infrastructure/FeedService.h \
           $$PWD/../Infrastructure/HomeService.h \
           $$PWD/../Infrastructure/ShortsFeedService.h \
           $$PWD/../Infrastructure/ShortDetailService.h \
           $$PWD/../Domain/ShortDetail.h \
           $$PWD/../../../Clients/Sailfish/Presentation/ShortsModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/FeedModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/ContentFilters.h \
           $$PWD/../../../Clients/Sailfish/Presentation/HomeSettings.h
SOURCES += $$PWD/../Domain/FeedSchedule.cpp \
           $$PWD/../Infrastructure/RssParser.cpp \
           $$PWD/../Infrastructure/FeedCacheStore.cpp \
           $$PWD/../Infrastructure/FeedService.cpp \
           $$PWD/../Infrastructure/ShortsFeedService.cpp \
           $$PWD/../Infrastructure/ShortDetailService.cpp \
           $$PWD/../Domain/ShortDetail.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/ShortsModel.cpp \
           $$PWD/../Infrastructure/HomeService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/FeedModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/ContentFilters.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/HomeSettings.cpp
