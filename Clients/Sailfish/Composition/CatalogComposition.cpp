// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CatalogComposition.h"

#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Core/Catalog/Infrastructure/CaptionTrackService.h"
#include "Core/Catalog/Infrastructure/ShortDetailService.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"
#include "Core/Catalog/Infrastructure/DislikeService.h"
#include "Core/Catalog/Infrastructure/FeedCacheStore.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/HomeService.h"
#include "Core/Catalog/Infrastructure/HttpTransport.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/RssParser.h"
#include "Core/Catalog/Infrastructure/SearchService.h"
#include "Core/Catalog/Infrastructure/ShortsStore.h"
#include "Core/Catalog/Infrastructure/SponsorBlockService.h"
#include "Core/Catalog/Infrastructure/SuggestService.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Core/Catalog/Infrastructure/YtDlpSearchFallback.h"
#include "Clients/Sailfish/Presentation/CaptionCues.h"
#include "Clients/Sailfish/Presentation/CatalogLibraryBridge.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Clients/Sailfish/Presentation/ChannelModel.h"
#include "Clients/Sailfish/Presentation/CommentsModel.h"
#include "Clients/Sailfish/Presentation/DBusLinkAdaptor.h"
#include "Clients/Sailfish/Presentation/DiscoverySearchModel.h"
#include "Clients/Sailfish/Presentation/DislikeInfo.h"
#include "Clients/Sailfish/Presentation/FeedModel.h"
#include "Clients/Sailfish/Presentation/HomeSettings.h"
#include "Clients/Sailfish/Presentation/ImageCacheFactory.h"
#include "Clients/Sailfish/Presentation/LinkRouter.h"
#include "Clients/Sailfish/Presentation/PlaylistModel.h"
#include "Clients/Sailfish/Presentation/SponsorSegmentsModel.h"
#include "Clients/Sailfish/Presentation/SuggestionModel.h"
#include "Clients/Sailfish/Presentation/TapIntent.h"
#include "Clients/Sailfish/Presentation/SettingsFile.h"
#include "Clients/Sailfish/Presentation/SignedCatalog.h"
#include "Clients/Sailfish/Presentation/QueueSeeder.h"
#include "Clients/Sailfish/Presentation/ShortsModel.h"
#include "Clients/Sailfish/Presentation/WatchedRows.h"
#include "Core/Catalog/Infrastructure/ShortsFeedService.h"
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/SignedCatalogTransport.h"
#include "Clients/Sailfish/Presentation/CatalogText.h"
#include "Clients/Sailfish/Presentation/ChannelAvatars.h"
#include "Clients/Sailfish/Presentation/LargeThumbnails.h"
#include "Clients/Sailfish/Composition/LibraryCatalogGlue.h"
#include "Core/Downloads/Infrastructure/DownloadManager.h"
#include "Clients/Sailfish/Presentation/WatchCaptions.h"
#include "Clients/Sailfish/Presentation/WatchDislikes.h"
#include "Clients/Sailfish/Presentation/AppSettings.h"
#include "Clients/Sailfish/Presentation/ContentFilters.h"
#include "Clients/Sailfish/Presentation/WatchInfo.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QTimer>
#include <QDBusConnection>
#include <QPointer>
#include <QQmlContext>
#include <QQmlEngine>
#include <QSettings>
#include <QtQml>

namespace nordtile { namespace video { namespace catalog {

// Declared in member order = construction order; destroyed in reverse, so models go before the
// services they use and services before the lanes and transports.
struct CatalogComposition::Parts {
    CatalogCounters counters;
    DiscoveryLocale locale = DiscoveryLocale::fromSystem();
    NetworkTransport interactive{QStringLiteral("discovery-interactive")};
    NetworkTransport background{QStringLiteral("discovery-background")};
    WorkLane interactiveLane{QStringLiteral("discovery-interactive"), 2};
    WorkLane backgroundLane{QStringLiteral("discovery-background"), 1};
    QScopedPointer<ClientIdentity> identity;
    QScopedPointer<InnerTubeClient> interactiveClient;
    QScopedPointer<InnerTubeClient> backgroundClient;
    QScopedPointer<FeedCacheStore> feedCache;

