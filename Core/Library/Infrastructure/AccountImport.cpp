// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountImport.h"

#include "Core/Library/Domain/LibraryPolicy.h"
#include <QJsonArray>
#include <QPointer>
#include <QSet>

namespace nordtile { namespace video { namespace library {

namespace {

std::string ss(const QString &s) { return s.toStdString(); }

QString text(const QJsonValue &v) {
    if (v.isString()) return v.toString();
    const QJsonObject o = v.toObject();
    if (o.contains(QStringLiteral("simpleText"))) return o.value(QStringLiteral("simpleText")).toString();
    if (o.contains(QStringLiteral("content"))) return o.value(QStringLiteral("content")).toString();
    QString out;
    for (const QJsonValue &run : o.value(QStringLiteral("runs")).toArray())
        out += run.toObject().value(QStringLiteral("text")).toString();
    return out;
}

QString absoluteUrl(QString url) {
    if (url.startsWith(QLatin1String("//"))) url.prepend(QStringLiteral("https:"));
    return url;
}

// The last (largest) entry of the first `thumbnails` or `sources` array found.
QString thumbnail(const QJsonValue &v) {
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) { const QString t = thumbnail(e); if (!t.isEmpty()) return t; }
        return QString();
    }
    const QJsonObject o = v.toObject();
    for (const char *key : { "thumbnails", "sources" }) {
        const QJsonArray a = o.value(QLatin1String(key)).toArray();
        if (!a.isEmpty()) return absoluteUrl(a.last().toObject().value(QStringLiteral("url")).toString());
    }
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (!it.value().isObject() && !it.value().isArray()) continue;
        const QString t = thumbnail(it.value());
        if (!t.isEmpty()) return t;
    }
    return QString();
}

int leadingCount(const QString &s) {
    QString digits;
    for (QChar c : s) {
        if (c.isDigit()) digits += c;
        else if (c == QLatin1Char(',') || c == QLatin1Char('.') || c.isSpace()) { if (!digits.isEmpty() && c.isSpace()) break; }
        else break;
    }
    bool ok = false;
    const int n = digits.toInt(&ok);
    return ok ? n : -1;
}

bool validChannelId(const QString &id) {
    if (id.size() != 24 || !id.startsWith(QLatin1String("UC"))) return false;
    for (QChar c : id)
        if (!(c.isLetterOrNumber() && c.unicode() < 128) && c != QLatin1Char('_') && c != QLatin1Char('-')) return false;
    return true;
}

QString validPlaylistId(const QString &id) {
    return QString::fromStdString(playlistIdFromUrl(ss(id)));
}

QString findContinuation(const QJsonValue &v) {
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) { const QString t = findContinuation(e); if (!t.isEmpty()) return t; }
        return QString();
    }
    if (!v.isObject()) return QString();
    const QJsonObject o = v.toObject();
    const QJsonObject command = o.value(QStringLiteral("continuationCommand")).toObject();
    if (!command.value(QStringLiteral("token")).toString().isEmpty()) return command.value(QStringLiteral("token")).toString();
    const QJsonObject next = o.value(QStringLiteral("nextContinuationData")).toObject();
    if (!next.value(QStringLiteral("continuation")).toString().isEmpty()) return next.value(QStringLiteral("continuation")).toString();
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        const QString t = findContinuation(it.value());
        if (!t.isEmpty()) return t;
    }
    return QString();
}

void addChannel(const QJsonObject &r, accountImport::Page *page) {
    const QString id = r.value(QStringLiteral("channelId")).toString();
    if (!validChannelId(id)) { ++page->skippedRows; return; }
    ChannelRecord c;
    c.channelId = ss(id);
    c.name = ss(text(r.value(QStringLiteral("title"))));
    c.avatarUrl = ss(thumbnail(r.value(QStringLiteral("thumbnail"))));
    page->channels.push_back(c);
}

void addPlaylist(const QString &rawId, const QString &title, const QString &channel, const QString &thumb, int count,
                 accountImport::Page *page) {
    if (rawId.startsWith(QLatin1String("UC"))) { ++page->skippedRows; return; } // a channel row, not a list
    const QString id = validPlaylistId(rawId);
    if (id.isEmpty()) { ++page->skippedRows; return; }
    RemotePlaylistRef p;
    p.remoteId = ss(id);
    p.title = ss(title);
    p.channel = ss(channel);
    p.thumbnailUrl = ss(thumb);
    p.itemCountHint = count;
    page->playlists.push_back(p);
}

