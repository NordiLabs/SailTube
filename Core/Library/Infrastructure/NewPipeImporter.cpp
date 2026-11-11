// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "NewPipeImporter.h"

#include "Core/Library/Domain/LibraryPolicy.h"
#include "ZipReader.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVariant>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <initializer_list>
#include <map>
#include <set>
#include <unistd.h>

namespace nordtile { namespace video { namespace library {

namespace {

NewPipeImportResult failure(NewPipeImportError error, const QString &message) {
    NewPipeImportResult r;
    r.batch.source = "newpipe";
    r.error = error;
    r.errorString = message;
    return r;
}

std::string str(const QVariant &v) { return v.isNull() ? std::string() : v.toString().toStdString(); }

bool isYouTube(const QVariant &serviceId) {
    return serviceId.isNull() || serviceId.toString().trimmed().isEmpty() || serviceId.toLongLong() == 0;
}

QSet<QString> columnsOf(QSqlDatabase &db, const QString &table) {
    QSet<QString> names;
    QSqlQuery q(db);
    if (q.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(table)))
        while (q.next()) names.insert(q.value(1).toString().toLower());
    return names;
}

// Column if present, else NULL, so optional columns keep the query shape.
QString optional(const QSet<QString> &cols, const char *name) {
    return cols.contains(QLatin1String(name)) ? QString::fromLatin1(name) : QStringLiteral("NULL");
}

bool hasAll(const QSet<QString> &cols, std::initializer_list<const char *> names) {
    for (const char *n : names)
        if (!cols.contains(QLatin1String(n))) return false;
    return true;
}

class Parser {
public:
    Parser(QSqlDatabase &db, qint64 nowMs, ImportBatch &batch) : db_(db), now_(nowMs), batch_(batch) {}

    void run() {
        subscriptions();
        const bool haveStreams = streams();
        if (haveStreams) {
            history();
            state();
            buildHistoryAndResume();
            playlists();
        } else {
            for (const char *s : {"stream_history", "stream_state", "playlists", "playlist_stream_join"})
                skip(s);
        }
        remotePlaylists();
    }

private:
    void skip(const char *section) { batch_.skippedSections.push_back(section); }

    bool select(QSqlQuery &q, const QString &sql) {
        q.setForwardOnly(true);
        return q.exec(sql);
    }

    void subscriptions() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("subscriptions"));
        if (!c.contains(QStringLiteral("url"))) { skip("subscriptions"); return; }
        QSqlQuery q(db_);
        if (!select(q, QStringLiteral("SELECT %1, url, %2, %3 FROM subscriptions ORDER BY rowid")
                           .arg(optional(c, "service_id"), optional(c, "name"), optional(c, "avatar_url")))) {
            skip("subscriptions");
            return;
        }
        std::vector<ChannelRecord> channels;
        std::vector<PendingHandle> handles;
        std::set<std::string> seen;
        int skipped = 0;
        while (q.next()) {
            const std::string url = str(q.value(1));
            if (!isYouTube(q.value(0))) { ++skipped; continue; }
            const std::string id = channelIdFromUrl(url);
            if (!id.empty()) {
                if (!seen.insert(id).second) continue;
                ChannelRecord ch;
                ch.channelId = id;
                ch.name = str(q.value(2));
                ch.avatarUrl = str(q.value(3));
                ch.addedMs = now_;
                channels.push_back(ch);
            } else if (isHandleUrl(url)) {
                if (!seen.insert(url).second) continue;
                PendingHandle h;
                h.url = url;
                h.name = str(q.value(2));
                h.addedMs = now_;
                handles.push_back(h);
            } else {
                ++skipped;
            }
        }
        batch_.subscriptions = channels;
        batch_.pendingHandles = handles;
        batch_.skippedRows += skipped;
    }

