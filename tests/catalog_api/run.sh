#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog API coverage (NFR-TST-06): domain helpers, JSON and renderer parsers, client identity,
# the work lane and the discovery services (channels, comments, feed, search, Shorts checks,
# SponsorBlock, suggestions, watch page) over FakeTransport and the recorded fixtures.
SUITE=catalog_api
MOC="Core/Catalog/Infrastructure/HttpTransport.h Core/Catalog/Infrastructure/WorkLane.h Core/Catalog/Infrastructure/ClientIdentity.h Core/Catalog/Infrastructure/InnerTubeClient.h \
Core/Catalog/Infrastructure/ChannelService.h Core/Catalog/Infrastructure/ShortsStore.h Core/Catalog/Infrastructure/CommentsService.h \
Core/Catalog/Infrastructure/FeedService.h Core/Catalog/Infrastructure/SearchService.h Core/Catalog/Infrastructure/SponsorBlockService.h \
Core/Catalog/Infrastructure/SuggestService.h Core/Catalog/Infrastructure/WatchNextService.h \
Core/Catalog/Infrastructure/ShortDetailService.h"
SOURCES="tests/catalog_api/ApiTest.cpp tests/catalog_api/ServicesTest.cpp \
Core/Catalog/Domain/TextParse.cpp Core/Catalog/Domain/Images.cpp Core/Catalog/Domain/RowFilter.cpp Core/Catalog/Domain/LocalizedText.cpp \
Core/Catalog/Domain/Captions.cpp Core/Catalog/Domain/ChannelReference.cpp Core/Catalog/Domain/Chapters.cpp Core/Catalog/Domain/Comments.cpp \
Core/Catalog/Domain/Description.cpp Core/Catalog/Domain/FeedSchedule.cpp Core/Catalog/Domain/Links.cpp Core/Catalog/Domain/SearchCache.cpp \
Core/Catalog/Domain/SearchFilter.cpp Core/Catalog/Domain/ShortsKnowledge.cpp Core/Catalog/Domain/SponsorSegments.cpp \
Core/Catalog/Infrastructure/HttpTransport.cpp Core/Catalog/Infrastructure/Json.cpp Core/Catalog/Infrastructure/ClientIdentity.cpp \
Core/Catalog/Infrastructure/InnerTubeClient.cpp Core/Catalog/Infrastructure/RendererParser.cpp \
Core/Catalog/Infrastructure/ChannelService.cpp Core/Catalog/Infrastructure/ShortsStore.cpp Core/Catalog/Infrastructure/CommentsService.cpp \
Core/Catalog/Infrastructure/RssParser.cpp Core/Catalog/Infrastructure/FeedCacheStore.cpp Core/Catalog/Infrastructure/FeedService.cpp \
Core/Catalog/Infrastructure/SearchService.cpp Core/Catalog/Infrastructure/SponsorBlockService.cpp Core/Catalog/Infrastructure/SuggestService.cpp \
Core/Catalog/Infrastructure/WatchNextService.cpp \
Core/Catalog/Infrastructure/ShortDetailService.cpp Core/Catalog/Domain/ShortDetail.cpp"
QT_MODULES="Qt5Core Qt5Network"
. "$(dirname "$0")/../catalog_common/compile.sh"