void walk(const QJsonValue &v, bool channels, accountImport::Page *page) {
    if (v.isArray()) {
        for (const QJsonValue &e : v.toArray()) walk(e, channels, page);
        return;
    }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    if (channels) {
        for (const char *key : { "channelRenderer", "gridChannelRenderer" }) {
            if (o.contains(QLatin1String(key))) { addChannel(o.value(QLatin1String(key)).toObject(), page); return; }
        }
    } else {
        for (const char *key : { "gridPlaylistRenderer", "playlistRenderer" }) {
            if (!o.contains(QLatin1String(key))) continue;
            const QJsonObject r = o.value(QLatin1String(key)).toObject();
            QString channel = text(r.value(QStringLiteral("shortBylineText")));
            if (channel.isEmpty()) channel = text(r.value(QStringLiteral("longBylineText")));
            int count = leadingCount(r.value(QStringLiteral("videoCount")).toString());
            if (count < 0) count = leadingCount(text(r.value(QStringLiteral("videoCountText"))));
            if (count < 0) count = leadingCount(text(r.value(QStringLiteral("videoCountShortText"))));
            const QJsonValue thumbs = r.contains(QStringLiteral("thumbnail")) ? r.value(QStringLiteral("thumbnail"))
                                                                              : r.value(QStringLiteral("thumbnails"));
            addPlaylist(r.value(QStringLiteral("playlistId")).toString(), text(r.value(QStringLiteral("title"))),
                        channel, thumbnail(thumbs), count, page);
            return;
        }
        if (o.contains(QStringLiteral("lockupViewModel"))) {
            const QJsonObject r = o.value(QStringLiteral("lockupViewModel")).toObject();
            const QString type = r.value(QStringLiteral("contentType")).toString();
            if (type.contains(QLatin1String("VIDEO"))) return; // not a list row
            const QJsonObject meta = r.value(QStringLiteral("metadata")).toObject()
                                         .value(QStringLiteral("lockupMetadataViewModel")).toObject();
            addPlaylist(r.value(QStringLiteral("contentId")).toString(), text(meta.value(QStringLiteral("title"))),
                        QString(), thumbnail(r.value(QStringLiteral("contentImage"))), -1, page);
            return;
        }
    }
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) walk(it.value(), channels, page);
}

} // namespace

namespace accountImport {

Page parseChannelsPage(const QJsonObject &reply) {
    Page page;
    walk(reply, true, &page);
    page.continuation = findContinuation(reply);
    return page;
}

Page parsePlaylistsPage(const QJsonObject &reply) {
    Page page;
    walk(reply, false, &page);
    page.continuation = findContinuation(reply);
    return page;
}

QMap<QByteArray, QByteArray> headersFor(const AccountIdentity &identity) {
    QMap<QByteArray, QByteArray> h;
    h.insert(QByteArrayLiteral("X-Goog-AuthUser"), QByteArray::number(qMax(0, identity.authUser)));
    if (!identity.pageId.isEmpty()) h.insert(QByteArrayLiteral("X-Goog-PageId"), identity.pageId.toUtf8());
    return h;
}

} // namespace accountImport

AccountImporter::AccountImporter(AccountBrowseTransport *transport, QObject *parent)
    : QObject(parent), transport_(transport) {}

bool AccountImporter::start(const AccountIdentity &identity) {
    if (running_ || !transport_) return false;
    running_ = true;
    identity_ = identity;
    batch_ = ImportBatch();
    batch_.source = "account";
    feed_ = Feed::Channels;
    pagesInFeed_ = 0;
    pagesRead_ = 0;
    ++run_;
    QJsonObject body;
    body.insert(QStringLiteral("browseId"), QStringLiteral("FEchannels"));
    request(body);
    return true;
}

void AccountImporter::cancel() {
    if (!running_) return;
    running_ = false;
    ++run_;
}

void AccountImporter::request(const QJsonObject &body) {
    const int run = run_;
    QPointer<AccountImporter> guard(this);
    transport_->browse(body, accountImport::headersFor(identity_), [guard, run](bool ok, const QJsonObject &reply) {
        if (guard) guard->onReply(run, ok, reply);
    });
}

void AccountImporter::onReply(int run, bool ok, const QJsonObject &reply) {
    if (run != run_ || !running_) return;
    if (!ok) { finish(QStringLiteral("network")); return; }
    ++pagesInFeed_;
    ++pagesRead_;
    emit progress(pagesRead_);
    int added = 0;
    QString next;
    if (feed_ == Feed::Channels) {
        const accountImport::Page page = accountImport::parseChannelsPage(reply);
        QSet<QString> seen;
        for (const ChannelRecord &c : batch_.subscriptions) seen.insert(QString::fromStdString(c.channelId));
        for (const ChannelRecord &c : page.channels) {
            if (seen.contains(QString::fromStdString(c.channelId))) continue;
            seen.insert(QString::fromStdString(c.channelId));
            batch_.subscriptions.push_back(c);
            ++added;
        }
        batch_.skippedRows += page.skippedRows;
        next = page.continuation;
    } else {
        const accountImport::Page page = accountImport::parsePlaylistsPage(reply);
        QSet<QString> seen;
        for (const RemotePlaylistRef &p : batch_.remotePlaylists) seen.insert(QString::fromStdString(p.remoteId));
        for (const RemotePlaylistRef &p : page.playlists) {
            if (seen.contains(QString::fromStdString(p.remoteId))) continue;
            seen.insert(QString::fromStdString(p.remoteId));
            batch_.remotePlaylists.push_back(p);
            ++added;
        }
        batch_.skippedRows += page.skippedRows;
        next = page.continuation;
    }
    // A page that adds nothing new ends the feed (IMP-02).
    const bool more = !next.isEmpty() && pagesInFeed_ < accountImport::maxPages && added > 0;
    if (more) {
        QJsonObject body;
        body.insert(QStringLiteral("continuation"), next);
        request(body);
        return;
    }
    if (feed_ == Feed::Channels) {
        feed_ = Feed::Playlists;
        pagesInFeed_ = 0;
        QJsonObject body;
        body.insert(QStringLiteral("browseId"), QStringLiteral("FEplaylist_aggregation"));
        request(body);
        return;
    }
    finish(QString());
}

void AccountImporter::finish(const QString &error) {
    running_ = false;
    ++run_;
    const ImportBatch batch = batch_;
    emit finished(batch, error);
}

}}}
