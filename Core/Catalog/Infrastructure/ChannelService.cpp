// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ChannelService.h"
#include "Json.h"
#include "RendererParser.h"
#include "ShortsStore.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/ChannelReference.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/TextParse.h"

#include <QJsonArray>
#include <QUrl>

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

QString tabKeyFromUrl(const QString &url) {
    QString path = url;
    const int q = path.indexOf(QLatin1Char('?'));
    if (q >= 0) path = path.left(q);
    const QStringList seg = path.split(QLatin1Char('/'), QString::SkipEmptyParts);
    if (seg.isEmpty()) return QString();
    const QString last = seg.last().toLower();
    if (seg.size() >= 2 && isChannelTabSuffix(str(last))) return last == QLatin1String("live") ? QStringLiteral("streams") : last;
    return QStringLiteral("featured");
}

QJsonObject selectedTabRenderer(const QJsonObject &root) {
    const QJsonArray tabs = json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
    for (const QJsonValue &t : tabs) {
        const QJsonObject r = t.toObject().value(QStringLiteral("tabRenderer")).toObject();
        if (r.value(QStringLiteral("selected")).toBool()) return r;
    }
    return QJsonObject();
}

void appendCandidates(const QJsonValue &holder, std::vector<ImageCandidate> &out) {
    QJsonArray list = holder.toObject().value(QStringLiteral("thumbnails")).toArray();
    if (list.isEmpty()) list = holder.toObject().value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        const QString url = o.value(QStringLiteral("url")).toString();
        if (url.isEmpty()) continue;
        out.push_back(ImageCandidate(str(url), o.value(QStringLiteral("width")).toInt(), o.value(QStringLiteral("height")).toInt()));
    }
}

// Rows of a channel's own tabs carry no owner (or the channel itself): adopt the header's.
void adoptOwner(std::vector<Item> &items, const QString &channelId, const QString &title) {
    const std::string id = str(channelId);
    const std::string name = str(title);
    for (Item &item : items) {
        if (item.kind == ItemKind::Channel) continue;
        if (!item.channelId.empty() && item.channelId != id) continue;
        if (!id.empty()) item.channelId = id;
        if (!name.empty()) item.channel = name;
    }
}

// continuationItemViewModel (the newer paging holder) is not known to the shared parser yet:
// take its token and drop it from the unknown-renderer counts.
void takeViewModelContinuation(const QJsonValue &root, ParsedItems &parsed) {
    parsed.unknown.remove(QStringLiteral("continuationItemViewModel"));
    if (!parsed.continuation.isEmpty()) return;
    json::forEachKey(root, QStringLiteral("continuationItemViewModel"), [&parsed](const QJsonObject &c) {
        parsed.continuation = json::continuationToken(c);
        return parsed.continuation.isEmpty();
    });
}

}  // namespace

ChannelService::ChannelService(InnerTubeClient *client, ShortsStore *shorts, CatalogCounters *counters, QObject *parent)
    : QObject(parent), client_(client), shorts_(shorts), counters_(counters), resolved_(kResolveCacheSize), tabs_(kTabCacheSize),
      titles_(kTabCacheSize) {
    if (shorts) {
        QPointer<ChannelService> self(this);
        shorts->setTabParamsLookup([self](const QString &channelId) {
            return self ? self->tabParams(channelId, QStringLiteral("shorts")) : fallbackTabParams(QStringLiteral("shorts"));
        });
    }
}

ShortsStore *ChannelService::shorts() const { return shorts_.data(); }

QString ChannelService::rssUrl(const QString &channelId) {
    return QStringLiteral("https://www.youtube.com/feeds/videos.xml?channel_id=") + channelId;
}

QString ChannelService::normalizeTab(const QString &tab) {
    const QString t = tab.trimmed().toLower();
    if (t == QLatin1String("live")) return QStringLiteral("streams");
    if (t.isEmpty() || t == QLatin1String("uploads")) return QStringLiteral("videos");
    if (t == QLatin1String("home")) return QStringLiteral("featured");
    return t;
}

