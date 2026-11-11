// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FeedService.h"

#include <algorithm>

#include "Json.h"
#include "RendererParser.h"

#include <QJsonArray>
#include <QPointer>
#include <QTimer>
#include <QUrl>
#include <memory>

namespace nordtile { namespace video { namespace catalog {

const char *const FeedService::kVideosTabParams = innertube::kTabParamsVideos;
const char *const FeedService::kLiveTabParams = innertube::kTabParamsStreams;

namespace {

bool goodOutcome(Outcome o) { return o == Outcome::Ok || o == Outcome::ValidEmpty; }

void keepVideos(const ParsedItems &parsed, std::vector<Item> &out) {
    for (const Item &it : parsed.items) if (isVideoLike(it) && !it.id.empty()) out.push_back(it);
}

bool allUndated(const std::vector<Item> &rows) {
    for (const Item &it : rows) {
        if (it.publishedEpoch >= 0 || it.publishedAgeSeconds >= 0) return false;
        if (it.liveStatus == media::LiveStatus::IsLive || it.liveStatus == media::LiveStatus::Upcoming) return false;
    }
    return true;
}

}  // namespace

FeedService::FeedService(HttpTransport *background, InnerTubeClient *innerTube, FeedCacheStore *store,
                         QObject *parent, FeedSchedule::Clock clock)
    : QObject(parent), background_(background), innerTube_(innerTube), store_(store), schedule_(clock),
      autoTimer_(new QTimer(this)),
      videosParams_(QString::fromLatin1(kVideosTabParams)), liveParams_(QString::fromLatin1(kLiveTabParams)) {
    autoTimer_->setInterval(kAutoRefreshCheckMs);
    connect(autoTimer_, &QTimer::timeout, this, [this] {
        if (cacheLoaded_ && !refreshing_ && schedule_.anyDue()) refresh(false);
    });
}

FeedService::~FeedService() {}

template <typename T>
void FeedService::runOnLane(std::function<T()> work, std::function<void(T)> done) {
    WorkLane *lane = innerTube_ ? innerTube_->lane() : nullptr;
    if (lane) lane->run<T>(this, work, done);
    else done(work());
}

void FeedService::count(Outcome outcome) {
    if (counters_) counters_->count(Area::Feed, outcome);
}

void FeedService::setRowFilter(const RowFilterSettings &settings) {
    filter_ = settings;
    if (cacheLoaded_) remerge();   // filter toggles re-read the cache offline
}

void FeedService::setShortsLookup(const ShortsLookup &lookup) {
    lookup_ = lookup;
    if (cacheLoaded_) remerge();
}

void FeedService::setChannels(const ChannelList &channels) {
    if (!cacheLoaded_) {
        pendingChannels_ = channels;
        havePendingChannels_ = true;
        return;
    }
    schedule_.setChannels(channels);
    remerge();
    emit stateChanged();
    if (started_ && schedule_.anyDue()) refresh(false);   // new channels are due at once
    saveNow();
}

void FeedService::start() {
    if (started_) return;
    started_ = true;
    if (!store_) { onCacheLoaded(std::vector<ChannelFeedState>()); return; }
    FeedCacheStore *store = store_;
    std::function<std::vector<ChannelFeedState>()> work = [store] { return store->load(); };
    std::function<void(std::vector<ChannelFeedState>)> done = [this](std::vector<ChannelFeedState> states) { onCacheLoaded(states); };
    runOnLane<std::vector<ChannelFeedState>>(work, done);
}

void FeedService::onCacheLoaded(const std::vector<ChannelFeedState> &states) {
    schedule_.restore(states);
    if (havePendingChannels_) {
        schedule_.setChannels(pendingChannels_);
        pendingChannels_.clear();
        havePendingChannels_ = false;
    }
    cacheLoaded_ = true;
    stale_ = schedule_.anyDue();
    remerge();              // paint the cache at once
    emit stateChanged();
    const bool force = pendingForce_;
    pendingForce_ = false;
    refresh(force);
}

void FeedService::setAutoRefresh(bool on) {
    if (on) autoTimer_->start();
    else autoTimer_->stop();
}

void FeedService::refresh(bool force) {
    if (!cacheLoaded_) {
        pendingForce_ = pendingForce_ || force;
        if (!started_) start();
        return;
    }
    if (refreshing_ && force) return;   // the pull-down Refresh is disabled while running
    const ChannelList due = schedule_.due(force);
    int added = 0;
    for (const ChannelRef &c : due) {
        const QString id = QString::fromStdString(c.id);
        if (activeRefresh_.contains(id)) continue;
        activeRefresh_.insert(id);
        Job job;
        job.channel = c;
        job.extend = false;
        queue_.append(job);
        ++added;
    }
    if (added == 0) {
        if (!refreshing_ && stale_) { stale_ = false; emit stateChanged(); }
        return;
    }
    if (!refreshing_) { roundFailures_ = 0; roundAttempts_ = 0; }
    refreshing_ = true;
    refreshLeft_ += added;
    roundAttempts_ += added;
    emit stateChanged();
    pump();
}

void FeedService::extend() {
    if (!cacheLoaded_ || extending_) return;
    const ChannelList list = schedule_.extendable(kFeedMaxParallel);
    if (list.empty()) return;
    for (const ChannelRef &c : list) {
        Job job;
        job.channel = c;
        job.extend = true;
        queue_.append(job);
    }
    extending_ = true;
    extendLeft_ += static_cast<int>(list.size());
    emit stateChanged();
    pump();
}

void FeedService::pump() {
    while (inFlight_ < kFeedMaxParallel && !queue_.isEmpty()) {
        const Job job = queue_.takeFirst();
        ++inFlight_;
        if (inFlight_ > peakInFlight_) peakInFlight_ = inFlight_;
        if (job.extend) startExtend(job.channel);
        else startRefresh(job.channel);
    }
}

void FeedService::finishJob(const ChannelRef &channel, bool extend) {
    --inFlight_;
    bool roundEnded = false;
    if (extend) {
        if (--extendLeft_ <= 0) { extendLeft_ = 0; extending_ = false; roundEnded = true; }
    } else {
        activeRefresh_.remove(QString::fromStdString(channel.id));
        if (--refreshLeft_ <= 0) {
            refreshLeft_ = 0;
            refreshing_ = false;
            stale_ = false;
            lastRoundFailures_ = roundFailures_;
            lastRoundAttempts_ = roundAttempts_;
            roundEnded = true;
        }
    }
    scheduleMerge();
    pump();
    if (roundEnded) {
        saveNow();
        emit stateChanged();
    }
}

// --- refresh: tier 1 (RSS) ------------------------------------------------------------------

void FeedService::startRefresh(const ChannelRef &channel) {
    schedule_.markAttempt(channel.id);
    if (!background_) { fetchVideosTab(channel, false, Outcome::Network); return; }
    HttpRequest request;
    request.url = RssParser::feedUrl(QString::fromStdString(channel.id));
    request.timeoutMs = kRssTimeoutMs;
    request.headers.append(qMakePair(QByteArray("Accept"), QByteArray("application/atom+xml, application/xml;q=0.9, */*;q=0.1")));
    background_->send(request, this, [this, channel](const HttpReply &http) {
        std::function<RssFeed()> work = [http] { return RssParser::parse(http.body, http.status); };
        std::function<void(RssFeed)> done = [this, channel](RssFeed feed) { onRss(channel, feed); };
        runOnLane<RssFeed>(work, done);
    });
}

void FeedService::onRss(const ChannelRef &channel, const RssFeed &feed) {
    count(feed.outcome);
    if (feed.outcome == Outcome::Ok) { enrichAndApply(channel, feed.items); return; }
    // Failed or empty (RSS is fast but unreliable): the Videos tab decides.
    fetchVideosTab(channel, false, feed.outcome);
}

void FeedService::enrichAndApply(const ChannelRef &channel, const std::vector<Item> &rows) {
    const ChannelFeedState *s = schedule_.find(channel.id);
    const std::vector<Item> previous = s ? s->rows : std::vector<Item>();
    if (!enrichLive_ || !innerTube_) {
        std::vector<Item> fresh = rows;
        carryLiveState(fresh, previous);
        succeed(channel, fresh, FeedSource::Rss, QString(), false, Outcome::Ok);
        return;
    }
    browseTab(channel, liveParams_, QStringLiteral("streams"), [this, channel, rows, previous](FeedChannelPage page) {
        learn(page);
        count(page.outcome);
        std::vector<Item> fresh = rows;
        if (goodOutcome(page.outcome)) applyLiveState(fresh, page.items);
        else carryLiveState(fresh, previous);
        succeed(channel, fresh, FeedSource::Rss, QString(), false, Outcome::Ok);
    });
}

// --- tier 2 (InnerTube Videos tab) and tier 3 (extractor) ------------------------------------

void FeedService::fetchVideosTab(const ChannelRef &channel, bool retried, Outcome previous) {
    if (!innerTube_) { tryExtractor(channel, previous); return; }
    const QString used = videosParams_;
    browseTab(channel, used, QStringLiteral("videos"), [this, channel, retried, used](FeedChannelPage page) {
        learn(page);
        count(page.outcome);
        if (page.outcome == Outcome::ShapeDrift && page.requestedTabListed && !retried &&
            !page.videosParams.isEmpty() && page.videosParams != used) {
            fetchVideosTab(channel, true, page.outcome);   // stale built-in params: retry with the learned ones
            return;
        }
        if (goodOutcome(page.outcome)) {
            succeed(channel, page.items, FeedSource::InnerTube, page.continuation, true, page.outcome);
            return;
        }
        tryExtractor(channel, page.outcome);
    });
}

void FeedService::tryExtractor(const ChannelRef &channel, Outcome previous) {
    if (!extractor_) { fail(channel, previous); return; }
    QPointer<FeedService> self(this);
    extractor_(channel, [self, channel](const ExtractorFeedResult &r) {
        if (!self) return;
        self->count(r.outcome);
        if (goodOutcome(r.outcome)) self->succeed(channel, r.rows, FeedSource::Extractor, QString(), false, r.outcome);
        else self->fail(channel, r.outcome);
    });
}

void FeedService::succeed(const ChannelRef &channel, const std::vector<Item> &rows, FeedSource source,
                          const QString &continuation, bool fromVideosTab, Outcome outcome) {
    schedule_.applySuccess(channel.id, rows, source, continuation.toStdString(), fromVideosTab, outcome);
    finishJob(channel, false);
}

void FeedService::fail(const ChannelRef &channel, Outcome outcome) {
    schedule_.applyFailure(channel.id, outcome);   // the last good rows stay
    ++roundFailures_;
    emit channelFailed(QString::fromStdString(channel.id), static_cast<int>(outcome));
    finishJob(channel, false);
}

// --- depth (DSC-07) -------------------------------------------------------------------------

void FeedService::startExtend(const ChannelRef &channel) {
    const ChannelFeedState *s = schedule_.find(channel.id);
    if (!s || !innerTube_) { finishJob(channel, true); return; }
    const std::string id = channel.id;
    std::function<void(FeedChannelPage)> done = [this, channel, id](FeedChannelPage page) {
        learn(page);
        count(page.outcome);
        if (goodOutcome(page.outcome)) schedule_.applyPage(id, page.items, page.continuation.toStdString());
        else schedule_.applyExtendFailure(id);
        finishJob(channel, true);
    };
    if (!s->continuation.empty()) browseContinuation(QString::fromStdString(s->continuation), done);
    else browseTab(channel, videosParams_, QStringLiteral("videos"), done);
}

// --- InnerTube calls ------------------------------------------------------------------------

void FeedService::browseTab(const ChannelRef &channel, const QString &params, const QString &tab,
                            std::function<void(FeedChannelPage)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("browseId"), QString::fromStdString(channel.id));
    body.insert(QStringLiteral("params"), params);
    const QString hl = innerTube_->locale().hl;
    std::function<FeedChannelPage(const InnerTubeReply &)> parse = [hl, tab](const InnerTubeReply &r) {
        return parseChannelTab(r, hl, tab);
    };
    innerTube_->post<FeedChannelPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, done);
}

void FeedService::browseContinuation(const QString &token, std::function<void(FeedChannelPage)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), token);
    const QString hl = innerTube_->locale().hl;
    std::function<FeedChannelPage(const InnerTubeReply &)> parse = [hl](const InnerTubeReply &r) {
        return parseChannelTab(r, hl, QString());
    };
    innerTube_->post<FeedChannelPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, done);
}

