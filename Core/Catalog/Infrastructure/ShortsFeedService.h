// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The Shorts section's feed (user-requested 2026-11-08, with the design component's ShortsView).
// Sources, in order:
//  * signed in (ACC-11): the account's own Shorts sequence (reel_watch_sequence over the signed
//    client, seeded by the first Shorts lockup of a signed Shorts search); rows carry ids and
//    thumbnails, the rest arrives when a Short plays;
//  * the subscribed channels' Shorts tabs, kChannelsPerPage channels per page, interleaved;
//  * popular Shorts for the request's language and region (the anonymous search for "#shorts"
//    with YouTube's Shorts type filter) whenever a page would hold fewer than kMinPageRows, then
//    page by page while a page holds at least kThinPageRows Shorts (the search's later pages
//    hold one to five);
//  * then YouTube's own Shorts sequence, one request per seed: every popular row names a seed
//    (its sequenceParams), and a seed's first page holds 10 to 25 Shorts (its continuation
//    repeats them). A page that adds nothing new tries the next seed, at most kSeedRequestsPerPage
//    requests per page. So the list does not end while seeds are left (about 50 after the
//    popular pages).
// Page 1 is cached for kCacheMs per mode, locale and channel list. Rows are filtered with the
// shared RowFilter (members-only, LIB-07 watched) and deduplicated; the thumbnail is the vertical
// frame. No new host.

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/Subscriptions.h"

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <map>
#include <set>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class ChannelService;
class InnerTubeClient;
struct InnerTubeReply;
struct ChannelPage;

// Where a row came from.
enum class ShortsSource { Subscriptions, Popular, Account };

struct ShortsRow {
    Item item;                 // kind Short, vertical thumbnail
    ShortsSource source = ShortsSource::Popular;
    std::string channelAvatar; // subscription rows, when the channel header had one
    std::string likeText;      // when YouTube gave one (not in lockups today)
    std::string sequenceParams; // the row's own sequence seed (popular rows), "" when it has none
};

struct ShortsPage {
    Outcome outcome = Outcome::Network;
    std::vector<Item> items;
    QString continuation;
    QString sequenceParams;    // the first lockup's sequence seed (search answers)
    std::map<std::string, QString> seeds;   // each lockup's own seed by video id (search answers)
};

class ShortsFeedService final : public QObject {
    Q_OBJECT
public:
    static const int kChannelsPerPage = 5;
    static const int kMinPageRows = 20;
    static const qint64 kCacheMs = 10LL * 60 * 1000;
    static const int kThinPageRows = 6;          // a popular page below this ends the popular pages
    static const int kSeedRequestsPerPage = 3;   // sequence requests one page may use

    ShortsFeedService(InnerTubeClient *anonymous, ChannelService *channels, CatalogCounters *counters = nullptr,
                      QObject *parent = nullptr);

    void setChannels(const ChannelList &channels);
    // Whether the user subscribes to `channelId` (the list the library pushed through the bridge).
    bool isSubscribed(const std::string &channelId) const { return subscribed_.count(channelId) > 0; }
    // ACC-11: a signed client while the account can sign, null otherwise.
    void setSignedClient(InnerTubeClient *client);
    bool signedIn() const { return signed_ != nullptr; }
    void setRowFilter(const RowFilterSettings &settings) { filter_ = settings; }
    void setClock(const std::function<qint64()> &clock) { clock_ = clock; }

    void refresh();
    void loadMore();
    void clear();

    const std::vector<ShortsRow> &rows() const { return rows_; }
    bool busy() const { return pending_ > 0; }
    bool canLoadMore() const;
    bool loaded() const { return loaded_; }
    Outcome outcome() const { return outcome_; }

    static QJsonObject popularBody();
    static ShortsPage parsePopular(const InnerTubeReply &reply, const QString &hl, bool continuation);
    static ShortsPage parseSequence(const InnerTubeReply &reply);

signals:
    void changed();
    // The subscription list changed: Subscribe buttons may flip.
    void subscriptionsChanged();

private:
    void startPage(bool first);
    void requestChannels(quint64 generation, std::vector<ChannelRef> batch);
    void requestPopular(quint64 generation, bool first);
    void requestSequence(quint64 generation, bool first);
    void requestSeeded(quint64 generation);
    void requestAfterPopular(quint64 generation);
    void rememberSeed(const std::string &seed);
    void finishPage(quint64 generation);
    void append(const std::vector<Item> &items, ShortsSource source, const ChannelPage *channel);
    qint64 nowMs() const;
    QString cacheKey() const;

    InnerTubeClient *anonymous_;
    InnerTubeClient *signed_ = nullptr;
    ChannelService *channels_;
    CatalogCounters *counters_;
    RowFilterSettings filter_;
    std::function<qint64()> clock_;

    ChannelList subscriptions_;
    std::set<std::string> subscribed_;
    std::size_t nextChannel_ = 0;
    QString popularContinuation_;
    bool popularStarted_ = false;
    bool popularEnded_ = false;
    QString sequenceParams_;
    QString sequenceContinuation_;
    std::vector<std::string> seeds_;   // popular rows' sequence seeds, in list order
    std::size_t nextSeed_ = 0;
    int seedAttempts_ = 0;             // sequence requests of the page in flight

    std::vector<ShortsRow> rows_;
    std::set<std::string> seen_;
    std::vector<ShortsRow> pageRows_;   // rows collected for the page in flight
    int pending_ = 0;
    quint64 generation_ = 0;
    bool loaded_ = false;
    Outcome outcome_ = Outcome::ValidEmpty;

    // Page 1 cache.
    QString cachedKey_;
    qint64 cachedAt_ = -1;
    std::vector<ShortsRow> cachedRows_;
    std::size_t cachedNextChannel_ = 0;
    QString cachedPopularContinuation_;
    bool cachedPopularStarted_ = false;
    QString cachedSequenceContinuation_;
    std::vector<std::string> cachedSeeds_;
    std::size_t cachedNextSeed_ = 0;
};

}}}