    QScopedPointer<ShortsStore> shorts;
    QScopedPointer<SearchService> search;
    QScopedPointer<SuggestService> suggest;
    QScopedPointer<FeedService> feed;
    QScopedPointer<HomeService> home;
    QScopedPointer<ChannelService> channels;
    QScopedPointer<PlaylistService> playlists;
    QScopedPointer<WatchNextService> watchNext;
    QScopedPointer<CommentsService> comments;
    QScopedPointer<CaptionService> captions;
    QScopedPointer<SponsorBlockService> sponsor;
    QScopedPointer<DislikeService> dislikes;
    // Library hooks run on the background lane: handle resolution and mirror walks (RES-02).
    QScopedPointer<ChannelService> backgroundChannels;
    QScopedPointer<PlaylistService> backgroundPlaylists;

    // PLT-03: the application's one settings file (AppSettings' file), never a default QSettings.
    QSettings settings{sailfish::appSettingsFile(), QSettings::IniFormat};
    QScopedPointer<sailfish::ImageCacheFactory> images;
    QScopedPointer<sailfish::DiscoverySearchModel> searchModel;
    QScopedPointer<sailfish::SuggestionModel> suggestionModel;
    QScopedPointer<sailfish::FeedModel> feedModel;
    QScopedPointer<sailfish::RecommendedModel> recommendedModel;
    QScopedPointer<sailfish::HomeSettings> homeSettings;
    QScopedPointer<sailfish::WatchInfo> watchInfo;
    QScopedPointer<sailfish::CommentsModel> commentsModel;
    QScopedPointer<sailfish::CaptionTracksModel> captionTracks;
    QScopedPointer<sailfish::CaptionCues> captionCues;
    QScopedPointer<CaptionTrackService> captionTrackList;
    QScopedPointer<sailfish::WatchCaptions> watchCaptions;
    QScopedPointer<sailfish::SponsorSegmentsModel> sponsorSegments;
    QScopedPointer<sailfish::DislikeInfo> dislikeInfo;
    QScopedPointer<sailfish::WatchDislikes> watchDislikes;
    QScopedPointer<sailfish::ContentFilters> contentFilters;
    QScopedPointer<sailfish::LinkRouter> links;
    QScopedPointer<sailfish::DBusLinkService> linkService;
    QScopedPointer<sailfish::CatalogLibraryBridge> library;
    QScopedPointer<sailfish::TapIntent> tapIntent;
    QScopedPointer<sailfish::CatalogText> text;
    QScopedPointer<sailfish::QueueSeeder> queueSeeder;   // LNK-04
    // The Shorts section (user-requested 2026-11-08).
    QScopedPointer<ShortsFeedService> shortsFeed;
    QScopedPointer<ShortDetailService> shortDetails;
    QScopedPointer<sailfish::ShortsModel> shortsModel;
    // Downloads' offline extras: their own SponsorBlock client on the background lane (RES-02).
    QScopedPointer<SponsorBlockService> backgroundSponsor;
    QScopedPointer<sailfish::CatalogSidecarSource> sidecar;
    // ACC-11: the signed transport and the services over it (destroyed before home / identity).
    QScopedPointer<account::SignedCatalogTransport> signedTransport;
    QScopedPointer<sailfish::SignedCatalog> signedCatalog;
};

namespace {
// The link router and its D-Bus service claimed early in main() (claimLinkService).
struct EarlyLinks {
    sailfish::LinkRouter *router = nullptr;
    sailfish::DBusLinkService *service = nullptr;
};
EarlyLinks &earlyLinks() { static EarlyLinks links; return links; }

// Before the settings are opened: an older build wrote the catalog keys (search history, caption
// and RYD choices) through a default QSettings, one level above the config directory.
bool migrateLegacySettings() {
    const int moved = sailfish::migrateSettingsFile(sailfish::legacyDefaultSettingsFile(), sailfish::appSettingsFile());
    if (moved > 0) qInfo("sailtube: moved %d catalog settings into %s", moved, qPrintable(sailfish::appSettingsFile()));
    return true;
}
}

CatalogComposition::CatalogComposition(resolver::YtDlpVideoService *extractor, const QString &cacheRoot,
                                       const QString &dataRoot, QObject *parent)
    : QObject(parent), d((migrateLegacySettings(), new Parts)) {
    Parts &p = *d;
    // Links first: the bus name is claimed early so activations reach this instance; main()
    // normally claimed it already (claimLinkService) and the composition adopts it here.
    EarlyLinks &early = earlyLinks();
    if (early.router) {
        p.links.reset(early.router);
        p.linkService.reset(early.service);
        early = EarlyLinks();
    } else {
        p.links.reset(new sailfish::LinkRouter);
        p.linkService.reset(new sailfish::DBusLinkService(p.links.data()));
        if (!p.linkService->registerOn(QDBusConnection::sessionBus()))
            qWarning("sailtube: D-Bus link service not registered (another instance or no session bus)");
    }

    p.identity.reset(new ClientIdentity(&p.interactive, cacheRoot + QStringLiteral("/catalog/client.json")));
    p.identity->start();
    p.interactiveClient.reset(new InnerTubeClient(&p.interactive, p.identity.data(), &p.interactiveLane, p.locale));
    p.backgroundClient.reset(new InnerTubeClient(&p.background, p.identity.data(), &p.backgroundLane, p.locale));

    p.shorts.reset(new ShortsStore(p.backgroundClient.data(), dataRoot + QStringLiteral("/catalog/shorts.txt")));
    p.shorts->load();
    const ShortsLookup shortsLookup = p.shorts->lookup();

    p.search.reset(new SearchService(p.interactiveClient.data(), &p.counters));
    if (extractor) p.search->setFallback(makeYtDlpSearchFallback(extractor));
    p.suggest.reset(new SuggestService(&p.interactive, p.locale, &p.counters));

    p.feedCache.reset(new FeedCacheStore(cacheRoot + QStringLiteral("/feed/feed.json")));
    p.feed.reset(new FeedService(&p.background, p.backgroundClient.data(), p.feedCache.data()));
    p.feed->setCounters(&p.counters);
    p.feed->setShortsLookup(shortsLookup);
    p.feed->setAutoRefresh(true);
    p.home.reset(new HomeService(p.interactiveClient.data(), false));
    p.home->setCounters(&p.counters);

    p.channels.reset(new ChannelService(p.interactiveClient.data(), p.shorts.data(), &p.counters));
    NetworkTransport *rssTransport = &p.interactive;   // a channel page's first paint is interactive
    p.channels->setRssFetch([rssTransport](const QString &channelId, QObject *context, ChannelService::RssDone done) {
        HttpRequest request;
        request.url = RssParser::feedUrl(channelId);
        request.timeoutMs = FeedService::kRssTimeoutMs;
        QPointer<QObject> guard(context);
        rssTransport->send(request, context, [guard, done](const HttpReply &reply) {
            if (!guard) return;
            const RssFeed feed = RssParser::parse(reply.body, reply.status);
            done(feed.outcome, feed.items);
        });
    });
    p.playlists.reset(new PlaylistService(p.interactiveClient.data(), &p.counters));
    p.watchNext.reset(new WatchNextService(p.interactiveClient.data(), &p.counters));
    p.watchNext->setShortsLookup(shortsLookup);
    p.comments.reset(new CommentsService(p.backgroundClient.data(), &p.counters));
    p.captions.reset(new CaptionService(&p.interactive, &p.interactiveLane));
    p.captions->setCounters(&p.counters);
    p.sponsor.reset(new SponsorBlockService(&p.interactive, &p.interactiveLane));
    p.sponsor->setCounters(&p.counters);
    p.dislikes.reset(new DislikeService(&p.background));
    p.dislikes->setCounters(&p.counters);
    p.backgroundChannels.reset(new ChannelService(p.backgroundClient.data(), p.shorts.data(), &p.counters));
    p.backgroundPlaylists.reset(new PlaylistService(p.backgroundClient.data(), &p.counters));
    p.library.reset(new sailfish::CatalogLibraryBridge(p.feed.data(), p.backgroundPlaylists.data(), p.backgroundChannels.data()));

    p.images.reset(new sailfish::ImageCacheFactory(sailfish::ImageCacheFactory::defaultDirectory()));
    p.searchModel.reset(new sailfish::DiscoverySearchModel(p.search.data()));
    p.searchModel->setShortsLookup(shortsLookup);
    p.suggestionModel.reset(new sailfish::SuggestionModel(p.suggest.data(), &p.settings));
    // Instant first paint of the search page: the last page 1 from disk, refreshed in place when
    // the page opens. It is search history, so it follows the history switch and its clearing.
    sailfish::SuggestionModel *suggestions = p.suggestionModel.data();
    p.searchModel->setSnapshot(cacheRoot + QStringLiteral("/catalog/last_search.json"),
                               [suggestions] { return !suggestions->historyPaused(); });
    p.searchModel->restoreSnapshot();
    sailfish::DiscoverySearchModel *searchModel = p.searchModel.data();
    connect(suggestions, &sailfish::SuggestionModel::historyChanged, searchModel, [suggestions, searchModel] {
        if (suggestions->historyCount() == 0 || suggestions->historyPaused()) searchModel->clearSnapshot();
    });
    p.feedModel.reset(new sailfish::FeedModel(p.feed.data()));
    p.recommendedModel.reset(new sailfish::RecommendedModel(p.home.data()));
    p.homeSettings.reset(new sailfish::HomeSettings);
    sailfish::ChannelModel::setDefaultService(p.channels.data());
    sailfish::PlaylistModel::setDefaultService(p.playlists.data());
    sailfish::WatchInfo::setDefaultService(p.watchNext.data());
    p.watchInfo.reset(new sailfish::WatchInfo(p.watchNext.data()));
    p.commentsModel.reset(new sailfish::CommentsModel(p.comments.data()));
    p.captionTracks.reset(new sailfish::CaptionTracksModel(&p.settings));
    p.captionCues.reset(new sailfish::CaptionCues(p.captions.data()));
    p.captionCues->follow(p.captionTracks.data());
    // SEL-06: the playing streamed video's caption track list (asked for with its watch data).
    p.captionTrackList.reset(new CaptionTrackService(p.interactiveClient.data()));
    p.captionTrackList->setCounters(&p.counters);
    p.watchCaptions.reset(new sailfish::WatchCaptions(p.watchInfo.data(), p.captionTrackList.data(), p.captionTracks.data()));
    p.sponsorSegments.reset(new sailfish::SponsorSegmentsModel(p.sponsor.data()));
    p.dislikeInfo.reset(new sailfish::DislikeInfo(p.dislikes.data(), &p.settings));
    // RYD-01: the playing video's estimate on watchInfo (asked for with its watch data, and only
    // while the setting is on).
    p.watchDislikes.reset(new sailfish::WatchDislikes(p.watchInfo.data(), p.dislikeInfo.data()));
    // SET-01 "Hide Shorts" for the feed, Home and related (search and the channel page take it
    // in QML). The value arrives in expose(), from the application settings.
    p.contentFilters.reset(new sailfish::ContentFilters);
    {
        FeedService *feed = p.feed.data();
        HomeService *home = p.home.data();
        WatchNextService *related = p.watchNext.data();
        p.home->setShortsLookup(shortsLookup);
        p.contentFilters->addList([feed] { return feed->rowFilter(); },
                                  [feed](const RowFilterSettings &s) { feed->setRowFilter(s); });
        p.contentFilters->addList([home] { return home->rowFilter(); },
                                  [home](const RowFilterSettings &s) { home->setRowFilter(s); });
        p.contentFilters->addList([related] { return related->rowFilterSettings(); },
                                  [related](const RowFilterSettings &s) { related->setRowFilterSettings(s); });
    }

    // The library component feeds the subscription list (FeedModel::setChannels); the feed starts
    // with the cached state and refreshes due channels in the background.
    p.feed->start();
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] { d->feed->saveSync(); });

