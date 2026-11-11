// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LibraryStore.h"

#include "Core/Catalog/Domain/Video.h"
#include "LibrarySchema.h"

#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QThread>
#include <QVariant>

#include <algorithm>
#include <fcntl.h>
#include <random>
#include <unistd.h>

namespace nordtile { namespace video { namespace library {

namespace {

qint64 systemClock() { return QDateTime::currentMSecsSinceEpoch(); }
qint64 (*g_clock)() = systemClock;

const int busyTimeoutMs = 50;        // RES-02: the longest a playback-thread write may wait
const int openBusyTimeoutMs = 2000;  // open/migrate only (start-up, not on the playback path)
const int retryIntervalMs = 200;
const int importChunkRows = 200;
const int importBusyDeadlineMs = 30000;
const char *const keyHistoryCap = "history_cap";
const char *const keyHistoryPaused = "history_paused";
const char *const importMarkerPrefix = "import/"; // per-source play counts already merged

const char *const historyColumns =
    "video_id, title, channel, channel_id, duration_s, thumbnail_url, position_ms, duration_ms,"
    " fraction, watched, watched_at_ms, play_count, seq";

QString qs(const std::string &s) { return QString::fromUtf8(s.data(), int(s.size())); }
std::string ss(const QString &s) {
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), std::size_t(b.size()));
}
QVariant v64(std::int64_t x) { return QVariant(qlonglong(x)); }

bool isBusyError(const QSqlError &e) {
    const QString code = e.nativeErrorCode();
    if (code == QLatin1String("5") || code == QLatin1String("6")) return true;
    const QString text = e.databaseText().toLower();
    return text.contains(QLatin1String("database is locked")) || text.contains(QLatin1String("is busy"))
           || text.contains(QLatin1String("table is locked"));
}

bool runSelect(QSqlQuery &q, const QString &sql, const QVariantList &bind = QVariantList()) {
    if (!q.prepare(sql)) {
        qWarning("library: prepare failed: %s", qPrintable(q.lastError().text()));
        return false;
    }
    for (int i = 0; i < bind.size(); ++i) q.bindValue(i, bind.at(i));
    if (!q.exec()) {
        qWarning("library: query failed: %s", qPrintable(q.lastError().text()));
        return false;
    }
    return true;
}

HistoryRecord historyFromRow(const QSqlQuery &q) {
    HistoryRecord r;
    r.video.videoId = ss(q.value(0).toString());
    r.video.title = ss(q.value(1).toString());
    r.video.channel = ss(q.value(2).toString());
    r.video.channelId = ss(q.value(3).toString());
    r.video.durationSeconds = q.value(4).toLongLong();
    r.video.thumbnailUrl = ss(q.value(5).toString());
    r.positionMs = q.value(6).toLongLong();
    r.durationMs = q.value(7).toLongLong();
    r.fraction = q.value(8).toDouble();
    r.watched = q.value(9).toInt() != 0;
    r.watchedAtMs = q.value(10).toLongLong();
    r.playCount = q.value(11).toInt();
    return r;
}

VideoEntry itemFromRow(const QSqlQuery &q, int base) {
    VideoEntry v;
    v.videoId = ss(q.value(base).toString());
    v.title = ss(q.value(base + 1).toString());
    v.channel = ss(q.value(base + 2).toString());
    v.channelId = ss(q.value(base + 3).toString());
    v.durationSeconds = q.value(base + 4).toLongLong();
    v.thumbnailUrl = ss(q.value(base + 5).toString());
    return v;
}

QVariantList itemBind(const QString &playlistId, int position, const VideoEntry &v, qint64 nowMs) {
    return QVariantList() << playlistId << position << qs(v.videoId) << qs(v.title) << qs(v.channel)
                          << qs(v.channelId) << v64(v.durationSeconds) << qs(v.thumbnailUrl) << v64(nowMs);
}

const char *const insertItemSql =
    "INSERT OR IGNORE INTO playlist_items(playlist_id, position, video_id, title, channel, channel_id,"
    " duration_s, thumbnail_url, added_ms) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)";

const char *const playlistSelect =
    "SELECT p.id, p.kind, p.title, p.remote_id, p.channel, p.thumbnail_url, p.created_ms, p.updated_ms,"
    " p.refreshed_ms, p.item_count_hint,"
    " (SELECT COUNT(*) FROM playlist_items i WHERE i.playlist_id = p.id),"
    " (SELECT i.thumbnail_url FROM playlist_items i WHERE i.playlist_id = p.id ORDER BY i.position LIMIT 1)"
    " FROM playlists p";
const char *const playlistOrder = " ORDER BY CASE WHEN p.kind = 1 THEN 0 ELSE 1 END, p.sort_key, p.created_ms DESC, p.id";

PlaylistRecord playlistFromRow(const QSqlQuery &q) {
    PlaylistRecord p;
    p.id = ss(q.value(0).toString());
    p.kind = static_cast<PlaylistKind>(q.value(1).toInt());
    p.title = ss(q.value(2).toString());
    p.remoteId = ss(q.value(3).toString());
    p.channel = ss(q.value(4).toString());
    p.thumbnailUrl = ss(q.value(5).toString());
    p.createdMs = q.value(6).toLongLong();
    p.updatedMs = q.value(7).toLongLong();
    p.refreshedMs = q.value(8).toLongLong();
    const int hint = q.value(9).toInt();
    p.itemCount = q.value(10).toInt();
    // A mirror saved from an import has no items until its first fetch: show the hint.
    if (p.kind == PlaylistKind::RemoteMirror && p.itemCount == 0 && p.refreshedMs == 0 && hint > 0) p.itemCount = hint;
    if (p.thumbnailUrl.empty()) p.thumbnailUrl = ss(q.value(11).toString());
    return p;
}

// Orphan cleanup runs once per directory and process: a second instance opened
// later (download engine, importer) must not delete a live import's temp file.
QMutex g_cleanMutex;
QSet<QString> g_cleanedDirs;

} // namespace

qint64 libraryNowMs() { return g_clock(); }
void setLibraryClockForTests(qint64 (*clock)()) { g_clock = clock ? clock : systemClock; }

LibraryStore::LibraryStore(const QString &databasePath, const QString &connectionName, QObject *parent)
    : QObject(parent), path_(databasePath), connection_(connectionName) {
    retry_.setSingleShot(true);
    retry_.setInterval(retryIntervalMs);
    connect(&retry_, &QTimer::timeout, this, &LibraryStore::flushPending);
}

LibraryStore::~LibraryStore() {
    blockSignals(true); // listeners may already be gone; a last flush must not call them
    if (open_) {
        flushPending();
        if (!pending_.empty())
            qWarning("library: %d write(s) still locked out at close", int(pending_.size()));
    }
    {
        QSqlDatabase db = QSqlDatabase::database(connection_, false);
        if (db.isValid()) db.close();
    }
    if (QSqlDatabase::contains(connection_)) QSqlDatabase::removeDatabase(connection_);
}

QSqlDatabase LibraryStore::database() const { return QSqlDatabase::database(connection_, false); }

bool LibraryStore::isOpen() const { return open_; }
int LibraryStore::schemaVersion() const { return version_; }

bool LibraryStore::exec(const QString &sql, const QVariantList &bind, QString *error) {
    QSqlQuery q(database());
    bool ok = q.prepare(sql);
    if (ok) {
        for (int i = 0; i < bind.size(); ++i) q.bindValue(i, bind.at(i));
        ok = q.exec();
    }
    if (!ok && error) *error = q.lastError().text();
    return ok;
}

void LibraryStore::removeOrphans() const {
    const QDir dir = QFileInfo(path_).absoluteDir();
    {
        QMutexLocker lock(&g_cleanMutex);
        if (g_cleanedDirs.contains(dir.absolutePath())) return;
        g_cleanedDirs.insert(dir.absolutePath());
    }
    const QStringList orphans = dir.entryList(QStringList() << QStringLiteral("*.tmp") << QStringLiteral("*.part-import"),
                                              QDir::Files | QDir::Hidden | QDir::System);
    for (const QString &name : orphans) {
        if (!QFile::remove(dir.filePath(name))) qWarning("library: cannot remove orphan %s", qPrintable(name));
    }
}

void LibraryStore::tightenPermissions() const {
    const QFile::Permissions owner = QFile::ReadOwner | QFile::WriteOwner;
    const QStringList files = QStringList() << path_ << path_ + QStringLiteral("-wal") << path_ + QStringLiteral("-shm");
    for (const QString &f : files) {
        if (QFile::exists(f)) QFile::setPermissions(f, owner);
    }
}

bool LibraryStore::open(QString *error) {
    if (open_) return true;
    QString message;
    const QFileInfo info(path_);
    const QString dirPath = info.absolutePath();
    if (!QDir(dirPath).exists()) {
        if (!QDir().mkpath(dirPath)) {
            if (error) *error = QStringLiteral("cannot create %1").arg(dirPath);
            return false;
        }
        QFile::setPermissions(dirPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    removeOrphans();
    if (!QFile::exists(path_)) {
        // Create it 0600 ourselves so SQLite's -wal/-shm inherit the private mode.
        const int fd = ::open(QFile::encodeName(path_).constData(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
        if (fd >= 0) ::close(fd);
    }
    tightenPermissions();
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        if (error) *error = QStringLiteral("the QSQLITE driver is not available");
        return false;
    }
    bool ok = false;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_);
        db.setDatabaseName(path_);
        db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=%1").arg(openBusyTimeoutMs));
        ok = db.open();
        if (!ok) message = db.lastError().text();
    }
    if (ok) {
        // Read the version before anything that would write to the file.
        QSqlQuery q(database());
        int version = 0;
        if (runSelect(q, QStringLiteral("PRAGMA user_version")) && q.next()) version = q.value(0).toInt();
        q.finish();
        if (version > librarySchemaVersion) {
            message = QStringLiteral("library schema version %1 is newer than this SailTube supports (%2)")
                          .arg(version).arg(librarySchemaVersion);
            ok = false;
        }
    }
    if (ok) {
        QSqlQuery q(database());
        if (runSelect(q, QStringLiteral("PRAGMA journal_mode=WAL")) && q.next()
            && q.value(0).toString().toLower() != QLatin1String("wal"))
            qWarning("library: WAL unavailable, journal mode %s", qPrintable(q.value(0).toString()));
        q.finish();
        ok = exec(QStringLiteral("PRAGMA synchronous=NORMAL"), QVariantList(), &message)
             && exec(QStringLiteral("PRAGMA foreign_keys=ON"), QVariantList(), &message)
             && exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(openBusyTimeoutMs), QVariantList(), &message)
             && migrate(&message)
             && exec(QStringLiteral("PRAGMA busy_timeout=%1").arg(busyTimeoutMs), QVariantList(), &message);
    }
    if (!ok) {
        if (error) *error = message;
        {
            QSqlDatabase db = database();
            if (db.isValid()) db.close();
        }
        QSqlDatabase::removeDatabase(connection_);
        return false;
    }
    tightenPermissions();
    open_ = true;
    loadIndexes();
    return true;
}

bool LibraryStore::migrate(QString *error) {
    QSqlDatabase db = database();
    QSqlQuery q(db);
    if (!q.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        if (error) *error = q.lastError().text();
        return false;
    }
    int version = 0;
    if (q.exec(QStringLiteral("PRAGMA user_version")) && q.next()) version = q.value(0).toInt();
    q.finish();
    bool ok = version <= librarySchemaVersion;
    if (!ok && error) *error = QStringLiteral("library schema version %1 is newer than this SailTube").arg(version);
    for (int v = version; ok && v < librarySchemaVersion; ++v) {
        const QStringList steps = librarySchemaStep(v);
        for (const QString &sql : steps) {
            if (!q.exec(sql)) {
                if (error) *error = QStringLiteral("migration %1: %2").arg(v).arg(q.lastError().text());
                ok = false;
                break;
            }
        }
    }
    if (ok && version < librarySchemaVersion) {
        // PRAGMA takes no bind values; the number is our own constant.
        ok = q.exec(QStringLiteral("PRAGMA user_version = %1").arg(librarySchemaVersion));
        if (!ok && error) *error = q.lastError().text();
    }
    if (ok) {
        // Watch later is part of the schema contract; restore it if a tool removed it.
        ok = q.exec(QStringLiteral("INSERT OR IGNORE INTO playlists(id, kind, title, sort_key, created_ms, updated_ms)"
                                   " VALUES('watch-later', 1, 'Watch later', -1000000000, 0, 0)"));
        if (!ok && error) *error = q.lastError().text();
    }
    if (ok) {
        ok = q.exec(QStringLiteral("COMMIT"));
        if (!ok && error) *error = q.lastError().text();
    }
    if (!ok) {
        QSqlQuery rollback(db);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    version_ = librarySchemaVersion;
    return true;
}

void LibraryStore::loadIndexes() {
    subscribed_.clear();
    history_.clear();
    bySeq_.clear();
    resume_.clear();
    meta_.clear();
    ahead_.clear();
    nextSeq_ = 1;
    resumeOrder_ = 0;
    const qint64 now = libraryNowMs();
    {
        // Best effort sweep; a locked database just means the rows are filtered below.
        Statements sweep;
        sweep << Statement{QStringLiteral("DELETE FROM resume WHERE touched_ms < ?"),
                           QVariantList() << v64(now - limits::resumeExpiryMs)};
        if (pending_.empty()) runTransaction(sweep);
    }
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QStringLiteral("SELECT channel_id FROM subscriptions"))) {
        while (q.next()) subscribed_.insert(q.value(0).toString());
    }
    if (runSelect(q, QStringLiteral("SELECT video_id, fraction, watched, seq FROM history"))) {
        while (q.next()) {
            HistoryMeta m;
            m.fraction = q.value(1).toDouble();
            m.watched = q.value(2).toInt() != 0;
            m.seq = q.value(3).toLongLong();
            const QString id = q.value(0).toString();
            history_.insert(id, m);
            bySeq_.insert(m.seq, id);
            nextSeq_ = std::max(nextSeq_, m.seq + 1);
        }
    }
    if (runSelect(q, QStringLiteral("SELECT video_id, position_ms, touched_ms FROM resume ORDER BY touched_ms, rowid"))) {
        while (q.next()) {
            ResumeMeta m;
            m.positionMs = q.value(1).toLongLong();
            m.touchedMs = q.value(2).toLongLong();
            if (now - m.touchedMs > limits::resumeExpiryMs) continue;
            m.order = ++resumeOrder_;
            resume_.insert(q.value(0).toString(), m);
        }
    }
    if (runSelect(q, QStringLiteral("SELECT key, value FROM meta WHERE substr(key, 1, ?) <> ?"),
                  QVariantList() << int(qstrlen(importMarkerPrefix)) << QString::fromLatin1(importMarkerPrefix))) {
        while (q.next()) meta_.insert(q.value(0).toString(), q.value(1).toString());
    }
}

