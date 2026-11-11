// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML-facing views of the library store (LIB-01..07). The models hold no
// policy: they translate the store's records into roles and forward edits.
// Video maps use the same keys as the search model and PlaybackSession hints:
// videoId, title, channel, channelId, durationSeconds, thumbnail.

#include "Core/Library/Domain/LibraryTypes.h"
#include <QAbstractListModel>
#include <QVariantMap>
#include <vector>

namespace nordtile { namespace video {
namespace library { class LibraryStore; }
namespace sailfish {

library::VideoEntry videoFromMap(const QVariantMap &map);
QVariantMap videoToMap(const library::VideoEntry &video);

// LIB-01. Also the list the catalog component's feed reads (channelIds()).
class SubscriptionsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int sortMode READ sortMode WRITE setSortMode NOTIFY sortModeChanged) // 0 A–Z, 1 latest upload
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    // Bumped on every change, so QML bindings of isSubscribed() re-evaluate.
    Q_PROPERTY(int revision READ revision NOTIFY countChanged)
    Q_PROPERTY(int pendingHandles READ pendingHandles NOTIFY countChanged)
public:
    enum Role { ChannelIdRole = Qt::UserRole + 1, NameRole, AvatarRole, LatestUploadRole };
    explicit SubscriptionsModel(library::LibraryStore *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int sortMode() const { return sort_; }
    void setSortMode(int mode);
    int count() const { return static_cast<int>(rows_.size()); }
    int revision() const { return revision_; }
    int pendingHandles() const;

    Q_INVOKABLE bool isSubscribed(const QString &channelId) const;
    Q_INVOKABLE bool subscribe(const QString &channelId, const QString &name, const QString &avatar);
    Q_INVOKABLE bool unsubscribe(const QString &channelId);
    // Returns the new state.
    Q_INVOKABLE bool toggle(const QString &channelId, const QString &name, const QString &avatar);
    Q_INVOKABLE QStringList channelIds() const;

signals:
    void sortModeChanged();
    void countChanged();

private:
    void reload();
    library::LibraryStore *store_;
    std::vector<library::ChannelRecord> rows_;
    int sort_ = 0;
    int revision_ = 0;
};

// LIB-02/03: the library's list of lists (watch later first).
class PlaylistsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int revision READ revision NOTIFY countChanged)
public:
    enum Role { IdRole = Qt::UserRole + 1, KindRole, TitleRole, ItemCountRole, ThumbnailRole, RemoteIdRole,
                ChannelRole, WatchLaterRole, MirrorRole, RefreshedRole };
    explicit PlaylistsModel(library::LibraryStore *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    int count() const { return static_cast<int>(rows_.size()); }
    int revision() const { return revision_; }

    Q_INVOKABLE QString create(const QString &title);
    // "New list and add": creates the list and adds the video in one step.
    Q_INVOKABLE QString createWith(const QString &title, const QVariantMap &video);
    Q_INVOKABLE bool rename(const QString &id, const QString &title);
    Q_INVOKABLE bool remove(const QString &id);
    Q_INVOKABLE bool move(const QString &id, int toIndex);
    Q_INVOKABLE bool contains(const QString &id, const QString &videoId) const;
    // false when the video is already in the list (LIB-02).
    Q_INVOKABLE bool add(const QString &id, const QVariantMap &video);
    Q_INVOKABLE bool removeVideo(const QString &id, const QString &videoId);
    Q_INVOKABLE bool inWatchLater(const QString &videoId) const;
    Q_INVOKABLE bool toggleWatchLater(const QVariantMap &video); // returns the new state
    // LIB-03 preview with Save: nothing is stored until save is called.
    Q_INVOKABLE QString saveRemote(const QString &remoteId, const QString &title, const QString &channel,
                                   const QString &thumbnail, int itemCount);
    Q_INVOKABLE QString mirrorFor(const QString &remoteId) const;
    Q_INVOKABLE QVariantMap get(int row) const;
    // A PlaylistItemsModel for one list, owned by the caller (QML: the page).
    Q_INVOKABLE QObject *openList(const QString &id);

signals:
    void countChanged();

private:
    void reload();
    library::LibraryStore *store_;
    std::vector<library::PlaylistRecord> rows_;
    int revision_ = 0;
};

// Items of one list. Play all / shuffle hand videoIds() or items() to the queue.
class PlaylistItemsModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString playlistId READ playlistId WRITE setPlaylistId NOTIFY playlistChanged)
    Q_PROPERTY(QString title READ title NOTIFY playlistChanged)
    Q_PROPERTY(int kind READ kind NOTIFY playlistChanged)
    Q_PROPERTY(QString remoteId READ remoteId NOTIFY playlistChanged)
    Q_PROPERTY(double refreshed READ refreshed NOTIFY playlistChanged)
    Q_PROPERTY(int count READ count NOTIFY playlistChanged)
