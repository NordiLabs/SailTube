// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "NordTileRuntimeLayout.h"
#include "Shared/Runtime/Child/PythonResolverChild.h"
#include "Shared/Runtime/Child/QuickJsChild.h"
#include "Core/Platform/Infrastructure/GstCapabilityProbe.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"
#ifndef SAILTUBE_STORE
#include "Core/Resolver/Infrastructure/PoTokenProvider.h"
#endif
#include "Core/Downloads/Infrastructure/DownloadManager.h"
#include "Core/Downloads/Infrastructure/GstRemuxer.h"
#include "Core/Downloads/Infrastructure/YtDlpDownloadResolver.h"
#include "Core/Library/Infrastructure/LibraryStore.h"
#include "Core/Storage/Infrastructure/StartupCleanup.h"
#include "Core/Worker/Infrastructure/PythonWorkerChild.h"
#include "Core/Worker/Infrastructure/WarmResolver.h"
#ifdef SAILTUBE_BETA
#include "Clients/Sailfish/Presentation/BetaStatus.h"
#include <QDateTime>
#endif
#ifndef SAILTUBE_STORE
#include "Core/Extractor/Infrastructure/ExtractorUpdater.h"
#endif
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/PinnedSessionParser.h"
#include "Clients/Sailfish/Presentation/AccountController.h"
#include "Core/Player/Domain/TempDirPolicy.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/HardwareDecodeGate.h"
#include "Core/Player/Infrastructure/MemoryTrim.h"
#include "Clients/Sailfish/Presentation/MemoryTrimScheduler.h"
#include "Core/Player/Infrastructure/TempDirSetup.h"
#ifndef SAILTUBE_STORE
#include "Core/Player/Infrastructure/HardwareProbeRunner.h"
#endif
#include "Core/Player/Infrastructure/CoverVideoItem.h"
#include "Core/Player/Infrastructure/OverlayController.h"
#include "Core/Player/Infrastructure/SharedStreamSourceSailTune.h"
#include "Core/Player/Infrastructure/YtDlpStreamResolver.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"
#include "Core/Player/Infrastructure/VideoSurfaceItem.h"
#include "Clients/Sailfish/Presentation/AppSettings.h"
#include "Clients/Sailfish/Presentation/CaptionCues.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Clients/Sailfish/Presentation/DownloadsModel.h"
#include "Clients/Sailfish/Presentation/ImportController.h"
#include "Clients/Sailfish/Presentation/LibraryModels.h"
#include "Clients/Sailfish/Presentation/LibraryPlaybackBridge.h"
#include "Clients/Sailfish/Presentation/PlaybackCaptions.h"
#include "Clients/Sailfish/Presentation/PlaybackSession.h"
#include "Clients/Sailfish/Presentation/PlaybackTiming.h"
#include "Clients/Sailfish/Presentation/SearchModel.h"
#include "Clients/Sailfish/Composition/CatalogComposition.h"
#include "Clients/Sailfish/Presentation/StartupTiming.h"
#include "Clients/Sailfish/Presentation/TapIntent.h"

#include <sailfishapp.h>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QLocale>
#include <QFileInfo>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkConfigurationManager>
#include <QNetworkDiskCache>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlNetworkAccessManagerFactory>
#include <QQuickView>
#include <QScopedPointer>
#include <QSharedPointer>
#include <QStandardPaths>
#include <QTranslator>
#include <QTimer>
#include <QUrl>
#include <future>
#include <memory>
#include <QElapsedTimer>
#include <QFile>
#include <cstdio>
#include <unistd.h>
#include <QFile>
#include <QtQml>