void LibraryStore::reload() {
    if (!open_) return;
    flushPending();
    if (!pending_.empty()) {
        reloadRequested_ = true; // after the queue drains; SQL is behind memory until then
        return;
    }
    reloadAndNotify();
}

void LibraryStore::reloadAndNotify() {
    const QHash<QString, HistoryMeta> historyBefore = history_;
    const QHash<QString, ResumeMeta> resumeBefore = resume_;
    loadIndexes();
    emit subscriptionsChanged();
    emit playlistsChanged();
    emit historyChanged();
    emitWatchChanges(historyBefore);
    QSet<QString> ids = QSet<QString>::fromList(resumeBefore.keys()) + QSet<QString>::fromList(resume_.keys());
    for (const QString &id : ids) {
        const bool a = resumeBefore.contains(id), b = resume_.contains(id);
        if (a != b || (a && resumeBefore.value(id).positionMs != resume_.value(id).positionMs)) emit resumeChanged(id);
    }
}

void LibraryStore::emitWatchChanges(const QHash<QString, HistoryMeta> &before) {
    QSet<QString> ids = QSet<QString>::fromList(before.keys()) + QSet<QString>::fromList(history_.keys());
    for (const QString &id : ids) {
        const bool a = before.contains(id), b = history_.contains(id);
        if (a != b) { emit watchStateChanged(id); continue; }
        const HistoryMeta &x = before[id];
        const HistoryMeta &y = history_[id];
        if (x.fraction != y.fraction || x.watched != y.watched) emit watchStateChanged(id);
    }
}

// ---- transactions and the busy queue ------------------------------------

