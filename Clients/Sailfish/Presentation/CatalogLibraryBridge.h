// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The catalog side of the library component's hooks:
//
//  * Feed subscriptions (DSC-05, LIB-01): the feed follows LibraryStore::subscriptions() and
//    subscriptionsChanged(). The list is pushed only from an open store, so a library that
//    failed to open never prunes the feed cache; an unchanged list is not pushed again.
//  * Feed write-back: after a merge, each channel's newest upload time (and the name RSS gave)
//    goes back through updateChannelInfo(), only when it moved forward.
//  * Remote playlists (LIB-03): exposed to QML as `remotePlaylists`. refresh(mirrorId) walks the
//    YouTube list (page 1 plus continuations, at most kMaxRemoteItems rows, in order) and
//    replaces the mirror's items in one call. A failed walk keeps the previous items. One walk
//    runs at a time; further mirrors queue.
//  * Import handles (IMP-01): imported @handle / custom URLs are resolved to UC ids one at a
//    time (navigation/resolve_url) and subscribed through resolvePendingHandle(). Failures stay
//    pending; an automatic run tries each URL once per session.
//  * Hide watched (LIB-07): installs the WatchStateIndex predicate into WatchedRows and keeps
//    the feed's row filter in step with it.
//
// All network work goes through the services handed in, which the composition builds on the
// background transport and lane (RES-02). Main thread only. The library objects may be
// destroyed first (main.cpp locals); the bridge holds them through QPointer.

#include "Core/Catalog/Domain/Subscriptions.h"
#include "Core/Library/Domain/LibraryTypes.h"

#include <QObject>
#include <QTimer>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nordtile { namespace video {
namespace library { class LibraryStore; }
namespace catalog { class FeedService; class PlaylistService; class ChannelService; class ShortsFeedService; struct PlaylistPage; struct Item; }
namespace sailfish {

class WatchStateIndex;

class CatalogLibraryBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString refreshingId READ refreshingId NOTIFY stateChanged)
    Q_PROPERTY(bool resolvingHandles READ resolvingHandles NOTIFY stateChanged)
public:
    static const int kMaxRemoteItems = 5000;
    static const int kHandleSpacingMs = 300;   // between two resolve_url calls of one run

    // Any service may be null (that hook is then inert). `nowMs` returns epoch milliseconds.
    CatalogLibraryBridge(catalog::FeedService *feed, catalog::PlaylistService *playlists,
                         catalog::ChannelService *channels, QObject *parent = nullptr,
                         std::function<qint64()> nowMs = std::function<qint64()>());

    // The Shorts section follows the same subscription list.
    void setShortsFeed(catalog::ShortsFeedService *shorts) { shorts_ = shorts; }
    // Channel avatars (the YouTube-style cards' round avatar): a stored subscription avatar
    // teaches the AvatarBook when the book has none; an avatar the book learned from an answer
    // (channel header, a row's owner) is written back to the subscription record, so the
    // Subscriptions feed shows it from the next start on. Runs 2 s after the book changes.
    void syncAvatars();
    // Connects the library objects; `watch` may be null (no hide-watched).
    void attach(library::LibraryStore *store, WatchStateIndex *watch);

    // LIB-03: fetches the YouTube list behind a mirror and replaces its items.
    Q_INVOKABLE void refresh(const QString &mirrorId);
    Q_INVOKABLE bool isRefreshing(const QString &mirrorId) const;
    // IMP-01: resolves the pending handles; `retryFailed` also retries URLs that failed earlier
    // in this session.
    Q_INVOKABLE void resolvePendingHandles(bool retryFailed = false);

    bool busy() const { return !current_.isEmpty() || resolving_; }
    QString refreshingId() const { return current_; }
    bool resolvingHandles() const { return resolving_; }
    int channelPushes() const { return pushes_; }   // tests

    static library::VideoEntry toEntry(const catalog::Item &item);

signals:
    void stateChanged();
    void refreshFinished(const QString &mirrorId, bool ok, int count);
    void handlesResolved(int resolved, int failed);

private:
    void pushChannels();
    void writeBack();
    void applyWatchedFilter();
    void startNextRefresh();
    void onPlaylistPage(quint64 generation, bool first, const catalog::PlaylistPage &page);
    void finishRefresh(bool ok);
    void nextHandle();

    catalog::FeedService *feed_;
    catalog::PlaylistService *playlists_;
    catalog::ChannelService *channels_;
    catalog::ShortsFeedService *shorts_ = nullptr;
    QTimer *avatarTimer_ = nullptr;
    bool syncingAvatars_ = false;
    std::function<qint64()> nowMs_;
    QPointer<library::LibraryStore> store_;
    QPointer<WatchStateIndex> watch_;

    // feed
    catalog::ChannelList pushed_;
    bool pushedOnce_ = false;
    int pushes_ = 0;
    std::map<std::string, qint64> written_;   // channel id -> newest upload ms known to the library

    // remote playlists
    QStringList queue_;
    QString current_;
    QString currentRemote_;
    quint64 generation_ = 0;
    std::vector<library::VideoEntry> entries_;
    QSet<QString> entryIds_;

    // handles
    bool resolving_ = false;
    std::vector<library::PendingHandle> handles_;
    QSet<QString> attempted_;
    int resolved_ = 0;
    int failed_ = 0;
};

}}}