void FeedService::learn(const FeedChannelPage &page) {
    if (!page.videosParams.isEmpty()) videosParams_ = page.videosParams;
    if (!page.liveParams.isEmpty()) liveParams_ = page.liveParams;
}

FeedChannelPage FeedService::parseChannelTab(const InnerTubeReply &reply, const QString &hl, const QString &tab) {
    FeedChannelPage page;
    page.outcome = reply.outcome;
    if (reply.outcome != Outcome::Ok) return page;
    const QJsonObject &root = reply.json;
    const RendererParser parser(hl);

    if (tab.isEmpty()) {
        // Continuation: onResponseReceivedActions[].appendContinuationItemsAction.continuationItems
        bool marker = false;
        QJsonArray items;
        for (const QJsonValue &v : root.value(QStringLiteral("onResponseReceivedActions")).toArray()) {
            const QJsonObject a = v.toObject();
            static const char *const keys[] = { "appendContinuationItemsAction", "reloadContinuationItemsCommand" };
            for (const char *key : keys) {
                const QJsonValue action = a.value(QLatin1String(key));
                if (!action.isObject()) continue;
                marker = true;
                for (const QJsonValue &x : action.toObject().value(QStringLiteral("continuationItems")).toArray()) items.append(x);
            }
        }
        if (!marker) { page.outcome = Outcome::ShapeDrift; return page; }
        const ParsedItems parsed = parser.parse(QJsonValue(items));
        keepVideos(parsed, page.items);
        page.continuation = parsed.continuation;
        page.outcome = page.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
        return page;
    }

    // The tab answer (Videos fallback, Live enrichment) names the channel's avatar: remembered
    // for its feed rows, which come from RSS without one.
    RendererParser::learnChannelAvatar(root);
    const QJsonArray tabs = json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
    if (tabs.isEmpty()) {
        // A terminated or missing channel answers with alerts and no tabs.
        page.outcome = root.contains(QStringLiteral("alerts")) ? Outcome::Unavailable : Outcome::ShapeDrift;
        return page;
    }
    QJsonObject selectedContent;
    QString selectedSuffix;
    for (const QJsonValue &v : tabs) {
        const QJsonObject t = v.toObject().value(QStringLiteral("tabRenderer")).toObject();
        if (t.isEmpty()) continue;
        const QString url = json::at(t, QStringLiteral("endpoint.commandMetadata.webCommandMetadata.url")).toString();
        const QString params = QUrl::fromPercentEncoding(json::at(t, QStringLiteral("endpoint.browseEndpoint.params")).toString().toLatin1());
        const QString suffix = url.section(QLatin1Char('?'), 0, 0).section(QLatin1Char('/'), -1);
        if (suffix == QLatin1String("videos")) page.videosParams = params;
        else if (suffix == QLatin1String("streams")) page.liveParams = params;
        if (suffix == tab) page.requestedTabListed = true;
        if (t.value(QStringLiteral("selected")).toBool()) {
            selectedContent = t.value(QStringLiteral("content")).toObject();
            selectedSuffix = suffix;
        }
    }
    page.requestedTabSelected = selectedSuffix == tab;
    if (!page.requestedTabSelected) {
        // Listed but not selected: the params were not understood (drift). Not listed: the
        // channel has nothing of that kind (no uploads, no streams).
        page.outcome = page.requestedTabListed ? Outcome::ShapeDrift : Outcome::ValidEmpty;
        return page;
    }
    if (!json::containsKey(selectedContent, QStringList() << QStringLiteral("richGridRenderer") << QStringLiteral("sectionListRenderer"))) {
        page.outcome = Outcome::ShapeDrift;
        return page;
    }
    const ParsedItems parsed = parser.parse(QJsonValue(selectedContent));
    keepVideos(parsed, page.items);
    page.continuation = parsed.continuation;
    if (page.items.empty()) page.outcome = Outcome::ValidEmpty;
    else if (allUndated(page.items)) { page.outcome = Outcome::ShapeDrift; page.items.clear(); page.continuation.clear(); }
    else page.outcome = Outcome::Ok;
    return page;
}