LibraryStore::TxResult LibraryStore::runTransaction(const Statements &statements, QString *error) {
    QSqlDatabase db = database();
    {
        QSqlQuery begin(db);
        if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
            if (error) *error = begin.lastError().text();
            return isBusyError(begin.lastError()) ? TxResult::Busy : TxResult::Failed;
        }
    }
    TxResult result = TxResult::Done;
    {
        QHash<QString, QSqlQuery *> prepared; // one prepare per distinct statement (5000-row inserts)
        for (const Statement &st : statements) {
            QSqlQuery *q = prepared.value(st.sql);
            if (!q) {
                q = new QSqlQuery(db);
                prepared.insert(st.sql, q);
                if (!q->prepare(st.sql)) {
                    if (error) *error = q->lastError().text();
                    result = isBusyError(q->lastError()) ? TxResult::Busy : TxResult::Failed;
                    break;
                }
            }
            for (int i = 0; i < st.bind.size(); ++i) q->bindValue(i, st.bind.at(i));
            if (!q->exec()) {
                if (error) *error = q->lastError().text();
                result = isBusyError(q->lastError()) ? TxResult::Busy : TxResult::Failed;
                break;
            }
        }
        qDeleteAll(prepared);
    }
    if (result == TxResult::Done) {
        QSqlQuery commit(db);
        if (commit.exec(QStringLiteral("COMMIT"))) return TxResult::Done;
        if (error) *error = commit.lastError().text();
        result = isBusyError(commit.lastError()) ? TxResult::Busy : TxResult::Failed;
    }
    QSqlQuery rollback(db);
    rollback.exec(QStringLiteral("ROLLBACK"));
    return result;
}

LibraryStore::TxResult LibraryStore::write(const Statements &statements) {
    if (!open_) return TxResult::Failed;
    if (statements.isEmpty()) return TxResult::Done;
    Pending p;
    p.id = ++nextPendingId_;
    p.statements = statements;
    pending_.push_back(p);
    flushPending();
    if (!pending_.empty()) return TxResult::Busy; // FIFO: ours is behind the blocked head
    return lastFailedId_ == p.id ? TxResult::Failed : TxResult::Done;
}

void LibraryStore::flushPending() {
    while (!pending_.empty()) {
        QString error;
        const TxResult r = runTransaction(pending_.front().statements, &error);
        if (r == TxResult::Busy) {
            if (!retry_.isActive()) retry_.start();
            return;
        }
        if (r == TxResult::Failed) {
            qWarning("library: write failed: %s", qPrintable(error));
            lastFailedId_ = pending_.front().id;
            reloadRequested_ = true; // memory went ahead of a write that never landed
        }
        pending_.pop_front();
    }
    retry_.stop();
    ahead_.clear();
    if (reloadRequested_) {
        reloadRequested_ = false;
        reloadAndNotify();
    }
}

bool LibraryStore::waitForPending(int timeoutMs) {
    QElapsedTimer timer;
    timer.start();
    flushPending();
    while (!pending_.empty() && timer.elapsed() < timeoutMs) {
        QThread::msleep(10);
        flushPending();
    }
    return pending_.empty();
}

bool LibraryStore::writeBlocking(const Statements &statements) {
    if (!waitForPending(importBusyDeadlineMs)) return false;
    QElapsedTimer timer;
    timer.start();
    for (;;) {
        QString error;
        const TxResult r = runTransaction(statements, &error);
        if (r == TxResult::Done) return true;
        if (r == TxResult::Failed || timer.elapsed() > importBusyDeadlineMs) {
            qWarning("library: import write failed: %s", qPrintable(error));
            return false;
        }
        QThread::msleep(10); // lets the playback thread take the lock between attempts
    }
}

// ---- LIB-01 subscriptions --------------------------------------------------

bool LibraryStore::isSubscribed(const QString &channelId) const { return subscribed_.contains(channelId); }

void LibraryStore::subscriptionStatements(const ChannelRecord &c, Statements *tx) const {
    const qint64 added = c.addedMs > 0 ? c.addedMs : libraryNowMs();
    const QString id = qs(c.channelId), name = qs(c.name), avatar = qs(c.avatarUrl);
    *tx << Statement{QStringLiteral("INSERT OR IGNORE INTO subscriptions(channel_id, name, avatar_url, latest_upload_ms,"
                                    " added_ms) VALUES(?, ?, ?, ?, ?)"),
                     QVariantList() << id << name << avatar << v64(std::max<std::int64_t>(0, c.latestUploadMs)) << v64(added)};
    *tx << Statement{QStringLiteral("UPDATE subscriptions SET name = CASE WHEN ? = '' THEN name ELSE ? END,"
                                    " avatar_url = CASE WHEN ? = '' THEN avatar_url ELSE ? END,"
                                    " latest_upload_ms = MAX(latest_upload_ms, ?) WHERE channel_id = ?"),
                     QVariantList() << name << name << avatar << avatar << v64(std::max<std::int64_t>(0, c.latestUploadMs)) << id};
}

bool LibraryStore::subscribe(const ChannelRecord &channel) {
    if (!open_ || !catalog::validChannelId(channel.channelId)) return false;
    Statements tx;
    subscriptionStatements(channel, &tx);
    if (write(tx) == TxResult::Failed) return false;
    subscribed_.insert(qs(channel.channelId));
    emit subscriptionsChanged();
    return true;
}

bool LibraryStore::unsubscribe(const QString &channelId) {
    if (!open_ || !subscribed_.contains(channelId)) return false;
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM subscriptions WHERE channel_id = ?"), QVariantList() << channelId};
    if (write(tx) == TxResult::Failed) return false;
    subscribed_.remove(channelId);
    emit subscriptionsChanged();
    return true;
}

std::vector<ChannelRecord> LibraryStore::subscriptions(SubscriptionSort sort) const {
    std::vector<ChannelRecord> out;
    if (!open_) return out;
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QStringLiteral("SELECT channel_id, name, avatar_url, latest_upload_ms, added_ms FROM subscriptions"))) {
        while (q.next()) {
            ChannelRecord c;
            c.channelId = ss(q.value(0).toString());
            c.name = ss(q.value(1).toString());
            c.avatarUrl = ss(q.value(2).toString());
            c.latestUploadMs = q.value(3).toLongLong();
            c.addedMs = q.value(4).toLongLong();
            out.push_back(c);
        }
    }
    sortSubscriptions(out, sort);
    return out;
}

QStringList LibraryStore::subscribedChannelIds() const {
    QStringList ids = subscribed_.toList();
    ids.sort();
    return ids;
}

void LibraryStore::updateChannelInfo(const QString &channelId, qint64 latestUploadMs, const QString &name,
                                     const QString &avatarUrl) {
    if (!open_ || !subscribed_.contains(channelId)) return;
    QSqlQuery q(database());
    if (!runSelect(q, QStringLiteral("SELECT name, avatar_url, latest_upload_ms FROM subscriptions WHERE channel_id = ?"),
                   QVariantList() << channelId) || !q.next())
        return;
    const QString newName = name.isEmpty() ? q.value(0).toString() : name;
    const QString newAvatar = avatarUrl.isEmpty() ? q.value(1).toString() : avatarUrl;
    const qint64 newLatest = latestUploadMs > 0 ? latestUploadMs : q.value(2).toLongLong();
    if (newName == q.value(0).toString() && newAvatar == q.value(1).toString() && newLatest == q.value(2).toLongLong())
        return;
    q.finish();
    Statements tx;
    tx << Statement{QStringLiteral("UPDATE subscriptions SET name = ?, avatar_url = ?, latest_upload_ms = ? WHERE channel_id = ?"),
                    QVariantList() << newName << newAvatar << v64(newLatest) << channelId};
    if (write(tx) != TxResult::Failed) emit subscriptionsChanged();
}

std::vector<PendingHandle> LibraryStore::pendingHandles() const {
    std::vector<PendingHandle> out;
    if (!open_) return out;
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QStringLiteral("SELECT url, name, added_ms FROM pending_handles ORDER BY added_ms, url"))) {
        while (q.next()) {
            PendingHandle h;
            h.url = ss(q.value(0).toString());
            h.name = ss(q.value(1).toString());
            h.addedMs = q.value(2).toLongLong();
            out.push_back(h);
        }
    }
    return out;
}

bool LibraryStore::resolvePendingHandle(const QString &url, const ChannelRecord &channel) {
    if (!open_ || !catalog::validChannelId(channel.channelId)) return false;
    ChannelRecord c = channel;
    if (c.name.empty()) {
        QSqlQuery q(database());
        if (runSelect(q, QStringLiteral("SELECT name FROM pending_handles WHERE url = ?"), QVariantList() << url) && q.next())
            c.name = ss(q.value(0).toString());
    }
    Statements tx;
    subscriptionStatements(c, &tx);
    tx << Statement{QStringLiteral("DELETE FROM pending_handles WHERE url = ?"), QVariantList() << url};
    if (write(tx) == TxResult::Failed) return false;
    subscribed_.insert(qs(c.channelId));
    emit subscriptionsChanged();
    return true;
}