    bool streams() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("streams"));
        if (!hasAll(c, {"uid", "url"})) { skip("streams"); return false; }
        QSqlQuery q(db_);
        if (!select(q, QStringLiteral("SELECT uid, %1, url, %2, %3, %4, %5, %6 FROM streams ORDER BY rowid")
                           .arg(optional(c, "service_id"), optional(c, "title"), optional(c, "duration"),
                                optional(c, "uploader"), optional(c, "uploader_url"),
                                optional(c, "thumbnail_url")))) {
            skip("streams");
            return false;
        }
        int skipped = 0;
        while (q.next()) {
            const qint64 uid = q.value(0).toLongLong();
            known_.insert(uid);
            if (!isYouTube(q.value(1))) { ++skipped; continue; }
            const std::string videoId = videoIdFromUrl(str(q.value(2)));
            if (videoId.empty()) { ++skipped; continue; }
            streamVideo_[uid] = videoId;
            if (videos_.count(videoId)) continue; // first row of a video describes it
            VideoEntry v;
            v.videoId = videoId;
            v.title = str(q.value(3));
            const qint64 seconds = q.value(4).isNull() ? -1 : q.value(4).toLongLong();
            v.durationSeconds = seconds > 0 ? seconds : -1;
            v.channel = str(q.value(5));
            v.channelId = channelIdFromUrl(str(q.value(6)));
            v.thumbnailUrl = str(q.value(7));
            videos_[videoId] = v;
        }
        batch_.skippedRows += skipped;
        return true;
    }

    // Resolves a stream uid: true with the video id for YouTube rows; counts
    // rows pointing at no stream at all as malformed.
    bool videoFor(qint64 uid, std::string *videoId, int *skipped) {
        std::map<qint64, std::string>::const_iterator it = streamVideo_.find(uid);
        if (it != streamVideo_.end()) { *videoId = it->second; return true; }
        if (!known_.count(uid)) ++*skipped;
        return false;
    }

    void history() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("stream_history"));
        if (!hasAll(c, {"stream_id", "access_date"})) { skip("stream_history"); return; }
        QSqlQuery q(db_);
        if (!select(q, QStringLiteral("SELECT stream_id, access_date, %1 FROM stream_history ORDER BY rowid")
                           .arg(optional(c, "repeat_count")))) {
            skip("stream_history");
            return;
        }
        std::map<std::string, Visits> visits;
        int skipped = 0;
        while (q.next()) {
            std::string id;
            if (!videoFor(q.value(0).toLongLong(), &id, &skipped)) continue;
            Visits &v = visits[id];
            v.latestMs = std::max(v.latestMs, q.value(1).toLongLong());
            v.repeats += q.value(2).isNull() ? 1 : q.value(2).toLongLong();
        }
        visits_ = visits;
        batch_.skippedRows += skipped;
    }

    void state() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("stream_state"));
        if (!hasAll(c, {"stream_id", "progress_time"})) { skip("stream_state"); return; }
        QSqlQuery q(db_);
        if (!select(q, QStringLiteral("SELECT stream_id, progress_time FROM stream_state ORDER BY rowid"))) {
            skip("stream_state");
            return;
        }
        std::map<std::string, qint64> progress;
        int skipped = 0;
        while (q.next()) {
            std::string id;
            if (!videoFor(q.value(0).toLongLong(), &id, &skipped)) continue;
            const qint64 ms = std::max<qint64>(0, q.value(1).toLongLong());
            std::map<std::string, qint64>::iterator it = progress.find(id);
            if (it == progress.end()) progress[id] = ms;
            else it->second = std::max(it->second, ms);
        }
        progress_ = progress;
        haveState_ = true;
        batch_.skippedRows += skipped;
    }

    void buildHistoryAndResume() {
        for (std::map<std::string, Visits>::const_iterator it = visits_.begin(); it != visits_.end(); ++it) {
            HistoryRecord r;
            r.video = videos_[it->first];
            r.durationMs = r.video.durationSeconds > 0 ? r.video.durationSeconds * 1000 : 0;
            std::map<std::string, qint64>::const_iterator s = progress_.find(it->first);
            const bool hasState = s != progress_.end();
            if (haveState_ && !hasState) {
                r.watched = true;
                r.fraction = 1;
                r.positionMs = r.durationMs;
            } else {
                r.positionMs = hasState ? s->second : 0;
                r.watched = reachesWatched(r.positionMs, r.durationMs);
                r.fraction = r.durationMs > 0
                                 ? std::min(1.0, double(r.positionMs) / double(r.durationMs)) : 0;
            }
            r.watchedAtMs = it->second.latestMs;
            r.playCount = int(std::max<qint64>(1, it->second.repeats));
            batch_.history.push_back(r);
        }
        std::sort(batch_.history.begin(), batch_.history.end(), [](const HistoryRecord &a, const HistoryRecord &b) {
            if (a.watchedAtMs != b.watchedAtMs) return a.watchedAtMs > b.watchedAtMs;
            return a.video.videoId < b.video.videoId;
        });
        for (std::map<std::string, qint64>::const_iterator it = progress_.begin(); it != progress_.end(); ++it) {
            const VideoEntry &v = videos_[it->first];
            const qint64 durationMs = v.durationSeconds > 0 ? v.durationSeconds * 1000 : 0;
            if (!resumeEligibleForImport(it->second, durationMs)) continue;
            ResumeRecord r;
            r.videoId = it->first;
            r.positionMs = it->second;
            r.durationMs = durationMs;
            r.touchedMs = now_;
            batch_.resume.push_back(r);
        }
    }

    void playlists() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("playlists"));
        const QSet<QString> j = columnsOf(db_, QStringLiteral("playlist_stream_join"));
        if (!hasAll(c, {"uid", "name"})) { skip("playlists"); return; }
        if (!hasAll(j, {"playlist_id", "stream_id", "join_index"})) {
            // Without the join the contents are unknown; an empty copy would block a later re-import.
            skip("playlists");
            skip("playlist_stream_join");
            return;
        }
        QSqlQuery q(db_);
        const QString order = c.contains(QStringLiteral("display_index")) ? QStringLiteral("display_index, rowid")
                                                                          : QStringLiteral("rowid");
        if (!select(q, QStringLiteral("SELECT uid, name FROM playlists ORDER BY %1").arg(order))) {
            skip("playlists");
            return;
        }
        std::vector<qint64> ids;
        std::vector<ImportedPlaylist> lists;
        while (q.next()) {
            ids.push_back(q.value(0).toLongLong());
            ImportedPlaylist p;
            p.title = str(q.value(1));
            lists.push_back(p);
        }
        QSqlQuery items(db_);
        if (!select(items, QStringLiteral("SELECT playlist_id, stream_id FROM playlist_stream_join"
                                          " ORDER BY playlist_id, join_index, rowid"))) {
            skip("playlists");
            skip("playlist_stream_join");
            return;
        }
        std::map<qint64, size_t> index;
        for (size_t i = 0; i < ids.size(); ++i) index[ids[i]] = i;
        std::vector<std::set<std::string> > seen(lists.size());
        int skipped = 0;
        while (items.next()) {
            std::map<qint64, size_t>::const_iterator p = index.find(items.value(0).toLongLong());
            if (p == index.end()) { ++skipped; continue; }
            std::string id;
            if (!videoFor(items.value(1).toLongLong(), &id, &skipped)) continue;
            if (!seen[p->second].insert(id).second) continue;
            lists[p->second].items.push_back(videos_[id]);
        }
        batch_.playlists = lists;
        batch_.skippedRows += skipped;
    }

    void remotePlaylists() {
        const QSet<QString> c = columnsOf(db_, QStringLiteral("remote_playlists"));
        if (!c.contains(QStringLiteral("url"))) { skip("remote_playlists"); return; }
        QSqlQuery q(db_);
        const QString order = c.contains(QStringLiteral("display_index")) ? QStringLiteral("display_index, rowid")
                                                                          : QStringLiteral("rowid");
        if (!select(q, QStringLiteral("SELECT %1, url, %2, %3, %4, %5 FROM remote_playlists ORDER BY %6")
                           .arg(optional(c, "service_id"), optional(c, "name"), optional(c, "uploader"),
                                optional(c, "thumbnail_url"), optional(c, "stream_count"), order))) {
            skip("remote_playlists");
            return;
        }
        std::vector<RemotePlaylistRef> refs;
        std::set<std::string> seen;
        int skipped = 0;
        while (q.next()) {
            if (!isYouTube(q.value(0))) { ++skipped; continue; }
            const std::string id = playlistIdFromUrl(str(q.value(1)));
            if (id.empty()) { ++skipped; continue; }
            if (!seen.insert(id).second) continue;
            RemotePlaylistRef r;
            r.remoteId = id;
            r.title = str(q.value(2));
            r.channel = str(q.value(3));
            r.thumbnailUrl = str(q.value(4));
            r.itemCountHint = q.value(5).isNull() ? -1 : q.value(5).toInt();
            refs.push_back(r);
        }
        batch_.remotePlaylists = refs;
        batch_.skippedRows += skipped;
    }

    struct Visits { qint64 latestMs = 0; qint64 repeats = 0; };

    QSqlDatabase &db_;
    qint64 now_;
    ImportBatch &batch_;
    std::set<qint64> known_;                       // every stream uid, YouTube or not
    std::map<qint64, std::string> streamVideo_;    // YouTube stream uid -> video id
    std::map<std::string, VideoEntry> videos_;
    std::map<std::string, Visits> visits_;
    std::map<std::string, qint64> progress_;
    bool haveState_ = false;
};

