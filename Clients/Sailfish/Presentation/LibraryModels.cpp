// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LibraryModels.h"

#include "Core/Library/Domain/LibraryPolicy.h"
#include "Core/Library/Infrastructure/LibraryStore.h"
#include <QQmlEngine>
#include <algorithm>
#include <random>

namespace nordtile { namespace video { namespace sailfish {

using namespace library;

namespace {
QString q(const std::string &s) { return QString::fromUtf8(s.data(), static_cast<int>(s.size())); }
std::string u(const QString &s) { return s.toUtf8().toStdString(); }
const char *const hideWatchedKey = "hide_watched";
const char *const hideWatchedSearchKey = "hide_watched_search";
}

VideoEntry videoFromMap(const QVariantMap &map) {
    VideoEntry v;
    v.videoId = u(map.value(QStringLiteral("videoId")).toString());
    v.title = u(map.value(QStringLiteral("title")).toString());
    v.channel = u(map.value(QStringLiteral("channel")).toString());
    v.channelId = u(map.value(QStringLiteral("channelId")).toString());
    v.thumbnailUrl = u(map.value(QStringLiteral("thumbnail")).toString());
    bool ok = false;
    const double seconds = map.value(QStringLiteral("durationSeconds"), -1).toDouble(&ok);
    v.durationSeconds = ok && seconds > 0 ? static_cast<std::int64_t>(seconds) : -1;
    return v;
}

QVariantMap videoToMap(const VideoEntry &v) {
    QVariantMap m;
    m.insert(QStringLiteral("videoId"), q(v.videoId));
    m.insert(QStringLiteral("title"), q(v.title));
    m.insert(QStringLiteral("channel"), q(v.channel));
    m.insert(QStringLiteral("channelId"), q(v.channelId));
    m.insert(QStringLiteral("durationSeconds"), static_cast<double>(v.durationSeconds));
    m.insert(QStringLiteral("thumbnail"), q(v.thumbnailUrl));
    return m;
}

// ---- subscriptions --------------------------------------------------------

SubscriptionsModel::SubscriptionsModel(LibraryStore *store, QObject *parent)
    : QAbstractListModel(parent), store_(store) {
    connect(store_, &LibraryStore::subscriptionsChanged, this, &SubscriptionsModel::reload);
    reload();
}

int SubscriptionsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QVariant SubscriptionsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= count()) return QVariant();
    const ChannelRecord &c = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case ChannelIdRole: return q(c.channelId);
    case NameRole: return q(c.name);
    case AvatarRole: return q(c.avatarUrl);
    case LatestUploadRole: return static_cast<double>(c.latestUploadMs);
    default: return QVariant();
    }
}

QHash<int, QByteArray> SubscriptionsModel::roleNames() const {
    return { { ChannelIdRole, "channelId" }, { NameRole, "name" }, { AvatarRole, "avatar" },
             { LatestUploadRole, "latestUpload" } };
}

void SubscriptionsModel::setSortMode(int mode) {
    mode = mode == 1 ? 1 : 0;
    if (mode == sort_) return;
    sort_ = mode;
    emit sortModeChanged();
    reload();
}

int SubscriptionsModel::pendingHandles() const { return static_cast<int>(store_->pendingHandles().size()); }

bool SubscriptionsModel::isSubscribed(const QString &channelId) const { return store_->isSubscribed(channelId); }

bool SubscriptionsModel::subscribe(const QString &channelId, const QString &name, const QString &avatar) {
    ChannelRecord c;
    c.channelId = u(channelId);
    c.name = u(name);
    c.avatarUrl = u(avatar);
    return store_->subscribe(c);
}

bool SubscriptionsModel::unsubscribe(const QString &channelId) { return store_->unsubscribe(channelId); }

bool SubscriptionsModel::toggle(const QString &channelId, const QString &name, const QString &avatar) {
    if (store_->isSubscribed(channelId)) { store_->unsubscribe(channelId); return false; }
    return subscribe(channelId, name, avatar);
}

QStringList SubscriptionsModel::channelIds() const { return store_->subscribedChannelIds(); }

void SubscriptionsModel::reload() {
    beginResetModel();
    rows_ = store_->subscriptions(sort_ == 1 ? SubscriptionSort::LatestUpload : SubscriptionSort::Name);
    endResetModel();
    ++revision_;
    emit countChanged();
}

