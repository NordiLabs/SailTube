// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PlaylistService.h"
#include "Json.h"
#include "RendererParser.h"
#include "Core/Catalog/Domain/ChannelReference.h"
#include "Core/Catalog/Domain/TextParse.h"

#include <QJsonArray>
#include <QPointer>
#include <QRegExp>

namespace nordtile { namespace video { namespace catalog {

namespace {

std::string str(const QString &q) { return q.toStdString(); }

bool hasErrorAlert(const QJsonObject &root) {
    bool error = false;
    json::forEachKey(root.value(QStringLiteral("alerts")), QStringLiteral("alertRenderer"), [&error](const QJsonObject &a) {
        if (a.value(QStringLiteral("type")).toString() == QLatin1String("ERROR")) error = true;
        return !error;
    });
    return error;
}

void finishRows(ParsedItems &parsed, const QJsonValue &root, PlaylistPage &p, int firstIndex) {
    parsed.unknown.remove(QStringLiteral("continuationItemViewModel"));
    if (parsed.continuation.isEmpty()) {
        json::forEachKey(root, QStringLiteral("continuationItemViewModel"), [&parsed](const QJsonObject &c) {
            parsed.continuation = json::continuationToken(c);
            return parsed.continuation.isEmpty();
        });
    }
    const std::string list = str(p.playlistId);
    int position = firstIndex;
    for (Item &item : parsed.items) {
        if (!isVideoLike(item)) continue;
        item.playlistId = list;
        if (item.playlistIndex < 0) item.playlistIndex = position;
        position = item.playlistIndex + 1;
        p.items.push_back(item);
    }
    p.continuation = parsed.continuation;
    p.unknown = parsed.unknown;
    p.outcome = p.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
}

}  // namespace

PlaylistService::PlaylistService(InnerTubeClient *client, CatalogCounters *counters, QObject *parent)
    : QObject(parent), client_(client), counters_(counters) {}

QString PlaylistService::normalizeListId(const QString &listId) {
    QString id = listId.trimmed();
    const int list = id.indexOf(QLatin1String("list="));
    if (list >= 0) {
        id = id.mid(list + 5);
        const int end = id.indexOf(QRegExp(QStringLiteral("[&#]")));
        if (end >= 0) id = id.left(end);
    }
    if (id.startsWith(QLatin1String("VL")) && id.size() > 2) id = id.mid(2);
    if (id.isEmpty()) return QString();
    for (const QChar c : id) {
        const bool ok = (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                        (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-') || c == QLatin1Char('_');
        if (!ok) return QString();
    }
    return id;
}

PlaylistPage PlaylistService::parseBrowse(const InnerTubeReply &reply, const QString &listId, const QString &hl) {
    PlaylistPage p;
    p.httpStatus = reply.httpStatus;
    p.playlistId = listId;
    if (reply.outcome != Outcome::Ok) {
        p.outcome = reply.httpStatus == 404 ? Outcome::Unavailable : reply.outcome;
        return p;
    }
    const QJsonObject &root = reply.json;
    const std::string lang = str(hl);
    const QJsonValue header = root.value(QStringLiteral("header"));
    QString headerOwner, headerOwnerId;

    // Current generation: pageHeaderViewModel.
    const QJsonObject phv = json::findFirst(header, QStringLiteral("pageHeaderViewModel"));
    if (!phv.isEmpty()) {
        p.title = json::text(json::at(phv, QStringLiteral("title.dynamicTextViewModel.text")));
        p.description = json::text(json::at(phv, QStringLiteral("description.descriptionPreviewViewModel.description")));
        const QJsonArray rows = json::at(phv, QStringLiteral("metadata.contentMetadataViewModel.metadataRows")).toArray();
        for (const QJsonValue &row : rows) {
            for (const QJsonValue &partValue : row.toObject().value(QStringLiteral("metadataParts")).toArray()) {
                const QJsonObject part = partValue.toObject();
                QString t = json::text(part.value(QStringLiteral("text")));
                if (t.isEmpty()) t = json::text(json::at(part, QStringLiteral("avatarStack.avatarStackViewModel.text")));
                if (t.isEmpty()) continue;
                const QString browse = json::firstBrowseId(part);
                if (headerOwnerId.isEmpty() && isChannelId(str(browse))) { headerOwnerId = browse; headerOwner = t; continue; }
                const MetaPart kind = classifyMetaPart(str(t), lang);
                if (kind == MetaPart::VideoCount && p.videoCount < 0) { p.videoCountText = t; p.videoCount = parseCountText(str(t), lang); }
                else if (kind == MetaPart::Views && p.viewText.isEmpty()) p.viewText = t;
            }
        }
    }
    // Older generation: playlistHeaderRenderer.
    const QJsonObject phr = json::findFirst(header, QStringLiteral("playlistHeaderRenderer"));
    if (!phr.isEmpty()) {
        if (p.title.isEmpty()) p.title = json::text(phr.value(QStringLiteral("title")));
        if (p.videoCount < 0) {
            QString t = json::text(phr.value(QStringLiteral("numVideosText")));
            if (t.isEmpty()) t = json::text(phr.value(QStringLiteral("stats")).toArray().at(0));
            if (!t.isEmpty()) { p.videoCountText = t; p.videoCount = parseCountText(str(t), lang); }
        }
        if (p.viewText.isEmpty()) p.viewText = json::text(phr.value(QStringLiteral("viewCountText")));
        if (p.description.isEmpty()) p.description = json::text(phr.value(QStringLiteral("descriptionText")));
        const QJsonValue owner = phr.value(QStringLiteral("ownerText"));
        if (!owner.isUndefined()) { p.owner = json::text(owner); p.ownerId = json::firstBrowseId(owner); }
    }
    // The sidebar names the owner without a "by" prefix.
    const QJsonObject vor = json::findFirst(root.value(QStringLiteral("sidebar")), QStringLiteral("videoOwnerRenderer"));
    if (!vor.isEmpty()) {
        const QString name = json::text(vor.value(QStringLiteral("title")));
        if (!name.isEmpty()) { p.owner = name; p.ownerId = json::firstBrowseId(vor); }
    }
    const QJsonObject primary = json::findFirst(root.value(QStringLiteral("sidebar")), QStringLiteral("playlistSidebarPrimaryInfoRenderer"));
    if (!primary.isEmpty()) {
        if (p.title.isEmpty()) p.title = json::text(primary.value(QStringLiteral("title")));
        const QJsonArray stats = primary.value(QStringLiteral("stats")).toArray();
        if (p.videoCount < 0 && !stats.isEmpty()) {
            const QString t = json::text(stats.at(0));
            p.videoCountText = t;
            p.videoCount = parseCountText(str(t), lang);
        }
    }
    if (p.owner.isEmpty()) { p.owner = headerOwner; p.ownerId = headerOwnerId; }
    if (p.title.isEmpty()) p.title = json::at(root, QStringLiteral("metadata.playlistMetadataRenderer.title")).toString();
    p.thumbnailUrl = json::bestImage(json::at(root, QStringLiteral("microformat.microformatDataRenderer.thumbnail")), false);

    // Rows: the (only) tab's section list.
    QJsonValue content;
    for (const QJsonValue &t : json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray()) {
        const QJsonObject r = t.toObject().value(QStringLiteral("tabRenderer")).toObject();
        if (r.contains(QStringLiteral("content"))) { content = r.value(QStringLiteral("content")); break; }
    }
    if (!json::containsKey(content, QStringList() << QStringLiteral("sectionListRenderer") << QStringLiteral("playlistVideoListRenderer"))) {
        p.outcome = hasErrorAlert(root) ? Outcome::Unavailable : Outcome::ShapeDrift;
        return p;
    }
    ParsedItems parsed = RendererParser(hl).parse(content);
    finishRows(parsed, content, p, 0);
    return p;
}

PlaylistPage PlaylistService::parseContinuation(const InnerTubeReply &reply, const QString &listId, int firstIndex,
                                                const QString &hl) {
    PlaylistPage p;
    p.httpStatus = reply.httpStatus;
    p.playlistId = listId;
    if (reply.outcome != Outcome::Ok) { p.outcome = reply.outcome; return p; }
    QJsonValue actions = reply.json.value(QStringLiteral("onResponseReceivedActions"));
    if (!actions.isArray()) actions = reply.json.value(QStringLiteral("onResponseReceivedEndpoints"));
    if (!actions.isArray() ||
        !json::containsKey(actions, QStringList() << QStringLiteral("appendContinuationItemsAction")
                                                  << QStringLiteral("reloadContinuationItemsCommand"))) {
        p.outcome = Outcome::ShapeDrift;
        return p;
    }
    ParsedItems parsed = RendererParser(hl).parse(actions);
    finishRows(parsed, actions, p, firstIndex);
    return p;
}

void PlaylistService::count(const PlaylistPage &page) {
    if (!counters_) return;
    counters_->count(Area::Playlist, page.outcome);
    for (auto it = page.unknown.constBegin(); it != page.unknown.constEnd(); ++it) counters_->unknownRenderer(str(it.key()), it.value());
}

void PlaylistService::open(const QString &listId, QObject *context, std::function<void(PlaylistPage)> done) {
    const QString id = normalizeListId(listId);
    if (id.isEmpty()) {
        PlaylistPage p;
        p.outcome = Outcome::Unavailable;
        done(p);
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("browseId"), QStringLiteral("VL") + id);
    const QString hl = client_->locale().hl;
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    QPointer<PlaylistService> self(this);
    std::function<PlaylistPage(const InnerTubeReply &)> parse = [id, hl](const InnerTubeReply &reply) {
        return parseBrowse(reply, id, hl);
    };
    std::function<void(PlaylistPage)> finish = [self, guard, hasContext, done](PlaylistPage page) {
        if (self) self->count(page);
        if (hasContext && !guard) return;
        done(page);
    };
    client_->post<PlaylistPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, finish);
}

void PlaylistService::more(const QString &listId, const QString &continuation, int firstIndex, QObject *context,
                           std::function<void(PlaylistPage)> done) {
    const QString id = normalizeListId(listId);
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), continuation);
    const QString hl = client_->locale().hl;
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    QPointer<PlaylistService> self(this);
    std::function<PlaylistPage(const InnerTubeReply &)> parse = [id, firstIndex, hl](const InnerTubeReply &reply) {
        return parseContinuation(reply, id, firstIndex, hl);
    };
    std::function<void(PlaylistPage)> finish = [self, guard, hasContext, done](PlaylistPage page) {
        if (self) self->count(page);
        if (hasContext && !guard) return;
        done(page);
    };
    client_->post<PlaylistPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, finish);
}

}}}
