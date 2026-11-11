// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The subscriptions feed (DSC-05/06/07). Lives on the UI thread, never blocks; parsing and
// cache I/O run on the InnerTube client's WorkLane.
//
// Per channel, in order (DSC-05):
//   1. RSS  GET https://www.youtube.com/feeds/videos.xml?channel_id=<UC...>, 10 s, on the
//      *background* transport (redirects followed by the transport). RSS rows are then
//      enriched with the channel's Live tab (running and upcoming streams, DSC-07).
//   2. InnerTube browse of the channel's Videos tab when RSS fails or is empty. The tab params
//      are learned from any channel answer's tab list (the tab whose URL ends in /videos); the
//      built-in value kVideosTabParams is the documented fallback, and a mismatch retries
//      once with the learned value.
//   3. The extractor through an optional hook. Refresh never requires it.
//
// One channel's failure affects only that channel: its last good rows stay (FeedSchedule).
// At most kFeedMaxParallel (10) channel jobs run at once. Refreshes happen only while the app
// runs: on start(), on refresh(), and from an in-process timer (setAutoRefresh); there are no
// wake-ups. At start the cache paints at once (stale-while-revalidate) and only due channels
// refresh, with refreshing() set for the small indicator.
//
// Depth (DSC-07): extend() pages channels through their Videos tab (first page, then
// continuations), newest frontier first, so the merged feed scrolls past 300 rows.

#include "FeedCacheStore.h"
#include "HttpTransport.h"
#include "InnerTubeClient.h"
#include "RssParser.h"
#include "Core/Catalog/Domain/FeedSchedule.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/Subscriptions.h"

#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

class QTimer;

namespace nordtile { namespace video { namespace catalog {

// Third tier (DSC-05). The hook must call `done` exactly once, on the service's thread.
struct ExtractorFeedResult {
    Outcome outcome = Outcome::Network;
    std::vector<Item> rows;
};
using FeedExtractorHook = std::function<void(const ChannelRef &channel, std::function<void(const ExtractorFeedResult &)> done)>;

// A parsed channel tab answer (first page or continuation).
struct FeedChannelPage {
    Outcome outcome = Outcome::Network;
    std::vector<Item> items;
    QString continuation;
    QString videosParams;          // learned from the tab list (percent-decoded), may be empty
    QString liveParams;
    bool requestedTabListed = false;
    bool requestedTabSelected = false;
};

class FeedService final : public QObject {
    Q_OBJECT
public:
    static const char *const kVideosTabParams;   // the Videos tab (documented fallback)
    static const char *const kLiveTabParams;     // the Live ("streams") tab
    static const int kRssTimeoutMs = 10000;
    static const int kAutoRefreshCheckMs = 60000;

    // `background` carries the RSS requests; `innerTube` should be built on the background
    // transport too (RES-02). `store` may be null (no persistence). `clock` returns epoch
    // seconds (tests).
    FeedService(HttpTransport *background, InnerTubeClient *innerTube, FeedCacheStore *store,
                QObject *parent = nullptr, FeedSchedule::Clock clock = FeedSchedule::Clock());
    ~FeedService() override;

    void setExtractor(const FeedExtractorHook &hook) { extractor_ = hook; }
    void setLiveEnrichment(bool on) { enrichLive_ = on; }
    void setCounters(CatalogCounters *counters) { counters_ = counters; }
    void setRowFilter(const RowFilterSettings &settings);
    const RowFilterSettings &rowFilter() const { return filter_; }
    void setShortsLookup(const ShortsLookup &lookup);

    // The subscription list (from the library component, LIB-01).
    void setChannels(const ChannelList &channels);

    // Paints the cache, then refreshes the due channels.
    void start();
    // Due channels only, or every channel when forced (pull-to-refresh). A forced refresh is
    // ignored while a refresh is running.
    void refresh(bool force);
    // DSC-07: extends up to 10 channels by one page each.
    void extend();
    // In-process timer that refreshes due channels while the app runs.
    void setAutoRefresh(bool on);