void LibraryStore::dropPendingHandle(const QString &url) {
    if (!open_) return;
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM pending_handles WHERE url = ?"), QVariantList() << url};
    if (write(tx) != TxResult::Failed) emit subscriptionsChanged();
}

// ---- LIB-02/03 playlists --------------------------------------------------

bool LibraryStore::playlistKind(const QString &id, int *kind) const {
    QSqlQuery q(database());
    if (!runSelect(q, QStringLiteral("SELECT kind FROM playlists WHERE id = ?"), QVariantList() << id) || !q.next())
        return false;
    *kind = q.value(0).toInt();
    return true;
}

QString LibraryStore::newLocalPlaylistId() const {
    std::random_device rd;
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::uint64_t entropy = (std::uint64_t(rd()) << 32) ^ std::uint64_t(rd())
                                      ^ std::uint64_t(QDateTime::currentMSecsSinceEpoch()) ^ (std::uint64_t(attempt) << 40);
        const QString id = qs(localPlaylistId(entropy));
        int kind = 0;
        if (!playlistKind(id, &kind)) return id;
    }
    return QString();
}

std::vector<PlaylistRecord> LibraryStore::playlists() const {
    std::vector<PlaylistRecord> out;
    if (!open_) return out;
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QString::fromLatin1(playlistSelect) + QString::fromLatin1(playlistOrder))) {
        while (q.next()) out.push_back(playlistFromRow(q));
    }
    return out;
}

bool LibraryStore::playlist(const QString &id, PlaylistRecord *out) const {
    if (!open_) return false;
    QSqlQuery q(database());
    if (!runSelect(q, QString::fromLatin1(playlistSelect) + QStringLiteral(" WHERE p.id = ?"), QVariantList() << id)
        || !q.next())
        return false;
    if (out) *out = playlistFromRow(q);
    return true;
}

QString LibraryStore::createPlaylist(const QString &title) {
    if (!open_) return QString();
    const QString id = newLocalPlaylistId();
    if (id.isEmpty()) return QString();
    const qint64 now = libraryNowMs();
    Statements tx;
    tx << Statement{QStringLiteral("INSERT INTO playlists(id, kind, title, sort_key, created_ms, updated_ms) VALUES(?, 0, ?,"
                                   " (SELECT COALESCE(MIN(sort_key), 1) - 1 FROM playlists WHERE kind <> 1), ?, ?)"),
                    QVariantList() << id << qs(normalizePlaylistTitle(ss(title), "New playlist")) << v64(now) << v64(now)};
    if (write(tx) == TxResult::Failed) return QString();
    emit playlistsChanged();
    return id;
}

bool LibraryStore::renamePlaylist(const QString &id, const QString &title) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind != int(PlaylistKind::Local)) return false;
    const std::string t = normalizePlaylistTitle(ss(title), std::string());
    if (t.empty()) return false;
    Statements tx;
    tx << Statement{QStringLiteral("UPDATE playlists SET title = ?, updated_ms = ? WHERE id = ?"),
                    QVariantList() << qs(t) << v64(libraryNowMs()) << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistsChanged();
    return true;
}

bool LibraryStore::deletePlaylist(const QString &id) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind == int(PlaylistKind::WatchLater)) return false;
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM playlist_items WHERE playlist_id = ?"), QVariantList() << id}
       << Statement{QStringLiteral("DELETE FROM playlists WHERE id = ?"), QVariantList() << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistChanged(id);
    emit playlistsChanged();
    return true;
}

bool LibraryStore::movePlaylist(const QString &id, int toIndex) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind != int(PlaylistKind::Local)) return false;
    QStringList order;
    {
        QSqlQuery q(database());
        q.setForwardOnly(true);
        if (!runSelect(q, QStringLiteral("SELECT p.id FROM playlists p WHERE p.kind <> 1") + QString::fromLatin1(playlistOrder)))
            return false;
        while (q.next()) order << q.value(0).toString();
    }
    const int from = order.indexOf(id);
    if (from < 0) return false;
    // toIndex counts in playlists(), where watch later holds index 0.
    const int to = std::max(0, std::min(toIndex - 1, order.size() - 1));
    if (to == from) return true;
    order.move(from, to);
    Statements tx;
    for (int i = 0; i < order.size(); ++i)
        tx << Statement{QStringLiteral("UPDATE playlists SET sort_key = ? WHERE id = ?"), QVariantList() << i << order.at(i)};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistsChanged();
    return true;
}

std::vector<VideoEntry> LibraryStore::playlistItems(const QString &id) const {
    std::vector<VideoEntry> out;
    if (!open_) return out;
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QStringLiteral("SELECT video_id, title, channel, channel_id, duration_s, thumbnail_url FROM playlist_items"
                                    " WHERE playlist_id = ? ORDER BY position"),
                  QVariantList() << id)) {
        while (q.next()) out.push_back(itemFromRow(q, 0));
    }
    return out;
}

bool LibraryStore::playlistContains(const QString &id, const QString &videoId) const {
    if (!open_) return false;
    QSqlQuery q(database());
    return runSelect(q, QStringLiteral("SELECT 1 FROM playlist_items WHERE playlist_id = ? AND video_id = ?"),
                     QVariantList() << id << videoId)
           && q.next();
}

bool LibraryStore::addToPlaylist(const QString &id, const VideoEntry &video) {
    int kind = -1;
    if (!open_ || !catalog::validVideoId(video.videoId) || !playlistKind(id, &kind)
        || kind == int(PlaylistKind::RemoteMirror))
        return false;
    if (playlistContains(id, qs(video.videoId))) return false;
    const qint64 now = libraryNowMs();
    QVariantList bind = itemBind(id, 0, video, now);
    bind[1] = id; // the position slot binds the subselect's playlist id
    Statements tx;
    tx << Statement{QStringLiteral("INSERT OR IGNORE INTO playlist_items(playlist_id, position, video_id, title, channel,"
                                   " channel_id, duration_s, thumbnail_url, added_ms) VALUES(?,"
                                   " (SELECT COALESCE(MAX(position), -1) + 1 FROM playlist_items WHERE playlist_id = ?),"
                                   " ?, ?, ?, ?, ?, ?, ?)"),
                    bind}
       << Statement{QStringLiteral("UPDATE playlists SET updated_ms = ? WHERE id = ?"), QVariantList() << v64(now) << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistChanged(id);
    emit playlistsChanged();
    return true;
}

bool LibraryStore::removeFromPlaylist(const QString &id, const QString &videoId) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind == int(PlaylistKind::RemoteMirror)) return false;
    int position = -1;
    {
        QSqlQuery q(database());
        if (!runSelect(q, QStringLiteral("SELECT position FROM playlist_items WHERE playlist_id = ? AND video_id = ?"),
                       QVariantList() << id << videoId) || !q.next())
            return false;
        position = q.value(0).toInt();
    }
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM playlist_items WHERE playlist_id = ? AND video_id = ?"), QVariantList() << id << videoId}
       << Statement{QStringLiteral("UPDATE playlist_items SET position = position - 1 WHERE playlist_id = ? AND position > ?"),
                    QVariantList() << id << position}
       << Statement{QStringLiteral("UPDATE playlists SET updated_ms = ? WHERE id = ?"), QVariantList() << v64(libraryNowMs()) << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistChanged(id);
    emit playlistsChanged();
    return true;
}