bool copyPrivate(const QString &from, const QString &to) {
    QFile in(from);
    if (!in.open(QIODevice::ReadOnly)) return false;
    const QByteArray target = QFile::encodeName(to);
    ::unlink(target.constData());
    const int fd = ::open(target.constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    QByteArray buffer(64 * 1024, Qt::Uninitialized);
    bool ok = true;
    for (;;) {
        const qint64 n = in.read(buffer.data(), buffer.size());
        if (n < 0) { ok = false; break; }
        if (n == 0) break;
        const char *p = buffer.constData();
        qint64 left = n;
        while (left > 0) {
            const ssize_t w = ::write(fd, p, size_t(left));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { ok = false; break; }
            p += w;
            left -= w;
        }
        if (!ok) break;
    }
    if (::close(fd) != 0) ok = false;
    if (!ok) ::unlink(target.constData());
    return ok;
}

// Deletes the private copy and anything SQLite or the extractor left beside it.
struct TempFileGuard {
    explicit TempFileGuard(const QString &p) : path(p) {}
    ~TempFileGuard() {
        const char *suffixes[] = {"", "-journal", "-wal", "-shm", ".part-import"};
        for (const char *s : suffixes) QFile::remove(path + QLatin1String(s));
    }
    QString path;
};

} // namespace

