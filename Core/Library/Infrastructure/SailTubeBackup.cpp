// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SailTubeBackup.h"

#include "Core/Catalog/Domain/Video.h"
#include "Core/Library/Domain/LibraryPolicy.h"
#include "ZipReader.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSaveFile>
#include <QSettings>
#include <QStringList>

namespace nordtile { namespace video { namespace library {

namespace {

const char *const formatName = "sailtube-backup";

QJsonValue num(qint64 v) { return QJsonValue(double(v)); }
QString qs(const std::string &s) { return QString::fromStdString(s); }
std::string ss(const QJsonObject &o, const char *key) { return o.value(QLatin1String(key)).toString().toStdString(); }
qint64 ll(const QJsonObject &o, const char *key, qint64 fallback = 0) {
    const QJsonValue v = o.value(QLatin1String(key));
    return v.isDouble() ? qint64(v.toDouble()) : fallback;
}

QJsonObject videoJson(const VideoEntry &v) {
    QJsonObject o;
    o.insert(QStringLiteral("videoId"), qs(v.videoId));
    o.insert(QStringLiteral("title"), qs(v.title));
    o.insert(QStringLiteral("channel"), qs(v.channel));
    o.insert(QStringLiteral("channelId"), qs(v.channelId));
    o.insert(QStringLiteral("durationSeconds"), num(v.durationSeconds));
    o.insert(QStringLiteral("thumbnailUrl"), qs(v.thumbnailUrl));
    return o;
}

bool videoFrom(const QJsonObject &o, VideoEntry *v) {
    v->videoId = ss(o, "videoId");
    v->title = ss(o, "title");
    v->channel = ss(o, "channel");
    v->channelId = ss(o, "channelId");
    v->durationSeconds = ll(o, "durationSeconds", -1);
    v->thumbnailUrl = ss(o, "thumbnailUrl");
    return catalog::validVideoId(v->videoId);
}

QJsonValue settingJson(const QVariant &v) {
    if (v.type() == QVariant::StringList) return QJsonArray::fromStringList(v.toStringList());
    if (v.type() == QVariant::Bool) return QJsonValue(v.toBool());
    if (v.type() == QVariant::Int || v.type() == QVariant::LongLong || v.type() == QVariant::Double
        || v.type() == QVariant::UInt || v.type() == QVariant::ULongLong)
        return QJsonValue(v.toDouble());
    if (v.canConvert<QString>()) return QJsonValue(v.toString());
    return QJsonValue(QJsonValue::Undefined);
}

QVariantMap filtered(const QVariantMap &in) {
    QVariantMap out;
    for (QVariantMap::const_iterator it = in.constBegin(); it != in.constEnd(); ++it)
        if (backupSettingAllowed(it.key()) && backupValueAllowed(it.value())) out.insert(it.key(), it.value());
    return out;
}

bool stringLooksSecret(const QString &s) {
    static const char *const names[] = {"__Secure-", "SAPISID", "APISID", "LOGIN_INFO", "SIDCC",
                                        "VISITOR_INFO1_LIVE", "SID=", "HSID=", "SSID=", "PREF="};
    for (const char *n : names)
        if (s.contains(QLatin1String(n))) return true;
    return false;
}

QVariantMap libraryMetaOnly(const QVariantMap &in) {
    QVariantMap out;
    const QStringList keys = backupLibraryMetaKeys();
    for (QVariantMap::const_iterator it = in.constBegin(); it != in.constEnd(); ++it)
        if (keys.contains(it.key()) && it.value().canConvert<QString>() && backupValueAllowed(it.value()))
            out.insert(it.key(), it.value().toString());
    return out;
}

} // namespace

QStringList backupLibraryMetaKeys() {
    return QStringList() << QStringLiteral("resume_behaviour") << QStringLiteral("hide_watched")
                         << QStringLiteral("hide_watched_search") << QStringLiteral("history_cap")
                         << QStringLiteral("history_paused");
}

bool backupSettingAllowed(const QString &key) {
    static const char *const prefixes[] = {"playback/", "library/", "content/", "downloads/", "ui/"};
    static const char *const banned[] = {"cookie", "token", "secret", "password", "auth", "sapisid"};
    bool prefixed = false;
    for (const char *p : prefixes) prefixed = prefixed || key.startsWith(QLatin1String(p));
    if (!prefixed) return false;
    for (const char *b : banned)
        if (key.contains(QLatin1String(b), Qt::CaseInsensitive)) return false;
    return true;
}

bool backupValueAllowed(const QVariant &value) {
    if (value.type() == QVariant::StringList) {
        for (const QString &s : value.toStringList())
            if (stringLooksSecret(s)) return false;
        return true;
    }
    if (value.type() == QVariant::List) {
        for (const QVariant &v : value.toList())
            if (v.type() != QVariant::String || stringLooksSecret(v.toString())) return false;
        return true;
    }
    if (value.type() == QVariant::Map || value.type() == QVariant::Hash) return false;
    return !stringLooksSecret(value.toString());
}

QVariantMap backupSettingsFromIni(const QString &iniPath) {
    QVariantMap out;
    if (iniPath.isEmpty() || !QFile::exists(iniPath)) return out;
    QSettings ini(iniPath, QSettings::IniFormat);
    for (const QString &key : ini.allKeys()) out.insert(key, ini.value(key));
    return filtered(out);
}

QByteArray sailTubeBackupJson(const ImportBatch &library, const QVariantMap &settings,
                              const QVariantMap &librarySettings, qint64 exportedMs) {
    QJsonObject root;
    root.insert(QStringLiteral("format"), QLatin1String(formatName));
    root.insert(QStringLiteral("version"), sailTubeBackupVersion);
    root.insert(QStringLiteral("exportedMs"), num(exportedMs));

    QJsonArray subs;
    for (const ChannelRecord &c : library.subscriptions) {
        QJsonObject o;
        o.insert(QStringLiteral("channelId"), qs(c.channelId));
        o.insert(QStringLiteral("name"), qs(c.name));
        o.insert(QStringLiteral("avatarUrl"), qs(c.avatarUrl));
        o.insert(QStringLiteral("latestUploadMs"), num(c.latestUploadMs));
        o.insert(QStringLiteral("addedMs"), num(c.addedMs));
        subs.append(o);
    }
    root.insert(QStringLiteral("subscriptions"), subs);

    QJsonArray handles;
    for (const PendingHandle &h : library.pendingHandles) {
        QJsonObject o;
        o.insert(QStringLiteral("url"), qs(h.url));
        o.insert(QStringLiteral("name"), qs(h.name));
        o.insert(QStringLiteral("addedMs"), num(h.addedMs));
        handles.append(o);
    }
    root.insert(QStringLiteral("pendingHandles"), handles);

    QJsonArray history;
    for (const HistoryRecord &r : library.history) {
        QJsonObject o = videoJson(r.video);
        o.insert(QStringLiteral("positionMs"), num(r.positionMs));
        o.insert(QStringLiteral("durationMs"), num(r.durationMs));
        o.insert(QStringLiteral("fraction"), r.fraction);
        o.insert(QStringLiteral("watched"), r.watched);
        o.insert(QStringLiteral("watchedAtMs"), num(r.watchedAtMs));
        o.insert(QStringLiteral("playCount"), r.playCount);
        history.append(o);
    }
    root.insert(QStringLiteral("history"), history);

    QJsonArray resume;
    for (const ResumeRecord &r : library.resume) {
        QJsonObject o;
        o.insert(QStringLiteral("videoId"), qs(r.videoId));
        o.insert(QStringLiteral("positionMs"), num(r.positionMs));
        o.insert(QStringLiteral("durationMs"), num(r.durationMs));
        o.insert(QStringLiteral("touchedMs"), num(r.touchedMs));
        resume.append(o);
    }
    root.insert(QStringLiteral("resume"), resume);

    QJsonArray lists;
    for (const ImportedPlaylist &p : library.playlists) {
        QJsonObject o;
        o.insert(QStringLiteral("title"), qs(p.title));
        QJsonArray items;
        for (const VideoEntry &v : p.items) items.append(videoJson(v));
        o.insert(QStringLiteral("items"), items);
        lists.append(o);
    }
    root.insert(QStringLiteral("playlists"), lists);

    QJsonArray remotes;
    for (const RemotePlaylistRef &r : library.remotePlaylists) {
        QJsonObject o;
        o.insert(QStringLiteral("remoteId"), qs(r.remoteId));
        o.insert(QStringLiteral("title"), qs(r.title));
        o.insert(QStringLiteral("channel"), qs(r.channel));
        o.insert(QStringLiteral("thumbnailUrl"), qs(r.thumbnailUrl));
        o.insert(QStringLiteral("itemCountHint"), r.itemCountHint);
        remotes.append(o);
    }
    root.insert(QStringLiteral("remotePlaylists"), remotes);

    QJsonObject settingsJson;
    const QVariantMap allowed = filtered(settings);
    for (QVariantMap::const_iterator it = allowed.constBegin(); it != allowed.constEnd(); ++it) {
        const QJsonValue v = settingJson(it.value());
        if (!v.isUndefined()) settingsJson.insert(it.key(), v);
    }
    root.insert(QStringLiteral("settings"), settingsJson);

    QJsonObject meta;
    const QVariantMap metaValues = libraryMetaOnly(librarySettings);
    for (QVariantMap::const_iterator it = metaValues.constBegin(); it != metaValues.constEnd(); ++it)
        meta.insert(it.key(), it.value().toString());
    root.insert(QStringLiteral("librarySettings"), meta);
    return QJsonDocument(root).toJson(QJsonDocument::Indented);
}

bool writeSailTubeBackup(const QString &targetPath, const ImportBatch &library, const QVariantMap &settings,
                         const QVariantMap &librarySettings, qint64 exportedMs, QString *error) {
    const QString path = importLocalPath(targetPath);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    // QSaveFile creates its temp file 0666 & ~umask; tighten it before any data lands.
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner)) {
        if (error) *error = QStringLiteral("cannot make the backup file private");
        file.cancelWriting();
        return false;
    }
    const QByteArray json = sailTubeBackupJson(library, settings, librarySettings, exportedMs);
    if (file.write(json) != json.size() || !file.commit()) {
        if (error) *error = file.errorString();
        file.cancelWriting();
        return false;
    }
    return true;
}

BackupRestoreResult parseSailTubeBackup(const QByteArray &json) {
    BackupRestoreResult result;
    result.batch.source = "sailtube";
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.error = QStringLiteral("the file is not a SailTube backup");
        return result;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("format")).toString() != QLatin1String(formatName)) {
        result.error = QStringLiteral("the file is not a SailTube backup");
        return result;
    }
    const QJsonValue version = root.value(QStringLiteral("version"));
    if (!version.isDouble() || version.toInt() < 1) {
        result.error = QStringLiteral("the backup has no valid version");
        return result;
    }
    if (version.toInt() > sailTubeBackupVersion) {
        result.error = QStringLiteral("the backup was made by a newer SailTube");
        return result;
    }
    result.exportedMs = ll(root, "exportedMs");
    ImportBatch &b = result.batch;

    for (const QJsonValue &v : root.value(QStringLiteral("subscriptions")).toArray()) {
        const QJsonObject o = v.toObject();
        ChannelRecord c;
        c.channelId = ss(o, "channelId");
        c.name = ss(o, "name");
        c.avatarUrl = ss(o, "avatarUrl");
        c.latestUploadMs = ll(o, "latestUploadMs");
        c.addedMs = ll(o, "addedMs");
        if (catalog::validChannelId(c.channelId)) b.subscriptions.push_back(c);
        else ++b.skippedRows;
    }
    for (const QJsonValue &v : root.value(QStringLiteral("pendingHandles")).toArray()) {
        const QJsonObject o = v.toObject();
        PendingHandle h;
        h.url = ss(o, "url");
        h.name = ss(o, "name");
        h.addedMs = ll(o, "addedMs");
        if (isHandleUrl(h.url)) b.pendingHandles.push_back(h);
        else ++b.skippedRows;
    }
    for (const QJsonValue &v : root.value(QStringLiteral("history")).toArray()) {
        const QJsonObject o = v.toObject();
        HistoryRecord r;
        if (!videoFrom(o, &r.video)) { ++b.skippedRows; continue; }
        r.positionMs = ll(o, "positionMs");
        r.durationMs = ll(o, "durationMs");
        r.fraction = qBound(0.0, o.value(QStringLiteral("fraction")).toDouble(), 1.0);
        r.watched = o.value(QStringLiteral("watched")).toBool();
        r.watchedAtMs = ll(o, "watchedAtMs");
        r.playCount = qMax(1, o.value(QStringLiteral("playCount")).toInt(1));
        b.history.push_back(r);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("resume")).toArray()) {
        const QJsonObject o = v.toObject();
        ResumeRecord r;
        r.videoId = ss(o, "videoId");
        r.positionMs = ll(o, "positionMs");
        r.durationMs = ll(o, "durationMs");
        r.touchedMs = ll(o, "touchedMs");
        if (catalog::validVideoId(r.videoId) && r.positionMs > 0) b.resume.push_back(r);
        else ++b.skippedRows;
    }
    for (const QJsonValue &v : root.value(QStringLiteral("playlists")).toArray()) {
        const QJsonObject o = v.toObject();
        ImportedPlaylist p;
        p.title = ss(o, "title");
        for (const QJsonValue &item : o.value(QStringLiteral("items")).toArray()) {
            VideoEntry e;
            if (videoFrom(item.toObject(), &e)) p.items.push_back(e);
            else ++b.skippedRows;
        }
        b.playlists.push_back(p);
    }
    for (const QJsonValue &v : root.value(QStringLiteral("remotePlaylists")).toArray()) {
        const QJsonObject o = v.toObject();
        RemotePlaylistRef r;
        r.remoteId = ss(o, "remoteId");
        r.title = ss(o, "title");
        r.channel = ss(o, "channel");
        r.thumbnailUrl = ss(o, "thumbnailUrl");
        r.itemCountHint = o.value(QStringLiteral("itemCountHint")).toInt(-1);
        if (!r.remoteId.empty() && playlistIdFromUrl(r.remoteId) == r.remoteId) b.remotePlaylists.push_back(r);
        else ++b.skippedRows;
    }
    QVariantMap settings = filtered(root.value(QStringLiteral("settings")).toObject().toVariantMap());
    for (QVariantMap::iterator it = settings.begin(); it != settings.end(); ++it)
        if (it.value().type() == QVariant::List) it.value() = it.value().toStringList(); // as QSettings reads them
    result.settings = settings;
    result.librarySettings = libraryMetaOnly(root.value(QStringLiteral("librarySettings")).toObject().toVariantMap());
    return result;
}

BackupRestoreResult readSailTubeBackup(const QString &input) {
    BackupRestoreResult result;
    result.batch.source = "sailtube";
    QFile f(importLocalPath(input));
    if (!f.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("the backup file cannot be read");
        return result;
    }
    if (f.size() > maxBackupBytes) {
        result.error = QStringLiteral("the backup file is too large");
        return result;
    }
    return parseSailTubeBackup(f.read(maxBackupBytes));
}

}}}
