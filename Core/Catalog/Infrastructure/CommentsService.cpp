// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CommentsService.h"
#include "Json.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/TextParse.h"

#include <QHash>
#include <QJsonArray>

namespace nordtile { namespace video { namespace catalog {

namespace {

const QString kCommentSection = QStringLiteral("comment-item-section");

QString str(const QJsonObject &o, const char *key) { return o.value(QLatin1String(key)).toString(); }
QJsonObject obj(const QJsonObject &o, const char *key) { return o.value(QLatin1String(key)).toObject(); }

// "1.3K", "66", "" (none) -> count; raw text kept by the caller.
std::int64_t countOrZero(const QString &text, const std::string &hl) {
    const QString t = text.trimmed();
    if (t.isEmpty()) return 0;
    return parseCountText(t.toStdString(), hl);
}

// A count text worth showing as the total: it must carry a number. "Comments" alone does not.
bool totalFrom(const QJsonValue &textHolder, const std::string &hl, std::string *text, std::int64_t *count) {
    // Prefer the first run ("7,712" of "7,712" + " Comments"), then the whole text.
    QString candidate;
    const QJsonArray runs = textHolder.toObject().value(QStringLiteral("runs")).toArray();
    if (!runs.isEmpty()) candidate = runs.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
    std::int64_t n = candidate.isEmpty() ? -1 : parseCountText(candidate.toStdString(), hl);
    if (n < 0) {
        candidate = json::text(textHolder).trimmed();
        n = candidate.isEmpty() ? -1 : parseCountText(candidate.toStdString(), hl);
    }
    if (n < 0) return false;
    *text = candidate.toStdString();
    *count = n;
    return true;
}

std::vector<SortOption> parseSorts(const QJsonObject &menu) {
    std::vector<SortOption> out;
    const QJsonArray items = menu.value(QStringLiteral("subMenuItems")).toArray();
    for (int i = 0; i < items.size(); ++i) {
        const QJsonObject item = items.at(i).toObject();
        SortOption opt;
        // The menu lists Top first and Newest second; titles are localized, so order decides.
        opt.sort = i == 1 ? CommentSort::Newest : CommentSort::Top;
        opt.title = json::text(item.value(QStringLiteral("title"))).toStdString();
        opt.selected = item.value(QStringLiteral("selected")).toBool();
        QString token = json::continuationToken(item);
        if (token.isEmpty()) token = json::at(item, QStringLiteral("continuation.reloadContinuationData.continuation")).toString();
        opt.token = token.toStdString();
        if (!opt.token.empty() && i < 2) out.push_back(opt);
    }
    return out;
}

// The first page token of a comments item section.
QString sectionToken(const QJsonObject &section) {
    for (const QJsonValue &v : section.value(QStringLiteral("contents")).toArray()) {
        const QJsonObject cir = v.toObject().value(QStringLiteral("continuationItemRenderer")).toObject();
        if (!cir.isEmpty()) {
            const QString t = json::continuationToken(cir);
            if (!t.isEmpty()) return t;
        }
    }
    return QString();
}

QString sectionMessage(const QJsonObject &section) {
    const QJsonObject m = json::findFirst(section, QStringLiteral("messageRenderer"));
    if (m.isEmpty()) return QString();
    QString t = json::text(m.value(QStringLiteral("text")));
    if (t.isEmpty()) t = json::text(m.value(QStringLiteral("subtext")));
    return t;
}

// thread.commentViewModel.commentViewModel (threads) or item.commentViewModel (replies).
QJsonObject viewModel(const QJsonObject &holder) {
    QJsonObject vm = obj(holder, "commentViewModel");
    if (vm.contains(QStringLiteral("commentViewModel"))) vm = obj(vm, "commentViewModel");
    return vm;
}

std::string stripEdited(const QString &published) {
    // "2 days ago (edited)" / "vor 2 Tagen (bearbeitet)": the age is the part before " (".
    const int cut = published.indexOf(QStringLiteral(" ("));
    return (cut > 0 ? published.left(cut) : published).trimmed().toStdString();
}

bool fromEntity(const QJsonObject &vm, const QJsonObject &entity, const std::string &hl, Comment *c) {
    const QJsonObject props = obj(entity, "properties");
    const QJsonObject author = obj(entity, "author");
    const QJsonObject toolbar = obj(entity, "toolbar");
    c->key = str(vm, "commentKey").toStdString();
    QString id = str(props, "commentId");
    if (id.isEmpty()) id = str(vm, "commentId");
    c->id = id.toStdString();
    c->text = json::text(props.value(QStringLiteral("content"))).toStdString();
    const QString published = str(props, "publishedTime");
    c->publishedText = published.toStdString();
    c->publishedAgeSeconds = published.isEmpty() ? -1 : parseAgeSeconds(stripEdited(published), hl);
    c->author = str(author, "displayName").toStdString();
    c->authorChannelId = str(author, "channelId").toStdString();
    c->avatarUrl = absoluteUrl(str(author, "avatarThumbnailUrl").toStdString());
    c->creator = author.value(QStringLiteral("isCreator")).toBool();
    c->verified = author.value(QStringLiteral("isVerified")).toBool();
    const QString likes = str(toolbar, "likeCountNotliked").trimmed();
    c->likeText = likes.toStdString();
    c->likeCount = countOrZero(likes, hl);
    const QString replies = str(toolbar, "replyCount").trimmed();
    c->replyCountText = replies.toStdString();
    c->replyCount = countOrZero(replies, hl);
    c->isReply = props.value(QStringLiteral("replyLevel")).toInt() > 0;
    c->pinned = !str(vm, "pinnedText").isEmpty();
    return !c->id.empty();
}

// Older generation: commentThreadRenderer.comment.commentRenderer carries the body inline.
bool fromCommentRenderer(const QJsonObject &r, const std::string &hl, Comment *c) {
    c->id = str(r, "commentId").toStdString();
    c->author = json::text(r.value(QStringLiteral("authorText"))).toStdString();
    c->authorChannelId = json::firstBrowseId(r.value(QStringLiteral("authorEndpoint"))).toStdString();
    c->avatarUrl = json::bestImage(r.value(QStringLiteral("authorThumbnail")), false).toStdString();
    c->text = json::text(r.value(QStringLiteral("contentText"))).toStdString();
    const QString published = json::text(r.value(QStringLiteral("publishedTimeText")));
    c->publishedText = published.toStdString();
    c->publishedAgeSeconds = published.isEmpty() ? -1 : parseAgeSeconds(stripEdited(published), hl);
    const QString likes = json::text(r.value(QStringLiteral("voteCount"))).trimmed();
    c->likeText = likes.toStdString();
    c->likeCount = countOrZero(likes, hl);
    c->replyCount = r.value(QStringLiteral("replyCount")).toInt();
    c->creator = r.value(QStringLiteral("authorIsChannelOwner")).toBool();
    c->pinned = r.contains(QStringLiteral("pinnedCommentBadge"));
    return !c->id.empty();
}

void countOutcome(CatalogCounters *counters, Outcome o) {
    if (counters) counters->count(Area::Comments, o);
}

}  // namespace

CommentsService::CommentsService(InnerTubeClient *client, CatalogCounters *counters, QObject *parent)
    : QObject(parent), client_(client), counters_(counters) {}

std::string CommentsService::hl() const { return client_->locale().hl.toStdString(); }

CommentsEntry CommentsService::parseEntry(const QJsonObject &next, const std::string &hl) {
    CommentsEntry e;
    bool sectionFound = false;
    QString panelToken, mainToken, message;

    // The comments engagement panel: count, sort menu and the Top token.
    for (const QJsonValue &pv : next.value(QStringLiteral("engagementPanels")).toArray()) {
        const QJsonObject panel = pv.toObject().value(QStringLiteral("engagementPanelSectionListRenderer")).toObject();
        if (panel.isEmpty()) continue;
        bool isComments = str(panel, "panelIdentifier").contains(QStringLiteral("comments"))
                          || str(panel, "targetId").contains(QStringLiteral("comments"));
        QJsonObject section;
        json::forEachKey(panel.value(QStringLiteral("content")), QStringLiteral("itemSectionRenderer"),
                         [&section](const QJsonObject &s) {
            if (s.value(QStringLiteral("sectionIdentifier")).toString() != kCommentSection) return true;
            section = s;
            return false;
        });
        if (!section.isEmpty()) isComments = true;
        if (!isComments) continue;
        const QJsonObject header = json::at(panel, QStringLiteral("header.engagementPanelTitleHeaderRenderer")).toObject();
        if (e.total < 0) totalFrom(header.value(QStringLiteral("contextualInfo")), hl, &e.totalText, &e.total);
        if (e.sorts.empty()) e.sorts = parseSorts(json::findFirst(header, QStringLiteral("sortFilterSubMenuRenderer")));
        if (!section.isEmpty()) {
            sectionFound = true;
            if (panelToken.isEmpty()) panelToken = sectionToken(section);
            if (message.isEmpty()) message = sectionMessage(section);
        }
    }

    // The main results' comments section.
    json::forEachKey(next.value(QStringLiteral("contents")), QStringLiteral("itemSectionRenderer"),
                     [&](const QJsonObject &s) {
        if (s.value(QStringLiteral("sectionIdentifier")).toString() != kCommentSection) return true;
        sectionFound = true;
        if (mainToken.isEmpty()) mainToken = sectionToken(s);
        if (message.isEmpty()) message = sectionMessage(s);
        const QJsonObject h = json::at(s, QStringLiteral("header.commentsHeaderRenderer")).toObject();
        if (e.total < 0 && !h.isEmpty()) totalFrom(h.value(QStringLiteral("countText")), hl, &e.totalText, &e.total);
        if (e.total < 0 && !h.isEmpty()) totalFrom(h.value(QStringLiteral("commentsCount")), hl, &e.totalText, &e.total);
        return false;
    });

    e.token = (!panelToken.isEmpty() ? panelToken : mainToken).toStdString();
    e.message = message.toStdString();
    if (!e.token.empty()) {
        e.outcome = Outcome::Ok;
    } else if (sectionFound) {
        // The section is there but offers nothing to page: comments are off (or zero).
        e.outcome = Outcome::ValidEmpty;
        e.commentsOff = true;
    } else if (json::containsKey(next, QStringList() << QStringLiteral("liveChatRenderer"))) {
        // Live streams carry a chat instead of a comments section.
        e.outcome = Outcome::ValidEmpty;
        e.commentsOff = true;
    } else {
        e.outcome = Outcome::ShapeDrift;
    }
    return e;
}

CommentsPage CommentsService::parsePage(const QJsonObject &answer, const std::string &hl, bool replies) {
    CommentsPage page;
    struct Pending { QJsonObject vm; QString repliesToken; QString replyCountText; };
    std::vector<Pending> pending;
    std::vector<Comment> inlineBodies;   // older generation, no entity join
    std::vector<int> order;              // >= 0: pending index; < 0: -(inline index) - 1
    bool container = false;

    for (const QJsonValue &ev : answer.value(QStringLiteral("onResponseReceivedEndpoints")).toArray()) {
        const QJsonObject endpoint = ev.toObject();
        QJsonObject action = obj(endpoint, "reloadContinuationItemsCommand");
        if (action.isEmpty()) action = obj(endpoint, "appendContinuationItemsAction");
        if (action.isEmpty()) continue;
        container = true;
        for (const QJsonValue &iv : action.value(QStringLiteral("continuationItems")).toArray()) {
            const QJsonObject item = iv.toObject();
            if (item.contains(QStringLiteral("commentsHeaderRenderer"))) {
                const QJsonObject h = obj(item, "commentsHeaderRenderer");
                if (page.total < 0) totalFrom(h.value(QStringLiteral("countText")), hl, &page.totalText, &page.total);
                if (page.total < 0) totalFrom(h.value(QStringLiteral("commentsCount")), hl, &page.totalText, &page.total);
                if (page.sorts.empty())
                    page.sorts = parseSorts(json::findFirst(h.value(QStringLiteral("sortMenu")), QStringLiteral("sortFilterSubMenuRenderer")));
            } else if (item.contains(QStringLiteral("commentThreadRenderer"))) {
                const QJsonObject thread = obj(item, "commentThreadRenderer");
                const QJsonObject vm = viewModel(thread);
                const QJsonObject repliesRenderer = json::at(thread, QStringLiteral("replies.commentRepliesRenderer")).toObject();
                if (!vm.isEmpty()) {
                    Pending p;
                    p.vm = vm;
                    p.repliesToken = json::continuationToken(repliesRenderer);
                    p.replyCountText = json::text(json::at(repliesRenderer, QStringLiteral("viewReplies.buttonRenderer.text")));
                    order.push_back(static_cast<int>(pending.size()));
                    pending.push_back(p);
                } else {
                    const QJsonObject cr = json::at(thread, QStringLiteral("comment.commentRenderer")).toObject();
                    Comment c;
                    if (!cr.isEmpty() && fromCommentRenderer(cr, hl, &c)) {
                        c.replyContinuation = json::continuationToken(repliesRenderer).toStdString();
                        order.push_back(-static_cast<int>(inlineBodies.size()) - 1);
                        inlineBodies.push_back(c);
                    }
                }
            } else if (item.contains(QStringLiteral("commentViewModel"))) {
                Pending p;
                p.vm = viewModel(item);
                order.push_back(static_cast<int>(pending.size()));
                pending.push_back(p);
            } else if (item.contains(QStringLiteral("commentRenderer"))) {
                Comment c;
                if (fromCommentRenderer(obj(item, "commentRenderer"), hl, &c)) {
                    order.push_back(-static_cast<int>(inlineBodies.size()) - 1);
                    inlineBodies.push_back(c);
                }
            } else if (item.contains(QStringLiteral("continuationItemRenderer"))) {
                const QString t = json::continuationToken(obj(item, "continuationItemRenderer"));
                if (!t.isEmpty()) page.continuation = t.toStdString();
            }
        }
    }
    if (!container) {
        page.outcome = Outcome::ShapeDrift;
        return page;
    }

    // Entity bodies by key. The mutation order carries no meaning.
    QHash<QString, QJsonObject> bodies;
    if (!pending.empty()) {
        for (const QJsonValue &mv : json::at(answer, QStringLiteral("frameworkUpdates.entityBatchUpdate.mutations")).toArray()) {
            const QJsonObject m = mv.toObject();
            const QJsonObject entity = json::at(m, QStringLiteral("payload.commentEntityPayload")).toObject();
            if (entity.isEmpty()) continue;
            QString key = str(entity, "key");
            if (key.isEmpty()) key = str(m, "entityKey");
            if (!key.isEmpty()) bodies.insert(key, entity);
        }
    }

    for (int ref : order) {
        if (ref < 0) {
            Comment c = inlineBodies[static_cast<std::size_t>(-ref - 1)];
            if (replies) c.isReply = true;
            page.comments.push_back(c);
            continue;
        }
        const Pending &p = pending[static_cast<std::size_t>(ref)];
        const auto it = bodies.constFind(str(p.vm, "commentKey"));
        Comment c;
        if (it == bodies.constEnd() || !fromEntity(p.vm, it.value(), hl, &c)) {
            ++page.missingBodies;
            continue;
        }
        if (replies) c.isReply = true;
        c.replyContinuation = p.repliesToken.toStdString();
        if (c.replyCountText.empty() && !p.replyCountText.isEmpty()) {
            c.replyCountText = p.replyCountText.toStdString();
            c.replyCount = parseCountText(c.replyCountText, hl);
        }
        page.comments.push_back(c);
    }

    if (!page.comments.empty()) page.outcome = Outcome::Ok;
    else if (page.missingBodies > 0) page.outcome = Outcome::ShapeDrift;  // view models without bodies
    else page.outcome = Outcome::ValidEmpty;                               // containers present, nothing in them
    return page;
}

quint64 CommentsService::fetchEntry(const QString &videoId, QObject *context, EntryCallback done) {
    QJsonObject body;
    body.insert(QStringLiteral("videoId"), videoId);
    body.insert(QStringLiteral("contentCheckOk"), true);
    body.insert(QStringLiteral("racyCheckOk"), true);
    const std::string lang = hl();
    CatalogCounters *counters = counters_;
    std::function<CommentsEntry(const InnerTubeReply &)> parse = [lang, counters](const InnerTubeReply &reply) {
        CommentsEntry e;
        if (reply.outcome != Outcome::Ok) e.outcome = reply.outcome;
        else e = parseEntry(reply.json, lang);
        countOutcome(counters, e.outcome);
        return e;
    };
    return client_->post<CommentsEntry>(QLatin1String(innertube::kEndpointNext), body, context, parse, done);
}

namespace {
QJsonObject continuationBody(const QString &token) {
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), token);
    return body;
}
}

quint64 CommentsService::fetchThreads(const QString &token, QObject *context, PageCallback done) {
    const std::string lang = hl();
    CatalogCounters *counters = counters_;
    std::function<CommentsPage(const InnerTubeReply &)> parse = [lang, counters](const InnerTubeReply &reply) {
        CommentsPage p;
        if (reply.outcome != Outcome::Ok) p.outcome = reply.outcome;
        else p = parsePage(reply.json, lang, false);
        countOutcome(counters, p.outcome);
        return p;
    };
    return client_->post<CommentsPage>(QLatin1String(innertube::kEndpointNext), continuationBody(token), context, parse, done);
}

quint64 CommentsService::fetchReplies(const QString &token, QObject *context, PageCallback done) {
    const std::string lang = hl();
    CatalogCounters *counters = counters_;
    std::function<CommentsPage(const InnerTubeReply &)> parse = [lang, counters](const InnerTubeReply &reply) {
        CommentsPage p;
        if (reply.outcome != Outcome::Ok) p.outcome = reply.outcome;
        else p = parsePage(reply.json, lang, true);
        countOutcome(counters, p.outcome);
        return p;
    };
    return client_->post<CommentsPage>(QLatin1String(innertube::kEndpointNext), continuationBody(token), context, parse, done);
}

void CommentsService::cancel(quint64 id) { if (id) client_->cancel(id); }

}}}