QString ChannelService::fallbackTabParams(const QString &tab) {
    // The web app's own tab params as recorded on 2026-11-08 (field 2 = tab name plus the web
    // client's view options). Used only until the channel's own tab list is known.
    const QString t = normalizeTab(tab);
    const QString known = innertube::channelTabParams(t);
    if (!known.isEmpty()) return known;
    // Other tabs (releases, podcasts, ...): the bare protobuf {2: name}.
    const QByteArray name = t.toUtf8();
    QByteArray proto;
    proto.append(static_cast<char>(0x12));
    proto.append(static_cast<char>(name.size()));
    proto.append(name);
    return QString::fromLatin1(proto.toBase64());
}

QString ChannelService::tabParams(const QString &channelId, const QString &tab) const {
    const QString t = normalizeTab(tab);
    const QList<ChannelTab> *tabs = tabs_.peek(str(channelId));
    if (tabs) {
        for (const ChannelTab &ct : *tabs)
            if (ct.key == t && !ct.params.isEmpty()) return ct.params;
    }
    return fallbackTabParams(t);
}

bool ChannelService::knowsTabs(const QString &channelId) const { return tabs_.contains(str(channelId)); }

QList<ChannelTab> ChannelService::knownTabs(const QString &channelId) const {
    const QList<ChannelTab> *tabs = tabs_.peek(str(channelId));
    return tabs ? *tabs : QList<ChannelTab>();
}

QString ChannelService::cachedChannelId(const QString &reference) const {
    const ChannelReference ref = parseChannelReference(str(reference));
    if (ref.kind == ChannelReference::Kind::ChannelId) return QString::fromStdString(ref.value);
    if (!ref.valid()) return QString();
    const QString *id = resolved_.peek(ref.key());
    return id ? *id : QString();
}

void ChannelService::count(Outcome outcome, const QMap<QString, int> &unknown) {
    if (!counters_) return;
    counters_->count(Area::Channel, outcome);
    for (auto it = unknown.constBegin(); it != unknown.constEnd(); ++it) counters_->unknownRenderer(str(it.key()), it.value());
}

// ---- resolve -------------------------------------------------------------------------------

Outcome ChannelService::parseResolve(const InnerTubeReply &reply, QString *channelId) {
    if (reply.outcome != Outcome::Ok) return reply.httpStatus == 404 ? Outcome::Unavailable : reply.outcome;
    const QJsonObject endpoint = reply.json.value(QStringLiteral("endpoint")).toObject();
    if (endpoint.isEmpty()) return hasErrorAlert(reply.json) ? Outcome::Unavailable : Outcome::ShapeDrift;
    const QString id = json::at(endpoint, QStringLiteral("browseEndpoint.browseId")).toString();
    if (isChannelId(str(id))) {
        if (channelId) *channelId = id;
        return Outcome::Ok;
    }
    // A known shape that is not a channel (urlEndpoint to an error page, a video, ...).
    return Outcome::Unavailable;
}

void ChannelService::resolve(const QString &reference, QObject *context, std::function<void(Outcome, QString)> done) {
    const ChannelReference ref = parseChannelReference(str(reference));
    if (!ref.valid()) { done(Outcome::Unavailable, QString()); return; }
    if (ref.kind == ChannelReference::Kind::ChannelId) { done(Outcome::Ok, QString::fromStdString(ref.value)); return; }
    if (const QString *cached = resolved_.get(ref.key())) { done(Outcome::Ok, *cached); return; }
    QJsonObject body;
    body.insert(QStringLiteral("url"), QString::fromStdString(ref.resolveUrl()));
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    QPointer<ChannelService> self(this);
    const std::string key = ref.key();
    std::function<QPair<int, QString>(const InnerTubeReply &)> parse = [](const InnerTubeReply &reply) {
        QString id;
        const Outcome o = parseResolve(reply, &id);
        return qMakePair(static_cast<int>(o), id);
    };
    std::function<void(QPair<int, QString>)> finish = [self, guard, hasContext, key, done](QPair<int, QString> r) {
        const Outcome o = static_cast<Outcome>(r.first);
        if (self) {
            if (o == Outcome::Ok) self->resolved_.put(key, r.second);
            if (self->counters_) self->counters_->count(Area::Channel, o);
        }
        if (hasContext && !guard) return;
        done(o, r.second);
    };
    client_->post<QPair<int, QString>>(QLatin1String(innertube::kEndpointResolveUrl), body, this, parse, finish);
}

