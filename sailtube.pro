# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
TEMPLATE = app
TARGET = harbour-sailtube
VERSION = 0.1.0
CONFIG += sailfishapp sailfishapp_no_deploy_qml sailfishapp_i18n c++11
QT += quick qml multimedia network sql

DEFINES += SAILTUBE_VERSION=\\\"$$VERSION\\\"
INCLUDEPATH += $$PWD $$PWD/Clients/Sailfish/Composition

TRANSLATIONS += translations/harbour-sailtube-en.ts \
                translations/harbour-sailtube-es.ts \
                translations/harbour-sailtube-pt.ts \
                translations/harbour-sailtube-fr.ts \
                translations/harbour-sailtube-de.ts \
                translations/harbour-sailtube-it.ts \
                translations/harbour-sailtube-ru.ts \
                translations/harbour-sailtube-ja.ts \
                translations/harbour-sailtube-zh_CN.ts \
                translations/harbour-sailtube-ar.ts
TRANSLATION_SOURCES += $$PWD/Clients/Sailfish $$PWD/Core

# Shared resolver runtime, pinned from SailTune (Shared/SHARED_MANIFEST.json, docs/SHARING.md).
HEADERS += Shared/Runtime/Child/PythonResolverChild.h \
           Shared/Runtime/Child/QuickJsChild.h \
           Shared/Runtime/Process/ResolverProcess.h \
           Shared/Playback/StreamSource.h
SOURCES += Shared/Runtime/Child/PythonResolverChild.cpp \
           Shared/Runtime/Child/QuickJsChild.cpp
include(Shared/SharedPlayback.pri)
include(Shared/SharedAccounts.pri)

# Core: Qt-free domain policies and Qt infrastructure adapters.
HEADERS += Core/Media/Domain/MediaFormat.h \
           Core/Media/Domain/StreamSelection.h \
           Core/Media/Domain/DashManifest.h \
           Core/Media/Infrastructure/DashManifestPreparer.h \
           Core/Catalog/Domain/Video.h \
           Core/Settings/Domain/SettingsDefaults.h \
           Core/Resolver/Infrastructure/YtDlpJson.h \
           Core/Resolver/Infrastructure/YtDlpRunner.h \
           Core/Resolver/Infrastructure/YtDlpVideoService.h \
           Core/Platform/Infrastructure/GstCapabilityProbe.h \
           Core/Diagnostics/Domain/ErrorCounters.h \
           Core/Diagnostics/Domain/DiagnosticsReport.h \
           Core/Diagnostics/Infrastructure/TimingLog.h \
           Core/Process/Infrastructure/HelperProcess.h \
           Core/Storage/Infrastructure/StartupCleanup.h \
           Core/Worker/Infrastructure/WarmResolver.h \
           Core/Worker/Infrastructure/PythonWorkerChild.h
SOURCES += Core/Media/Domain/StreamSelection.cpp \
           Core/Settings/Domain/SettingsDefaults.cpp \
           Core/Media/Domain/DashManifest.cpp \
           Core/Media/Infrastructure/DashManifestPreparer.cpp \
           Core/Resolver/Infrastructure/YtDlpJson.cpp \
           Core/Resolver/Infrastructure/YtDlpRunner.cpp \
           Core/Resolver/Infrastructure/YtDlpVideoService.cpp \
           Core/Platform/Infrastructure/GstCapabilityProbe.cpp \
           Core/Diagnostics/Infrastructure/TimingLog.cpp \
           Core/Process/Infrastructure/HelperProcess.cpp \
           Core/Storage/Infrastructure/StartupCleanup.cpp \
           Core/Worker/Infrastructure/WarmResolver.cpp \
           Core/Worker/Infrastructure/PythonWorkerChild.cpp

# Playback component: player, selection additions, shared-interface adapter.
include(playback.pri)

# Catalog component (Core/Catalog and its presentation models), see Core/Catalog/catalog.pri.
include(Core/Catalog/catalog.pri)

