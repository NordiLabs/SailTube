# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Catalog component (discovery, links, captions, SponsorBlock, RYD). Foundation files here; each
# area lists its own files in pri/<area>.pri. Paths are relative to the repository root.
QT += network

HEADERS += $$PWD/Domain/Item.h \
           $$PWD/Domain/Outcome.h \
           $$PWD/Domain/TextParse.h \
           $$PWD/Domain/Images.h \
           $$PWD/Domain/RowFilter.h \
           $$PWD/Domain/AvatarBook.h \
           $$PWD/Domain/ThumbnailBook.h \
           $$PWD/Domain/LocalizedText.h \
           $$PWD/Infrastructure/HttpTransport.h \
           $$PWD/Infrastructure/WorkLane.h \
           $$PWD/Infrastructure/Json.h \
           $$PWD/Infrastructure/ClientIdentity.h \
           $$PWD/Infrastructure/InnerTubeClient.h \
           $$PWD/Infrastructure/InnerTubeDefinitions.h \
           $$PWD/Infrastructure/RendererParser.h
SOURCES += $$PWD/Domain/TextParse.cpp \
           $$PWD/Domain/Images.cpp \
           $$PWD/Domain/RowFilter.cpp \
           $$PWD/Domain/LocalizedText.cpp \
           $$PWD/Infrastructure/HttpTransport.cpp \
           $$PWD/Infrastructure/Json.cpp \
           $$PWD/Infrastructure/ClientIdentity.cpp \
           $$PWD/Infrastructure/InnerTubeClient.cpp \
           $$PWD/Infrastructure/RendererParser.cpp

HEADERS += $$PWD/../../Clients/Sailfish/Composition/CatalogComposition.h \
           $$PWD/../../Clients/Sailfish/Presentation/WatchedRows.h \
           $$PWD/../../Clients/Sailfish/Presentation/TapIntent.h \
           $$PWD/../../Clients/Sailfish/Presentation/CatalogText.h \
           $$PWD/../../Clients/Sailfish/Presentation/ChannelAvatars.h \
           $$PWD/../../Clients/Sailfish/Presentation/LargeThumbnails.h \
           $$PWD/../../Clients/Sailfish/Presentation/SettingsFile.h \
           $$PWD/../../Clients/Sailfish/Presentation/SignedCatalog.h \
           $$PWD/../../Clients/Sailfish/Presentation/QueueSeeder.h
SOURCES += $$PWD/../../Clients/Sailfish/Composition/CatalogComposition.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/WatchedRows.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/TapIntent.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/CatalogText.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/ChannelAvatars.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/LargeThumbnails.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/SettingsFile.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/SignedCatalog.cpp \
           $$PWD/../../Clients/Sailfish/Presentation/QueueSeeder.cpp

include(pri/search.pri)
include(pri/feed.pri)
include(pri/channel.pri)
include(pri/watch.pri)
include(pri/comments.pri)
include(pri/captions.pri)
include(pri/links.pri)
include(pri/images.pri)
include(pri/library.pri)
