// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DiscoverySearchModel.h"
#include "WatchedRows.h"
#include "ChannelAvatars.h"
#include "LargeThumbnails.h"
#include "Core/Catalog/Infrastructure/SearchService.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include "Core/Catalog/Infrastructure/FeedCacheStore.h"
#include <QtGlobal>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Item;
using catalog::ItemKind;
using catalog::Outcome;

namespace {

QString clockText(qint64 seconds) {
    if (seconds < 0) return QString();
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    if (h > 0) return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(s, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString q(const std::string &s) { return QString::fromStdString(s); }

const char *kindName(ItemKind k) {
    switch (k) {
    case ItemKind::Video: return "video";
    case ItemKind::Short: return "short";
    case ItemKind::Channel: return "channel";
    case ItemKind::Playlist: return "playlist";
    }
    return "video";
}

}  // namespace

DiscoverySearchModel::DiscoverySearchModel(catalog::SearchService *service, QObject *parent, catalog::CatalogClock now)
    : QAbstractListModel(parent), service_(service), cache_(now) {
    // LIB-07: the library's hide-watched setting (search only with its own option).
    connect(WatchedRows::instance(), &WatchedRows::changed, this, [this] { if (!raw_.empty()) refilter(); });
    ChannelAvatars::follow(this, ChannelAvatarRole);
    LargeThumbnails::follow(this, ThumbnailLargeRole);
}

DiscoverySearchModel::~DiscoverySearchModel() { if (op_ && service_) service_->cancel(op_); }

int DiscoverySearchModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : static_cast<int>(items_.size());
}

const Item *DiscoverySearchModel::itemAt(int row) const {
    if (row < 0 || row >= static_cast<int>(items_.size())) return nullptr;
    return &items_[static_cast<std::size_t>(row)];
}

QVariant DiscoverySearchModel::data(const QModelIndex &index, int role) const {
    const Item *p = index.isValid() ? itemAt(index.row()) : nullptr;
    if (!p) return QVariant();
    const Item &v = *p;
    const bool videoLike = catalog::isVideoLike(v);
    switch (role) {
    case VideoIdRole: return videoLike ? q(v.id) : QString();
    case TitleRole: return q(v.title);
    case ChannelRole: return q(v.channel);
    case ChannelIdRole: return v.kind == ItemKind::Channel ? q(v.id) : q(v.channelId);
    case DurationRole: return v.durationSeconds >= 0 ? clockText(v.durationSeconds) : q(v.durationText);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ViewCountRole: return static_cast<double>(v.viewCount);
    case ThumbnailRole: return v.thumbnailUrl.empty() ? q(v.avatarUrl) : q(v.thumbnailUrl);
    case LiveRole: return v.liveStatus == media::LiveStatus::IsLive;
    case KindRole: return QString::fromLatin1(kindName(v.kind));
    case ItemIdRole: return q(v.id);
    case UpcomingRole: return v.liveStatus == media::LiveStatus::Upcoming;
    case MembersOnlyRole: return v.membersOnly;
    case RestrictedRole: return v.restricted;
    case ShortRole: return v.kind == ItemKind::Short;
    case AvatarRole: return q(v.avatarUrl);
    case SubscriberCountRole: return static_cast<double>(v.subscriberCount);
    case SubscriberTextRole: return q(v.subscriberText);
    case VideoCountRole: return static_cast<double>(v.videoCount);
    case VideoCountTextRole: return q(v.videoCountText);
    case ViewTextRole: return q(v.viewText);
    case PublishedTextRole: return q(v.publishedText);
    case HandleRole: return q(v.handle);
    case PlaylistIdRole: return v.kind == ItemKind::Playlist ? q(v.id) : q(v.playlistId);
    case ChannelAvatarRole: return ChannelAvatars::forRow(v);
    case ThumbnailLargeRole: return LargeThumbnails::forThumbnail(v.thumbnailUrl.empty() ? v.avatarUrl : v.thumbnailUrl);
    default: return QVariant();
    }
}

QHash<int, QByteArray> DiscoverySearchModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(VideoIdRole, "videoId"); r.insert(TitleRole, "title"); r.insert(ChannelRole, "channel");
    r.insert(ChannelIdRole, "channelId"); r.insert(DurationRole, "duration"); r.insert(DurationSecondsRole, "durationSeconds");
    r.insert(ViewCountRole, "viewCount"); r.insert(ThumbnailRole, "thumbnail"); r.insert(LiveRole, "live");
    r.insert(KindRole, "kind"); r.insert(ItemIdRole, "itemId"); r.insert(UpcomingRole, "upcoming");
    r.insert(MembersOnlyRole, "membersOnly"); r.insert(RestrictedRole, "restricted"); r.insert(ShortRole, "isShort");
    r.insert(AvatarRole, "avatar"); r.insert(SubscriberCountRole, "subscriberCount"); r.insert(SubscriberTextRole, "subscriberText");
    r.insert(VideoCountRole, "videoCount"); r.insert(VideoCountTextRole, "videoCountText"); r.insert(ViewTextRole, "viewText");
    r.insert(PublishedTextRole, "publishedText"); r.insert(HandleRole, "handle"); r.insert(PlaylistIdRole, "playlistId");
    r.insert(ChannelAvatarRole, "channelAvatar");
    r.insert(ThumbnailLargeRole, "thumbnailLarge");
    return r;
}