# Library component: local-first library, imports and downloads.
LIBS += -lz
HEADERS += Core/Library/Domain/LibraryTypes.h \
           Core/Library/Domain/LibraryPolicy.h \
           Core/Library/Domain/ProgressTracker.h \
           Core/Library/Infrastructure/LibrarySchema.h \
           Core/Library/Infrastructure/LibraryStore.h \
           Core/Library/Infrastructure/ZipReader.h \
           Core/Library/Infrastructure/NewPipeImporter.h \
           Core/Library/Infrastructure/TakeoutImporter.h \
           Core/Library/Infrastructure/SailTubeBackup.h \
           Core/Library/Infrastructure/ImportJob.h \
           Core/Library/Infrastructure/AccountImport.h \
           Core/Downloads/Domain/DownloadTypes.h \
           Core/Downloads/Domain/FileNaming.h \
           Core/Downloads/Domain/DownloadPolicy.h \
           Core/Downloads/Domain/SidecarPolicy.h \
           Core/Downloads/Domain/Mp4Layout.h \
           Core/Downloads/Infrastructure/DownloadResolver.h \
           Core/Downloads/Infrastructure/YtDlpDownloadResolver.h \
           Core/Downloads/Infrastructure/DownloadQueueStore.h \
           Core/Downloads/Infrastructure/DownloadLocation.h \
           Core/Downloads/Infrastructure/DownloadEngine.h \
           Core/Downloads/Infrastructure/DownloadManager.h \
           Core/Downloads/Infrastructure/SidecarSource.h \
           Core/Downloads/Infrastructure/SidecarStore.h \
           Core/Downloads/Infrastructure/Remuxer.h \
           Core/Downloads/Infrastructure/GstRemuxer.h \
           Clients/Sailfish/Presentation/LibraryModels.h \
           Clients/Sailfish/Presentation/LibraryPlaybackBridge.h \
           Clients/Sailfish/Presentation/ImportController.h \
           Clients/Sailfish/Presentation/DownloadsModel.h \
           Core/Account/Domain/AccountTypes.h \
           Core/Account/Domain/AccountPolicy.h \
           Core/Account/Infrastructure/AccountStore.h \
           Core/Account/Infrastructure/AuthTransport.h \
           Core/Account/Infrastructure/AccountService.h \
           Core/Account/Infrastructure/BrowserCookieImport.h \
           Core/Account/Infrastructure/SessionTextImport.h \
           Core/Account/Infrastructure/SignedCatalogTransport.h \
           Core/Account/Infrastructure/PinnedSessionParser.h \
           Clients/Sailfish/Presentation/AccountController.h \
           Clients/Sailfish/Composition/LibraryCatalogGlue.h
SOURCES += Core/Library/Domain/LibraryPolicy.cpp \
           Core/Library/Domain/ProgressTracker.cpp \
           Core/Library/Infrastructure/LibraryStore.cpp \
           Core/Library/Infrastructure/ZipReader.cpp \
           Core/Library/Infrastructure/NewPipeImporter.cpp \
           Core/Library/Infrastructure/TakeoutImporter.cpp \
           Core/Library/Infrastructure/SailTubeBackup.cpp \
           Core/Library/Infrastructure/ImportJob.cpp \
           Core/Library/Infrastructure/AccountImport.cpp \
           Core/Downloads/Domain/FileNaming.cpp \
           Core/Downloads/Domain/DownloadPolicy.cpp \
           Core/Downloads/Domain/SidecarPolicy.cpp \
           Core/Downloads/Domain/Mp4Layout.cpp \
           Core/Downloads/Infrastructure/YtDlpDownloadResolver.cpp \
           Core/Downloads/Infrastructure/DownloadQueueStore.cpp \
           Core/Downloads/Infrastructure/DownloadLocation.cpp \
           Core/Downloads/Infrastructure/DownloadEngine.cpp \
           Core/Downloads/Infrastructure/DownloadManager.cpp \
           Core/Downloads/Infrastructure/SidecarStore.cpp \
           Core/Downloads/Infrastructure/GstRemuxer.cpp \
           Clients/Sailfish/Presentation/LibraryModels.cpp \
           Clients/Sailfish/Presentation/LibraryPlaybackBridge.cpp \
           Clients/Sailfish/Presentation/ImportController.cpp \
           Clients/Sailfish/Presentation/DownloadsModel.cpp \
           Core/Account/Domain/AccountPolicy.cpp \
           Core/Account/Infrastructure/AccountStore.cpp \
           Core/Account/Infrastructure/AuthTransport.cpp \
           Core/Account/Infrastructure/AccountService.cpp \
           Core/Account/Infrastructure/BrowserCookieImport.cpp \
           Core/Account/Infrastructure/SignedCatalogTransport.cpp \
           Core/Account/Infrastructure/PinnedSessionParser.cpp \
           Clients/Sailfish/Presentation/AccountController.cpp \
           Clients/Sailfish/Composition/LibraryCatalogGlue.cpp
