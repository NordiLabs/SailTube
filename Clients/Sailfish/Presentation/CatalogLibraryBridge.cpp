// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CatalogLibraryBridge.h"

#include "LibraryModels.h"
#include "WatchedRows.h"
#include "ChannelAvatars.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/FeedService.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/ShortsFeedService.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QDateTime>
#include <QTimer>
#include <algorithm>

namespace nordtile { namespace video { namespace sailfish {

using catalog::Item;
using catalog::Outcome;

namespace {
bool sameChannels(const catalog::ChannelList &a, const catalog::ChannelList &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name) return false;
    return true;
}
}

CatalogLibraryBridge::CatalogLibraryBridge(catalog::FeedService *feed, catalog::PlaylistService *playlists,
                                           catalog::ChannelService *channels, QObject *parent,
                                           std::function<qint64()> nowMs)
    : QObject(parent), feed_(feed), playlists_(playlists), channels_(channels), nowMs_(std::move(nowMs)) {
    if (!nowMs_) nowMs_ = [] { return QDateTime::currentMSecsSinceEpoch(); };
    if (feed_) connect(feed_, &catalog::FeedService::feedChanged, this, [this] { writeBack(); });
    connect(WatchedRows::instance(), &WatchedRows::changed, this, [this] { applyWatchedFilter(); });
    avatarTimer_ = new QTimer(this);
    avatarTimer_->setSingleShot(true);
    avatarTimer_->setInterval(2000);
    connect(avatarTimer_, &QTimer::timeout, this, [this] { syncAvatars(); });
    connect(ChannelAvatars::instance(), &ChannelAvatars::changed, this, [this] { if (!avatarTimer_->isActive()) avatarTimer_->start(); });
}

void CatalogLibraryBridge::syncAvatars() {
    if (syncingAvatars_ || !store_ || !store_->isOpen()) return;
    syncingAvatars_ = true;   // updateChannelInfo() announces a change, which comes back here
    catalog::AvatarBook &book = catalog::AvatarBook::instance();
    for (const library::ChannelRecord &c : store_->subscriptions(library::SubscriptionSort::Name)) {
        const std::string known = book.lookup(c.channelId);
        if (known.empty()) {
            if (!c.avatarUrl.empty()) book.learn(c.channelId, c.avatarUrl);
        } else if (known != c.avatarUrl) {
            // The book's entry comes from an answer and is the fresher one.
            store_->updateChannelInfo(QString::fromStdString(c.channelId), 0, QString(), QString::fromStdString(known));
        }
    }
    syncingAvatars_ = false;
}

library::VideoEntry CatalogLibraryBridge::toEntry(const Item &item) {
    library::VideoEntry e;
    e.videoId = item.id;
    e.title = item.title;
    e.channel = item.channel;
    e.channelId = item.channelId;
    e.durationSeconds = item.durationSeconds > 0 ? item.durationSeconds : -1;
    e.thumbnailUrl = !item.thumbnailUrl.empty() ? item.thumbnailUrl : catalog::listThumbnailUrl(item.id);
    return e;
}

void CatalogLibraryBridge::attach(library::LibraryStore *store, WatchStateIndex *watch) {
    store_ = store;
    watch_ = watch;
    if (store) {
        connect(store, &library::LibraryStore::subscriptionsChanged, this, [this] {
            pushChannels();
            syncAvatars();
            // An import may have left new handles; each URL is tried once automatically.
            if (store_ && !store_->pendingHandles().empty()) resolvePendingHandles(false);
        });
        pushChannels();
        syncAvatars();   // stored avatars are known before the feed's cached rows are painted
        // After start-up traffic: handles left by an earlier import.
        QTimer::singleShot(3000, this, [this] {
            if (store_ && !store_->pendingHandles().empty()) resolvePendingHandles(false);
        });
    }
    if (watch) {
        QPointer<WatchStateIndex> guard(watch);
        WatchedRows::instance()->setPredicate([guard](const QString &videoId, const QString &context) {
            return guard && guard->hidden(videoId, context);
        });
        connect(watch, &WatchStateIndex::changed, WatchedRows::instance(), [] { WatchedRows::instance()->notifyChanged(); });
        connect(watch, &WatchStateIndex::settingsChanged, WatchedRows::instance(), [] { WatchedRows::instance()->notifyChanged(); });
    }
    applyWatchedFilter();
}

// ---- feed --------------------------------------------------------------------------------------

void CatalogLibraryBridge::pushChannels() {
    if (!feed_ || !store_ || !store_->isOpen()) return;   // never prune the feed from a closed store
    catalog::ChannelList list;
    for (const library::ChannelRecord &c : store_->subscriptions(library::SubscriptionSort::Name)) {
        list.push_back(catalog::ChannelRef(c.channelId, c.name));
        qint64 &known = written_[c.channelId];
        known = std::max<qint64>(known, c.latestUploadMs);
    }
    if (pushedOnce_ && sameChannels(list, pushed_)) return;
    pushed_ = list;
    pushedOnce_ = true;
    ++pushes_;
    feed_->setChannels(list);
    if (shorts_) shorts_->setChannels(list);
}

void CatalogLibraryBridge::writeBack() {
    if (!feed_ || !store_ || !store_->isOpen()) return;
    for (const catalog::ChannelFeedState &s : feed_->schedule().states()) {
        if (!store_->isSubscribed(QString::fromStdString(s.channel.id))) continue;
        qint64 newest = -1;
        std::string name;
        for (const Item &row : s.rows) {
            if (row.publishedEpoch > newest) newest = row.publishedEpoch;
            if (name.empty() && row.channelId == s.channel.id) name = row.channel;
        }
        if (newest <= 0) continue;
        const qint64 ms = newest * 1000;
        qint64 &known = written_[s.channel.id];
        if (ms <= known) continue;
        known = ms;
        // The name only when RSS gave one that differs; the avatar is not in the feed.
        const QString fresh = name.empty() || name == s.channel.name ? QString() : QString::fromStdString(name);
        store_->updateChannelInfo(QString::fromStdString(s.channel.id), ms, fresh, QString());
    }
}

void CatalogLibraryBridge::applyWatchedFilter() {
    if (!feed_) return;
    catalog::RowFilterSettings settings = feed_->rowFilter();
    const bool had = static_cast<bool>(settings.hideWatched);
    settings.hideWatched = WatchedRows::instance()->forContext(QStringLiteral("feed"));
    if (!had && !settings.hideWatched) return;
    feed_->setRowFilter(settings);   // re-merges the cached feed offline
}

// ---- remote playlists (LIB-03) -----------------------------------------------------------------

bool CatalogLibraryBridge::isRefreshing(const QString &mirrorId) const {
    return current_ == mirrorId || queue_.contains(mirrorId);
}

void CatalogLibraryBridge::refresh(const QString &mirrorId) {
    if (mirrorId.isEmpty() || isRefreshing(mirrorId)) return;
    queue_.append(mirrorId);
    if (current_.isEmpty()) startNextRefresh();
}

void CatalogLibraryBridge::startNextRefresh() {
    while (current_.isEmpty() && !queue_.isEmpty()) {
        const QString id = queue_.takeFirst();
        library::PlaylistRecord record;
        if (!playlists_ || !store_ || !store_->playlist(id, &record) || record.kind != library::PlaylistKind::RemoteMirror ||
            record.remoteId.empty()) {
            emit refreshFinished(id, false, 0);
            continue;
        }
        current_ = id;
        currentRemote_ = QString::fromStdString(record.remoteId);
        entries_.clear();
        entryIds_.clear();
        const quint64 generation = ++generation_;
        emit stateChanged();
        playlists_->open(currentRemote_, this, [this, generation](catalog::PlaylistPage page) {
            onPlaylistPage(generation, true, page);
        });
    }
}

void CatalogLibraryBridge::onPlaylistPage(quint64 generation, bool first, const catalog::PlaylistPage &page) {
    if (generation != generation_ || current_.isEmpty()) return;
    if (page.outcome != Outcome::Ok && page.outcome != Outcome::ValidEmpty) { finishRefresh(false); return; }
    if (first && store_ && !page.title.isEmpty()) {
        // Re-saving refreshes the mirror's header (title, owner, thumbnail, count hint).
        library::RemotePlaylistRef ref;
        ref.remoteId = currentRemote_.toStdString();
        ref.title = page.title.toStdString();
        ref.channel = page.owner.toStdString();
        ref.thumbnailUrl = page.thumbnailUrl.toStdString();
        ref.itemCountHint = page.videoCount >= 0 ? static_cast<int>(std::min<qint64>(page.videoCount, kMaxRemoteItems)) : -1;
        store_->saveRemotePlaylist(ref);
    }
    for (const Item &item : page.items) {
        if (static_cast<int>(entries_.size()) >= kMaxRemoteItems) break;
        if (!catalog::isVideoLike(item) || item.id.empty()) continue;
        const QString id = QString::fromStdString(item.id);
        if (entryIds_.contains(id)) continue;   // a list may repeat a video; the mirror keeps one
        entryIds_.insert(id);
        entries_.push_back(toEntry(item));
    }
    if (!page.continuation.isEmpty() && static_cast<int>(entries_.size()) < kMaxRemoteItems) {
        const int firstIndex = static_cast<int>(entries_.size());
        playlists_->more(currentRemote_, page.continuation, firstIndex, this, [this, generation](catalog::PlaylistPage next) {
            onPlaylistPage(generation, false, next);
        });
        return;
    }
    finishRefresh(true);
}

void CatalogLibraryBridge::finishRefresh(bool ok) {
    const QString id = current_;
    int count = 0;
    if (ok && store_) {
        ok = store_->replaceRemoteItems(id, entries_, nowMs_());
        count = static_cast<int>(entries_.size());
    }
    current_.clear();
    currentRemote_.clear();
    entries_.clear();
    entryIds_.clear();
    emit stateChanged();
    emit refreshFinished(id, ok, ok ? count : 0);
    startNextRefresh();
}

// ---- import handles (IMP-01) -------------------------------------------------------------------

void CatalogLibraryBridge::resolvePendingHandles(bool retryFailed) {
    if (resolving_ || !channels_ || !store_ || !store_->isOpen()) return;
    handles_.clear();
    for (const library::PendingHandle &h : store_->pendingHandles()) {
        const QString url = QString::fromStdString(h.url);
        if (!retryFailed && attempted_.contains(url)) continue;
        handles_.push_back(h);
    }
    if (handles_.empty()) return;
    std::reverse(handles_.begin(), handles_.end());   // pop_back keeps the import order
    resolving_ = true;
    resolved_ = failed_ = 0;
    emit stateChanged();
    nextHandle();
}

void CatalogLibraryBridge::nextHandle() {
    if (handles_.empty() || !store_) {
        resolving_ = false;
        handles_.clear();
        emit stateChanged();
        emit handlesResolved(resolved_, failed_);
        return;
    }
    const library::PendingHandle handle = handles_.back();
    handles_.pop_back();
    const QString url = QString::fromStdString(handle.url);
    attempted_.insert(url);
    channels_->resolve(url, this, [this, handle](Outcome outcome, QString channelId) {
        if (!store_) { nextHandle(); return; }
        if (outcome == Outcome::Ok && channelId.startsWith(QLatin1String("UC")) && channelId.size() == 24) {
            library::ChannelRecord channel;
            channel.channelId = channelId.toStdString();
            channel.name = handle.name;
            if (store_->resolvePendingHandle(QString::fromStdString(handle.url), channel)) ++resolved_;
            else ++failed_;
        } else {
            ++failed_;
            if (outcome == Outcome::RateLimited) handles_.clear();   // stop this run; retried later
        }
        if (handles_.empty()) { nextHandle(); return; }
        QTimer::singleShot(kHandleSpacingMs, this, [this] { nextHandle(); });
    });
}

}}}
