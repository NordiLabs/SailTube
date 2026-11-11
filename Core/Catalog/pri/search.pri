# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog area "search": owned by the catalog component. Paths relative to the repository root.
# Search (DSC-08..DSC-10), suggestions and history (DSC-11), timing lines (NFR-PERF-09).
HEADERS += $$PWD/../Domain/SearchFilter.h \
           $$PWD/../Domain/SearchCache.h \
           $$PWD/../Infrastructure/SearchService.h \
           $$PWD/../Infrastructure/YtDlpSearchFallback.h \
           $$PWD/../Infrastructure/SuggestService.h \
           $$PWD/../../../Clients/Sailfish/Presentation/DiscoverySearchModel.h \
           $$PWD/../../../Clients/Sailfish/Presentation/SuggestionModel.h
SOURCES += $$PWD/../Domain/SearchFilter.cpp \
           $$PWD/../Domain/SearchCache.cpp \
           $$PWD/../Infrastructure/SearchService.cpp \
           $$PWD/../Infrastructure/YtDlpSearchFallback.cpp \
           $$PWD/../Infrastructure/SuggestService.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/DiscoverySearchModel.cpp \
           $$PWD/../../../Clients/Sailfish/Presentation/SuggestionModel.cpp