# STORE variant (Harbour): `qmake CONFIG+=sailtube_store` compiles out the FULL-only download keep-alive (DL-09, PLT-11).
sailtube_store: DEFINES += SAILTUBE_STORE

# Sailfish client.
HEADERS += Clients/Sailfish/Composition/NordTileRuntimeLayout.h \
           Clients/Sailfish/Presentation/AppSettings.h \
           Clients/Sailfish/Presentation/ErrorText.h \
           Clients/Sailfish/Presentation/PlaybackSession.h \
           Clients/Sailfish/Presentation/SearchModel.h \
           Clients/Sailfish/Presentation/StartupTiming.h
SOURCES += Clients/Sailfish/Composition/main.cpp \
           Clients/Sailfish/Presentation/AppSettings.cpp \
           Clients/Sailfish/Presentation/PlaybackSession.cpp \
           Clients/Sailfish/Presentation/SearchModel.cpp \
           Clients/Sailfish/Presentation/StartupTiming.cpp

# QML lives outside the template's default qml directory.
qml.files = Clients/Sailfish/qml/harbour-sailtube.qml \
            Clients/Sailfish/qml/BetaEnded.qml \
            Clients/Sailfish/qml/pages \
            Clients/Sailfish/qml/cover \
            Clients/Sailfish/qml/components \
            Clients/Sailfish/qml/logic \
            Clients/Sailfish/qml/sections
qml.path = /usr/share/$${TARGET}/qml
INSTALLS += qml

SAILFISHAPP_ICONS = 86x86 108x108 128x128 172x172

OTHER_FILES += rpm/harbour-sailtube.spec harbour-sailtube.desktop \
               Core/Worker/Runtime/worker.py Core/Worker/Runtime/Worker.qml \
               Shared/Runtime/Python/resolver.py Shared/Runtime/Python/Resolver.qml

# [FULL] NFR-MNT-01 extractor updates (verified yt-dlp archive): not in the STORE build.
!sailtube_store {
    HEADERS += Core/Extractor/Domain/ReleasePolicy.h \
               Core/Extractor/Infrastructure/ExtractorUpdater.h
    SOURCES += Core/Extractor/Domain/ReleasePolicy.cpp \
               Core/Extractor/Infrastructure/ExtractorUpdater.cpp
    OTHER_FILES += Core/Extractor/Runtime/verify_release.py Core/Extractor/Runtime/yt-dlp-release-key.asc
}

# Time-limited public beta (docs/BETA.md): FULL only, never Harbour. The build fails unless
# SAILTUBE_BETA_EXPIRES is a real date, not in the past and not later than 2026-12-30.
sailtube_beta {
    sailtube_store: error("The beta profile is FULL only: it is not for Harbour (drop CONFIG+=sailtube_store)")
    BETA_CHECK = $$system(python3 -I $$PWD/tools/check_beta_date.py "$$SAILTUBE_BETA_EXPIRES")
    !equals(BETA_CHECK, "ok"): error("SAILTUBE_BETA_EXPIRES=$$SAILTUBE_BETA_EXPIRES: $$BETA_CHECK")
    BETA_BUILD_MS = $$system(python3 -I $$PWD/tools/check_beta_date.py --build-ms)
    isEmpty(BETA_BUILD_MS): error("sailtube_beta: cannot read the build time")
    isEmpty(SAILTUBE_BETA_RELEASE): SAILTUBE_BETA_RELEASE = 0.beta3
    DEFINES += SAILTUBE_BETA SAILTUBE_BETA_EXPIRES=\\\"$$SAILTUBE_BETA_EXPIRES\\\" \
               SAILTUBE_BETA_BUILD_MS=$${BETA_BUILD_MS}LL SAILTUBE_BETA_RELEASE=\\\"$$SAILTUBE_BETA_RELEASE\\\"
    # The beta is FULL only (the spec refuses --with beta together with --with store); stating it
    # here also tells the STORE privacy/runtime checks that these files never ship in STORE.
    !sailtube_store {
        HEADERS += Core/Beta/Domain/BetaPolicy.h Core/Beta/Infrastructure/BetaClock.h Clients/Sailfish/Presentation/BetaStatus.h
        SOURCES += Core/Beta/Infrastructure/BetaClock.cpp Clients/Sailfish/Presentation/BetaStatus.cpp
    }
}
# Device-only test hooks (rpmbuild --with debug_hooks), e.g. the beta date override SAILTUBE_BETA_NOW.
sailtube_debug_hooks: DEFINES += SAILTUBE_DEBUG_HOOKS