    // E12: likely-next-tap signals for playback's speculative resolve. A tap that opens a video
    // also drops the search's next-page prefetch, so the video's first bytes come first.
    p.tapIntent.reset(new sailfish::TapIntent);
    // Generated counts and ages in the discovery (device) language, as YouTube writes them.
    p.text.reset(new sailfish::CatalogText(p.locale.hl));
    p.queueSeeder.reset(new sailfish::QueueSeeder(p.playlists.data()));
    sailfish::CatalogRowsModel::setLanguage(p.locale.hl);
    p.tapIntent->setActive([] {
        QGuiApplication *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
        return !gui || gui->applicationState() == Qt::ApplicationActive;
    });
    connect(p.tapIntent.data(), &sailfish::TapIntent::opened, p.searchModel.data(), [searchModel] { searchModel->cancelPrefetch(); });

    // Shorts: subscriptions' Shorts tabs, then popular Shorts for the locale; signed in, the
    // account's own sequence (SignedCatalog). The library bridge hands it the subscriptions.
    p.shortsFeed.reset(new ShortsFeedService(p.interactiveClient.data(), p.channels.data(), &p.counters));
    p.shortsModel.reset(new sailfish::ShortsModel(p.shortsFeed.data(), p.tapIntent.data()));
    // The Short on screen's on-picture data: one details request per Short (the one on screen and
    // the next), the dislike estimate of the one on screen while that setting is on.
    p.shortDetails.reset(new ShortDetailService(p.interactiveClient.data(), &p.counters));
    p.shortsModel->setDetails(p.shortDetails.data());
    p.shortsModel->setDislikes(p.dislikes.data(), p.dislikeInfo.data());
    p.library->setShortsFeed(p.shortsFeed.data());
    ShortsFeedService *shortsFeed = p.shortsFeed.data();
    connect(sailfish::WatchedRows::instance(), &sailfish::WatchedRows::changed, shortsFeed, [shortsFeed] {
        RowFilterSettings s;
        s.hideWatched = sailfish::WatchedRows::instance()->forContext(QStringLiteral("feed"));
        shortsFeed->setRowFilter(s);
    });

