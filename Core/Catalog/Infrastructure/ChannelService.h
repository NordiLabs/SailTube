// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Channels (DSC-12, FINTUBE_SPEC 5.7 behaviour).
//
//  * References: UC ids and /channel/UC... are used directly. @handle, /c/ and /user/ go through
//    POST /youtubei/v1/navigation/resolve_url {url} -> endpoint.browseEndpoint.browseId, after
//    stripping tab suffixes (ChannelReference.h). Session cache: 256 references (LRU).
//  * Tabs: POST /youtubei/v1/browse {browseId, params}. The params come from the channel's own
//    tab list (tabRenderer endpoints, keyed by the URL suffix: videos, shorts, streams,
//    playlists, releases, ...), learnt from any answer for that channel. Until a list is known,
//    the web app's recorded params are used (fallbackTabParams); when an answer shows that the
//    channel's own params differ, the call is repeated once with them. A requested tab missing
//    from the list is reported as `tabMissing` (a channel without a Videos tab opens Playlists or
//    Releases: the caller decides).
//  * Continuations: POST /browse {continuation}; rows from onResponseReceivedActions /
//    onResponseReceivedEndpoints.
//  * Header: pageHeaderViewModel (title, avatar, @handle, subscriber and video counts) with the
//    older c4TabbedHeaderRenderer as fallback, plus channelMetadataRenderer (id, title, avatar,
//    vanity URL, RSS URL, description). Avatars go through chooseAvatar (DSC-19).
//  * Valid empty vs drift (DSC-01): the selected tab holds a richGridRenderer/sectionListRenderer
//    without rows -> ValidEmpty; the tab container itself missing -> ShapeDrift; an ERROR alert
//    without contents -> Unavailable. HTTP failures are classified, never drift.
//  * RSS first paint: GET https://www.youtube.com/feeds/videos.xml?channel_id=<UC> through an
//    injected RssFetch (the feed package owns the RSS parser).
//  * Every listing feeds the persistent Shorts knowledge (DSC-17) and plans the background
//    Shorts-tab checks of channels whose rows hold candidates.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/LruStore.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class ShortsStore;

struct ChannelHeader {
    QString channelId;
    QString title;
    QString handle;          // "@NASA"
    QString avatarUrl;       // square, 176 px, absolute
    QString subscriberText;
    qint64 subscriberCount = -1;
    QString videoCountText;
    qint64 videoCount = -1;
    QString description;
    QString rssUrl;
    QString source;          // "pageHeaderViewModel", "c4TabbedHeaderRenderer", "channelMetadataRenderer", ""
    bool isEmpty() const { return title.isEmpty() && channelId.isEmpty(); }
};

struct ChannelTab {
    QString key;             // URL suffix: videos, shorts, streams, playlists, releases, featured, ...
    QString title;           // display title in the answer's language
    QString params;          // decoded base64 params
    bool selected = false;
};

struct ChannelPage {
    Outcome outcome = Outcome::Network;
    int httpStatus = 0;
    QString channelId;
    QString requestedTab;
    QString selectedTab;     // the tab this answer shows
    bool tabMissing = false; // the requested tab is not in the channel's tab list
    QList<ChannelTab> tabs;
    ChannelHeader header;
    std::vector<Item> items;
    QString continuation;
    QMap<QString, int> unknown;
    int unparsed = 0;
    bool hasTab(const QString &key) const {
        for (const ChannelTab &t : tabs) if (t.key == key) return true;
        return false;
    }
};

class ChannelService final : public QObject {
    Q_OBJECT
public:
    using RssDone = std::function<void(Outcome, std::vector<Item>)>;
    // Fetches and parses a channel's RSS feed; `done` runs on `context`'s thread (or not at all
    // when `context` is gone).
    using RssFetch = std::function<void(const QString &channelId, QObject *context, RssDone done)>;

    static const int kResolveCacheSize = 256;
    static const int kTabCacheSize = 64;

    ChannelService(InnerTubeClient *client, ShortsStore *shorts = nullptr, CatalogCounters *counters = nullptr,
                   QObject *parent = nullptr);

    InnerTubeClient *client() const { return client_; }
    ShortsStore *shorts() const;

    void setRssFetch(RssFetch fetch) { rss_ = std::move(fetch); }
    bool hasRss() const { return static_cast<bool>(rss_); }
    // The RSS feed URL of a UC id (for the feed's fetcher).
    static QString rssUrl(const QString &channelId);

    // Reference -> UC id. `done(outcome, channelId)`; Unavailable for an unknown handle,
    // Http4xx for an unparseable reference.
    void resolve(const QString &reference, QObject *context, std::function<void(Outcome, QString)> done);
    // The cached UC id of a reference ("" when it needs a resolve call first).
    QString cachedChannelId(const QString &reference) const;

    // Page 1 of a tab ("videos", "shorts", "streams" (alias "live"), "playlists", "releases").
    void browseTab(const QString &channelId, const QString &tab, QObject *context, std::function<void(ChannelPage)> done);
    // A continuation page of a tab.
    void browseMore(const QString &channelId, const QString &tab, const QString &continuation, QObject *context,
                    std::function<void(ChannelPage)> done);
    // First paint: the channel's RSS rows (Unavailable when no fetcher is set).
    void rss(const QString &channelId, QObject *context, RssDone done);

    // The channel's own tab params once known, else the recorded web params.
    QString tabParams(const QString &channelId, const QString &tab) const;
    bool knowsTabs(const QString &channelId) const;
    QList<ChannelTab> knownTabs(const QString &channelId) const;

    static QString normalizeTab(const QString &tab);
    static QString fallbackTabParams(const QString &tab);

    // Parsers (run on the lane; exposed for tests).
    static ChannelPage parseBrowse(const InnerTubeReply &reply, const QString &channelId, const QString &requestedTab,
                                   const QString &hl);
    static ChannelPage parseContinuation(const InnerTubeReply &reply, const QString &channelId, const QString &tab,
                                         const QString &hl);
    static ChannelHeader parseHeader(const QJsonObject &root, const QString &hl);
    static QList<ChannelTab> parseTabs(const QJsonObject &root);
    static Outcome parseResolve(const InnerTubeReply &reply, QString *channelId);

private:
    void browseAttempt(const QString &channelId, const QString &tab, const QString &params, int attempt,
                       QObject *context, std::function<void(ChannelPage)> done);
    void learn(const ChannelPage &page);
    void count(Outcome outcome, const QMap<QString, int> &unknown);
    InnerTubeClient *client_;
    QPointer<ShortsStore> shorts_;
    CatalogCounters *counters_;
    RssFetch rss_;
    LruStore<std::string, QString> resolved_;
    LruStore<std::string, QList<ChannelTab>> tabs_;
    LruStore<std::string, QString> titles_;
};

}}}
