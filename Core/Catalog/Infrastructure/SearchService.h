// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Search (DSC-08) over the first data tier, with the warm extractor as second tier (DSC-01).
//
//  * Page 1: POST /youtubei/v1/search {query, params}; rows from
//    contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer.
//  * Next pages: POST /search {continuation}; rows from
//    onResponseReceivedCommands[].appendContinuationItemsAction.continuationItems (or the
//    body slot of a reloadContinuationItemsCommand, which YouTube also sends for later pages).
//  * ValidEmpty when the section list / continuation command exists and holds no rows;
//    ShapeDrift only when those container markers are missing. HTTP failures stay HTTP failures.
//  * On drift or any failure (never on ValidEmpty) the fallback hook runs: the yt-dlp flat
//    search in the app (YtDlpSearchFallback.h), a fake in tests. Fallback pages continue with
//    synthetic "ytdlp:<offset>" tokens.
//
// Parsing runs on the client's WorkLane; callbacks arrive on the service's thread.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/SearchFilter.h"

#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct SearchResult {
    Outcome outcome = Outcome::Network;
    std::vector<Item> rows;          // unfiltered, document order
    QString continuation;            // "" = last page
    QMap<QString, int> unknown;      // unknown renderers (DSC-03 diagnostics)
    int httpStatus = 0;
    qint64 elapsedMs = 0;
    bool fallback = false;           // rows came from the second tier
    Outcome firstTierOutcome = Outcome::Network;
};

// Second tier. `search` answers `done(outcome, rows)` on `context`'s thread and returns an id
// for `cancel`. `rows` are offset..offset+count-1 of a videos search for `query`.
struct SearchFallback {
    using Done = std::function<void(Outcome, const std::vector<Item> &)>;
    std::function<quint64(const QString &query, const SearchFilter &filter, int offset, int count, QObject *context, Done done)> search;
    std::function<void(quint64)> cancel;
    explicit operator bool() const { return static_cast<bool>(search); }
};

class SearchService final : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const SearchResult &)>;
    static const int kFallbackPageSize = 20;

    SearchService(InnerTubeClient *client, CatalogCounters *counters = nullptr, QObject *parent = nullptr);
    ~SearchService() override;

    void setFallback(const SearchFallback &fallback) { fallback_ = fallback; }
    InnerTubeClient *client() const { return client_; }

    // `filter` is sent as is (callers apply effectiveSearchFilter for videos-only mode).
    quint64 firstPage(const QString &query, const SearchFilter &filter, Callback done);
    // `rowsSoFar` is the number of rows already received (the fallback's offset when the
    // first tier fails on a continuation).
    quint64 nextPage(const QString &query, const SearchFilter &filter, const QString &continuation, int rowsSoFar, Callback done);
    // A next page fetched ahead (NFR-PERF-09): InnerTube only. A failure is reported as is and
    // never starts the yt-dlp tier, which would compete with playback for the resolver.
    quint64 prefetchPage(const QString &query, const SearchFilter &filter, const QString &continuation, Callback done);
    void cancel(quint64 id);
    int pending() const { return ops_.size(); }

    // Parsers (exposed for tests; they run on the lane).
    static SearchResult parseFirstPage(const QJsonObject &answer, const QString &hl);
    static SearchResult parseContinuation(const QJsonObject &answer, const QString &hl);
    static QJsonObject firstPageBody(const QString &query, const SearchFilter &filter);
    static QString fallbackToken(int offset) { return QStringLiteral("ytdlp:%1").arg(offset); }
    static bool isFallbackToken(const QString &token) { return token.startsWith(QLatin1String("ytdlp:")); }

private:
    struct Op { quint64 innerId = 0; quint64 fallbackId = 0; bool noFallback = false; };
    quint64 begin();
    void post(quint64 op, const QJsonObject &body, bool first, const QString &query, const SearchFilter &filter, int offset, Callback done);
    void runFallback(quint64 op, const QString &query, const SearchFilter &filter, int offset, const SearchResult &firstTier, Callback done);
    void finish(quint64 op, const SearchResult &result, const Callback &done);

    InnerTubeClient *client_;
    CatalogCounters *counters_;
    SearchFallback fallback_;
    QHash<quint64, Op> ops_;
    quint64 next_ = 0;
};

}}}