// ---- playlists ------------------------------------------------------------

PlaylistsModel::PlaylistsModel(LibraryStore *store, QObject *parent) : QAbstractListModel(parent), store_(store) {
    connect(store_, &LibraryStore::playlistsChanged, this, &PlaylistsModel::reload);
    connect(store_, &LibraryStore::playlistChanged, this, &PlaylistsModel::reload);
    reload();
}

int PlaylistsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QVariant PlaylistsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= count()) return QVariant();
    const PlaylistRecord &p = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case IdRole: return q(p.id);
    case KindRole: return static_cast<int>(p.kind);
    case TitleRole: return q(p.title);
    case ItemCountRole: return p.itemCount;
    case ThumbnailRole: return q(p.thumbnailUrl);
    case RemoteIdRole: return q(p.remoteId);
    case ChannelRole: return q(p.channel);
    case WatchLaterRole: return p.kind == PlaylistKind::WatchLater;
    case MirrorRole: return p.kind == PlaylistKind::RemoteMirror;
    case RefreshedRole: return static_cast<double>(p.refreshedMs);
    default: return QVariant();
    }
}

QHash<int, QByteArray> PlaylistsModel::roleNames() const {
    return { { IdRole, "playlistId" }, { KindRole, "kind" }, { TitleRole, "title" }, { ItemCountRole, "itemCount" },
             { ThumbnailRole, "thumbnail" }, { RemoteIdRole, "remoteId" }, { ChannelRole, "channel" },
             { WatchLaterRole, "watchLater" }, { MirrorRole, "mirror" }, { RefreshedRole, "refreshed" } };
}

QString PlaylistsModel::create(const QString &title) { return store_->createPlaylist(title); }

QString PlaylistsModel::createWith(const QString &title, const QVariantMap &video) {
    const QString id = store_->createPlaylist(title);
    if (!id.isEmpty()) store_->addToPlaylist(id, videoFromMap(video));
    return id;
}

bool PlaylistsModel::rename(const QString &id, const QString &title) { return store_->renamePlaylist(id, title); }
bool PlaylistsModel::remove(const QString &id) { return store_->deletePlaylist(id); }
bool PlaylistsModel::move(const QString &id, int toIndex) { return store_->movePlaylist(id, toIndex); }
bool PlaylistsModel::contains(const QString &id, const QString &videoId) const { return store_->playlistContains(id, videoId); }
bool PlaylistsModel::add(const QString &id, const QVariantMap &video) { return store_->addToPlaylist(id, videoFromMap(video)); }
bool PlaylistsModel::removeVideo(const QString &id, const QString &videoId) { return store_->removeFromPlaylist(id, videoId); }

bool PlaylistsModel::inWatchLater(const QString &videoId) const {
    return store_->playlistContains(QString::fromLatin1(watchLaterPlaylistId), videoId);
}

bool PlaylistsModel::toggleWatchLater(const QVariantMap &video) {
    const QString list = QString::fromLatin1(watchLaterPlaylistId);
    const QString id = video.value(QStringLiteral("videoId")).toString();
    if (store_->playlistContains(list, id)) { store_->removeFromPlaylist(list, id); return false; }
    return store_->addToPlaylist(list, videoFromMap(video));
}

namespace {
// The browse id form "VL<listId>" names the same list (LIB-03 preview, LNK-04 links).
QString bareListId(const QString &id) {
    const QString bare = id.startsWith(QLatin1String("VL")) && id.size() > 4 ? id.mid(2) : id;
    const std::string normal = library::playlistIdFromUrl(bare.toStdString()); // URLs with list= too
    return normal.empty() ? bare : QString::fromStdString(normal);
}
}

QString PlaylistsModel::saveRemote(const QString &remoteId, const QString &title, const QString &channel,
                                   const QString &thumbnail, int itemCount) {
    RemotePlaylistRef ref;
    ref.remoteId = u(bareListId(remoteId));
    ref.title = u(title);
    ref.channel = u(channel);
    ref.thumbnailUrl = u(thumbnail);
    ref.itemCountHint = itemCount;
    return store_->saveRemotePlaylist(ref);
}

QString PlaylistsModel::mirrorFor(const QString &remoteId) const { return store_->mirrorIdForRemote(bareListId(remoteId)); }

QVariantMap PlaylistsModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