public:
    enum Role { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationSecondsRole, ThumbnailRole };
    explicit PlaylistItemsModel(library::LibraryStore *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString playlistId() const { return id_; }
    void setPlaylistId(const QString &id);
    QString title() const;
    int kind() const;
    QString remoteId() const;
    double refreshed() const;
    int count() const { return static_cast<int>(rows_.size()); }

    Q_INVOKABLE bool remove(const QString &videoId);
    Q_INVOKABLE bool move(int from, int to);
    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE QVariantList items(bool shuffled) const;
    // LIB-03: replace a mirror's items after a fetch (maps as above).
    Q_INVOKABLE bool replaceRemoteItems(const QVariantList &videos);

signals:
    void playlistChanged();

private:
    void reload();
    library::LibraryStore *store_;
    QString id_;
    library::PlaylistRecord header_;
    std::vector<library::VideoEntry> rows_;
};

// LIB-04, newest first, 40 per page. The model stays attached to views that are always alive (the
// Library tab's History shelf), and the store reports a change on every progress save (every 30 s
// while a video plays), so a change is applied as the smallest model update: dataChanged for rows
// that only changed their values, a move or an insert at the front for the video just watched, a
// remove for a deleted entry. Views keep their scroll position; only an unrelated change resets.
class HistoryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int total READ total NOTIFY changed)
    Q_PROPERTY(bool paused READ paused WRITE setPaused NOTIFY changed)
    Q_PROPERTY(int cap READ cap WRITE setCap NOTIFY changed)
public:
    enum Role { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationSecondsRole,
                ThumbnailRole, PositionRole, DurationMsRole, FractionRole, WatchedRole, WatchedAtRole, PlayCountRole };
    static const int pageSize = 40;
    explicit HistoryModel(library::LibraryStore *store, QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;
    bool canFetchMore(const QModelIndex &parent) const override;
    void fetchMore(const QModelIndex &parent) override;

    int count() const { return static_cast<int>(rows_.size()); }
    int total() const;
    bool paused() const;
    void setPaused(bool paused);
    int cap() const;
    void setCap(int cap);

    Q_INVOKABLE void clear();          // the page runs the remorse timer first
    Q_INVOKABLE void remove(const QString &videoId);
    Q_INVOKABLE void markWatched(const QVariantMap &video);
    Q_INVOKABLE void unmarkWatched(const QString &videoId);
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void changed();

private:
    void reload();
    bool applyIncrementally(const std::vector<library::HistoryRecord> &next);
    library::LibraryStore *store_;
    std::vector<library::HistoryRecord> rows_;
    bool dirty_ = false;
};

// LIB-06/07: per-thumbnail lookups. Delegates ask for their own id and listen
// to changed(videoId); nothing re-sends the whole history.
class WatchStateIndex final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hideWatched READ hideWatched WRITE setHideWatched NOTIFY settingsChanged)
    Q_PROPERTY(bool hideWatchedInSearch READ hideWatchedInSearch WRITE setHideWatchedInSearch NOTIFY settingsChanged)
public:
    explicit WatchStateIndex(library::LibraryStore *store, QObject *parent = nullptr);

    // {showProgress: bool, progress: 0..1, watched: bool}
    Q_INVOKABLE QVariantMap state(const QString &videoId) const;
    // context: "feed", "channel", "search" or anything else (never hidden).
    Q_INVOKABLE bool hidden(const QString &videoId, const QString &context) const;
    Q_INVOKABLE double resumePosition(const QString &videoId) const; // ms, -1 none

    bool hideWatched() const;
    void setHideWatched(bool);
    bool hideWatchedInSearch() const;
    void setHideWatchedInSearch(bool);

signals:
    void changed(const QString &videoId);
    void settingsChanged();

private:
    library::LibraryStore *store_;
};

}}}