// ---- parsing -------------------------------------------------------------------------------

QList<ChannelTab> ChannelService::parseTabs(const QJsonObject &root) {
    QList<ChannelTab> out;
    const QJsonArray tabs = json::at(root, QStringLiteral("contents.twoColumnBrowseResultsRenderer.tabs")).toArray();
    for (const QJsonValue &t : tabs) {
        const QJsonObject r = t.toObject().value(QStringLiteral("tabRenderer")).toObject();
        if (r.isEmpty()) continue;   // expandableTabRenderer (channel search) and unknown tab kinds
        ChannelTab tab;
        const QJsonObject endpoint = r.value(QStringLiteral("endpoint")).toObject();
        tab.key = tabKeyFromUrl(json::at(endpoint, QStringLiteral("commandMetadata.webCommandMetadata.url")).toString());
        tab.title = json::text(r.value(QStringLiteral("title")));
        tab.params = QUrl::fromPercentEncoding(json::at(endpoint, QStringLiteral("browseEndpoint.params")).toString().toLatin1());
        tab.selected = r.value(QStringLiteral("selected")).toBool();
        if (tab.key.isEmpty()) continue;
        out.append(tab);
    }
    return out;
}

ChannelHeader ChannelService::parseHeader(const QJsonObject &root, const QString &hl) {
    ChannelHeader h;
    const std::string lang = str(hl);
    std::vector<ImageCandidate> avatars;
    const QJsonObject md = json::at(root, QStringLiteral("metadata.channelMetadataRenderer")).toObject();
    if (!md.isEmpty()) {
        h.source = QStringLiteral("channelMetadataRenderer");
        h.channelId = md.value(QStringLiteral("externalId")).toString();
        h.title = md.value(QStringLiteral("title")).toString();
        h.description = md.value(QStringLiteral("description")).toString();
        h.rssUrl = md.value(QStringLiteral("rssUrl")).toString();
        const QString vanity = md.value(QStringLiteral("vanityChannelUrl")).toString();
        const int at = vanity.indexOf(QLatin1String("/@"));
        if (at >= 0) h.handle = vanity.mid(at + 1);
        appendCandidates(md.value(QStringLiteral("avatar")), avatars);
    }
    const QJsonValue header = root.value(QStringLiteral("header"));
    const QJsonObject phv = json::findFirst(header, QStringLiteral("pageHeaderViewModel"));
    const QJsonObject c4 = json::findFirst(header, QStringLiteral("c4TabbedHeaderRenderer"));
    if (!phv.isEmpty()) {
        h.source = QStringLiteral("pageHeaderViewModel");
        const QString title = json::text(json::at(phv, QStringLiteral("title.dynamicTextViewModel.text")));
        if (!title.isEmpty()) h.title = title;
        appendCandidates(json::at(phv, QStringLiteral("image.decoratedAvatarViewModel.avatar.avatarViewModel.image")), avatars);
        const QJsonArray rows = json::at(phv, QStringLiteral("metadata.contentMetadataViewModel.metadataRows")).toArray();
        for (const QJsonValue &row : rows) {
            for (const QJsonValue &partValue : row.toObject().value(QStringLiteral("metadataParts")).toArray()) {
                const QString t = json::text(partValue.toObject().value(QStringLiteral("text"))).trimmed();
                if (t.isEmpty()) continue;
                if (t.startsWith(QLatin1Char('@'))) { h.handle = t; continue; }
                const MetaPart kind = classifyMetaPart(str(t), lang);
                if (kind == MetaPart::Subscribers) {
                    h.subscriberText = t;
                    h.subscriberCount = parseSubscriberText(str(t), lang);
                } else if (kind == MetaPart::VideoCount) {
                    h.videoCountText = t;
                    h.videoCount = parseCountText(str(t), lang);
                }
            }
        }
    } else if (!c4.isEmpty()) {
        h.source = QStringLiteral("c4TabbedHeaderRenderer");
        const QString id = c4.value(QStringLiteral("channelId")).toString();
        if (!id.isEmpty()) h.channelId = id;
        const QString title = json::text(c4.value(QStringLiteral("title")));
        if (!title.isEmpty()) h.title = title;
        appendCandidates(c4.value(QStringLiteral("avatar")), avatars);
        const QString handle = json::text(c4.value(QStringLiteral("channelHandleText")));
        if (handle.startsWith(QLatin1Char('@'))) h.handle = handle;
        const QString subs = json::text(c4.value(QStringLiteral("subscriberCountText")));
        if (!subs.isEmpty()) { h.subscriberText = subs; h.subscriberCount = parseSubscriberText(str(subs), lang); }
        const QString videos = json::text(c4.value(QStringLiteral("videosCountText")));
        if (!videos.isEmpty()) { h.videoCountText = videos; h.videoCount = parseCountText(str(videos), lang); }
    }
    h.avatarUrl = QString::fromStdString(chooseAvatar(avatars));
    return h;
}

