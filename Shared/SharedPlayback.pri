# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SailTune's shared instant-playback component, pinned verbatim under Shared/SailTune/ with SailTune's
# own relative layout (Core/SharedPlayback/, tests/sharedplayback/) so its includes stay unchanged.
# SailTube-owned glue: not in the manifest. Do not edit the pinned files (docs/SHARING.md).
# Qt Core + Qt Network only. Players reach it through Shared/Playback/StreamSource.h (playback component adapter).
SHARED_PLAYBACK = $$PWD/SailTune/Core/SharedPlayback
QT *= network
HEADERS += $$PWD/SailTune/Core/Beta/ServerDate.h \
           $$SHARED_PLAYBACK/Domain/CircuitBreaker.h \
           $$SHARED_PLAYBACK/Domain/FormatLadder.h \
           $$SHARED_PLAYBACK/Domain/HttpResponseParser.h \
           $$SHARED_PLAYBACK/Domain/InnerTubeClient.h \
           $$SHARED_PLAYBACK/Domain/Playability.h \
           $$SHARED_PLAYBACK/Domain/ResolveCache.h \
           $$SHARED_PLAYBACK/Domain/StreamTypes.h \
           $$SHARED_PLAYBACK/Infrastructure/FastStreamResolver.h \
           $$SHARED_PLAYBACK/Infrastructure/HttpsConnection.h \
           $$SHARED_PLAYBACK/Infrastructure/MediaAddress.h \
           $$SHARED_PLAYBACK/Infrastructure/MediaProxy.h \
           $$SHARED_PLAYBACK/Infrastructure/PlayerResponseParser.h
SOURCES += $$SHARED_PLAYBACK/Infrastructure/FastStreamResolver.cpp \
           $$SHARED_PLAYBACK/Infrastructure/HttpsConnection.cpp \
           $$SHARED_PLAYBACK/Infrastructure/MediaProxy.cpp \
           $$SHARED_PLAYBACK/Infrastructure/PlayerResponseParser.cpp
OTHER_FILES += $$SHARED_PLAYBACK/README.md
