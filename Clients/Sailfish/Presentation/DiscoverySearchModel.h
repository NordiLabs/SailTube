// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Search results for SearchPage.qml over the discovery tiers (DSC-08..DSC-10). A drop-in
// successor of SearchModel: the same roles and search()/loadMore()/cancel()/get() API, plus
// row kinds (mixed results), badges, channel/playlist counts, the `sp` filters and the
// hide-Shorts / hide-members-only settings with visible hidden-row counts (DSC-18).
//
//  * Paging stops only without a continuation token; pages that add no visible rows trigger
//    up to 5 automatic fetches in a row (DSC-09, SearchPaging).
//  * Results are cached per (query, filters, kind): page 1 is reused while < 2 min old and
//    cached deeper pages are replayed without a request (DSC-10, SearchCache).
//  * The first page logs "sailtube-timing search page1 <ms>" (NFR-PERF-09).

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/SearchCache.h"
#include "Core/Catalog/Domain/SearchFilter.h"

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QString>
#include <set>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog { class SearchService; struct SearchResult; } } }

namespace nordtile { namespace video { namespace sailfish {

class DiscoverySearchModel final : public QAbstractListModel {
    Q_OBJECT
    // SearchModel compatible.
    Q_PROPERTY(QString query READ query NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    // Discovery additions.
    // The rows are the last search's page 1 from disk, shown at start until refreshInPlace()
    // (or a new search) replaces them.
    Q_PROPERTY(bool restored READ restored NOTIFY stateChanged)
    Q_PROPERTY(bool validEmpty READ validEmpty NOTIFY stateChanged)            // searched, zero hits
    Q_PROPERTY(bool fallbackActive READ fallbackActive NOTIFY stateChanged)    // rows from the extractor
    Q_PROPERTY(int hiddenShorts READ hiddenShorts NOTIFY stateChanged)
    Q_PROPERTY(int hiddenMembersOnly READ hiddenMembersOnly NOTIFY stateChanged)
    Q_PROPERTY(int hiddenCount READ hiddenCount NOTIFY stateChanged)
    // Filters (values of the catalog::Search* enums; 0 = any / relevance).
    Q_PROPERTY(int sort READ sort WRITE setSort NOTIFY filtersChanged)
    Q_PROPERTY(int uploadDate READ uploadDate WRITE setUploadDate NOTIFY filtersChanged)
    Q_PROPERTY(int type READ type WRITE setType NOTIFY filtersChanged)
    Q_PROPERTY(int duration READ duration WRITE setDuration NOTIFY filtersChanged)
    Q_PROPERTY(bool live READ live WRITE setLive NOTIFY filtersChanged)
    Q_PROPERTY(bool mixed READ mixed WRITE setMixed NOTIFY filtersChanged)
    Q_PROPERTY(bool filtersActive READ filtersActive NOTIFY filtersChanged)
    Q_PROPERTY(bool hideShorts READ hideShorts WRITE setHideShorts NOTIFY hidingChanged)
    Q_PROPERTY(bool hideMembersOnly READ hideMembersOnly WRITE setHideMembersOnly NOTIFY hidingChanged)
public:
    enum Role {
        VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationRole,
        DurationSecondsRole, ViewCountRole, ThumbnailRole, LiveRole,
        // Additions.
        KindRole, ItemIdRole, UpcomingRole, MembersOnlyRole, RestrictedRole, ShortRole, AvatarRole,
        SubscriberCountRole, SubscriberTextRole, VideoCountRole, VideoCountTextRole, ViewTextRole,
        PublishedTextRole, HandleRole, PlaylistIdRole,
        ChannelAvatarRole,  // like `avatar`, with the avatar known for the channel as the fallback
        ThumbnailLargeRole  // the 1280x720 picture for cards (LargeThumbnails)
    };

    explicit DiscoverySearchModel(catalog::SearchService *service, QObject *parent = nullptr,
                                  catalog::CatalogClock now = catalog::CatalogClock());
    ~DiscoverySearchModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return query_; }
    bool busy() const { return busy_; }
    bool canLoadMore() const { return canLoadMore_ && !busy_; }
    QString errorText() const { return errorText_; }
    int count() const { return static_cast<int>(items_.size()); }
    bool validEmpty() const { return validEmpty_; }
    bool fallbackActive() const { return fallbackActive_; }
    int hiddenShorts() const { return hiddenShorts_; }
    int hiddenMembersOnly() const { return hiddenMembersOnly_; }
    int hiddenWatched() const { return hiddenWatched_; }
    int hiddenCount() const { return hiddenShorts_ + hiddenMembersOnly_ + hiddenRestricted_ + hiddenWatched_; }