    // NFR-PERF-09: the first interactive request skips the TCP + TLS handshake. Once shortly
    // after start (after the playback component's own first requests) and whenever the app comes back
    // to the foreground; the transport limits it to once a minute.
    NetworkTransport *interactive = &p.interactive;
    QTimer::singleShot(1500, this, [interactive] { interactive->warm(QStringLiteral("www.youtube.com")); });
    if (QGuiApplication *gui = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
        connect(gui, &QGuiApplication::applicationStateChanged, this, [interactive](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive) interactive->warm(QStringLiteral("www.youtube.com"));
        });
}

CatalogComposition::~CatalogComposition() {
    d->feed->saveSync();
    d->interactiveLane.drain();
    d->backgroundLane.drain();
}

sailfish::TapIntent *CatalogComposition::tapIntent() const { return d->tapIntent.data(); }

void CatalogComposition::claimLinkService() {
    EarlyLinks &early = earlyLinks();
    if (early.router) return;
    early.router = new sailfish::LinkRouter;
    early.service = new sailfish::DBusLinkService(early.router);
    if (!early.service->registerOn(QDBusConnection::sessionBus()))
        qWarning("sailtube: D-Bus link service not registered (another instance or no session bus)");
}

void CatalogComposition::connectAccount(account::AccountService *accountService) {
    if (!accountService || d->signedCatalog) return;
    Parts &p = *d;
    p.signedTransport.reset(new account::SignedCatalogTransport(accountService));
    p.signedCatalog.reset(new sailfish::SignedCatalog(p.signedTransport.data(), p.identity.data(), p.locale, p.home.data(),
                                                      p.interactiveClient.data(), &p.counters));
    sailfish::SignedCatalog *signedCatalog = p.signedCatalog.data();
    signedCatalog->setShortsFeed(p.shortsFeed.data());
    QPointer<account::AccountService> account(accountService);
    signedCatalog->setSignedIn(accountService->canSign());
    // DSC-04: the landing choice "Recommended" is offered and honoured while signed in.
    sailfish::HomeSettings *homeSettings = p.homeSettings.data();
    homeSettings->setSignedIn(accountService->canSign());
    connect(accountService, &account::AccountService::stateChanged, signedCatalog, [signedCatalog, homeSettings, account] {
        if (!account) return;
        signedCatalog->setSignedIn(account->canSign());
        homeSettings->setSignedIn(account->canSign());
    });
    connect(accountService, &account::AccountService::cachesInvalidated, signedCatalog, [signedCatalog] { signedCatalog->invalidate(); });
}

void CatalogComposition::connectDownloads(downloads::DownloadManager *manager) {
    if (!manager) return;
    Parts &p = *d;
    if (!p.sidecar) {
        p.backgroundSponsor.reset(new SponsorBlockService(&p.background, &p.backgroundLane));
        p.backgroundSponsor->setCounters(&p.counters);
        p.sidecar.reset(new sailfish::CatalogSidecarSource(p.backgroundClient.data(), &p.background,
                                                          p.backgroundSponsor.data(), p.captionTracks.data()));
    }
    // The composition outlives the manager (main.cpp local); the manager only calls into it.
    manager->setSidecarSource(p.sidecar.data());
}

void CatalogComposition::connectLibrary(library::LibraryStore *store, sailfish::WatchStateIndex *watch) {
    d->library->attach(store, watch);
}

void CatalogComposition::expose(QQmlEngine *engine, QQmlContext *context) {
    Parts &p = *d;
    engine->setNetworkAccessManagerFactory(p.images.data());
    qmlRegisterType<sailfish::ChannelModel>("org.nordilabs.sailtube", 1, 0, "ChannelModel");
    qmlRegisterType<sailfish::PlaylistModel>("org.nordilabs.sailtube", 1, 0, "PlaylistModel");
    qmlRegisterType<sailfish::WatchInfo>("org.nordilabs.sailtube", 1, 0, "WatchInfo");
    context->setContextProperty(QStringLiteral("searchModel"), p.searchModel.data());
    context->setContextProperty(QStringLiteral("suggestionModel"), p.suggestionModel.data());
    context->setContextProperty(QStringLiteral("feedModel"), p.feedModel.data());
    context->setContextProperty(QStringLiteral("recommendedModel"), p.recommendedModel.data());
    context->setContextProperty(QStringLiteral("homeSettings"), p.homeSettings.data());
    context->setContextProperty(QStringLiteral("watchInfo"), p.watchInfo.data());
    context->setContextProperty(QStringLiteral("commentsModel"), p.commentsModel.data());
    context->setContextProperty(QStringLiteral("captionTracks"), p.captionTracks.data());
    context->setContextProperty(QStringLiteral("captionCues"), p.captionCues.data());
    context->setContextProperty(QStringLiteral("sponsorSegments"), p.sponsorSegments.data());
    context->setContextProperty(QStringLiteral("dislikes"), p.dislikeInfo.data());
    context->setContextProperty(QStringLiteral("linkRouter"), p.links.data());
    context->setContextProperty(QStringLiteral("remotePlaylists"), p.library.data());
    context->setContextProperty(QStringLiteral("tapIntent"), p.tapIntent.data());
    context->setContextProperty(QStringLiteral("catalogText"), p.text.data());
    // SET-01: the feed, Home and related follow the app's "Hide Shorts" switch (main.cpp sets
    // `appSettings` before this call). Read once now and on every change of that key.
    if (sailfish::AppSettings *app = qobject_cast<sailfish::AppSettings *>(
            context->contextProperty(QStringLiteral("appSettings")).value<QObject *>())) {
        sailfish::ContentFilters *filters = p.contentFilters.data();
        const QString key = QLatin1String(sailfish::ContentFilters::kHideShortsKey);
        filters->setHideShorts(app->value(key).toBool());
        connect(app, &sailfish::AppSettings::valueChanged, filters, [app, filters, key](const QString &changed) {
            if (changed == key) filters->setHideShorts(app->value(key).toBool());
        });
    } else {
        qWarning("sailtube-catalog: no appSettings in the QML context; Hide Shorts stays off for the feed, Home and related");
    }
    // Channel avatars by id for lists of other components (the catalog models have a channelAvatar role).
    context->setContextProperty(QStringLiteral("channelAvatars"), sailfish::ChannelAvatars::instance());
    context->setContextProperty(QStringLiteral("largeThumbnails"), sailfish::LargeThumbnails::instance());
    context->setContextProperty(QStringLiteral("queueSeeder"), p.queueSeeder.data());
    context->setContextProperty(QStringLiteral("shortsModel"), p.shortsModel.data());
}

QStringList CatalogComposition::allowedHosts() {
    return {QStringLiteral("www.youtube.com"), QStringLiteral("suggestqueries.google.com"), QStringLiteral("i.ytimg.com"),
            QStringLiteral("*.ytimg.com"), QStringLiteral("yt3.ggpht.com"), QStringLiteral("*.googleusercontent.com"),
            QStringLiteral("sponsor.ajay.app"), QStringLiteral("returnyoutubedislikeapi.com")};
}

}}}