ChannelPage ChannelService::parseBrowse(const InnerTubeReply &reply, const QString &channelId, const QString &requestedTab,
                                        const QString &hl) {
    ChannelPage p;
    p.httpStatus = reply.httpStatus;
    p.channelId = channelId;
    p.requestedTab = normalizeTab(requestedTab);
    if (reply.outcome != Outcome::Ok) {
        p.outcome = reply.httpStatus == 404 ? Outcome::Unavailable : reply.outcome;
        return p;
    }
    const QJsonObject &root = reply.json;
    p.header = parseHeader(root, hl);
    if (p.channelId.isEmpty()) p.channelId = p.header.channelId;
    // The channel's own avatar, for its rows in every list (AvatarBook).
    if (!p.header.avatarUrl.isEmpty())
        AvatarBook::instance().learn(str(p.header.channelId.isEmpty() ? p.channelId : p.header.channelId), str(p.header.avatarUrl), true);
    p.tabs = parseTabs(root);
    if (p.tabs.isEmpty()) {
        // A terminated or missing channel answers with an ERROR alert and no tab list.
        p.outcome = hasErrorAlert(root) ? Outcome::Unavailable : Outcome::ShapeDrift;
        return p;
    }
    for (const ChannelTab &t : p.tabs) if (t.selected) p.selectedTab = t.key;
    if (!p.hasTab(p.requestedTab)) {
        p.tabMissing = true;
        p.outcome = Outcome::ValidEmpty;
        return p;
    }
    if (p.selectedTab != p.requestedTab) {
        // The params did not select the tab (stale fallback params): the caller retries with
        // the channel's own params; if it cannot, this is drift.
        p.outcome = Outcome::ShapeDrift;
        return p;
    }
    const QJsonValue content = selectedTabRenderer(root).value(QStringLiteral("content"));
    if (!json::containsKey(content, QStringList() << QStringLiteral("richGridRenderer") << QStringLiteral("sectionListRenderer"))) {
        p.outcome = Outcome::ShapeDrift;
        return p;
    }
    ParsedItems parsed = RendererParser(hl).parse(content);
    takeViewModelContinuation(content, parsed);
    p.items = std::move(parsed.items);
    p.continuation = parsed.continuation;
    p.unknown = parsed.unknown;
    p.unparsed = parsed.unparsed;
    adoptOwner(p.items, p.channelId, p.header.title);
    p.outcome = p.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return p;
}

ChannelPage ChannelService::parseContinuation(const InnerTubeReply &reply, const QString &channelId, const QString &tab,
                                              const QString &hl) {
    ChannelPage p;
    p.httpStatus = reply.httpStatus;
    p.channelId = channelId;
    p.requestedTab = normalizeTab(tab);
    p.selectedTab = p.requestedTab;
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
    takeViewModelContinuation(actions, parsed);
    p.items = std::move(parsed.items);
    p.continuation = parsed.continuation;
    p.unknown = parsed.unknown;
    p.unparsed = parsed.unparsed;
    adoptOwner(p.items, p.channelId, QString());   // the service adds the channel's name
    p.outcome = p.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return p;
}

// ---- browse --------------------------------------------------------------------------------

void ChannelService::learn(const ChannelPage &page) {
    if (page.channelId.isEmpty()) return;
    if (!page.tabs.isEmpty()) tabs_.put(str(page.channelId), page.tabs);
    if (!page.header.title.isEmpty()) titles_.put(str(page.channelId), page.header.title);
}