    int sort() const { return static_cast<int>(filter_.sort); }
    int uploadDate() const { return static_cast<int>(filter_.uploadDate); }
    int type() const { return static_cast<int>(filter_.type); }
    int duration() const { return static_cast<int>(filter_.duration); }
    bool live() const { return filter_.live; }
    bool mixed() const { return mixed_; }
    bool filtersActive() const { return !filter_.isDefault(); }
    bool hideShorts() const { return hiding_.hideShorts; }
    bool hideMembersOnly() const { return hiding_.hideMembersOnly; }
    void setSort(int v);
    void setUploadDate(int v);
    void setType(int v);
    void setDuration(int v);
    void setLive(bool v);
    void setMixed(bool v);
    void setHideShorts(bool v);
    void setHideMembersOnly(bool v);

    // DSC-17 Shorts knowledge (persistent stores owned elsewhere).
    void setShortsLookup(const catalog::ShortsLookup &lookup) { lookup_ = lookup; }
    catalog::SearchCache &cache() { return cache_; }
    catalog::SearchFilter filter() const { return filter_; }
    const catalog::Item *itemAt(int row) const;

    Q_INVOKABLE void search(const QString &query);
    // The user started typing: open the connection the search will use (NFR-PERF-09).
    Q_INVOKABLE void prewarm();
    // Drops a pending next-page prefetch (a video is about to open: its bytes come first).
    Q_INVOKABLE void cancelPrefetch();
    // Re-runs the restored search and swaps the rows when page 1 arrives (no empty flash).
    Q_INVOKABLE void refreshInPlace();
    bool restored() const { return restored_; }

    // Last-search snapshot (instant first paint): written after each fresh page 1 while
    // `allowed()` says so (the search history is not paused), read by restoreSnapshot().
    static const qint64 kSnapshotMaxAgeMs = 7LL * 24 * 3600 * 1000;
    void setSnapshot(const QString &path, const std::function<bool()> &allowed = std::function<bool()>());
    bool restoreSnapshot(qint64 nowMs = -1);
    void clearSnapshot();
    // Next page fetched ahead into the cache after a shown page (default on; tests).
    void setPrefetch(bool on, int delayMs = kPrefetchDelayMs) { prefetchOn_ = on; prefetchDelayMs_ = delayMs; }
    static const int kPrefetchDelayMs = 250;
    int prefetches() const { return prefetches_; }   // tests
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QVariantMap get(int row) const;
    // Applies all filters at once and re-runs the query (one request).
    Q_INVOKABLE void setFilters(int sort, int uploadDate, int type, int duration, bool live, bool mixed);
    Q_INVOKABLE void resetFilters();

    static QString normalizedQuery(const QString &query);
    static QString outcomeText(catalog::Outcome outcome);

signals:
    void stateChanged();
    void filtersChanged();
    void hidingChanged();

private:
    void applyFilter(const catalog::SearchFilter &filter, bool mixed);
    void requestPage(std::size_t index);
    void onResult(std::size_t index, const catalog::SearchResult &result);
    void applyPage(std::size_t index, const catalog::CachedSearchPage &page);
    void refilter();
    void startSearch(const QString &text, bool keepShown);
    void schedulePrefetch();
    void writeSnapshot(const catalog::CachedSearchPage &page);
    void startPrefetch(quint64 searchGeneration);
    std::string kind() const { return mixed_ ? "mixed" : "videos"; }

    catalog::SearchService *service_;
    catalog::SearchCache cache_;
    catalog::SearchPaging paging_;
    catalog::SearchFilter filter_;
    catalog::RowFilterSettings hiding_;
    catalog::ShortsLookup lookup_;
    bool mixed_ = true;

    std::vector<catalog::Item> raw_;     // every row received, unfiltered
    std::vector<catalog::Item> items_;   // visible rows
    std::set<std::string> seen_;
    int hiddenShorts_ = 0, hiddenMembersOnly_ = 0, hiddenRestricted_ = 0, hiddenWatched_ = 0;
    catalog::RowFilterSettings hidingNow() const;   // hiding_ plus the LIB-07 watched hook

    QString query_;
    std::string key_;
    std::string token_;
    std::size_t pages_ = 0;
    QString errorText_;
    bool busy_ = false;
    bool canLoadMore_ = false;
    bool validEmpty_ = false;
    bool fallbackActive_ = false;
    quint64 op_ = 0;
    quint64 generation_ = 0;
    QElapsedTimer timing_;
    bool timingPending_ = false;

    // Next-page prefetch: one at a time, for the current search only (searchGeneration_).
    bool prefetchOn_ = true;
    int prefetchDelayMs_ = kPrefetchDelayMs;
    quint64 searchGeneration_ = 0;
    quint64 prefetchOp_ = 0;
    quint64 prefetchTicket_ = 0;

    QString snapshotPath_;
    std::function<bool()> snapshotAllowed_;
    bool restored_ = false;
    bool replaceOnFirst_ = false;
    std::size_t prefetchIndex_ = 0;
    bool prefetchAwaited_ = false;   // loadMore() arrived while the prefetch was in flight
    int prefetches_ = 0;
};

}}}
