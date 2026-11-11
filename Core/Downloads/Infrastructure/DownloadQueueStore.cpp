// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DownloadQueueStore.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

namespace nordtile { namespace video { namespace downloads {

namespace {

const char columns[] =
    "video_id, kind, quality_height, state, title, channel, channel_id, duration_s, thumbnail_url,"
    " directory, file_name, video_format_id, audio_format_id, video_container, audio_container,"
    " video_codec, audio_codec, video_bytes_total, video_bytes_done, audio_bytes_total, audio_bytes_done,"
    " output_layout, error, notice, extras, created_ms, updated_ms, sort_key";
const int columnCount = 28;

QString q(const std::string &s) { return QString::fromStdString(s); }
std::string s(const QVariant &v) { return v.toString().toStdString(); }

QVariantList values(const DownloadRecord &r) {
    QVariantList v;
    v << q(r.videoId) << static_cast<int>(r.kind) << r.qualityHeight << static_cast<int>(r.state)
      << q(r.title) << q(r.channel) << q(r.channelId) << static_cast<qlonglong>(r.durationSeconds) << q(r.thumbnailUrl)
      << q(r.directory) << q(r.fileName) << q(r.videoFormatId) << q(r.audioFormatId)
      << q(r.videoContainer) << q(r.audioContainer) << q(r.videoCodec) << q(r.audioCodec)
      << static_cast<qlonglong>(r.videoBytesTotal) << static_cast<qlonglong>(r.videoBytesDone)
      << static_cast<qlonglong>(r.audioBytesTotal) << static_cast<qlonglong>(r.audioBytesDone)
      << static_cast<int>(r.layout) << q(r.error) << q(r.notice) << q(DownloadQueueStore::writeExtras(r))
      << static_cast<qlonglong>(r.createdMs) << static_cast<qlonglong>(r.updatedMs) << static_cast<qlonglong>(r.sortKey);
    return v;
}

State stateFrom(int v) { return v >= 0 && v <= static_cast<int>(State::Cancelled) ? static_cast<State>(v) : State::Failed; }
OutputLayout layoutFrom(int v) {
    return v >= 0 && v <= static_cast<int>(OutputLayout::TrackPair) ? static_cast<OutputLayout>(v) : OutputLayout::SingleFile;
}

} // namespace

DownloadQueueStore::DownloadQueueStore(const QString &databasePath, const QString &connectionName)
    : path_(databasePath), connection_(connectionName) {}

DownloadQueueStore::~DownloadQueueStore() {
    if (QSqlDatabase::contains(connection_)) {
        { QSqlDatabase db = QSqlDatabase::database(connection_, false); if (db.isOpen()) db.close(); }
        QSqlDatabase::removeDatabase(connection_);
    }
}

bool DownloadQueueStore::open(QString *error) {
    if (open_) return true;
    QSqlDatabase db = QSqlDatabase::contains(connection_) ? QSqlDatabase::database(connection_, false)
                                                          : QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection_);
    db.setDatabaseName(path_);
    if (!db.open()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    QSqlQuery query(db);
    // Background lane: wait for the library's writer rather than fail.
    query.exec(QStringLiteral("PRAGMA busy_timeout=1000"));
    query.exec(QStringLiteral("PRAGMA synchronous=NORMAL"));
    int version = 0;
    if (query.exec(QStringLiteral("PRAGMA user_version")) && query.next()) version = query.value(0).toInt();
    bool table = false;
    if (query.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type='table' AND name='downloads'")) && query.next())
        table = true;
    if (version < 1 || !table) {
        if (error) *error = QStringLiteral("library schema missing (user_version %1)").arg(version);
        db.close();
        return false;
    }
    open_ = true;
    return true;
}

bool DownloadQueueStore::exec(const QString &sql, const QVariantList &bind, QString *error) const {
    if (!open_) return false;
    QSqlQuery query(QSqlDatabase::database(connection_, false));
    if (!query.prepare(sql)) { if (error) *error = query.lastError().text(); return false; }
    for (const QVariant &v : bind) query.addBindValue(v);
    if (!query.exec()) { if (error) *error = query.lastError().text(); return false; }
    return true;
}

std::vector<DownloadRecord> DownloadQueueStore::loadAll() const {
    std::vector<DownloadRecord> out;
    if (!open_) return out;
    QSqlQuery query(QSqlDatabase::database(connection_, false));
    if (!query.exec(QStringLiteral("SELECT id, %1 FROM downloads ORDER BY sort_key, id").arg(QLatin1String(columns))))
        return out;
    while (query.next()) {
        DownloadRecord r;
        int i = 0;
        r.id = query.value(i++).toLongLong();
        r.videoId = s(query.value(i++));
        r.kind = query.value(i++).toInt() == 1 ? Kind::Video : Kind::Audio;
        r.qualityHeight = query.value(i++).toInt();
        r.state = stateFrom(query.value(i++).toInt());
        r.title = s(query.value(i++));
        r.channel = s(query.value(i++));
        r.channelId = s(query.value(i++));
        r.durationSeconds = query.value(i++).toLongLong();
        r.thumbnailUrl = s(query.value(i++));
        r.directory = s(query.value(i++));
        r.fileName = s(query.value(i++));
        r.videoFormatId = s(query.value(i++));
        r.audioFormatId = s(query.value(i++));
        r.videoContainer = s(query.value(i++));
        r.audioContainer = s(query.value(i++));
        r.videoCodec = s(query.value(i++));
        r.audioCodec = s(query.value(i++));
        r.videoBytesTotal = query.value(i++).toLongLong();
        r.videoBytesDone = query.value(i++).toLongLong();
        r.audioBytesTotal = query.value(i++).toLongLong();
        r.audioBytesDone = query.value(i++).toLongLong();
        r.layout = layoutFrom(query.value(i++).toInt());
        r.error = s(query.value(i++));
        r.notice = s(query.value(i++));
        r.extras = s(query.value(i++));
        r.createdMs = query.value(i++).toLongLong();
        r.updatedMs = query.value(i++).toLongLong();
        r.sortKey = query.value(i++).toLongLong();
        readExtras(&r);
        out.push_back(r);
    }
    return out;
}

bool DownloadQueueStore::insert(DownloadRecord *r) {
    if (!open_ || !r) return false;
    QSqlQuery query(QSqlDatabase::database(connection_, false));
    QStringList marks;
    for (int i = 0; i < columnCount; ++i) marks << QStringLiteral("?");
    if (!query.prepare(QStringLiteral("INSERT INTO downloads(%1) VALUES(%2)").arg(QLatin1String(columns), marks.join(QLatin1Char(',')))))
        return false;
    for (const QVariant &v : values(*r)) query.addBindValue(v);
    if (!query.exec()) return false;
    r->id = query.lastInsertId().toLongLong();
    r->extras = writeExtras(*r);
    return r->id > 0;
}

bool DownloadQueueStore::update(const DownloadRecord &r) {
    const QStringList names = QString::fromLatin1(columns).split(QLatin1Char(','));
    QStringList sets;
    for (const QString &n : names) sets << n.trimmed() + QStringLiteral("=?");
    QVariantList bind = values(r);
    bind << static_cast<qlonglong>(r.id);
    return exec(QStringLiteral("UPDATE downloads SET %1 WHERE id=?").arg(sets.join(QLatin1Char(','))), bind);
}

bool DownloadQueueStore::updateProgress(qint64 id, qint64 videoBytesDone, qint64 audioBytesDone, qint64 nowMs) {
    return exec(QStringLiteral("UPDATE downloads SET video_bytes_done=?, audio_bytes_done=?, updated_ms=? WHERE id=?"),
                QVariantList() << videoBytesDone << audioBytesDone << nowMs << id);
}

bool DownloadQueueStore::remove(qint64 id) {
    return exec(QStringLiteral("DELETE FROM downloads WHERE id=?"), QVariantList() << id);
}

qint64 DownloadQueueStore::nextSortKey() const {
    if (!open_) return 0;
    QSqlQuery query(QSqlDatabase::database(connection_, false));
    if (query.exec(QStringLiteral("SELECT COALESCE(MAX(sort_key), 0) + 1 FROM downloads")) && query.next())
        return query.value(0).toLongLong();
    return 0;
}

QString DownloadQueueStore::metaValue(const QString &key, const QString &fallback) const {
    if (!open_) return fallback;
    QSqlQuery query(QSqlDatabase::database(connection_, false));
    query.prepare(QStringLiteral("SELECT value FROM meta WHERE key=?"));
    query.addBindValue(key);
    if (query.exec() && query.next()) return query.value(0).toString();
    return fallback;
}

bool DownloadQueueStore::setMetaValue(const QString &key, const QString &value) {
    return exec(QStringLiteral("INSERT OR REPLACE INTO meta(key, value) VALUES(?, ?)"), QVariantList() << key << value);
}

void DownloadQueueStore::readExtras(DownloadRecord *r) {
    const QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r->extras)).object();
    r->baseName = o.value(QStringLiteral("base")).toString().toStdString();
    r->thumbnailFile = o.value(QStringLiteral("thumb")).toString().toStdString();
    r->restarts = o.value(QStringLiteral("restarts")).toInt();
    r->muxExtension = o.value(QStringLiteral("muxExt")).toString().toStdString();
}

std::string DownloadQueueStore::writeExtras(const DownloadRecord &r) {
    QJsonObject o = QJsonDocument::fromJson(QByteArray::fromStdString(r.extras)).object();
    o.insert(QStringLiteral("base"), q(r.baseName));
    o.insert(QStringLiteral("thumb"), q(r.thumbnailFile));
    o.insert(QStringLiteral("restarts"), r.restarts);
    o.insert(QStringLiteral("muxExt"), q(r.muxExtension));
    return QJsonDocument(o).toJson(QJsonDocument::Compact).toStdString();
}

}}}