QObject *PlaylistsModel::openList(const QString &id) {
    PlaylistItemsModel *model = new PlaylistItemsModel(store_);
    model->setPlaylistId(id);
    QQmlEngine::setObjectOwnership(model, QQmlEngine::JavaScriptOwnership);
    return model;
}

void PlaylistsModel::reload() {
    beginResetModel();
    rows_ = store_->playlists();
    endResetModel();
    ++revision_;
    emit countChanged();
}

// ---- playlist items ---------------------------------------------------------

PlaylistItemsModel::PlaylistItemsModel(LibraryStore *store, QObject *parent)
    : QAbstractListModel(parent), store_(store) {
    connect(store_, &LibraryStore::playlistChanged, this, [this](const QString &id) { if (id == id_) reload(); });
    connect(store_, &LibraryStore::playlistsChanged, this, [this]() { if (!id_.isEmpty()) reload(); });
}

int PlaylistItemsModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QVariant PlaylistItemsModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= count()) return QVariant();
    const VideoEntry &v = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case VideoIdRole: return q(v.videoId);
    case TitleRole: return q(v.title);
    case ChannelRole: return q(v.channel);
    case ChannelIdRole: return q(v.channelId);
    case DurationSecondsRole: return static_cast<double>(v.durationSeconds);
    case ThumbnailRole: return q(v.thumbnailUrl);
    default: return QVariant();
    }
}

QHash<int, QByteArray> PlaylistItemsModel::roleNames() const {
    return { { VideoIdRole, "videoId" }, { TitleRole, "title" }, { ChannelRole, "channel" },
             { ChannelIdRole, "channelId" }, { DurationSecondsRole, "durationSeconds" }, { ThumbnailRole, "thumbnail" } };
}

void PlaylistItemsModel::setPlaylistId(const QString &id) {
    if (id == id_) return;
    id_ = id;
    reload();
}

QString PlaylistItemsModel::title() const { return q(header_.title); }
int PlaylistItemsModel::kind() const { return static_cast<int>(header_.kind); }
QString PlaylistItemsModel::remoteId() const { return q(header_.remoteId); }
double PlaylistItemsModel::refreshed() const { return static_cast<double>(header_.refreshedMs); }

bool PlaylistItemsModel::remove(const QString &videoId) { return store_->removeFromPlaylist(id_, videoId); }
bool PlaylistItemsModel::move(int from, int to) { return store_->moveInPlaylist(id_, from, to); }

QVariantMap PlaylistItemsModel::get(int row) const {
    if (row < 0 || row >= count()) return QVariantMap();
    return videoToMap(rows_[static_cast<std::size_t>(row)]);
}

QVariantList PlaylistItemsModel::items(bool shuffled) const {
    std::vector<VideoEntry> list = rows_;
    if (shuffled) {
        std::random_device seed;
        std::mt19937 generator(seed());
        std::shuffle(list.begin(), list.end(), generator);
    }
    QVariantList out;
    for (const VideoEntry &v : list) out.append(videoToMap(v));
    return out;
}

bool PlaylistItemsModel::replaceRemoteItems(const QVariantList &videos) {
    std::vector<VideoEntry> items;
    items.reserve(static_cast<std::size_t>(videos.size()));
    for (const QVariant &v : videos) items.push_back(videoFromMap(v.toMap()));
    return store_->replaceRemoteItems(id_, items, libraryNowMs());
}

void PlaylistItemsModel::reload() {
    beginResetModel();
    header_ = PlaylistRecord();
    rows_.clear();
    if (!id_.isEmpty() && store_->playlist(id_, &header_)) rows_ = store_->playlistItems(id_);
    endResetModel();
    emit playlistChanged();
}

// ---- history ----------------------------------------------------------------

const int HistoryModel::pageSize; // odr-used by std::max

HistoryModel::HistoryModel(LibraryStore *store, QObject *parent) : QAbstractListModel(parent), store_(store) {
    connect(store_, &LibraryStore::historyChanged, this, &HistoryModel::reload);
    reload();
}

int HistoryModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : count(); }