QVariantMap DiscoverySearchModel::get(int row) const {
    QVariantMap m;
    if (!itemAt(row)) return m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

QString DiscoverySearchModel::normalizedQuery(const QString &query) {
    QString s = query;
    s.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")), QStringLiteral(" "));
    return s.simplified().left(200);
}

QString DiscoverySearchModel::outcomeText(Outcome outcome) {
    switch (outcome) {
    case Outcome::Ok: case Outcome::ValidEmpty: return QString();
    case Outcome::Network: return QCoreApplication::translate("SailTubeErrors", "YouTube could not be reached. Check the connection.");
    case Outcome::RateLimited: return QCoreApplication::translate("SailTubeErrors", "YouTube is limiting requests from this network. Try again later.");
    case Outcome::Http5xx: return QCoreApplication::translate("SailTubeErrors", "YouTube is having problems. Try again later.");
    case Outcome::Http4xx: return QCoreApplication::translate("SailTubeErrors", "YouTube rejected the search. Try again later.");
    case Outcome::ShapeDrift: return QCoreApplication::translate("SailTubeErrors", "YouTube answered in a form SailTube cannot read yet. Try again later or update SailTube.");
    case Outcome::Unavailable: return QCoreApplication::translate("SailTubeErrors", "Search is not available right now.");
    }
    return QString();
}

void DiscoverySearchModel::search(const QString &text) { startSearch(text, false); }

void DiscoverySearchModel::startSearch(const QString &text, bool keepShown) {
    const QString nq = normalizedQuery(text);
    cancel();
    cancelPrefetch();
    ++searchGeneration_;
    replaceOnFirst_ = keepShown;
    if (!keepShown) {
        beginResetModel();
        raw_.clear();
        items_.clear();
        seen_.clear();
        endResetModel();
        restored_ = false;
    }
    hiddenShorts_ = hiddenMembersOnly_ = hiddenRestricted_ = hiddenWatched_ = 0;
    query_ = nq;
    errorText_.clear();
    canLoadMore_ = false;
    validEmpty_ = false;
    fallbackActive_ = false;
    pages_ = 0;
    token_.clear();
    paging_.reset();
    key_ = catalog::SearchCache::key(nq.toStdString(), catalog::effectiveSearchFilter(filter_, mixed_), kind());
    emit stateChanged();
    if (nq.isEmpty()) return;
    timing_.start();
    timingPending_ = true;
    requestPage(0);
}

void DiscoverySearchModel::loadMore() {
    if (!canLoadMore()) return;
    paging_.userRequested();
    errorText_.clear();
    requestPage(pages_);
}

void DiscoverySearchModel::cancel() {
    ++generation_;
    if (!busy_ && !op_) return;
    if (op_ && service_) service_->cancel(op_);
    op_ = 0;
    busy_ = false;
    emit stateChanged();
}

void DiscoverySearchModel::requestPage(std::size_t index) {
    if (const catalog::CachedSearchPage *cached = cache_.page(key_, index)) {
        const catalog::CachedSearchPage copy = *cached;   // applyPage may touch the cache
        applyPage(index, copy);
        return;
    }
    if (!service_) return;
    if (prefetchOp_ && prefetchIndex_ == index) {
        // The page is already on its way: show it when it lands instead of asking twice.
        prefetchAwaited_ = true;
        busy_ = true;
        emit stateChanged();
        return;
    }
    if (index > 0 && token_.empty()) return;
    const quint64 generation = ++generation_;
    busy_ = true;
    emit stateChanged();
    catalog::SearchService::Callback done = [this, generation, index](const catalog::SearchResult &r) {
        if (generation != generation_) return;
        op_ = 0;
        busy_ = false;
        onResult(index, r);
    };
    const catalog::SearchFilter f = catalog::effectiveSearchFilter(filter_, mixed_);
    const quint64 id = index == 0 ? service_->firstPage(query_, f, done)
                                  : service_->nextPage(query_, f, q(token_), static_cast<int>(raw_.size()), done);
    if (busy_ && generation == generation_) op_ = id;
}

void DiscoverySearchModel::onResult(std::size_t index, const catalog::SearchResult &r) {
    if (catalog::isFailure(r.outcome)) {
        errorText_ = outcomeText(r.outcome);
        // A failed deeper page can be retried by scrolling; page 1 has nothing to continue.
        canLoadMore_ = index > 0 && !token_.empty();
        timingPending_ = false;
        emit stateChanged();
        return;
    }
    catalog::CachedSearchPage page;
    page.rows = r.rows;
    page.continuation = r.continuation.toStdString();
    page.fallback = r.fallback;
    // Fallback pages are not cached: their offsets belong to a different tier.
    if (!r.fallback) cache_.store(key_, index, page);
    applyPage(index, page);
}

void DiscoverySearchModel::applyPage(std::size_t index, const catalog::CachedSearchPage &page) {
    if (index == 0 && replaceOnFirst_) {
        // refreshInPlace(): the restored rows go only now, in one reset.
        replaceOnFirst_ = false;
        restored_ = false;
        beginResetModel();
        raw_.clear();
        items_.clear();
        seen_.clear();
        endResetModel();
    }
    if (index == 0 && !page.fallback && !page.rows.empty()) writeSnapshot(page);
    raw_.insert(raw_.end(), page.rows.begin(), page.rows.end());
    const catalog::RowFilterResult f = catalog::filterRows(page.rows, hidingNow(), &seen_, lookup_);
    hiddenShorts_ += f.hiddenShorts;
    hiddenWatched_ += f.hiddenWatched;
    hiddenMembersOnly_ += f.hiddenMembersOnly;
    hiddenRestricted_ += f.hiddenRestricted;
    if (!f.visible.empty()) {
        const int first = static_cast<int>(items_.size());
        beginInsertRows(QModelIndex(), first, first + static_cast<int>(f.visible.size()) - 1);
        items_.insert(items_.end(), f.visible.begin(), f.visible.end());
        endInsertRows();
    }
    pages_ = index + 1;
    token_ = page.continuation;
    canLoadMore_ = !token_.empty();
    fallbackActive_ = fallbackActive_ || page.fallback;
    if (index == 0) validEmpty_ = page.rows.empty();
    if (timingPending_ && index == 0) {
        timingPending_ = false;
        qInfo("sailtube-timing search page1 %lld", static_cast<long long>(timing_.elapsed()));
    }
    const catalog::PagingStep step = paging_.afterPage(static_cast<int>(f.visible.size()), !token_.empty());
    emit stateChanged();
    if (step == catalog::PagingStep::Wait) schedulePrefetch();
    if (step == catalog::PagingStep::FetchNext) requestPage(pages_);
}

void DiscoverySearchModel::prewarm() {
    if (service_ && service_->client() && service_->client()->transport())
        service_->client()->transport()->warm(QStringLiteral("www.youtube.com"));
}

void DiscoverySearchModel::refreshInPlace() {
    if (!restored_ || query_.isEmpty()) return;
    startSearch(query_, true);
}

void DiscoverySearchModel::setSnapshot(const QString &path, const std::function<bool()> &allowed) {
    snapshotPath_ = path;
    snapshotAllowed_ = allowed;
}

void DiscoverySearchModel::writeSnapshot(const catalog::CachedSearchPage &page) {
    if (snapshotPath_.isEmpty() || (snapshotAllowed_ && !snapshotAllowed_())) return;
    QJsonObject o;
    o.insert(QStringLiteral("v"), 1);
    o.insert(QStringLiteral("q"), query_);
    o.insert(QStringLiteral("saved"), static_cast<double>(QDateTime::currentMSecsSinceEpoch()));
    o.insert(QStringLiteral("sort"), static_cast<int>(filter_.sort));
    o.insert(QStringLiteral("date"), static_cast<int>(filter_.uploadDate));
    o.insert(QStringLiteral("type"), static_cast<int>(filter_.type));
    o.insert(QStringLiteral("len"), static_cast<int>(filter_.duration));
    o.insert(QStringLiteral("live"), filter_.live);
    o.insert(QStringLiteral("mixed"), mixed_);
    QJsonArray rows;
    for (const Item &i : page.rows) rows.append(catalog::cachedItemToJson(i));
    o.insert(QStringLiteral("rows"), rows);
    QDir().mkpath(QFileInfo(snapshotPath_).absolutePath());
    QSaveFile file(snapshotPath_);
    if (!file.open(QIODevice::WriteOnly)) return;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    file.commit();
}

bool DiscoverySearchModel::restoreSnapshot(qint64 nowMs) {
    if (snapshotPath_.isEmpty() || !query_.isEmpty() || !items_.empty()) return false;
    QFile file(snapshotPath_);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QJsonObject o = QJsonDocument::fromJson(file.readAll()).object();
    const qint64 now = nowMs >= 0 ? nowMs : QDateTime::currentMSecsSinceEpoch();
    const qint64 saved = static_cast<qint64>(o.value(QStringLiteral("saved")).toDouble());
    const QString q0 = normalizedQuery(o.value(QStringLiteral("q")).toString());
    const QJsonArray rows = o.value(QStringLiteral("rows")).toArray();
    if (o.value(QStringLiteral("v")).toInt() != 1 || q0.isEmpty() || rows.isEmpty() || saved <= 0 ||
        now - saved > kSnapshotMaxAgeMs || now < saved)
        return false;
    catalog::SearchFilter f;
    f.sort = static_cast<catalog::SearchSort>(qBound(0, o.value(QStringLiteral("sort")).toInt(), 3));
    f.uploadDate = static_cast<catalog::UploadDateFilter>(qBound(0, o.value(QStringLiteral("date")).toInt(), 5));
    f.type = static_cast<catalog::ResultTypeFilter>(qBound(0, o.value(QStringLiteral("type")).toInt(), 3));
    f.duration = static_cast<catalog::DurationFilter>(qBound(0, o.value(QStringLiteral("len")).toInt(), 3));
    f.live = o.value(QStringLiteral("live")).toBool();
    filter_ = f;
    mixed_ = o.value(QStringLiteral("mixed")).toBool(true);
    query_ = q0;
    key_ = catalog::SearchCache::key(q0.toStdString(), catalog::effectiveSearchFilter(filter_, mixed_), kind());
    std::vector<Item> list;
    for (const QJsonValue &v : rows) list.push_back(catalog::cachedItemFromJson(v.toObject()));
    raw_ = list;
    seen_.clear();
    const catalog::RowFilterResult r = catalog::filterRows(raw_, hidingNow(), &seen_, lookup_);
    beginResetModel();
    items_ = r.visible;
    endResetModel();
    hiddenShorts_ = r.hiddenShorts;
    hiddenMembersOnly_ = r.hiddenMembersOnly;
    hiddenRestricted_ = r.hiddenRestricted;
    hiddenWatched_ = r.hiddenWatched;
    restored_ = true;
    canLoadMore_ = false;   // its tokens are stale; refreshInPlace() brings fresh ones
    pages_ = 1;
    emit filtersChanged();
    emit stateChanged();
    return true;
}

void DiscoverySearchModel::clearSnapshot() {
    if (!snapshotPath_.isEmpty()) QFile::remove(snapshotPath_);
}

void DiscoverySearchModel::cancelPrefetch() {
    ++prefetchTicket_;   // a scheduled, not yet started prefetch is dropped too
    if (prefetchOp_ && service_) service_->cancel(prefetchOp_);
    prefetchOp_ = 0;
    if (prefetchAwaited_) {
        prefetchAwaited_ = false;
        busy_ = false;
        emit stateChanged();
    }
}

void DiscoverySearchModel::schedulePrefetch() {
    if (!prefetchOn_ || !service_ || token_.empty() || fallbackActive_ || prefetchOp_) return;
    if (cache_.page(key_, pages_)) return;
    const quint64 generation = searchGeneration_;
    const quint64 ticket = prefetchTicket_;
    QTimer::singleShot(prefetchDelayMs_, this, [this, generation, ticket] {
        if (ticket == prefetchTicket_) startPrefetch(generation);
    });
}

void DiscoverySearchModel::startPrefetch(quint64 generation) {
    // Still the same search, nothing else in flight, and the page is still missing.
    if (generation != searchGeneration_ || busy_ || prefetchOp_ || token_.empty() || cache_.page(key_, pages_)) return;
    const std::size_t index = pages_;
    const std::string key = key_;
    prefetchIndex_ = index;
    ++prefetches_;
    catalog::SearchService::Callback done = [this, generation, index, key](const catalog::SearchResult &r) {
        if (generation != searchGeneration_ || !prefetchOp_) return;
        prefetchOp_ = 0;
        const bool awaited = prefetchAwaited_;
        prefetchAwaited_ = false;
        if (!catalog::isFailure(r.outcome) && !r.fallback) {
            catalog::CachedSearchPage page;
            page.rows = r.rows;
            page.continuation = r.continuation.toStdString();
            cache_.store(key, index, page);
        }
        if (awaited) {
            busy_ = false;
            if (pages_ == index) onResult(index, r);
            else emit stateChanged();
        }
    };
    const catalog::SearchFilter f = catalog::effectiveSearchFilter(filter_, mixed_);
    prefetchOp_ = service_->prefetchPage(query_, f, q(token_), done);
}

catalog::RowFilterSettings DiscoverySearchModel::hidingNow() const {
    catalog::RowFilterSettings s = hiding_;
    s.hideWatched = WatchedRows::instance()->forContext(QStringLiteral("search"));
    return s;
}

void DiscoverySearchModel::refilter() {
    seen_.clear();
    const catalog::RowFilterResult f = catalog::filterRows(raw_, hidingNow(), &seen_, lookup_);
    hiddenShorts_ = f.hiddenShorts;
    hiddenMembersOnly_ = f.hiddenMembersOnly;
    hiddenRestricted_ = f.hiddenRestricted;
    hiddenWatched_ = f.hiddenWatched;
    bool same = f.visible.size() == items_.size();
    for (std::size_t i = 0; same && i < items_.size(); ++i)
        same = f.visible[i].kind == items_[i].kind && f.visible[i].id == items_[i].id;
    if (!same) {
        // Keeps the view in place when nothing visible changed (a watch-state tick).
        beginResetModel();
        items_ = f.visible;
        endResetModel();
    }
    emit stateChanged();
}

void DiscoverySearchModel::applyFilter(const catalog::SearchFilter &filter, bool mixed) {
    if (filter == filter_ && mixed == mixed_) return;
    filter_ = filter;
    mixed_ = mixed;
    emit filtersChanged();
    if (!query_.isEmpty()) search(query_);   // any change re-runs the query
}

void DiscoverySearchModel::setSort(int v) {
    catalog::SearchFilter f = filter_;
    f.sort = static_cast<catalog::SearchSort>(qBound(0, v, 3));
    applyFilter(f, mixed_);
}

void DiscoverySearchModel::setUploadDate(int v) {
    catalog::SearchFilter f = filter_;
    f.uploadDate = static_cast<catalog::UploadDateFilter>(qBound(0, v, 5));
    applyFilter(f, mixed_);
}

void DiscoverySearchModel::setType(int v) {
    catalog::SearchFilter f = filter_;
    f.type = static_cast<catalog::ResultTypeFilter>(qBound(0, v, 3));
    applyFilter(f, mixed_);
}

void DiscoverySearchModel::setDuration(int v) {
    catalog::SearchFilter f = filter_;
    f.duration = static_cast<catalog::DurationFilter>(qBound(0, v, 3));
    applyFilter(f, mixed_);
}

void DiscoverySearchModel::setLive(bool v) {
    catalog::SearchFilter f = filter_;
    f.live = v;
    applyFilter(f, mixed_);
}

void DiscoverySearchModel::setMixed(bool v) { applyFilter(filter_, v); }

void DiscoverySearchModel::setFilters(int sort, int uploadDate, int type, int duration, bool live, bool mixed) {
    catalog::SearchFilter f;
    f.sort = static_cast<catalog::SearchSort>(qBound(0, sort, 3));
    f.uploadDate = static_cast<catalog::UploadDateFilter>(qBound(0, uploadDate, 5));
    f.type = static_cast<catalog::ResultTypeFilter>(qBound(0, type, 3));
    f.duration = static_cast<catalog::DurationFilter>(qBound(0, duration, 3));
    f.live = live;
    applyFilter(f, mixed);
}

void DiscoverySearchModel::resetFilters() { applyFilter(catalog::SearchFilter(), mixed_); }

void DiscoverySearchModel::setHideShorts(bool v) {
    if (v == hiding_.hideShorts) return;
    hiding_.hideShorts = v;
    emit hidingChanged();
    refilter();
}

void DiscoverySearchModel::setHideMembersOnly(bool v) {
    if (v == hiding_.hideMembersOnly) return;
    hiding_.hideMembersOnly = v;
    emit hidingChanged();
    refilter();
}

}}}
