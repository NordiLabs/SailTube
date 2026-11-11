// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FeedCacheStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

namespace nordtile { namespace video { namespace catalog {

namespace {

QString qs(const std::string &s) { return QString::fromStdString(s); }
std::string ss(const QJsonValue &v) { return v.toString().toStdString(); }
std::int64_t num(const QJsonValue &v, std::int64_t fallback = -1) {
    return v.isDouble() ? static_cast<std::int64_t>(v.toDouble()) : fallback;
}
int clampEnum(const QJsonValue &v, int max, int fallback) {
    const int n = v.isDouble() ? v.toInt() : fallback;
    return n < 0 || n > max ? fallback : n;
}

QJsonObject itemToJson(const Item &it) {
    QJsonObject o;
    o.insert(QStringLiteral("k"), static_cast<int>(it.kind));
    o.insert(QStringLiteral("id"), qs(it.id));
    o.insert(QStringLiteral("t"), qs(it.title));
    if (!it.channel.empty()) o.insert(QStringLiteral("c"), qs(it.channel));
    if (!it.channelId.empty()) o.insert(QStringLiteral("ci"), qs(it.channelId));
    if (it.durationSeconds >= 0) o.insert(QStringLiteral("d"), static_cast<double>(it.durationSeconds));
    if (!it.durationText.empty()) o.insert(QStringLiteral("dt"), qs(it.durationText));
    if (it.viewCount >= 0) o.insert(QStringLiteral("v"), static_cast<double>(it.viewCount));
    if (!it.viewText.empty()) o.insert(QStringLiteral("vt"), qs(it.viewText));
    if (it.publishedEpoch >= 0) o.insert(QStringLiteral("pe"), static_cast<double>(it.publishedEpoch));
    if (it.publishedAgeSeconds >= 0) o.insert(QStringLiteral("pa"), static_cast<double>(it.publishedAgeSeconds));
    if (!it.publishedText.empty()) o.insert(QStringLiteral("pt"), qs(it.publishedText));
    if (!it.thumbnailUrl.empty()) o.insert(QStringLiteral("th"), qs(it.thumbnailUrl));
    if (!it.avatarUrl.empty()) o.insert(QStringLiteral("av"), qs(it.avatarUrl));
    o.insert(QStringLiteral("ls"), static_cast<int>(it.liveStatus));
    if (it.membersOnly) o.insert(QStringLiteral("mo"), true);
    if (it.restricted) o.insert(QStringLiteral("r"), true);
    if (it.shortEvidence != ShortEvidence::None) o.insert(QStringLiteral("se"), static_cast<int>(it.shortEvidence));
    return o;
}

Item itemFromJson(const QJsonObject &o) {
    Item it;
    it.kind = static_cast<ItemKind>(clampEnum(o.value(QStringLiteral("k")), static_cast<int>(ItemKind::Playlist), 0));
    it.id = ss(o.value(QStringLiteral("id")));
    it.title = ss(o.value(QStringLiteral("t")));
    it.channel = ss(o.value(QStringLiteral("c")));
    it.channelId = ss(o.value(QStringLiteral("ci")));
    it.durationSeconds = num(o.value(QStringLiteral("d")));
    it.durationText = ss(o.value(QStringLiteral("dt")));
    it.viewCount = num(o.value(QStringLiteral("v")));
    it.viewText = ss(o.value(QStringLiteral("vt")));
    it.publishedEpoch = num(o.value(QStringLiteral("pe")));
    it.publishedAgeSeconds = num(o.value(QStringLiteral("pa")));
    it.publishedText = ss(o.value(QStringLiteral("pt")));
    it.thumbnailUrl = ss(o.value(QStringLiteral("th")));
    it.avatarUrl = ss(o.value(QStringLiteral("av")));
    it.liveStatus = static_cast<media::LiveStatus>(clampEnum(o.value(QStringLiteral("ls")),
        static_cast<int>(media::LiveStatus::Unknown), static_cast<int>(media::LiveStatus::Unknown)));
    it.membersOnly = o.value(QStringLiteral("mo")).toBool();
    it.restricted = o.value(QStringLiteral("r")).toBool();
    it.shortEvidence = static_cast<ShortEvidence>(clampEnum(o.value(QStringLiteral("se")), static_cast<int>(ShortEvidence::NotShort), 0));
    return it;
}

}  // namespace

QByteArray FeedCacheStore::serialize(const std::vector<ChannelFeedState> &states) {
    QJsonArray channels;
    for (const ChannelFeedState &s : states) {
        QJsonObject c;
        c.insert(QStringLiteral("id"), qs(s.channel.id));
        c.insert(QStringLiteral("name"), qs(s.channel.name));
        c.insert(QStringLiteral("fetchedAt"), static_cast<double>(s.fetchedAt));
        c.insert(QStringLiteral("lastAttemptAt"), static_cast<double>(s.lastAttemptAt));
        c.insert(QStringLiteral("outcome"), static_cast<int>(s.lastOutcome));
        c.insert(QStringLiteral("source"), static_cast<int>(s.source));
        if (!s.continuation.empty()) c.insert(QStringLiteral("continuation"), qs(s.continuation));
        c.insert(QStringLiteral("paged"), s.pagedFromVideosTab);
        c.insert(QStringLiteral("exhausted"), s.exhausted);
        c.insert(QStringLiteral("failures"), s.consecutiveFailures);
        QJsonArray rows;
        for (const Item &it : s.rows) rows.append(itemToJson(it));
        c.insert(QStringLiteral("rows"), rows);
        channels.append(c);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), kVersion);
    root.insert(QStringLiteral("channels"), channels);
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

std::vector<ChannelFeedState> FeedCacheStore::deserialize(const QByteArray &data, bool *ok) {
    std::vector<ChannelFeedState> out;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    const QJsonObject root = doc.object();
    if (error.error != QJsonParseError::NoError || !doc.isObject() || root.value(QStringLiteral("version")).toInt() != kVersion ||
        !root.value(QStringLiteral("channels")).isArray()) {
        if (ok) *ok = false;
        return out;
    }
    for (const QJsonValue &v : root.value(QStringLiteral("channels")).toArray()) {
        const QJsonObject c = v.toObject();
        ChannelFeedState s(ChannelRef(ss(c.value(QStringLiteral("id"))), ss(c.value(QStringLiteral("name")))));
        if (!isFeedChannelId(s.channel.id)) continue;
        s.fetchedAt = num(c.value(QStringLiteral("fetchedAt")));
        s.lastAttemptAt = num(c.value(QStringLiteral("lastAttemptAt")));
        s.lastOutcome = static_cast<Outcome>(clampEnum(c.value(QStringLiteral("outcome")), static_cast<int>(Outcome::Unavailable), 0));
        s.source = static_cast<FeedSource>(clampEnum(c.value(QStringLiteral("source")), static_cast<int>(FeedSource::Extractor), 0));
        s.continuation = ss(c.value(QStringLiteral("continuation")));
        s.pagedFromVideosTab = c.value(QStringLiteral("paged")).toBool();
        s.exhausted = c.value(QStringLiteral("exhausted")).toBool();
        s.consecutiveFailures = c.value(QStringLiteral("failures")).toInt();
        for (const QJsonValue &r : c.value(QStringLiteral("rows")).toArray()) {
            Item it = itemFromJson(r.toObject());
            if (!it.id.empty()) s.rows.push_back(it);
        }
        out.push_back(s);
    }
    if (ok) *ok = true;
    return out;
}

bool FeedCacheStore::save(const std::vector<ChannelFeedState> &states) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!write(states)) return false;
    ++written_;
    return true;
}

bool FeedCacheStore::save(const std::vector<ChannelFeedState> &states, std::uint64_t order) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (order < written_) return true;   // a newer snapshot is on disk already
    if (!write(states)) return false;
    written_ = order;
    return true;
}

std::uint64_t FeedCacheStore::lastWrittenOrder() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return written_;
}

bool FeedCacheStore::write(const std::vector<ChannelFeedState> &states) const {
    if (path_.isEmpty()) return false;
    const QByteArray data = serialize(states);
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (file.write(data) != data.size()) { file.cancelWriting(); return false; }
    if (!file.commit()) return false;
    QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

std::vector<ChannelFeedState> FeedCacheStore::load(bool *ok) const {
    if (ok) *ok = true;
    QFile file(path_);
    if (path_.isEmpty() || !file.exists()) return std::vector<ChannelFeedState>();
    if (!file.open(QIODevice::ReadOnly)) { if (ok) *ok = false; return std::vector<ChannelFeedState>(); }
    return deserialize(file.readAll(), ok);
}

QJsonObject cachedItemToJson(const Item &item) { return itemToJson(item); }
Item cachedItemFromJson(const QJsonObject &object) { return itemFromJson(object); }

}}}