namespace {
using namespace nordtile::video;

// Opt-in start-up trace (docs/PERF_STARTUP.md): SAILTUBE_STARTUP_TRACE=1 prints one line per phase with the
// milliseconds since the process started (kernel start time, so dynamic loading counts too);
// SAILTUBE_STARTUP_TRACE=exit also quits once the QML scene exists (no display needed) or after the first frame.
class StartupTrace {
public:
    StartupTrace() : mode_(qgetenv("SAILTUBE_STARTUP_TRACE")) { clock_.start(); offset_ = processAgeMs(); }
    bool enabled() const { return !mode_.isEmpty(); }
    bool exitAfterScene() const { return mode_ == "exit"; }
    void mark(const char *phase) {
        if (enabled()) std::fprintf(stderr, "sailtube-startup %-22s %6lld ms\n", phase, static_cast<long long>(offset_ + clock_.elapsed()));
    }
private:
    static qint64 processAgeMs() {
#ifdef Q_OS_LINUX
        QFile stat(QStringLiteral("/proc/self/stat"));
        QFile uptime(QStringLiteral("/proc/uptime"));
        if (!stat.open(QIODevice::ReadOnly) || !uptime.open(QIODevice::ReadOnly)) return 0;
        const QByteArray s = stat.readAll();
        const QList<QByteArray> f = s.mid(s.lastIndexOf(')') + 2).split(' ');
        const long hz = ::sysconf(_SC_CLK_TCK);
        if (f.size() < 20 || hz <= 0) return 0;
        const double startedS = f.at(19).toDouble() / hz;   // field 22: start time in clock ticks since boot
        const double nowS = uptime.readAll().split(' ').value(0).toDouble();
        return qMax<qint64>(0, static_cast<qint64>((nowS - startedS) * 1000));
#else
        return 0;
#endif
    }
    QByteArray mode_;
    QElapsedTimer clock_;
    qint64 offset_ = 0;
};

// Thumbnails: a bounded private disk cache, no cookies.
class ThumbnailNetworkFactory final : public QQmlNetworkAccessManagerFactory {
public:
    explicit ThumbnailNetworkFactory(const QString &directory) : directory_(directory) {}
    QNetworkAccessManager *create(QObject *parent) override {
        QNetworkAccessManager *manager = new QNetworkAccessManager(parent);
        QNetworkDiskCache *cache = new QNetworkDiskCache(manager);
        cache->setCacheDirectory(directory_);
        cache->setMaximumCacheSize(64 * 1024 * 1024);
        manager->setCache(cache);
        return manager;
    }
private:
    QString directory_;
};

// PLT-09: /tmp is RAM-backed and GLib caches the temp directory on first use.
// Point TMPDIR at a private flash directory, emptied at start, before anything
// initialises GLib or GStreamer.
// TempDirPolicy decides the directory (never in a RAM-backed tree).
void useFlashTemporaryDirectory() {
    // Empties the directory, makes it owner-only, exports TMPDIR; refuses a cache in RAM.
    const QString flash = nordtile::player::prepareFlashTempDir(QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
    if (!flash.isEmpty()) qputenv("TMPDIR", QFile::encodeName(flash));  // exported here as well, before GLib starts
}

// PLT-08: decoder ranks for every GStreamer pipeline in the process, including any built outside
// the playback component's engine (Qt or other code may load GStreamer too, also on STORE). SailTune device
// finding (SFOS 5.2): gst-droid's droidadec outranks libav's AAC decoder and produces no frames,
// so playback is silent; droidvdec stays demoted as well. Appended to a value the user set
// (later entries win). Must run before the first GStreamer call; the engine's own demotion
// (GstEngine::initialize) stays.
void pinDecoderRanks() {
    static const char ranks[] = "droidadec:NONE,droidvdec:NONE,avdec_aac:MAX";
    QByteArray value = qgetenv("GST_PLUGIN_FEATURE_RANK");
    if (!value.isEmpty()) value += ',';
    value += ranks;
    qputenv("GST_PLUGIN_FEATURE_RANK", value);
}

// SEL-02: the network class of the next selection (0 unmetered, 1 mobile, -1 unknown).
int networkClass(const QNetworkConfigurationManager &manager) {
    const QNetworkConfiguration c = manager.defaultConfiguration();
    switch (c.bearerType()) {
    case QNetworkConfiguration::BearerEthernet:
    case QNetworkConfiguration::BearerWLAN:
        return 0;
    case QNetworkConfiguration::Bearer2G:
    case QNetworkConfiguration::BearerCDMA2000:
    case QNetworkConfiguration::BearerWCDMA:
    case QNetworkConfiguration::BearerHSPA:
    case QNetworkConfiguration::BearerWiMAX:
    case QNetworkConfiguration::BearerEVDO:
    case QNetworkConfiguration::BearerLTE:
    case QNetworkConfiguration::Bearer3G:
    case QNetworkConfiguration::Bearer4G:
        return 1;
    default:
        return -1;
    }
}

QString runtimeFile(const char *relative) {
    return QString::fromLatin1(NORDTILE_RUNTIME_ROOT) + QLatin1Char('/') + QString::fromLatin1(relative);
}
}

#ifdef SAILTUBE_BETA
// Time-limited beta (docs/BETA.md): after the end only this page runs. Nothing else is restored or
// contacted: no resolver, catalog, downloads or network access manager exists. The library is opened only
// so a backup can be exported; user data is never deleted or modified.
int runBetaEnded(QGuiApplication *app, nordtile::video::sailfish::BetaStatus *status) {
    using namespace nordtile::video;
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    library::LibraryStore store(dataRoot + QStringLiteral("/library/library.db"), QStringLiteral("library-beta-ended"));
    QString error;
    if (!store.open(&error)) qWarning("sailtube: library unavailable: %s", qPrintable(error));
    sailfish::ImportController importer(&store, dataRoot + QStringLiteral("/library"),
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf"));
    QScopedPointer<QQuickView> view(SailfishApp::createView());
    QQmlContext *context = view->rootContext();
    context->setContextProperty(QStringLiteral("beta"), status);
    context->setContextProperty(QStringLiteral("importer"), &importer);
    const QByteArray qmlRoot = qgetenv("SAILTUBE_QML_ROOT");
    view->setSource(qmlRoot.isEmpty() ? SailfishApp::pathTo(QStringLiteral("qml/BetaEnded.qml"))
                                      : QUrl::fromLocalFile(QString::fromLocal8Bit(qmlRoot) + QStringLiteral("/BetaEnded.qml")));
    if (qgetenv("SAILTUBE_STARTUP_TRACE") == "exit" && QGuiApplication::platformName() == QLatin1String("minimal")) {
        std::fprintf(stderr, "sailtube-beta ended page %s\n", view->rootObject() ? "created" : "missing");
        return view->rootObject() ? 0 : 3;
    }
    view->show();
    return app->exec();
}
#endif

int main(int argc, char *argv[]) {
    // Before any thread exists: cap the glibc arenas (one switch: kMemoryTrimEnabled).
    nordtile::player::limitMallocArenas();
    sailfish::timingClock();  // NFR-PERF-02 marker origin: "launch" is t=0
    QElapsedTimer startClock;
    startClock.start();
    // Child modes run before any GUI, media or settings object exists.
    if (nordtile::music::sailfish::infrastructure::quickJsChildRequested())
        return nordtile::music::sailfish::infrastructure::runQuickJsChild(argc, argv);
    if (nordtile::music::sailfish::infrastructure::pythonResolverChildRequested())
        return nordtile::music::sailfish::infrastructure::runPythonResolverChild(argc, argv);
    if (nordtile::video::worker::pythonWorkerRequested())
        return nordtile::video::worker::runPythonWorkerChild(argc, argv, NORDTILE_RUNTIME_ROOT "/runtime");
    StartupTrace trace;
    trace.mark("entry");

    // Must match the desktop file's X-Sailjail section (PLT-03). Set before the
    // application object so the temporary directory can move first.
    QCoreApplication::setOrganizationName(QStringLiteral("org.nordilabs.sailtube"));
    QCoreApplication::setApplicationName(QStringLiteral("SailTube"));
    useFlashTemporaryDirectory();
    pinDecoderRanks();  // before anything can initialise GStreamer (PLT-08)
    {
        // NFR-ROB-02: temp, spool and stale lock files a crash left behind (top level of the app's
        // own directories; each store cleans its own subdirectory when it opens).
        const nordtile::storage::CleanupReport cleanup = nordtile::storage::removeOrphans(QStringList()
            << QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
            << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
            << QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        if (!cleanup.removed.isEmpty()) qInfo("sailtube: removed %d orphaned temp/lock file(s) at start", cleanup.removed.size());
    }
    trace.mark("main");
    QScopedPointer<QGuiApplication> app(SailfishApp::application(argc, argv));
    trace.mark("application");
    app->setOrganizationName(QStringLiteral("org.nordilabs.sailtube"));
    app->setApplicationName(QStringLiteral("SailTube"));
#ifdef SAILTUBE_BETA
    // Time-limited beta (docs/BETA.md), decided before anything is restored or contacted.
    std::function<qint64()> betaNow;
#ifdef SAILTUBE_DEBUG_HOOKS
    // Device test hook only (rpmbuild --with debug_hooks): SAILTUBE_BETA_NOW=2026-12-31T10:00:00 fakes the clock.
    const QByteArray betaOverride = qgetenv("SAILTUBE_BETA_NOW");
    if (!betaOverride.isEmpty()) {
        const qint64 fixedMs = QDateTime::fromString(QString::fromLatin1(betaOverride), Qt::ISODate).toMSecsSinceEpoch();
        betaNow = [fixedMs]() { return fixedMs; };
    }
#endif
    const QDate betaExpiry = QDate::fromString(QStringLiteral(SAILTUBE_BETA_EXPIRES), Qt::ISODate);
    beta::BetaClock betaClock(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/beta/clock"),
                              betaExpiry, SAILTUBE_BETA_BUILD_MS, betaNow);
    sailfish::BetaStatus betaStatus(&betaClock, betaExpiry, QStringLiteral(SAILTUBE_VERSION "-" SAILTUBE_BETA_RELEASE));
    if (betaStatus.expired()) return runBetaEnded(app.data(), &betaStatus);
#endif
    // LNK-03 / L1: own the link service's bus name before anything slow, so a D-Bus activation
    // call is delivered to this process at once (catalog component).
    catalog::CatalogComposition::claimLinkService();
    // NFR-PERF-01: start-up stages, "sailtube-timing startup <stage> <ms since main()>".
    sailfish::StartupTiming timing(startClock);
    timing.mark(QStringLiteral("app"));

    // I18N-03: the English catalog sits under the device language, so plural forms and any
    // message missing from the device catalog resolve in English. Translators are searched
    // newest first, so the device catalog is installed again on top.
    const QString translations = SailfishApp::pathTo(QStringLiteral("translations")).toLocalFile();
    QTranslator *english = new QTranslator(app.data());
    if (english->load(QStringLiteral("harbour-sailtube-en"), translations)) {
        app->installTranslator(english);
        QTranslator *local = new QTranslator(app.data());
        if (local->load(QLocale(), QStringLiteral("harbour-sailtube"), QStringLiteral("-"), translations))
            app->installTranslator(local);
        else
            delete local;
    } else {
        delete english;
    }

    timing.mark(QStringLiteral("translations"));
    sailfish::AppSettings settings;
    sailfish::setTimingEnabled(settings.timingLog());
    // DIA-03: always printed, so a device log says whether markers are expected and where to turn
    // them on (diagnostics/timingLog in that file, or SAILTUBE_TIMING=1).
    qInfo("sailtube: timing log %s (settings %s)", sailfish::timingEnabled() ? "on" : "off",
          qPrintable(settings.settingsFilePath()));
    if (sailfish::timingEnabled()) qInfo("sailtube-timing: launch t=0");
    // The player's GStreamer: after TMPDIR (PLT-09), before any pipeline.
    // Demotes droidvdec to rank NONE whatever the setting (PLT-08); the
    // hardware-decode setting reaches each item's plan through the session
    // preferences (PLY-20/21).
    // NFR-PERF-01: gst_init, the PLT-08 demotion and the registry probe run on the GStreamer
    // start-up worker while the resolver, library and models below are built.
    std::shared_ptr<std::promise<platform::GstProbeReport>> probePromise(new std::promise<platform::GstProbeReport>);
    std::future<platform::GstProbeReport> probeFuture = probePromise->get_future();
    const platform::AdaptivePipeline adaptiveMode = settings.adaptivePipeline();
    const bool hardwareDecoding = settings.hardwareDecoding();
    QObject::connect(nordtile::player::GstEngineInit::instance(), &nordtile::player::GstEngineInit::ready, app.data(),
                     [](bool ok, const QString &error, qint64 ms) {
        if (!ok) qWarning("sailtube: GStreamer unavailable: %s", qPrintable(error));
        else qInfo("sailtube: gstreamer initialised on the start-up worker in %lld ms", static_cast<long long>(ms));
        nordtile::video::sailfish::timingMark("engine-ready", QStringLiteral("ok=%1 ms=%2").arg(ok ? 1 : 0).arg(ms));
    });
    nordtile::player::GstEngine::startInitialize([probePromise, adaptiveMode, hardwareDecoding]() {
        probePromise->set_value(platform::probeGstreamer(adaptiveMode, hardwareDecoding));
    });
    timing.mark(QStringLiteral("gst-started"));
    trace.mark("gst-started");   // core StartupTrace; gst_init and the probe run on the start-up worker
    resolver::RunnerOptions runnerOptions;
    const QString self = QCoreApplication::applicationFilePath();
    const QByteArray developmentResolver = qgetenv("SAILTUBE_DEVELOPMENT_YTDLP");
    if (!developmentResolver.isEmpty()) {
        // Development only: an independently installed yt-dlp executable.
        runnerOptions.executablePath = QString::fromLocal8Bit(developmentResolver);
        runnerOptions.pythonChild = false;
    } else if (QFileInfo(runtimeFile("runtime/yt-dlp.pyz")).isFile() && QFileInfo(runtimeFile("runtime/resolver.py")).isFile()) {
        runnerOptions.executablePath = self;
        runnerOptions.pythonChild = true;
        if (QFileInfo(runtimeFile("lib/libsailtune-quickjs.so.1")).isFile())
            runnerOptions.javascriptRuntime = QStringLiteral("quickjs:") + self;
    }
#ifndef SAILTUBE_STORE
    // RES-14 [FULL]: the optional PO-token helper. Idle (no process, no file access) until the
    // user enables it; then install/start/verify run in the background and nothing waits on it.
    // Declared before the services, so it outlives the argument source they hold.
    resolver::PoTokenProviderOptions poTokenOptions;
    poTokenOptions.root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/potoken");
    poTokenOptions.heapMb = settings.poTokenHeapMb();
    resolver::PoTokenProvider poToken(poTokenOptions);
#endif
    // RES-01: warm resolver workers (import yt-dlp once, player-JS cache on). Latency lane for play and
    // details, background lane for downloads; started a moment after the first frame (see below).
#ifndef SAILTUBE_STORE
    // NFR-MNT-01 [FULL]: verified extractor updates. The warm workers load the active archive (an installed,
    // signature-verified update, else the bundled one); a new install takes effect at the next start.
    nordtile::video::extractor::ProcessVerifierOptions verifierOptions;
    verifierOptions.program = self;          // the app binary in worker mode, task "verify"
    verifierOptions.workerTask = true;
    nordtile::video::extractor::UpdaterOptions updaterOptions;
    updaterOptions.root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/extractor");
    updaterOptions.bundledArchive = runtimeFile("runtime/yt-dlp.pyz");
    updaterOptions.bundledVersion = nordtile::video::extractor::ExtractorUpdater::bundledVersionFromManifest(
        runtimeFile("licenses/python-runtime/source-manifest.json"));
    updaterOptions.verifier = nordtile::video::extractor::processVerifier(verifierOptions);
    nordtile::video::extractor::ExtractorUpdater extractorUpdater(updaterOptions);
#endif
    worker::WorkerOptions workerOptions;
    workerOptions.workerProgram = self;
#ifndef SAILTUBE_STORE
    workerOptions.archivePath = extractorUpdater.activeArchive(runtimeFile("runtime/yt-dlp.pyz"));
#else
    workerOptions.archivePath = runtimeFile("runtime/yt-dlp.pyz");
#endif
    const QString workerCache = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    workerOptions.workDirectory = workerCache + QStringLiteral("/worker");
    workerOptions.cacheDirectory = workerCache + QStringLiteral("/yt-dlp");
    const bool warmAvailable = runnerOptions.pythonChild && !runnerOptions.executablePath.isEmpty()
        && QFileInfo(runtimeFile("runtime/worker.py")).isFile() && qgetenv("SAILTUBE_NO_WARM_RESOLVER").isEmpty();
    worker::WarmResolver warmResolver(workerOptions);
    if (warmAvailable) { runnerOptions.warm = &warmResolver; runnerOptions.warmLane = 0; }
    resolver::YtDlpRunner runner(runnerOptions);
    resolver::YtDlpVideoService service(&runner);
#ifndef SAILTUBE_STORE
    // Arguments only for runs whose request has poProvider and while the helper is ready.
    // PlaybackSession sets StreamRequest::poProvider from poToken.ready() (setPoProviderReady
    // below) and YtDlpStreamResolver passes it to details() as DetailsOptions::poProvider.
    resolver::PoTokenProvider *poTokenPointer = &poToken;
    service.setPoTokenArguments([poTokenPointer]() { return poTokenPointer->extractorArguments(); });
    const auto poTokenSetting = [&settings, poTokenPointer]() {
        poTokenPointer->setHeapMb(settings.poTokenHeapMb());
        poTokenPointer->setEnabled(settings.poTokenProvider());
    };
    poTokenSetting();
    QObject::connect(&settings, &sailfish::AppSettings::changed, &poToken, poTokenSetting);
    QObject::connect(&poToken, &resolver::PoTokenProvider::stateChanged, &poToken, [poTokenPointer]() {
        qInfo("sailtube: po-token helper %s", qPrintable(poTokenPointer->diagnostics()));
    });
#endif
    trace.mark("resolver");

    const QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    QNetworkAccessManager network;
    sailfish::SearchModel search(&service);
#ifdef SAILTUBE_STORE
    const bool storeVariant = true;
#else
    const bool storeVariant = false;
#endif
    // The shared stream source (RES-01..19, RES-21/22): SailTune's fast path, probe, breaker
    // and MediaProxy on their own worker thread (ThreadedStreamSource), with core's yt-dlp
    // runner as the extractor fallback on this (GUI) thread, where its QProcess lives.
    // Destroyed in reverse order: playback, then the source, then the fallback, then service.
    playeradapter::YtDlpStreamResolver extractor(&service);
    QPointer<playeradapter::YtDlpStreamResolver> extractorGuard(&extractor);
    const std::shared_ptr<nordtile::player::threaded::Channel> guiChannel = nordtile::player::threaded::Channel::here();
    nordtile::player::ThreadedStreamSource::Options sourceOptions;
    sourceOptions.monotonicMs = []() { return sailfish::timingNowMs(); };  // the launch clock
    sourceOptions.hooks.warmupStart = [](qint64 at) { sailfish::timingMarkAt("warmup-start", at); };
    sourceOptions.hooks.warmupDone = [](qint64 at, bool ok, qint64 visitorMs) {
        sailfish::timingMarkAt("warmup-done", at, QStringLiteral("ok=%1 visitorMs=%2").arg(ok ? 1 : 0).arg(visitorMs));
    };
    nordtile::player::ThreadedStreamSource streamSource([guiChannel, extractorGuard]() {
        nordtile::player::SailTuneBackend::Config config;  // runs on the worker thread
        // The pinned proxy can fetch the first 256 KiB and the last 16 KiB of every stream at open().
        // Off by default: the interleaved host A/B (docs/PERF_PLAYBACK.md, 2026-11-11) showed warm
        // starts about 300 ms slower with it and no gain on cold ones, for two more connections
        // and up to 272 KiB per opened stream. SAILTUBE_PREBUFFER=0|1 overrides the default for one
        // launch (device A/B, B5).
        const bool prebufferByDefault = false;
        const QByteArray prebuffer = qgetenv("SAILTUBE_PREBUFFER");
        if (prebuffer.isEmpty() ? !prebufferByDefault : prebuffer == "0") {
            config.proxy.prebufferBytes = 0;
            config.proxy.tailBytes = 0;
        }
        config.fallbackChannel = guiChannel;
        config.fallback = [extractorGuard]() { return static_cast<nordtile::playback::StreamResolver *>(extractorGuard.data()); };
        return static_cast<nordtile::player::StreamSourceBackend *>(new nordtile::player::SailTuneBackend(config));
    }, sourceOptions);
    // Display metadata off the start path, on its own runner so it never takes a playback
    // resolve slot (RES-02).
    resolver::RunnerOptions metadataRunnerOptions = runnerOptions;
    metadataRunnerOptions.maximumConcurrent = 1;
    resolver::YtDlpRunner metadataRunner(metadataRunnerOptions);
    resolver::YtDlpVideoService metadataService(&metadataRunner);
    resolver::YtDlpVideoService *metadataPointer = &metadataService;
    // PLT-11: the display keep-on uses Nemo.KeepAlive, FULL only.
    // PLY-20: per-build hardware-decode probe; its verdict sets hardwareDecodeVerified for selection.
    nordtile::player::HardwareDecodeGate::Options hardwareOptions;
    nordtile::player::HardwareProbeRunner *hardwareRunner = nullptr;
#ifndef SAILTUBE_STORE
    nordtile::player::GstHardwareProbeRunner gstProbeRunner;
    hardwareRunner = &gstProbeRunner;
    hardwareOptions.buildKey = nordtile::player::GstHardwareProbeRunner::currentBuildKey(QStringLiteral(SAILTUBE_VERSION));
    hardwareOptions.droidvdecPresent = nordtile::player::GstHardwareProbeRunner::elementPresent("droidvdec");
    hardwareOptions.droideglsinkPresent = nordtile::player::GstHardwareProbeRunner::elementPresent("droideglsink");
#endif
    hardwareOptions.clipDirectory = cacheRoot + QStringLiteral("/probe");
    hardwareOptions.storage = nordtile::player::HardwareDecodeGate::iniStorage(
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/hardware-probe.conf"));
    nordtile::player::HardwareDecodeGate hardwareGate(hardwareRunner, hardwareOptions);
    // NFR-PERF-01: the session starts with the assumed capabilities; the probe's arrive below.
    sailfish::PlaybackSession playback(&streamSource, streamSource.fetcher(), &settings, media::DecoderCapabilities(), !storeVariant);
    playback.setHardwareDecodeGate(&hardwareGate);
    // malloc_trim(0) after a pipeline teardown and once when idle (kMemoryTrimEnabled switches it).
    sailfish::MemoryTrimScheduler memoryTrim(playback.player());
    // Cover video (playback/coverVideo, FULL; AppSettings returns false on STORE).
    playback.setCoverVideoEnabled(settings.coverVideo());
    QObject::connect(&settings, &sailfish::AppSettings::changed, &playback,
                     [&playback, &settings]() { playback.setCoverVideoEnabled(settings.coverVideo()); });
#ifndef SAILTUBE_STORE
    // RES-14 [FULL]: requests carry poProvider only while the helper is ready.
    playback.setPoProviderReady([poTokenPointer]() { return poTokenPointer->ready(); });
#endif
    // RES-21 / E12: the session re-warms the resolver while lists are visible (rowsVisible).
    playback.setWarmUp([&streamSource]() { streamSource.warmUp(); });
    playback.setMetadataSource([metadataPointer](const QString &videoId, const sailfish::PlaybackSession::ResolveDone &done) {
        const quint64 id = metadataPointer->details(videoId, false,
            [done](resolver::ErrorCode error, const catalog::VideoDetails &details) { done(static_cast<int>(error), details); });
        return std::function<void()>([metadataPointer, id]() { metadataPointer->cancel(id); });
    });
    QNetworkConfigurationManager networkConfigurations;
    playback.setNetworkProbe([&networkConfigurations]() { return networkClass(networkConfigurations); });
    playback.setArtworkCache(&network, cacheRoot + QStringLiteral("/artwork"));

    // Library component: the local-first library under the Sailjail data layout (PLT-03).
    const QString dataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    library::LibraryStore libraryStore(dataRoot + QStringLiteral("/library/library.db"), QStringLiteral("library-main"));
    QString libraryError;
    if (!libraryStore.open(&libraryError)) qWarning("sailtube: library unavailable: %s", qPrintable(libraryError));
    trace.mark("library-open");
    sailfish::SubscriptionsModel subscriptions(&libraryStore);
    sailfish::PlaylistsModel playlists(&libraryStore);
    sailfish::HistoryModel history(&libraryStore);
    sailfish::WatchStateIndex watchIndex(&libraryStore);
    sailfish::LibraryPlaybackBridge libraryPlayback(&playback, &libraryStore);
    // Extracted archives go next to the database, where open() sweeps orphans (PLT-09: never /tmp).
    sailfish::ImportController importer(&libraryStore, dataRoot + QStringLiteral("/library"),
        QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf"));

    // Downloads resolve on their own runner, so they never take a playback resolve slot (RES-02).
    resolver::RunnerOptions downloadRunnerOptions = runnerOptions;
    downloadRunnerOptions.maximumConcurrent = 1;
    downloadRunnerOptions.warmLane = 1;
    resolver::YtDlpRunner downloadRunner(downloadRunnerOptions);
    resolver::YtDlpVideoService downloadService(&downloadRunner);
    downloads::YtDlpDownloadResolver downloadResolver(&downloadService);
    downloads::GstRemuxer remuxer;
    // NFR-PERF-01: the probe result; blocks only if the worker is still probing.
    const platform::GstProbeReport probe = probeFuture.get();
    qInfo("sailtube: gstreamer %s loaded=%d playbin3-switch=%d adaptive=%d h264hw=%d vp9hw=%d probe=%lldms",
          qPrintable(probe.version), probe.loaded, probe.playbinSwitchable, probe.capabilities.adaptive,
          probe.capabilities.h264.hardware, probe.capabilities.vp9.hardware, static_cast<long long>(probe.elapsedMs));
    timing.mark(QStringLiteral("gst-probe"));
    playback.setCapabilities(probe.capabilities);
    downloads::DownloadManagerConfig downloadConfig;
    downloadConfig.databasePath = libraryStore.databasePath();
    downloadConfig.engine.decoders = probe.capabilities;
    downloads::DownloadManager downloadManager(downloadConfig, &downloadResolver, &remuxer);
    QString downloadError;
    if (!downloadManager.open(&downloadError)) qWarning("sailtube: downloads unavailable: %s", qPrintable(downloadError));
    trace.mark("downloads-open");
    // The "Download quality" setting decides what a download starts as (audio only, or a height);
    // the playback ceiling only fills in when the setting names no height.
    const auto downloadCeiling = [&settings, &downloadManager]() {
        downloadManager.setDownloadDefaults(settings.value(QStringLiteral("downloads/quality")).toInt(), settings.maxHeight());
    };
    downloadCeiling();
    QObject::connect(&settings, &sailfish::AppSettings::changed, &downloadManager, downloadCeiling);
    sailfish::DownloadsModel downloadsModel(&downloadManager, !storeVariant);
    // PLY-26 / DL-07: a downloaded video opens from the device (no resolve, works offline).
    playback.setLocalMediaLookup([&downloadManager](const QString &id) { return downloadManager.localMedia(id); });

    // Account rows (library component): optional, never blocks anything (ACC-01). No vault adapter
    // yet (SailTune's is to be pinned): FULL keeps the session in a 0600 file with a one-time
    // disclosure, STORE refuses to store it (ACC-10). The paste parser comes with the same pin.
    account::AccountStoreOptions accountOptions;
    accountOptions.variant = storeVariant ? account::BuildVariant::Store : account::BuildVariant::Full;
    accountOptions.privateDir = dataRoot + QStringLiteral("/account");
    account::AccountStore accountStore(accountOptions);
    accountStore.open();
    account::NetworkAuthTransport accountTransport;
    account::AccountService accountService(&accountStore, &accountTransport);
    // ACC-11 / RES-08: the extractor's cookie rung. A per-call 0600 jar while signed-in features are on
    // (empty otherwise); released exactly once per call so rotations fold back and the jar is deleted.
    {
        const auto cookiesBegin = [&accountService]() {
            const account::AccountService::ExtractorCall c = accountService.beginExtractorCall();
            resolver::ExtractorCookies e;
            e.jarPath = c.jarPath;
            e.generation = c.generation;
            return e;
        };
        const auto cookiesEnd = [&accountService](const resolver::ExtractorCookies &e) {
            account::AccountService::ExtractorCall c;
            c.jarPath = e.jarPath;
            c.generation = e.generation;
            accountService.endExtractorCall(c);
        };
        runner.setCookieHooks(cookiesBegin, cookiesEnd);
        downloadRunner.setCookieHooks(cookiesBegin, cookiesEnd);
    }
    accountService.setRequestLocale(QLocale::system().name().section(QLatin1Char('_'), 0, 0),
                             QLocale::system().name().section(QLatin1Char('_'), 1, 1));
    sailfish::AccountController::Options accountUi;
    accountUi.variant = accountOptions.variant;
    accountUi.home = QDir::homePath();
    accountUi.scratchDir = accountOptions.privateDir + QStringLiteral("/scratch");
    account::PinnedSessionParser sessionParser;   // ACC-04: SailTune's parser, pinned
    sailfish::AccountController accountController(&accountStore, &accountService, &sessionParser, accountUi);
    // IMP-02: the account import is offered only while a verified session exists.
    const auto accountImport = [&importer, &accountService]() {
        const bool on = accountService.state() == account::SessionState::SignedIn;
        importer.setAccountTransport(on ? &accountService : nullptr);
        importer.setAccountIdentity(accountService.selection().authUser,
                                    QString::fromStdString(accountService.selection().pageId));
    };
    QObject::connect(&accountService, &account::AccountService::stateChanged, &importer, accountImport);
    QObject::connect(&accountService, &account::AccountService::accountsChanged, &importer, accountImport);
    accountService.start();

    qmlRegisterUncreatableType<sailfish::PlaybackSession>("org.nordilabs.sailtube", 1, 0, "PlaybackSession",
                                                          QStringLiteral("Provided by the application"));
    // The playback component's player in QML: VideoSurfaceItem { source: playback.mediaObject } and the overlay (PLY-17).
    qmlRegisterType<nordtile::player::VideoSurfaceItem>("org.nordilabs.sailtube", 1, 0, "VideoSurfaceItem");
    // Cover video: the same player's frames, live in the active cover (~15 fps, only while active).
    qmlRegisterType<nordtile::player::CoverVideoItem>("org.nordilabs.sailtube", 1, 0, "CoverVideoItem");
    qmlRegisterType<nordtile::player::OverlayController>("org.nordilabs.sailtube", 1, 0, "OverlayController");
    qmlRegisterUncreatableType<nordtile::player::VideoPlayer>("org.nordilabs.sailtube", 1, 0, "VideoPlayer",
                                                             QStringLiteral("Use playback.mediaObject"));
    // Outlives the view: the engine keeps a pointer to the factory.
    ThumbnailNetworkFactory thumbnails(cacheRoot + QStringLiteral("/thumbnails"));
    timing.mark(QStringLiteral("models"));
    QScopedPointer<QQuickView> view(SailfishApp::createView());
    trace.mark("view-created");
    // PLT-07: on libhybris the GL context can be lost while the display is off;
    // do not keep GL resources across it, and repaint shortly after waking.
    view->setPersistentOpenGLContext(false);
    view->setPersistentSceneGraph(false);
    // The video surface re-uploads its frame on the new context (PLY-19) when
    // the session asks for a repaint; PLY-09 freeze follows the activation.
    QQuickView *viewPointer = view.data();
    sailfish::PlaybackSession *playbackPointer = &playback;
    QObject::connect(app.data(), &QGuiApplication::applicationStateChanged, viewPointer,
                     [viewPointer, playbackPointer](Qt::ApplicationState state) {
        playbackPointer->setAppActive(state == Qt::ApplicationActive);
        if (state == Qt::ApplicationActive)
            QTimer::singleShot(250, viewPointer, [viewPointer, playbackPointer]() {
                viewPointer->update();
                emit playbackPointer->repaintRequested();
            });
    });
    view->engine()->setNetworkAccessManagerFactory(&thumbnails);
    QQmlContext *context = view->rootContext();
    context->setContextProperty(QStringLiteral("appSettings"), &settings);
    context->setContextProperty(QStringLiteral("searchModel"), &search);
    context->setContextProperty(QStringLiteral("playback"), &playback);
    context->setContextProperty(QStringLiteral("subscriptionsModel"), &subscriptions);
    context->setContextProperty(QStringLiteral("playlistsModel"), &playlists);
    context->setContextProperty(QStringLiteral("historyModel"), &history);
    context->setContextProperty(QStringLiteral("watchIndex"), &watchIndex);
    context->setContextProperty(QStringLiteral("libraryPlayback"), &libraryPlayback);
    context->setContextProperty(QStringLiteral("importer"), &importer);
    context->setContextProperty(QStringLiteral("account"), &accountController);
    context->setContextProperty(QStringLiteral("downloads"), &downloadsModel);
    context->setContextProperty(QStringLiteral("storeVariant"), storeVariant);
    context->setContextProperty(QStringLiteral("deviceCapabilities"), probe.toVariant());
    context->setContextProperty(QStringLiteral("resolverAvailable"), service.available());
    context->setContextProperty(QStringLiteral("javascriptRuntimeAvailable"), !runnerOptions.javascriptRuntime.isEmpty());
    context->setContextProperty(QStringLiteral("appVersion"), QStringLiteral(SAILTUBE_VERSION));
#ifdef SAILTUBE_BETA
    context->setContextProperty(QStringLiteral("beta"), &betaStatus);   // docs/BETA.md
#endif
#ifndef SAILTUBE_STORE
    context->setContextProperty(QStringLiteral("extractorUpdater"), &extractorUpdater);   // NFR-MNT-01 [FULL]
#endif
    context->setContextProperty(QStringLiteral("startupTiming"), &timing);
    // Catalog component: discovery, links, captions, SponsorBlock (replaces searchModel and the image factory).
    // Owned by the application object so it outlives the view's QML teardown.
    // The catalog's yt-dlp search tier (DSC-01 second tier) runs on the background warm lane on a
    // runner of its own, so a search never delays a tap-to-play resolve nor waits behind a download.
    resolver::RunnerOptions searchRunnerOptions = runnerOptions;
    searchRunnerOptions.maximumConcurrent = 1;
    searchRunnerOptions.warmLane = 1;
    resolver::YtDlpRunner searchRunner(searchRunnerOptions);
    resolver::YtDlpVideoService searchService(&searchRunner);
    catalog::CatalogComposition *catalogParts = new catalog::CatalogComposition(&searchService, cacheRoot,
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation), app.data());
    catalogParts->connectLibrary(&libraryStore, &watchIndex);   // feed, remote playlists, handles, hide watched
    catalogParts->connectDownloads(&downloadManager);            // DL-07 offline captions and segments
    catalogParts->connectAccount(&accountService);               // ACC-11 signed home, Watch later, Liked
    catalogParts->expose(view->engine(), context);
    trace.mark("catalog-exposed");
    // SEL-06 / DL-07: playback.captionTracks / captionText through the catalog's caption models
    // (the same captionTracks / captionCues the captions page uses). The adapter is the session's
    // child; the models (the composition's, owned by the application object) outlive it.
    {
        sailfish::CaptionTracksModel *captionTracks = qobject_cast<sailfish::CaptionTracksModel *>(
            context->contextProperty(QStringLiteral("captionTracks")).value<QObject *>());
        sailfish::CaptionCues *captionCues = qobject_cast<sailfish::CaptionCues *>(
            context->contextProperty(QStringLiteral("captionCues")).value<QObject *>());
        if (captionTracks && captionCues)
            playback.setCaptionSource(new sailfish::CatalogCaptionSource(captionTracks, captionCues, &playback));
    }
    // E12 / RES-11: catalog's likely-next-tap signals; playback decides (direct connections, so
    // `opened` reaches the session before the page's playback.open()).
    if (sailfish::TapIntent *intent = catalogParts->tapIntent()) {
        QObject::connect(intent, &sailfish::TapIntent::touchDown, &playback, &sailfish::PlaybackSession::pressStarted, Qt::DirectConnection);
        QObject::connect(intent, &sailfish::TapIntent::touchCancelled, &playback, &sailfish::PlaybackSession::pressCancelled, Qt::DirectConnection);
        QObject::connect(intent, &sailfish::TapIntent::opened, &playback, &sailfish::PlaybackSession::openingFromPress, Qt::DirectConnection);
        QObject::connect(intent, &sailfish::TapIntent::likelyVisible, &playback, &sailfish::PlaybackSession::rowsVisible, Qt::DirectConnection);
    }
    // NFR-PERF-02 / RES-21: once the first rows are on screen ("content"), warm the shared resolver
    // (visitor id, connection) and preload the decoder plugins; both run on worker threads. A 4 s
    // fallback covers an empty feed or no network.
    {
        nordtile::player::ThreadedStreamSource *sourcePointer = &streamSource;
        QSharedPointer<bool> warmed(new bool(false));
        const auto afterContent = [sourcePointer, warmed]() {
            if (*warmed) return;
            *warmed = true;
            sourcePointer->warmUp();
        };
        QObject::connect(&timing, &sailfish::StartupTiming::marked, app.data(), [afterContent](const QString &stage) {
            if (stage == QLatin1String("content")) afterContent();
        });
        if (timing.hasMark(QStringLiteral("content"))) afterContent();
        QTimer::singleShot(4000, app.data(), afterContent);
        // Decoder preload (NFR-PERF-02): after the first UI frame, not at "content". Loading libav
        // takes the dynamic loader's lock, which the first paint's Qt/QML library loads also need
        // (run 6: decoders-preloaded landed 1 ms before first-frame); 4 s fallback.
        sailfish::runOnceAfterFirst(viewPointer, &QQuickWindow::frameSwapped, app.data(), []() {
            QTimer::singleShot(500, QCoreApplication::instance(), []() { nordtile::player::GstEngine::preloadDecoders(); });
        });
        QTimer::singleShot(4000, app.data(), []() { nordtile::player::GstEngine::preloadDecoders(); });
        QObject::connect(nordtile::player::GstEngineInit::instance(), &nordtile::player::GstEngineInit::decodersPreloaded,
                         app.data(), [](int loaded, qint64 ms) {
            qInfo("sailtube: preloaded %d GStreamer features in %lld ms", loaded, static_cast<long long>(ms));
            nordtile::video::sailfish::timingMark("decoders-preloaded", QStringLiteral("count=%1 ms=%2").arg(loaded).arg(ms));
        });
    }
    // NFR-PERF-02: "first-frame" is the swap after the surface got its first frame.
    QObject::connect(viewPointer, &QQuickWindow::frameSwapped, viewPointer,
                     [playbackPointer]() { playbackPointer->frameSwapped(); }, Qt::DirectConnection);
    // Development only: QML from a checkout (the container has no installed package).
    const QByteArray qmlRoot = qgetenv("SAILTUBE_QML_ROOT");
    view->setSource(qmlRoot.isEmpty() ? SailfishApp::pathToMainQml()
                                      : QUrl::fromLocalFile(QString::fromLocal8Bit(qmlRoot) + QStringLiteral("/harbour-sailtube.qml")));
    timing.mark(QStringLiteral("qml-loaded"));
    trace.mark("qml-scene");
    // NFR-PERF-01: cold start to the first Silica frame, read from the journal on the device.
    QMetaObject::Connection *firstFrame = new QMetaObject::Connection;
    *firstFrame = QObject::connect(view.data(), &QQuickWindow::frameSwapped, view.data(), [firstFrame, startClock, &timing]() {
        qInfo("sailtube: first frame %lld ms after main()", static_cast<long long>(startClock.elapsed()));
        timing.mark(QStringLiteral("first-frame"));
        QObject::disconnect(*firstFrame);
        delete firstFrame;
    });
    if (trace.enabled()) {
        QObject::connect(view.data(), &QQuickWindow::frameSwapped, app.data(), [&trace]() {
            static bool first = true;
            if (!first) return;
            first = false;
            trace.mark("first-frame");
            if (trace.exitAfterScene()) QTimer::singleShot(0, QCoreApplication::instance(), []() { QCoreApplication::exit(0); });
        });
        if (trace.exitAfterScene() && QGuiApplication::platformName() == QLatin1String("minimal")) {
            std::fprintf(stderr, "sailtube-startup qml status %d, root %s\n", static_cast<int>(view->status()),
                         view->rootObject() ? "created" : "missing");
            return view->rootObject() ? 0 : 3;   // no GL here: the scene is the last measurable phase
        }
    }
    // RES-01: start the latency worker once the first frame is on screen, so its import never competes
    // with start-up; the background lane starts on its first call.
    if (warmAvailable) {
        QMetaObject::Connection *warmFrame = new QMetaObject::Connection;
        *warmFrame = QObject::connect(view.data(), &QQuickWindow::frameSwapped, &warmResolver, [warmFrame, &warmResolver]() {
            QObject::disconnect(*warmFrame);
            delete warmFrame;
            QTimer::singleShot(1500, &warmResolver, [&warmResolver]() { warmResolver.warmUp(worker::Lane::Latency); });
        });
    }
    view->show();
    // Warm-up (RES-05, RES-21) starts after the "content" mark (above), not on the first frame.
    const int code = app->exec();
    // The account service is destroyed before the runners: hand back every jar still out first.
    runner.dropCookieHooks();
    downloadRunner.dropCookieHooks();
    libraryPlayback.flush();
    playback.stop();
    return code;
}