bool LibraryStore::moveInPlaylist(const QString &id, int from, int to) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind == int(PlaylistKind::RemoteMirror)) return false;
    QString videoId;
    int count = 0;
    {
        QSqlQuery q(database());
        if (!runSelect(q, QStringLiteral("SELECT COUNT(*) FROM playlist_items WHERE playlist_id = ?"), QVariantList() << id)
            || !q.next())
            return false;
        count = q.value(0).toInt();
        if (from < 0 || to < 0 || from >= count || to >= count) return false;
        if (from == to) return true;
        if (!runSelect(q, QStringLiteral("SELECT video_id FROM playlist_items WHERE playlist_id = ? AND position = ?"),
                       QVariantList() << id << from) || !q.next())
            return false;
        videoId = q.value(0).toString();
    }
    Statements tx;
    if (from < to)
        tx << Statement{QStringLiteral("UPDATE playlist_items SET position = position - 1 WHERE playlist_id = ?"
                                       " AND position > ? AND position <= ?"),
                        QVariantList() << id << from << to};
    else
        tx << Statement{QStringLiteral("UPDATE playlist_items SET position = position + 1 WHERE playlist_id = ?"
                                       " AND position >= ? AND position < ?"),
                        QVariantList() << id << to << from};
    tx << Statement{QStringLiteral("UPDATE playlist_items SET position = ? WHERE playlist_id = ? AND video_id = ?"),
                    QVariantList() << to << id << videoId}
       << Statement{QStringLiteral("UPDATE playlists SET updated_ms = ? WHERE id = ?"), QVariantList() << v64(libraryNowMs()) << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistChanged(id);
    return true;
}

QString LibraryStore::mirrorIdForRemote(const QString &remoteId) const {
    if (!open_) return QString();
    const std::string rid = playlistIdFromUrl(ss(remoteId));
    if (rid.empty()) return QString();
    QSqlQuery q(database());
    if (runSelect(q, QStringLiteral("SELECT id FROM playlists WHERE remote_id = ? AND kind = 2"), QVariantList() << qs(rid))
        && q.next())
        return q.value(0).toString();
    return QString();
}

QString LibraryStore::saveRemotePlaylist(const RemotePlaylistRef &ref) {
    if (!open_) return QString();
    const std::string rid = playlistIdFromUrl(ref.remoteId);
    if (rid.empty()) return QString();
    const QString existing = mirrorIdForRemote(qs(rid));
    const QString title = qs(normalizePlaylistTitle(ref.title, std::string()));
    const qint64 now = libraryNowMs();
    Statements tx;
    QString id = existing;
    if (!existing.isEmpty()) {
        tx << Statement{QStringLiteral("UPDATE playlists SET title = CASE WHEN ? = '' THEN title ELSE ? END,"
                                       " channel = CASE WHEN ? = '' THEN channel ELSE ? END,"
                                       " thumbnail_url = CASE WHEN ? = '' THEN thumbnail_url ELSE ? END,"
                                       " item_count_hint = CASE WHEN ? >= 0 THEN ? ELSE item_count_hint END,"
                                       " updated_ms = ? WHERE id = ?"),
                        QVariantList() << title << title << qs(ref.channel) << qs(ref.channel) << qs(ref.thumbnailUrl)
                                       << qs(ref.thumbnailUrl) << ref.itemCountHint << ref.itemCountHint << v64(now) << id};
    } else {
        id = QStringLiteral("r-") + qs(rid);
        tx << Statement{QStringLiteral("INSERT INTO playlists(id, kind, title, remote_id, channel, thumbnail_url, sort_key,"
                                       " created_ms, updated_ms, refreshed_ms, item_count_hint) VALUES(?, 2, ?, ?, ?, ?,"
                                       " (SELECT COALESCE(MIN(sort_key), 1) - 1 FROM playlists WHERE kind <> 1), ?, ?, 0, ?)"),
                        QVariantList() << id << (title.isEmpty() ? qs(rid) : title) << qs(rid) << qs(ref.channel)
                                       << qs(ref.thumbnailUrl) << v64(now) << v64(now) << ref.itemCountHint};
    }
    if (write(tx) == TxResult::Failed) return QString();
    emit playlistsChanged();
    return id;
}

bool LibraryStore::replaceRemoteItems(const QString &id, const std::vector<VideoEntry> &items, qint64 refreshedMs) {
    int kind = -1;
    if (!open_ || !playlistKind(id, &kind) || kind != int(PlaylistKind::RemoteMirror)) return false;
    const qint64 now = libraryNowMs();
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM playlist_items WHERE playlist_id = ?"), QVariantList() << id};
    QSet<QString> seen;
    int position = 0;
    for (const VideoEntry &v : items) {
        const QString vid = qs(v.videoId);
        if (!catalog::validVideoId(v.videoId) || seen.contains(vid)) continue;
        seen.insert(vid);
        tx << Statement{QString::fromLatin1(insertItemSql), itemBind(id, position++, v, now)};
    }
    tx << Statement{QStringLiteral("UPDATE playlists SET refreshed_ms = ?, updated_ms = ?, item_count_hint = ? WHERE id = ?"),
                    QVariantList() << v64(refreshedMs) << v64(now) << position << id};
    if (write(tx) == TxResult::Failed) return false;
    emit playlistChanged(id);
    emit playlistsChanged();
    return true;
}

// ---- LIB-04 history ------------------------------------------------------

bool LibraryStore::readHistory(const QString &videoId, HistoryRecord *out) const {
    if (!history_.contains(videoId)) return false; // the index is authoritative for presence
    const auto ahead = ahead_.constFind(videoId);
    if (ahead != ahead_.constEnd()) {
        *out = ahead.value();
        return true;
    }
    QSqlQuery q(database());
    if (!runSelect(q, QStringLiteral("SELECT %1 FROM history WHERE video_id = ?").arg(QLatin1String(historyColumns)),
                   QVariantList() << videoId) || !q.next())
        return false;
    *out = historyFromRow(q);
    return true;
}

