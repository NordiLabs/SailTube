// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The local-first library on SQLite (QtSql, QSQLITE driver). One instance per
// thread: the main (playback) thread owns the primary store; imports and the
// download engine's persistence open their own instance on the same file.
//
// Robustness (NFR-ROB-02): every mutation is one SQLite transaction (journal
// mode WAL, synchronous NORMAL), so a crash leaves the previous or the next
// state, never a torn one. The file and its -wal/-shm are 0600 in the private
// data directory. open() removes orphaned "*.tmp" / "*.part-import" files left
// by interrupted imports and exports in the library directory.
//
// Isolation (RES-02): reads used by playback (resumePositionMs, watchState,
// isSubscribed) are answered from in-memory indexes loaded at open() and kept
// current incrementally; they never wait for SQLite or for background writers.
// A write that meets SQLITE_BUSY (an import holds the write lock) is kept in a
// small pending queue and retried by a timer, so the playback thread never
// blocks for more than the 50 ms busy timeout.
//
// Schema versioning: PRAGMA user_version. Version 1 is described in
// LibrarySchema.h; migrate() upgrades step by step inside one transaction and
// refuses a database newer than the code: open() fails with an error and
// leaves the file untouched.

#include "Core/Library/Domain/LibraryPolicy.h"
#include "Core/Library/Domain/LibraryTypes.h"
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>
#include <deque>
#include <vector>

class QSqlDatabase;

namespace nordtile { namespace video { namespace library {

class LibraryStore final : public QObject {
    Q_OBJECT
public:
    // `connectionName` must be unique per instance (QSqlDatabase connection name).
    LibraryStore(const QString &databasePath, const QString &connectionName, QObject *parent = nullptr);
    ~LibraryStore() override;

    bool open(QString *error = nullptr);
    bool isOpen() const;
    QString databasePath() const { return path_; }
    int schemaVersion() const;
    // Re-read the in-memory indexes (after another instance wrote, e.g. an import).
    void reload();

    // ---- LIB-01 subscriptions --------------------------------------------
    bool isSubscribed(const QString &channelId) const;                  // O(1), in memory
    bool subscribe(const ChannelRecord &channel);                       // false if invalid UC id
    bool unsubscribe(const QString &channelId);
    std::vector<ChannelRecord> subscriptions(SubscriptionSort sort) const;
    QStringList subscribedChannelIds() const;                           // for the feed (catalog component)
    // Feed write-back: newest upload time and refreshed name/avatar (empty = keep).
    void updateChannelInfo(const QString &channelId, qint64 latestUploadMs, const QString &name,
                           const QString &avatarUrl);
    std::vector<PendingHandle> pendingHandles() const;
    // A handle resolved online: subscribes the UC id and drops the pending row.
    bool resolvePendingHandle(const QString &url, const ChannelRecord &channel);
    void dropPendingHandle(const QString &url);

    // ---- LIB-02/03 playlists ---------------------------------------------
    // Watch later always exists (created by the migration) and is listed first.
    std::vector<PlaylistRecord> playlists() const;                      // local + watch later + mirrors
    bool playlist(const QString &id, PlaylistRecord *out) const;
    QString createPlaylist(const QString &title);                       // returns the new id, new lists on top
    bool renamePlaylist(const QString &id, const QString &title);       // local lists only
    bool deletePlaylist(const QString &id);                             // not watch later
    // Reorder lists in the library (local lists; watch later stays first).
    bool movePlaylist(const QString &id, int toIndex);
    std::vector<VideoEntry> playlistItems(const QString &id) const;
    bool playlistContains(const QString &id, const QString &videoId) const;
    // false (no-op) when the video is already in the list.
    bool addToPlaylist(const QString &id, const VideoEntry &video);
    bool removeFromPlaylist(const QString &id, const QString &videoId);
    bool moveInPlaylist(const QString &id, int from, int to);
    // LIB-03 mirrors: save keeps one mirror per remote id (re-save updates the
    // header); replace items after a successful fetch (up to 5000).
    QString saveRemotePlaylist(const RemotePlaylistRef &ref);           // returns the mirror id
    bool replaceRemoteItems(const QString &id, const std::vector<VideoEntry> &items, qint64 refreshedMs);
    QString mirrorIdForRemote(const QString &remoteId) const;           // empty when not saved