NewPipeImportResult parseNewPipeDatabase(const QString &databasePath, qint64 nowMs) {
    if (nowMs <= 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE")))
        return failure(NewPipeImportError::DriverMissing, QStringLiteral("the SQLite driver is not available"));
    NewPipeImportResult result;
    result.batch.source = "newpipe";
    const QString connection = QStringLiteral("sailtube-newpipe-") + QUuid::createUuid().toString();
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(databasePath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        QStringList tables;
        bool readable = db.open();
        if (readable) {
            QSqlQuery q(db);
            readable = q.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type = 'table'"));
            while (readable && q.next()) tables << q.value(0).toString().toLower();
        }
        if (!readable) {
            result = failure(NewPipeImportError::DatabaseOpenFailed,
                             QStringLiteral("the backup database cannot be read"));
        } else {
            bool any = false;
            for (const char *t : {"subscriptions", "streams", "stream_history", "stream_state", "playlists",
                                  "playlist_stream_join", "remote_playlists"})
                any = any || tables.contains(QLatin1String(t));
            if (!any) {
                result = failure(NewPipeImportError::NotNewPipeDatabase,
                                 QStringLiteral("the database is not a NewPipe backup"));
            } else {
                Parser(db, nowMs, result.batch).run();
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

NewPipeImportResult importNewPipeBackup(const QString &input, const QString &privateDir, qint64 nowMs) {
    if (privateDir.isEmpty() || !QDir().mkpath(privateDir))
        return failure(NewPipeImportError::NoPrivateDirectory, QStringLiteral("no private directory for the import"));
    QFile::setPermissions(privateDir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const QString path = importLocalPath(input);
    const QFileInfo info(path);
    if (path.isEmpty() || !info.isFile())
        return failure(NewPipeImportError::FileNotFound, QStringLiteral("the backup file does not exist"));

    QByteArray magic;
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return failure(NewPipeImportError::FileNotFound, QStringLiteral("the backup file cannot be opened"));
        magic = f.read(16);
    }
    const bool isSqlite = magic == QByteArray("SQLite format 3\0", 16);
    const bool isZip = magic.startsWith("PK\x03\x04") || magic.startsWith("PK\x05\x06");
    if (!isSqlite && !isZip)
        return failure(NewPipeImportError::UnrecognizedFormat,
                       QStringLiteral("not a NewPipe backup (.zip) or database (.db)"));

    const QString temp = QDir(privateDir).filePath(
        QStringLiteral("newpipe-import-") + QUuid::createUuid().toString().mid(1, 36) + QStringLiteral(".tmp"));
    TempFileGuard guard(temp);
    if (isZip) {
        ZipReader zip(path);
        if (!zip.open()) return failure(NewPipeImportError::ZipError, zip.errorString());
        const QList<ZipEntry> entries = zip.entries();
        int pick = zip.indexOf(QStringLiteral("newpipe.db"));
        for (int i = 0; pick < 0 && i < entries.size(); ++i) {
            const ZipEntry &e = entries.at(i);
            if (!e.isDirectory() && e.name.endsWith(QLatin1String(".db"), Qt::CaseInsensitive)) pick = i;
        }
        if (pick < 0)
            return failure(NewPipeImportError::NoDatabaseInZip, QStringLiteral("the archive holds no database"));
        if (!zip.extract(pick, temp)) {
            return failure(zip.error() == ZipReader::Error::WriteFailed ? NewPipeImportError::CopyFailed
                                                                        : NewPipeImportError::ZipError,
                           zip.errorString());
        }
    } else {
        if (info.size() > ZipReader::maxEntryBytes)
            return failure(NewPipeImportError::TooLarge, QStringLiteral("the database is too large"));
        if (!copyPrivate(path, temp))
            return failure(NewPipeImportError::CopyFailed, QStringLiteral("cannot copy the database"));
    }
    return parseNewPipeDatabase(temp, nowMs);
}

}}}