    bool started() const { return started_; }
    bool cacheLoaded() const { return cacheLoaded_; }
    bool refreshing() const { return refreshing_; }
    bool extending() const { return extending_; }
    bool stale() const { return stale_; }
    bool canExtend() const { return cacheLoaded_ && !extending_ && schedule_.canExtend(); }
    int channelCount() const { return static_cast<int>(schedule_.size()); }
    int lastRoundFailures() const { return lastRoundFailures_; }
    int lastRoundAttempts() const { return lastRoundAttempts_; }
    bool lastRoundAllFailed() const { return lastRoundAttempts_ > 0 && lastRoundFailures_ == lastRoundAttempts_; }
    int inFlight() const { return inFlight_; }
    int peakInFlight() const { return peakInFlight_; }
    int savesCompleted() const { return saves_; }
    // A save runs on the lane, or one is waiting for it to end. While true the file may still
    // hold an older snapshot.
    bool saving() const { return saving_ || savePending_; }

    const MergedFeed &merged() const { return merged_; }
    const FeedSchedule &schedule() const { return schedule_; }

    // Persists a copy of the state on the lane. While a save runs, further calls are coalesced
    // into one more save, taken from the state as it is when the running one ends.
    void saveNow();
    // Synchronous save for shutdown. A lane save still running at that moment holds an older
    // snapshot and cannot replace this one (FeedCacheStore numbers the snapshots).
    bool saveSync();

    // Parses a channel tab answer. `tab` is the URL suffix of the requested tab ("videos",
    // "streams"); empty for a continuation page. Runs on a lane.
    static FeedChannelPage parseChannelTab(const InnerTubeReply &reply, const QString &hl, const QString &tab);

signals:
    void feedChanged();
    void stateChanged();
    void channelFailed(const QString &channelId, int outcome);
    void saved(bool ok);

private:
    struct Job {
        ChannelRef channel;
        bool extend;
    };
    void onCacheLoaded(const std::vector<ChannelFeedState> &states);
    void pump();
    void startRefresh(const ChannelRef &channel);
    void onRss(const ChannelRef &channel, const RssFeed &feed);
    void enrichAndApply(const ChannelRef &channel, const std::vector<Item> &rows);
    void fetchVideosTab(const ChannelRef &channel, bool retried, Outcome previous);
    void tryExtractor(const ChannelRef &channel, Outcome previous);
    void succeed(const ChannelRef &channel, const std::vector<Item> &rows, FeedSource source,
                 const QString &continuation, bool fromVideosTab, Outcome outcome);
    void fail(const ChannelRef &channel, Outcome outcome);
    void startExtend(const ChannelRef &channel);
    void finishJob(const ChannelRef &channel, bool extend);
    void browseTab(const ChannelRef &channel, const QString &params, const QString &tab,
                   std::function<void(FeedChannelPage)> done);
    void browseContinuation(const QString &token, std::function<void(FeedChannelPage)> done);
    void learn(const FeedChannelPage &page);
    void count(Outcome outcome);
    void scheduleMerge();
    void remerge();
    template <typename T> void runOnLane(std::function<T()> work, std::function<void(T)> done);

    HttpTransport *background_;
    InnerTubeClient *innerTube_;
    FeedCacheStore *store_;
    FeedSchedule schedule_;
    MergedFeed merged_;
    RowFilterSettings filter_;
    ShortsLookup lookup_;
    FeedExtractorHook extractor_;
    CatalogCounters *counters_ = nullptr;
    QTimer *autoTimer_;

    ChannelList pendingChannels_;
    bool havePendingChannels_ = false;
    bool pendingForce_ = false;
    bool started_ = false;
    bool cacheLoaded_ = false;
    bool refreshing_ = false;
    bool extending_ = false;
    bool stale_ = false;
    bool enrichLive_ = true;
    bool mergePending_ = false;
    bool saving_ = false;
    bool savePending_ = false;

    QList<Job> queue_;
    QSet<QString> activeRefresh_;
    int inFlight_ = 0;
    int peakInFlight_ = 0;
    int refreshLeft_ = 0;
    int extendLeft_ = 0;
    int roundFailures_ = 0;
    int roundAttempts_ = 0;
    int lastRoundFailures_ = 0;
    int lastRoundAttempts_ = 0;
    int saves_ = 0;
    std::uint64_t nextSnapshotOrder();
    std::uint64_t snapshots_ = 0;   // the number of the last snapshot taken for saving
    QString videosParams_;
    QString liveParams_;
};

}}}