    // ---- LIB-04 history ----------------------------------------------------
    // One call per player report. Updates history (unless paused) and the
    // resume point (always, by LIB-05 rules). newSession = first report after open().
    void recordProgress(const PlaybackProgress &progress, bool newSession);
    // Newest first, paged.
    std::vector<HistoryRecord> history(int offset, int count) const;
    int historyCount() const;
    bool historyRecord(const QString &videoId, HistoryRecord *out) const;
    void markWatched(const QString &videoId, const VideoEntry &video); // creates a record if needed
    void unmarkWatched(const QString &videoId);                        // no stub when absent
    void removeFromHistory(const QString &videoId);
    void clearHistory();                                                // resume points stay
    int historyCap() const;
    bool setHistoryCap(int cap);                                        // false (unchanged) outside 100..5000; trims at once
    bool historyPaused() const;
    void setHistoryPaused(bool paused);

    // ---- LIB-05 resume -----------------------------------------------------
    qint64 resumePositionMs(const QString &videoId) const;             // -1 none; in memory, O(1)
    void clearResume(const QString &videoId);

    // ---- LIB-06 watched state ---------------------------------------------
    WatchState watchState(const QString &videoId) const;               // in memory, O(1)

    // ---- IMP-* --------------------------------------------------------------
    // Applies one importer batch in chunked transactions (200 rows each) so a
    // concurrent playback-thread write waits at most one chunk. Local wins.
    ImportSummary applyImport(const ImportBatch &batch);
    // IMP-04: a full snapshot for the SailTube backup exporter.
    ImportBatch snapshot() const;

    // ---- library settings kept with the data (exported in backups) ---------
    QString metaValue(const QString &key, const QString &fallback = QString()) const;
    void setMetaValue(const QString &key, const QString &value);

signals:
    void subscriptionsChanged();
    void playlistsChanged();                  // list of lists, titles, counts
    void playlistChanged(const QString &id);  // items of one list
    void historyChanged();
    // LIB-06 incremental: exactly the video whose fraction/watched changed.
    void watchStateChanged(const QString &videoId);
    void resumeChanged(const QString &videoId);

private:
    struct Statement {
        QString sql;
        QVariantList bind;
    };
    typedef QList<Statement> Statements;
    struct Pending {
        quint64 id = 0;
        Statements statements;
    };
    enum class TxResult { Done, Busy, Failed };
    struct HistoryMeta {
        double fraction = 0;
        bool watched = false;
        qint64 seq = 0;
    };
    struct ResumeMeta {
        qint64 positionMs = 0;
        qint64 touchedMs = 0;
        quint64 order = 0;            // tie-break for equal touch times (insertion order)
    };

    QSqlDatabase database() const;
    bool exec(const QString &sql, const QVariantList &bind = QVariantList(), QString *error = nullptr);
    TxResult runTransaction(const Statements &statements, QString *error = nullptr);
    // Queues `statements` behind any pending write and flushes; Busy means it stays queued.
    TxResult write(const Statements &statements);
    // Blocking variant for background instances (imports): retries a busy lock for a while.
    bool writeBlocking(const Statements &statements);
    void flushPending();
    bool waitForPending(int timeoutMs);
    void subscriptionStatements(const ChannelRecord &channel, Statements *tx) const;
    bool migrate(QString *error);
    void loadIndexes();
    void reloadAndNotify();
    bool readHistory(const QString &videoId, HistoryRecord *out) const;
    void putHistory(const HistoryRecord &record, qint64 seq, Statements *tx);
    void trimHistory(int cap, Statements *tx, QStringList *evicted);
    void sweepResume(qint64 nowMs, Statements *tx, QStringList *removed);
    void capResume(Statements *tx, QStringList *removed);
    void emitWatchChanges(const QHash<QString, HistoryMeta> &before);
    bool playlistKind(const QString &id, int *kind) const;
    QString newLocalPlaylistId() const;
    void removeOrphans() const;
    void tightenPermissions() const;

    QString path_;
    QString connection_;
    bool open_ = false;
    bool reloadRequested_ = false;
    int version_ = 0;
    QSet<QString> subscribed_;
    QHash<QString, HistoryMeta> history_;     // every history row (LIB-06 lookups)
    QMap<qint64, QString> bySeq_;            // recency order of history_
    qint64 nextSeq_ = 1;
    QHash<QString, ResumeMeta> resume_;
    quint64 resumeOrder_ = 0;
    QHash<QString, QString> meta_;
    // History records written while their transaction waits in pending_ (SQL is behind).
    QHash<QString, HistoryRecord> ahead_;
    std::deque<Pending> pending_;
    quint64 nextPendingId_ = 0;
    quint64 lastFailedId_ = 0;
    QTimer retry_;
};

// Milliseconds since the epoch; tests may override it.
qint64 libraryNowMs();
void setLibraryClockForTests(qint64 (*clock)());

}}}