QVariant HistoryModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= count()) return QVariant();
    const HistoryRecord &h = rows_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case VideoIdRole: return q(h.video.videoId);
    case TitleRole: return q(h.video.title);
    case ChannelRole: return q(h.video.channel);
    case ChannelIdRole: return q(h.video.channelId);
    case DurationSecondsRole: return static_cast<double>(h.video.durationSeconds);
    case ThumbnailRole: return q(h.video.thumbnailUrl);
    case PositionRole: return static_cast<double>(h.positionMs);
    case DurationMsRole: return static_cast<double>(h.durationMs);
    case FractionRole: return h.fraction;
    case WatchedRole: return h.watched;
    case WatchedAtRole: return static_cast<double>(h.watchedAtMs);
    case PlayCountRole: return h.playCount;
    default: return QVariant();
    }
}

QHash<int, QByteArray> HistoryModel::roleNames() const {
    return { { VideoIdRole, "videoId" }, { TitleRole, "title" }, { ChannelRole, "channel" },
             { ChannelIdRole, "channelId" }, { DurationSecondsRole, "durationSeconds" }, { ThumbnailRole, "thumbnail" },
             { PositionRole, "position" }, { DurationMsRole, "durationMs" }, { FractionRole, "fraction" },
             { WatchedRole, "watched" }, { WatchedAtRole, "watchedAt" }, { PlayCountRole, "playCount" } };
}

bool HistoryModel::canFetchMore(const QModelIndex &parent) const {
    return !parent.isValid() && count() < store_->historyCount();
}

void HistoryModel::fetchMore(const QModelIndex &parent) {
    if (parent.isValid()) return;
    const std::vector<HistoryRecord> more = store_->history(count(), pageSize);
    if (more.empty()) return;
    beginInsertRows(QModelIndex(), count(), count() + static_cast<int>(more.size()) - 1);
    rows_.insert(rows_.end(), more.begin(), more.end());
    endInsertRows();
    emit changed();
}

int HistoryModel::total() const { return store_->historyCount(); }
bool HistoryModel::paused() const { return store_->historyPaused(); }
void HistoryModel::setPaused(bool paused) {
    if (paused == store_->historyPaused()) return;
    store_->setHistoryPaused(paused);
    emit changed();
}
int HistoryModel::cap() const { return store_->historyCap(); }
void HistoryModel::setCap(int cap) {
    if (!validHistoryCap(cap) || cap == store_->historyCap()) return;
    if (store_->setHistoryCap(cap)) emit changed();
}

void HistoryModel::clear() { store_->clearHistory(); }
void HistoryModel::remove(const QString &videoId) { store_->removeFromHistory(videoId); }
void HistoryModel::markWatched(const QVariantMap &video) {
    store_->markWatched(video.value(QStringLiteral("videoId")).toString(), videoFromMap(video));
}
void HistoryModel::unmarkWatched(const QString &videoId) { store_->unmarkWatched(videoId); }

QVariantMap HistoryModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

void HistoryModel::reload() {
    // Keep as many rows as were loaded, at least one page.
    const int keep = std::max(pageSize, count());
    std::vector<HistoryRecord> next = store_->history(0, keep);
    if (!applyIncrementally(next)) {
        beginResetModel();
        rows_ = std::move(next);
        endResetModel();
    }
    emit changed();
}

namespace {
bool sameHistoryValues(const HistoryRecord &a, const HistoryRecord &b) {
    return a.positionMs == b.positionMs && a.durationMs == b.durationMs && a.fraction == b.fraction
        && a.watched == b.watched && a.watchedAtMs == b.watchedAtMs && a.playCount == b.playCount
        && a.video.title == b.video.title && a.video.channel == b.video.channel && a.video.channelId == b.video.channelId
        && a.video.thumbnailUrl == b.video.thumbnailUrl && a.video.durationSeconds == b.video.durationSeconds;
}
}