// --- merge and persistence ------------------------------------------------------------------

void FeedService::scheduleMerge() {
    if (mergePending_) return;
    mergePending_ = true;
    QTimer::singleShot(0, this, [this] { if (mergePending_) remerge(); });
}

void FeedService::remerge() {
    mergePending_ = false;
    merged_ = schedule_.merged(filter_, lookup_);
    emit feedChanged();
}

void FeedService::saveNow() {
    if (!store_) return;
    if (saving_) { savePending_ = true; return; }
    saving_ = true;
    // DSC-06: persist a copy, never the live containers.
    std::shared_ptr<std::vector<ChannelFeedState>> copy = std::make_shared<std::vector<ChannelFeedState>>(schedule_.snapshot());
    FeedCacheStore *store = store_;
    const std::uint64_t order = nextSnapshotOrder();
    std::function<bool()> work = [store, copy, order] { return store->save(*copy, order); };
    std::function<void(bool)> done = [this](bool ok) {
        saving_ = false;
        ++saves_;
        emit saved(ok);
        if (savePending_) { savePending_ = false; saveNow(); }
    };
    runOnLane<bool>(work, done);
}

bool FeedService::saveSync() {
    if (!store_) return false;
    const std::uint64_t order = nextSnapshotOrder();
    return store_->save(schedule_.snapshot(), order);
}

std::uint64_t FeedService::nextSnapshotOrder() {
    // Above every snapshot this service has numbered (one may still be on the lane) and above
    // everything the store has written (an earlier service may have used the same store).
    snapshots_ = std::max(snapshots_, store_->lastWrittenOrder()) + 1;
    return snapshots_;
}

}}}