void ChannelService::browseTab(const QString &channelId, const QString &tab, QObject *context,
                               std::function<void(ChannelPage)> done) {
    const QString t = normalizeTab(tab);
    browseAttempt(channelId, t, tabParams(channelId, t), 0, context, done);
}

void ChannelService::browseAttempt(const QString &channelId, const QString &tab, const QString &params, int attempt,
                                   QObject *context, std::function<void(ChannelPage)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("browseId"), channelId);
    if (!params.isEmpty()) body.insert(QStringLiteral("params"), params);
    const QString hl = client_->locale().hl;
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    QPointer<ChannelService> self(this);
    std::function<ChannelPage(const InnerTubeReply &)> parse = [channelId, tab, hl](const InnerTubeReply &reply) {
        return parseBrowse(reply, channelId, tab, hl);
    };
    std::function<void(ChannelPage)> finish = [self, guard, hasContext, channelId, tab, params, attempt, done](ChannelPage page) {
        if (!self) return;
        const bool knewTabs = self->knowsTabs(channelId);
        self->learn(page);
        if (attempt < 2 && (!hasContext || guard)) {
            // The answer shows another tab while the channel lists the requested one: repeat
            // once with the channel's own params.
            if (page.outcome == Outcome::ShapeDrift && !page.tabMissing && page.hasTab(tab) && page.selectedTab != tab) {
                const QString own = self->tabParams(channelId, tab);
                if (own != params) {
                    self->browseAttempt(channelId, tab, own, attempt + 1, guard.data(), done);
                    return;
                }
            }
            // Stale fallback params rejected before any tab list is known: learn the list from
            // the channel's home tab, then ask again.
            if (page.httpStatus == 400 && !knewTabs && !params.isEmpty()) {
                self->browseAttempt(channelId, tab, QString(), attempt + 1, guard.data(), done);
                return;
            }
        }
        self->count(page.outcome, page.unknown);
        if (ShortsStore *store = self->shorts()) {
            store->observe(page.items, tab == QLatin1String("shorts"));
            if (tab != QLatin1String("shorts")) store->requestChecks(page.items);
        }
        if (hasContext && !guard) return;
        done(page);
    };
    client_->post<ChannelPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, finish);
}

void ChannelService::browseMore(const QString &channelId, const QString &tab, const QString &continuation, QObject *context,
                                std::function<void(ChannelPage)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), continuation);
    const QString hl = client_->locale().hl;
    const QString t = normalizeTab(tab);
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    QPointer<ChannelService> self(this);
    std::function<ChannelPage(const InnerTubeReply &)> parse = [channelId, t, hl](const InnerTubeReply &reply) {
        return parseContinuation(reply, channelId, t, hl);
    };
    std::function<void(ChannelPage)> finish = [self, guard, hasContext, t, done](ChannelPage page) {
        if (!self) return;
        if (const QString *title = self->titles_.peek(str(page.channelId))) adoptOwner(page.items, page.channelId, *title);
        else adoptOwner(page.items, page.channelId, QString());
        page.tabs = self->knownTabs(page.channelId);
        self->count(page.outcome, page.unknown);
        if (ShortsStore *store = self->shorts()) {
            store->observe(page.items, t == QLatin1String("shorts"));
            if (t != QLatin1String("shorts")) store->requestChecks(page.items);
        }
        if (hasContext && !guard) return;
        done(page);
    };
    client_->post<ChannelPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, finish);
}

void ChannelService::rss(const QString &channelId, QObject *context, RssDone done) {
    if (!rss_ || !isChannelId(str(channelId))) { done(Outcome::Unavailable, std::vector<Item>()); return; }
    QPointer<ChannelService> self(this);
    rss_(channelId, context, [self, done](Outcome outcome, std::vector<Item> rows) {
        if (self) {
            if (self->counters_) self->counters_->count(Area::Channel, outcome);
            if (ShortsStore *store = self->shorts()) {
                store->observe(rows);                       // /shorts/ links confirm at once
                store->knowledge().fillDurations(rows);     // known durations fill RSS rows
            }
        }
        done(outcome, std::move(rows));
    });
}

}}}