// The usual changes, in this order: the video just watched comes to the front (moved from its old
// row, or new), entries that left the window are removed (deleted, or pushed out at the end; at most
// two), rows that entered at the end are appended, and rows whose values changed are re-announced.
// The steps are first tried on the ids alone; if they do not lead to `next`, nothing is touched and
// the caller resets.
bool HistoryModel::applyIncrementally(const std::vector<HistoryRecord> &next) {
    if (rows_.empty() || next.empty()) return false;
    std::vector<std::string> ids, want;
    for (const HistoryRecord &h : rows_) ids.push_back(h.video.videoId);
    for (const HistoryRecord &h : next) want.push_back(h.video.videoId);

    int frontFrom = -1;         // old row of next[0]; -2: not there (insert); -1: already at the front
    if (ids[0] != want[0]) {
        const auto it = std::find(ids.begin(), ids.end(), want[0]);
        frontFrom = it == ids.end() ? -2 : static_cast<int>(it - ids.begin());
        if (frontFrom >= 0) ids.erase(it);
        ids.insert(ids.begin(), want[0]);
    }
    std::vector<int> removals;  // rows, each valid after the ones before it
    while (removals.size() < 2) {
        std::size_t i = 0;
        while (i < ids.size() && i < want.size() && ids[i] == want[i]) ++i;
        if (i >= ids.size()) break;                // ids is a prefix of want
        removals.push_back(static_cast<int>(i));
        ids.erase(ids.begin() + static_cast<std::ptrdiff_t>(i));
    }
    if (ids.size() > want.size() || !std::equal(ids.begin(), ids.end(), want.begin())) return false;

    if (frontFrom >= 0) {
        beginMoveRows(QModelIndex(), frontFrom, frontFrom, QModelIndex(), 0);
        const HistoryRecord moved = rows_[static_cast<std::size_t>(frontFrom)];
        rows_.erase(rows_.begin() + frontFrom);
        rows_.insert(rows_.begin(), moved);
        endMoveRows();
    } else if (frontFrom == -2) {
        beginInsertRows(QModelIndex(), 0, 0);
        rows_.insert(rows_.begin(), next[0]);
        endInsertRows();
    }
    for (int row : removals) {
        beginRemoveRows(QModelIndex(), row, row);
        rows_.erase(rows_.begin() + row);
        endRemoveRows();
    }
    if (rows_.size() < next.size()) {
        beginInsertRows(QModelIndex(), count(), static_cast<int>(next.size()) - 1);
        rows_.insert(rows_.end(), next.begin() + static_cast<std::ptrdiff_t>(rows_.size()), next.end());
        endInsertRows();
    }
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (sameHistoryValues(rows_[i], next[i])) continue;
        rows_[i] = next[i];
        const QModelIndex at = index(static_cast<int>(i), 0);
        emit dataChanged(at, at);
    }
    return true;
}

// ---- watch state ------------------------------------------------------------

WatchStateIndex::WatchStateIndex(LibraryStore *store, QObject *parent) : QObject(parent), store_(store) {
    connect(store_, &LibraryStore::watchStateChanged, this, &WatchStateIndex::changed);
}

QVariantMap WatchStateIndex::state(const QString &videoId) const {
    const OverlayFlags f = overlayFor(store_->watchState(videoId));
    QVariantMap m;
    m.insert(QStringLiteral("showProgress"), f.showProgress);
    m.insert(QStringLiteral("progress"), f.progress);
    m.insert(QStringLiteral("watched"), f.watchedBadge);
    return m;
}

bool WatchStateIndex::hidden(const QString &videoId, const QString &context) const {
    ListContext c = ListContext::Other;
    if (context == QLatin1String("feed")) c = ListContext::Feed;
    else if (context == QLatin1String("channel")) c = ListContext::Channel;
    else if (context == QLatin1String("search")) c = ListContext::Search;
    HideWatchedSettings s;
    s.enabled = hideWatched();
    s.includeSearch = hideWatchedInSearch();
    return hideFromList(store_->watchState(videoId), c, s);
}

double WatchStateIndex::resumePosition(const QString &videoId) const {
    return static_cast<double>(store_->resumePositionMs(videoId));
}

bool WatchStateIndex::hideWatched() const {
    return store_->metaValue(QString::fromLatin1(hideWatchedKey), QStringLiteral("0")) == QLatin1String("1");
}
void WatchStateIndex::setHideWatched(bool on) {
    if (on == hideWatched()) return;
    store_->setMetaValue(QString::fromLatin1(hideWatchedKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit settingsChanged();
}
bool WatchStateIndex::hideWatchedInSearch() const {
    return store_->metaValue(QString::fromLatin1(hideWatchedSearchKey), QStringLiteral("0")) == QLatin1String("1");
}
void WatchStateIndex::setHideWatchedInSearch(bool on) {
    if (on == hideWatchedInSearch()) return;
    store_->setMetaValue(QString::fromLatin1(hideWatchedSearchKey), on ? QStringLiteral("1") : QStringLiteral("0"));
    emit settingsChanged();
}

}}}