void LibraryStore::putHistory(const HistoryRecord &r, qint64 seq, Statements *tx) {
    const QString id = qs(r.video.videoId);
    *tx << Statement{QStringLiteral("INSERT OR REPLACE INTO history(%1) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                         .arg(QLatin1String(historyColumns)),
                     QVariantList() << id << qs(r.video.title) << qs(r.video.channel) << qs(r.video.channelId)
                                    << v64(r.video.durationSeconds) << qs(r.video.thumbnailUrl) << v64(r.positionMs)
                                    << v64(r.durationMs) << r.fraction << (r.watched ? 1 : 0) << v64(r.watchedAtMs)
                                    << r.playCount << v64(seq)};
    const auto old = history_.constFind(id);
    if (old != history_.constEnd()) bySeq_.remove(old.value().seq);
    HistoryMeta m;
    m.fraction = r.fraction;
    m.watched = r.watched;
    m.seq = seq;
    history_.insert(id, m);
    bySeq_.insert(seq, id);
    nextSeq_ = std::max(nextSeq_, seq + 1);
    ahead_.insert(id, r);
}

void LibraryStore::trimHistory(int cap, Statements *tx, QStringList *evicted) {
    while (history_.size() > cap && !bySeq_.isEmpty()) {
        const QString id = bySeq_.begin().value();
        bySeq_.erase(bySeq_.begin());
        history_.remove(id);
        ahead_.remove(id);
        *tx << Statement{QStringLiteral("DELETE FROM history WHERE video_id = ?"), QVariantList() << id};
        if (evicted) *evicted << id;
    }
}

void LibraryStore::sweepResume(qint64 nowMs, Statements *tx, QStringList *removed) {
    for (auto it = resume_.begin(); it != resume_.end();) {
        if (nowMs - it.value().touchedMs > limits::resumeExpiryMs) {
            if (removed) *removed << it.key();
            it = resume_.erase(it);
        } else {
            ++it;
        }
    }
    *tx << Statement{QStringLiteral("DELETE FROM resume WHERE touched_ms < ?"), QVariantList() << v64(nowMs - limits::resumeExpiryMs)};
}

void LibraryStore::capResume(Statements *tx, QStringList *removed) {
    while (resume_.size() > limits::resumeCapacity) {
        auto oldest = resume_.begin();
        for (auto it = resume_.begin(); it != resume_.end(); ++it) {
            const ResumeMeta &a = it.value(), &b = oldest.value();
            if (a.touchedMs < b.touchedMs || (a.touchedMs == b.touchedMs && a.order < b.order)) oldest = it;
        }
        *tx << Statement{QStringLiteral("DELETE FROM resume WHERE video_id = ?"), QVariantList() << oldest.key()};
        if (removed) *removed << oldest.key();
        resume_.erase(oldest);
    }
}

void LibraryStore::recordProgress(const PlaybackProgress &progress, bool newSession) {
    if (!open_ || !catalog::validVideoId(progress.video.videoId)) return;
    const QString id = qs(progress.video.videoId);
    const qint64 now = libraryNowMs();
    const WatchState stateBefore = watchState(id);
    const qint64 resumeBefore = resumePositionMs(id);
    Statements tx;
    QStringList evicted, resumeRemoved;
    const bool paused = historyPaused();
    if (!paused) {
        HistoryRecord existing;
        const bool has = readHistory(id, &existing);
        HistoryRecord merged = mergeProgress(has ? &existing : nullptr, progress, now, newSession);
        merged.video.videoId = progress.video.videoId;
        putHistory(merged, nextSeq_, &tx);
        trimHistory(historyCap(), &tx, &evicted);
    }
    const std::int64_t position = resumePositionToStore(progress);
    if (position >= 0) {
        ResumeMeta m;
        m.positionMs = position;
        m.touchedMs = now;
        m.order = ++resumeOrder_;
        resume_.insert(id, m);
        tx << Statement{QStringLiteral("INSERT OR REPLACE INTO resume(video_id, position_ms, duration_ms, touched_ms) VALUES(?, ?, ?, ?)"),
                        QVariantList() << id << v64(position) << v64(progress.durationMs) << v64(now)};
    } else if (resume_.remove(id)) {
        tx << Statement{QStringLiteral("DELETE FROM resume WHERE video_id = ?"), QVariantList() << id};
    }
    sweepResume(now, &tx, &resumeRemoved);
    capResume(&tx, &resumeRemoved);
    write(tx); // Busy: queued and retried by retry_; memory already holds the new state

    if (!paused) emit historyChanged();
    const WatchState stateAfter = watchState(id);
    if (stateAfter.known != stateBefore.known || stateAfter.fraction != stateBefore.fraction
        || stateAfter.watched != stateBefore.watched)
        emit watchStateChanged(id);
    for (const QString &e : evicted) emit watchStateChanged(e);
    if (resumePositionMs(id) != resumeBefore) emit resumeChanged(id);
    for (const QString &r : resumeRemoved) {
        if (r != id) emit resumeChanged(r);
    }
}

std::vector<HistoryRecord> LibraryStore::history(int offset, int count) const {
    std::vector<HistoryRecord> out;
    if (!open_ || count <= 0) return out;
    QSqlQuery q(database());
    q.setForwardOnly(true);
    if (runSelect(q, QStringLiteral("SELECT %1 FROM history ORDER BY seq DESC LIMIT ? OFFSET ?").arg(QLatin1String(historyColumns)),
                  QVariantList() << count << std::max(0, offset))) {
        while (q.next()) {
            const QString id = q.value(0).toString();
            const auto ahead = ahead_.constFind(id);
            out.push_back(ahead != ahead_.constEnd() ? ahead.value() : historyFromRow(q));
        }
    }
    return out;
}

int LibraryStore::historyCount() const { return history_.size(); }

bool LibraryStore::historyRecord(const QString &videoId, HistoryRecord *out) const {
    HistoryRecord r;
    if (!open_ || !readHistory(videoId, &r)) return false;
    if (out) *out = r;
    return true;
}

void LibraryStore::markWatched(const QString &videoId, const VideoEntry &video) {
    if (!open_ || !catalog::validVideoId(ss(videoId))) return;
    const qint64 now = libraryNowMs();
    HistoryRecord record;
    const bool has = readHistory(videoId, &record);
    if (!has) {
        record = HistoryRecord();
        record.video = video;
        record.video.videoId = ss(videoId);
        record.playCount = 1;
    } else {
        if (record.video.title.empty()) record.video.title = video.title;
        if (record.video.channel.empty()) record.video.channel = video.channel;
        if (record.video.channelId.empty()) record.video.channelId = video.channelId;
        if (record.video.durationSeconds < 0) record.video.durationSeconds = video.durationSeconds;
        if (record.video.thumbnailUrl.empty()) record.video.thumbnailUrl = video.thumbnailUrl;
    }
    Statements tx;
    QStringList evicted;
    putHistory(library::markWatched(record, now), nextSeq_, &tx);
    trimHistory(historyCap(), &tx, &evicted);
    const bool hadResume = resume_.remove(videoId);
    tx << Statement{QStringLiteral("DELETE FROM resume WHERE video_id = ?"), QVariantList() << videoId};
    write(tx);
    emit historyChanged();
    emit watchStateChanged(videoId);
    for (const QString &e : evicted) emit watchStateChanged(e);
    if (hadResume) emit resumeChanged(videoId);
}

void LibraryStore::unmarkWatched(const QString &videoId) {
    HistoryRecord record;
    if (!open_ || !readHistory(videoId, &record)) return; // no stub record when absent
    Statements tx;
    putHistory(library::unmarkWatched(record), history_.value(videoId).seq, &tx);
    write(tx);
    emit historyChanged();
    emit watchStateChanged(videoId);
}

void LibraryStore::removeFromHistory(const QString &videoId) {
    if (!open_ || !history_.contains(videoId)) return;
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM history WHERE video_id = ?"), QVariantList() << videoId};
    bySeq_.remove(history_.value(videoId).seq);
    history_.remove(videoId);
    ahead_.remove(videoId);
    write(tx);
    emit historyChanged();
    emit watchStateChanged(videoId);
}

void LibraryStore::clearHistory() {
    if (!open_) return;
    const QHash<QString, HistoryMeta> before = history_;
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM history"), QVariantList()};
    history_.clear();
    bySeq_.clear();
    ahead_.clear();
    write(tx);
    emit historyChanged();
    emitWatchChanges(before);
}

int LibraryStore::historyCap() const {
    return clampHistoryCap(meta_.value(QString::fromLatin1(keyHistoryCap)).toInt());
}

bool LibraryStore::setHistoryCap(int cap) {
    if (!open_) return false;
    if (!validHistoryCap(cap)) {
        qWarning("library: history cap %d refused (allowed %d..%d)", cap, limits::historyCapMinimum,
                 limits::historyCapMaximum);
        return false;
    }
    const int c = cap;
    Statements tx;
    tx << Statement{QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"),
                    QVariantList() << QString::fromLatin1(keyHistoryCap) << QString::number(c)};
    meta_.insert(QString::fromLatin1(keyHistoryCap), QString::number(c));
    QStringList evicted;
    trimHistory(c, &tx, &evicted);
    if (write(tx) == TxResult::Failed) return false;
    if (!evicted.isEmpty()) emit historyChanged();
    for (const QString &e : evicted) emit watchStateChanged(e);
    return true;
}

bool LibraryStore::historyPaused() const {
    return meta_.value(QString::fromLatin1(keyHistoryPaused)) == QLatin1String("1");
}

void LibraryStore::setHistoryPaused(bool paused) {
    setMetaValue(QString::fromLatin1(keyHistoryPaused), paused ? QStringLiteral("1") : QStringLiteral("0"));
}

// ---- LIB-05 / LIB-06 -------------------------------------------------------

qint64 LibraryStore::resumePositionMs(const QString &videoId) const {
    const auto it = resume_.constFind(videoId);
    if (it == resume_.constEnd()) return -1;
    if (libraryNowMs() - it.value().touchedMs > limits::resumeExpiryMs) return -1;
    return it.value().positionMs;
}

void LibraryStore::clearResume(const QString &videoId) {
    if (!open_ || !resume_.contains(videoId)) return;
    resume_.remove(videoId);
    Statements tx;
    tx << Statement{QStringLiteral("DELETE FROM resume WHERE video_id = ?"), QVariantList() << videoId};
    write(tx);
    emit resumeChanged(videoId);
}

WatchState LibraryStore::watchState(const QString &videoId) const {
    WatchState s;
    const auto it = history_.constFind(videoId);
    if (it == history_.constEnd()) return s;
    s.known = true;
    s.fraction = it.value().fraction;
    s.watched = it.value().watched;
    return s;
}

// ---- meta ---------------------------------------------------------------

QString LibraryStore::metaValue(const QString &key, const QString &fallback) const {
    const auto it = meta_.constFind(key);
    return it == meta_.constEnd() ? fallback : it.value();
}

void LibraryStore::setMetaValue(const QString &key, const QString &value) {
    if (!open_ || key.startsWith(QLatin1String(importMarkerPrefix))) return;
    meta_.insert(key, value);
    Statements tx;
    tx << Statement{QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"), QVariantList() << key << value};
    write(tx);
}

// ---- IMP-* --------------------------------------------------------------

namespace {
struct IncomingHistory {
    HistoryRecord record;
    int batchIndex = 0;
};
}

ImportSummary LibraryStore::applyImport(const ImportBatch &batch) {
    ImportSummary s;
    s.skippedRows = batch.skippedRows;
    s.skippedSections = batch.skippedSections;
    if (!open_) return s;
    waitForPending(importBusyDeadlineMs);
    // Decide against what SQL holds now (another instance may have written); diff
    // against what this instance's users last saw for the signals.
    const QHash<QString, HistoryMeta> historyBefore = history_;
    const QHash<QString, ResumeMeta> resumeBefore = resume_;
    loadIndexes();
    const qint64 now = libraryNowMs();
    const QString source = qs(batch.source);
    Statements tx;

    // Subscriptions and pending handles: existing rows are left as they are.
    QSet<QString> pendingUrls;
    {
        QSqlQuery q(database());
        q.setForwardOnly(true);
        if (runSelect(q, QStringLiteral("SELECT url FROM pending_handles"))) {
            while (q.next()) pendingUrls.insert(q.value(0).toString());
        }
    }
    auto addChannel = [&](const ChannelRecord &c) {
        const QString id = qs(c.channelId);
        if (subscribed_.contains(id)) return;
        ChannelRecord added = c;
        if (added.addedMs <= 0) added.addedMs = now;
        tx << Statement{QStringLiteral("INSERT OR IGNORE INTO subscriptions(channel_id, name, avatar_url, latest_upload_ms,"
                                       " added_ms) VALUES(?, ?, ?, ?, ?)"),
                        QVariantList() << id << qs(added.name) << qs(added.avatarUrl)
                                       << v64(std::max<std::int64_t>(0, added.latestUploadMs)) << v64(added.addedMs)};
        subscribed_.insert(id);
        ++s.subscriptionsAdded;
    };
    for (const ChannelRecord &c : batch.subscriptions) {
        if (!catalog::validChannelId(c.channelId)) { ++s.skippedRows; continue; }
        addChannel(c);
    }
    for (const PendingHandle &h : batch.pendingHandles) {
        const std::string channelId = channelIdFromUrl(h.url);
        if (!channelId.empty()) {
            ChannelRecord c;
            c.channelId = channelId;
            c.name = h.name;
            c.addedMs = h.addedMs;
            addChannel(c);
            continue;
        }
        if (!isHandleUrl(h.url)) { ++s.skippedRows; continue; }
        const QString url = qs(h.url).trimmed();
        if (pendingUrls.contains(url)) continue;
        pendingUrls.insert(url);
        tx << Statement{QStringLiteral("INSERT OR IGNORE INTO pending_handles(url, name, added_ms) VALUES(?, ?, ?)"),
                        QVariantList() << url << qs(h.name) << v64(h.addedMs > 0 ? h.addedMs : now)};
        ++s.handlesPending;
    }

    // Playlists. A list titled with the watch-later id (SailTube backups) merges
    // into the built-in list; any other list is skipped when a local one has its name.
    std::vector<std::string> localNames;
    QSet<QString> remoteKnown;
    qint64 sortTop = 0;
    QSet<QString> watchLaterItems;
    int watchLaterNext = 0;
    {
        QSqlQuery q(database());
        q.setForwardOnly(true);
        if (runSelect(q, QStringLiteral("SELECT title, kind, remote_id FROM playlists"))) {
            while (q.next()) {
                if (q.value(1).toInt() == int(PlaylistKind::RemoteMirror)) remoteKnown.insert(q.value(2).toString());
                else localNames.push_back(ss(q.value(0).toString()));
            }
        }
        if (runSelect(q, QStringLiteral("SELECT COALESCE(MIN(sort_key), 1) FROM playlists WHERE kind <> 1")) && q.next())
            sortTop = q.value(0).toLongLong();
        if (runSelect(q, QStringLiteral("SELECT video_id, position FROM playlist_items WHERE playlist_id = ?"),
                      QVariantList() << QString::fromLatin1(watchLaterPlaylistId))) {
            while (q.next()) {
                watchLaterItems.insert(q.value(0).toString());
                watchLaterNext = std::max(watchLaterNext, q.value(1).toInt() + 1);
            }
        }
    }
    QStringList changedPlaylists;
    std::vector<std::pair<const ImportedPlaylist *, std::string> > accepted;
    for (const ImportedPlaylist &pl : batch.playlists) {
        if (pl.title == watchLaterPlaylistId) {
            int added = 0;
            for (const VideoEntry &v : pl.items) {
                const QString vid = qs(v.videoId);
                if (!catalog::validVideoId(v.videoId) || watchLaterItems.contains(vid)) continue;
                watchLaterItems.insert(vid);
                tx << Statement{QString::fromLatin1(insertItemSql),
                                itemBind(QString::fromLatin1(watchLaterPlaylistId), watchLaterNext++, v, now)};
                ++added;
            }
            s.playlistItemsAdded += added;
            if (added) changedPlaylists << QString::fromLatin1(watchLaterPlaylistId);
            continue;
        }
        const std::string title = normalizePlaylistTitle(pl.title, "Imported playlist");
        bool clash = false;
        for (const std::string &n : localNames) {
            if (sameListName(n, title)) { clash = true; break; }
        }
        if (clash) { ++s.playlistsSkipped; continue; }
        localNames.push_back(title);
        accepted.push_back(std::make_pair(&pl, title));
    }
    std::vector<const RemotePlaylistRef *> mirrors;
    std::vector<std::string> mirrorIds;
    for (const RemotePlaylistRef &r : batch.remotePlaylists) {
        const std::string rid = playlistIdFromUrl(r.remoteId);
        if (rid.empty()) { ++s.skippedRows; continue; }
        if (remoteKnown.contains(qs(rid))) continue;
        remoteKnown.insert(qs(rid));
        mirrors.push_back(&r);
        mirrorIds.push_back(rid);
    }
    // Imported lists go on top in batch order (locals first, then mirrors).
    qint64 sortKey = sortTop - qint64(accepted.size() + mirrors.size());
    QSet<QString> newIds;
    for (std::size_t i = 0; i < accepted.size(); ++i) {
        QString id;
        for (int attempt = 0; attempt < 16 && (id.isEmpty() || newIds.contains(id)); ++attempt) id = newLocalPlaylistId();
        if (id.isEmpty() || newIds.contains(id)) continue;
        newIds.insert(id);
        tx << Statement{QStringLiteral("INSERT INTO playlists(id, kind, title, sort_key, created_ms, updated_ms) VALUES(?, 0, ?, ?, ?, ?)"),
                        QVariantList() << id << qs(accepted[i].second) << v64(sortKey++) << v64(now) << v64(now)};
        QSet<QString> seen;
        int position = 0;
        for (const VideoEntry &v : accepted[i].first->items) {
            const QString vid = qs(v.videoId);
            if (!catalog::validVideoId(v.videoId) || seen.contains(vid)) continue;
            seen.insert(vid);
            tx << Statement{QString::fromLatin1(insertItemSql), itemBind(id, position++, v, now)};
        }
        s.playlistItemsAdded += position;
        ++s.playlistsAdded;
        changedPlaylists << id;
    }
    for (std::size_t i = 0; i < mirrors.size(); ++i) {
        const RemotePlaylistRef &r = *mirrors[i];
        const std::string title = normalizePlaylistTitle(r.title, mirrorIds[i]);
        tx << Statement{QStringLiteral("INSERT OR IGNORE INTO playlists(id, kind, title, remote_id, channel, thumbnail_url, sort_key,"
                                       " created_ms, updated_ms, refreshed_ms, item_count_hint) VALUES(?, 2, ?, ?, ?, ?, ?, ?, ?, 0, ?)"),
                        QVariantList() << QStringLiteral("r-") + qs(mirrorIds[i]) << qs(title) << qs(mirrorIds[i])
                                       << qs(r.channel) << qs(r.thumbnailUrl) << v64(sortKey++) << v64(now) << v64(now)
                                       << r.itemCountHint};
        ++s.remotePlaylistsAdded;
    }

    // History: aggregate duplicates, then local records gain only a sticky watched
    // flag and the play counts this source has not contributed yet.
    std::vector<IncomingHistory> incoming;
    {
        QHash<QString, int> index;
        for (std::size_t i = 0; i < batch.history.size(); ++i) {
            const HistoryRecord &h = batch.history[i];
            if (!catalog::validVideoId(h.video.videoId)) { ++s.skippedRows; continue; }
            const QString id = qs(h.video.videoId);
            const auto found = index.constFind(id);
            if (found == index.constEnd()) {
                IncomingHistory in;
                in.record = h;
                in.record.playCount = std::max(1, h.playCount);
                in.batchIndex = int(i);
                index.insert(id, int(incoming.size()));
                incoming.push_back(in);
                continue;
            }
            HistoryRecord &m = incoming[std::size_t(found.value())].record;
            m.watched = m.watched || h.watched;
            m.fraction = std::max(m.fraction, h.fraction);
            m.playCount += std::max(1, h.playCount);
            if (h.watchedAtMs > m.watchedAtMs) {
                m.watchedAtMs = h.watchedAtMs;
                m.positionMs = h.positionMs;
            }
        }
    }
    QHash<QString, int> markers;
    const QString markerPrefix = QString::fromLatin1(importMarkerPrefix) + source + QLatin1Char('/');
    const bool ownBackup = batch.source == "sailtube"; // its counts already include ours: take the larger
    if (!ownBackup) {
        QSqlQuery q(database());
        q.setForwardOnly(true);
        if (runSelect(q, QStringLiteral("SELECT key, value FROM meta WHERE substr(key, 1, ?) = ?"),
                      QVariantList() << markerPrefix.size() << markerPrefix)) {
            while (q.next()) markers.insert(q.value(0).toString().mid(markerPrefix.size()), q.value(1).toInt());
        }
    }
    auto setMarker = [&](const QString &id, int count) {
        if (ownBackup || markers.value(id) >= count) return;
        tx << Statement{QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"),
                        QVariantList() << markerPrefix + id << QString::number(count)};
    };
    std::vector<IncomingHistory> fresh;
    for (const IncomingHistory &in : incoming) {
        const QString id = qs(in.record.video.videoId);
        HistoryRecord local;
        if (!readHistory(id, &local)) {
            fresh.push_back(in);
            continue;
        }
        const bool gainWatched = in.record.watched && !local.watched;
        int delta = ownBackup ? in.record.playCount - local.playCount : in.record.playCount - markers.value(id);
        delta = std::max(0, delta);
        if (gainWatched || delta > 0) {
            tx << Statement{QStringLiteral("UPDATE history SET watched = CASE WHEN ? = 1 THEN 1 ELSE watched END,"
                                           " play_count = play_count + ? WHERE video_id = ?"),
                            QVariantList() << (gainWatched ? 1 : 0) << delta << id};
            if (gainWatched) history_[id].watched = true;
            ++s.historyUpdated;
        }
        setMarker(id, in.record.playCount);
    }
    // New records go below every local one, in their own access order; ties keep
    // the batch's newest-first order. Only as many as the cap leaves room for.
    std::stable_sort(fresh.begin(), fresh.end(), [](const IncomingHistory &a, const IncomingHistory &b) {
        if (a.record.watchedAtMs != b.record.watchedAtMs) return a.record.watchedAtMs < b.record.watchedAtMs;
        return a.batchIndex > b.batchIndex;
    });
    const int cap = historyCap();
    const int room = std::max(0, cap - history_.size());
    const std::size_t skip = fresh.size() > std::size_t(room) ? fresh.size() - std::size_t(room) : 0;
    const qint64 minSeq = bySeq_.isEmpty() ? nextSeq_ : bySeq_.firstKey();
    const qint64 firstSeq = minSeq - qint64(fresh.size() - skip);
    for (std::size_t i = skip; i < fresh.size(); ++i) {
        HistoryRecord r = fresh[i].record;
        r.fraction = std::min(1.0, std::max(0.0, r.fraction));
        const qint64 seq = firstSeq + qint64(i - skip);
        tx << Statement{QStringLiteral("INSERT OR IGNORE INTO history(%1) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)")
                            .arg(QLatin1String(historyColumns)),
                        QVariantList() << qs(r.video.videoId) << qs(r.video.title) << qs(r.video.channel)
                                       << qs(r.video.channelId) << v64(r.video.durationSeconds) << qs(r.video.thumbnailUrl)
                                       << v64(r.positionMs) << v64(r.durationMs) << r.fraction << (r.watched ? 1 : 0)
                                       << v64(r.watchedAtMs) << r.playCount << v64(seq)};
        HistoryMeta m;
        m.fraction = r.fraction;
        m.watched = r.watched;
        m.seq = seq;
        history_.insert(qs(r.video.videoId), m);
        bySeq_.insert(seq, qs(r.video.videoId));
        setMarker(qs(r.video.videoId), r.playCount);
        ++s.historyAdded;
    }
    // Covers rows another instance added meanwhile.
    tx << Statement{QStringLiteral("DELETE FROM history WHERE video_id IN"
                                   " (SELECT video_id FROM history ORDER BY seq DESC LIMIT -1 OFFSET ?)"),
                    QVariantList() << cap};

    // Resume points never overwrite local ones; a fresh stamp gives them the full 30 days.
    int resumeRoom = limits::resumeCapacity - resume_.size();
    for (const ResumeRecord &r : batch.resume) {
        if (!catalog::validVideoId(r.videoId)) { ++s.skippedRows; continue; }
        const QString id = qs(r.videoId);
        if (!resumeEligibleForImport(r.positionMs, r.durationMs) || resume_.contains(id) || resumeRoom <= 0) continue;
        tx << Statement{QStringLiteral("INSERT OR IGNORE INTO resume(video_id, position_ms, duration_ms, touched_ms) VALUES(?, ?, ?, ?)"),
                        QVariantList() << id << v64(r.positionMs) << v64(r.durationMs) << v64(now)};
        ResumeMeta m;
        m.positionMs = r.positionMs;
        m.touchedMs = now;
        m.order = ++resumeOrder_;
        resume_.insert(id, m);
        --resumeRoom;
        ++s.resumeAdded;
    }

    // Chunked transactions: a playback-thread write waits for one chunk at most.
    bool ok = true;
    for (int i = 0; ok && i < tx.size(); i += importChunkRows) ok = writeBlocking(tx.mid(i, importChunkRows));
    if (!ok) s.skippedSections.push_back("storage"); // partial import; a re-run completes it

    loadIndexes();
    if (s.subscriptionsAdded || s.handlesPending) emit subscriptionsChanged();
    if (s.playlistsAdded || s.remotePlaylistsAdded || !changedPlaylists.isEmpty()) emit playlistsChanged();
    for (const QString &id : changedPlaylists) emit playlistChanged(id);
    if (s.historyAdded || s.historyUpdated || history_.size() != historyBefore.size()) emit historyChanged();
    emitWatchChanges(historyBefore);
    QSet<QString> resumeIds = QSet<QString>::fromList(resumeBefore.keys()) + QSet<QString>::fromList(resume_.keys());
    for (const QString &id : resumeIds) {
        const bool a = resumeBefore.contains(id), b = resume_.contains(id);
        if (a != b || (a && resumeBefore.value(id).positionMs != resume_.value(id).positionMs)) emit resumeChanged(id);
    }
    return s;
}

ImportBatch LibraryStore::snapshot() const {
    ImportBatch b;
    b.source = "sailtube";
    if (!open_) return b;
    b.subscriptions = subscriptions(SubscriptionSort::Name);
    b.pendingHandles = pendingHandles();
    b.history = history(0, std::max(1, history_.size()));
    {
        const qint64 now = libraryNowMs();
        QSqlQuery q(database());
        q.setForwardOnly(true);
        if (runSelect(q, QStringLiteral("SELECT video_id, position_ms, duration_ms, touched_ms FROM resume"
                                        " ORDER BY touched_ms DESC, rowid DESC"))) {
            while (q.next()) {
                ResumeRecord r;
                r.videoId = ss(q.value(0).toString());
                r.positionMs = q.value(1).toLongLong();
                r.durationMs = q.value(2).toLongLong();
                r.touchedMs = q.value(3).toLongLong();
                if (!resumeExpired(r, now)) b.resume.push_back(r);
            }
        }
    }
    // Watch later travels as a playlist titled with its fixed id; applyImport merges
    // such a list into the built-in one instead of skipping it by name.
    const std::vector<PlaylistRecord> lists = playlists();
    for (const PlaylistRecord &p : lists) {
        if (p.kind == PlaylistKind::RemoteMirror) {
            RemotePlaylistRef r;
            r.remoteId = p.remoteId;
            r.title = p.title;
            r.channel = p.channel;
            r.thumbnailUrl = p.thumbnailUrl;
            r.itemCountHint = p.itemCount;
            b.remotePlaylists.push_back(r);
            continue;
        }
        ImportedPlaylist ip;
        ip.title = p.kind == PlaylistKind::WatchLater ? std::string(watchLaterPlaylistId) : p.title;
        ip.items = playlistItems(qs(p.id));
        b.playlists.push_back(ip);
    }
    return b;
}

}}}
