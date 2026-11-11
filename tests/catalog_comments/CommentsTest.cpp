// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Comments (DSC-16) and the host side of NFR-PERF-08. Offline against the recorded fixtures
// (comments_p1/p2, comments_replies_p1/p2, next_en). Derived variants are built in code and
// say so where they are made.

#include "tests/catalog_common/TestSupport.h"

#include "Clients/Sailfish/Presentation/CommentsModel.h"
#include "Core/Catalog/Domain/Comments.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>
#include <algorithm>
#include <set>

using namespace nordtile::video::catalog;
using nordtile::video::sailfish::CommentsModel;
using catalog_test::FakeTransport;
using catalog_test::fixture;
using catalog_test::fixtureJson;
using catalog_test::waitFor;

namespace {

const char *kVideo = "9zCH37330f8";

// ------------------------------------------------------------------ derived-fixture helpers

// Rewrites every string value stored under one of `keys` (anywhere) by appending `suffix`.
// Used to make a recorded page look like another page with new ids and entity keys; the
// key join keeps working because view-model keys and entity keys get the same suffix.
QJsonValue suffixIds(const QJsonValue &v, const QString &suffix) {
    static const QSet<QString> keys = QSet<QString>() << QStringLiteral("commentId") << QStringLiteral("commentKey")
                                                      << QStringLiteral("key") << QStringLiteral("entityKey");
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue &x : v.toArray()) out.append(suffixIds(x, suffix));
        return out;
    }
    if (!v.isObject()) return v;
    const QJsonObject o = v.toObject();
    QJsonObject out;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) {
        if (keys.contains(it.key()) && it.value().isString()) out.insert(it.key(), it.value().toString() + suffix);
        else out.insert(it.key(), suffixIds(it.value(), suffix));
    }
    return out;
}

// Replaces every string equal to `from` by `to` (continuation tokens).
QJsonValue replaceString(const QJsonValue &v, const QString &from, const QString &to) {
    if (v.isString()) return v.toString() == from ? QJsonValue(to) : v;
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue &x : v.toArray()) out.append(replaceString(x, from, to));
        return out;
    }
    if (!v.isObject()) return v;
    const QJsonObject o = v.toObject();
    QJsonObject out;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) out.insert(it.key(), replaceString(it.value(), from, to));
    return out;
}

// Keeps only the items of each onResponseReceivedEndpoints action that `keep` accepts.
QJsonObject filterItems(const QJsonObject &answer, const std::function<bool(const QJsonObject &)> &keep) {
    QJsonObject out = answer;
    QJsonArray endpoints;
    const QStringList actions = QStringList() << QStringLiteral("reloadContinuationItemsCommand") << QStringLiteral("appendContinuationItemsAction");
    for (const QJsonValue &ev : answer.value(QStringLiteral("onResponseReceivedEndpoints")).toArray()) {
        QJsonObject e = ev.toObject();
        for (const QString &name : actions) {
            if (!e.contains(name)) continue;
            QJsonObject action = e.value(name).toObject();
            QJsonArray items;
            for (const QJsonValue &iv : action.value(QStringLiteral("continuationItems")).toArray())
                if (keep(iv.toObject())) items.append(iv);
            action.insert(QStringLiteral("continuationItems"), items);
            e.insert(name, action);
        }
        endpoints.append(e);
    }
    out.insert(QStringLiteral("onResponseReceivedEndpoints"), endpoints);
    return out;
}

QJsonObject withMutations(const QJsonObject &answer, const QJsonArray &mutations) {
    QJsonObject fu = answer.value(QStringLiteral("frameworkUpdates")).toObject();
    QJsonObject ebu = fu.value(QStringLiteral("entityBatchUpdate")).toObject();
    ebu.insert(QStringLiteral("mutations"), mutations);
    fu.insert(QStringLiteral("entityBatchUpdate"), ebu);
    QJsonObject out = answer;
    out.insert(QStringLiteral("frameworkUpdates"), fu);
    return out;
}

QJsonArray mutationsOf(const QJsonObject &answer) {
    return answer.value(QStringLiteral("frameworkUpdates")).toObject().value(QStringLiteral("entityBatchUpdate"))
        .toObject().value(QStringLiteral("mutations")).toArray();
}

// Derived watch-next: every comments item section holds a message instead of a continuation.
QJsonValue commentsTurnedOff(const QJsonValue &v) {
    if (v.isArray()) {
        QJsonArray out;
        for (const QJsonValue &x : v.toArray()) out.append(commentsTurnedOff(x));
        return out;
    }
    if (!v.isObject()) return v;
    QJsonObject o = v.toObject();
    if (o.value(QStringLiteral("sectionIdentifier")).toString() == QStringLiteral("comment-item-section")) {
        QJsonArray contents;
        contents.append(QJsonDocument::fromJson("{\"messageRenderer\":{\"text\":{\"runs\":[{\"text\":\"Comments are turned off.\"}]}}}").object());
        o.insert(QStringLiteral("contents"), contents);
        return o;
    }
    QJsonObject out;
    for (auto it = o.constBegin(); it != o.constEnd(); ++it) out.insert(it.key(), commentsTurnedOff(it.value()));
    return out;
}

QByteArray bytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray tokenBody(const std::string &token) { return QByteArray("\"continuation\":\"") + QByteArray::fromStdString(token) + "\""; }

CommentsPage page(const char *name, bool replies) {
    return CommentsService::parsePage(fixtureJson(QString::fromLatin1(name) + QStringLiteral(".json")), "en", replies);
}

// Page k of an endless derived list: comments_p2 with ids suffixed "-k" and its continuation
// renamed to `nextToken` (empty: the last page, continuation item removed).
QJsonObject derivedPage(int k, const QString &nextToken) {
    const QJsonObject base = fixtureJson(QStringLiteral("comments_p2.json"));
    const QString original = QString::fromStdString(CommentsService::parsePage(base, "en", false).continuation);
    QJsonObject o = suffixIds(base, QStringLiteral("-%1").arg(k)).toObject();
    if (nextToken.isEmpty())
        o = filterItems(o, [](const QJsonObject &item) { return !item.contains(QStringLiteral("continuationItemRenderer")); });
    else
        o = replaceString(o, original, nextToken).toObject();
    return o;
}

// The background discovery stack of the app, over a fake transport.
struct Stack {
    FakeTransport fake;
    WorkLane lane;
    InnerTubeClient client;
    CatalogCounters counters;
    CommentsService service;
    Stack() : lane(QStringLiteral("background")), client(&fake, nullptr, &lane, DiscoveryLocale()), service(&client, &counters) {}
};

struct Scheduled { int ms; std::function<void()> fire; };

std::string entryToken() { return CommentsService::parseEntry(fixtureJson(QStringLiteral("next_en.json")), "en").token; }

// A default top-level request over the recorded pages: p1 + p2 + the derived last page.
const int kRequestsPerLoad = 3;

// Serves next_en, comments_p1 (the entry token), comments_p2 and the two replies pages.
void serveRecorded(FakeTransport &fake) {
    const CommentsPage p1 = page("comments_p1", false);
    const CommentsPage r1 = page("comments_replies_p1", true);
    fake.on(QStringLiteral("youtubei/v1/next"), QByteArray("\"videoId\":\"") + kVideo + "\"", 200, fixture(QStringLiteral("next_en.json")));
    fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(entryToken()), 200, fixture(QStringLiteral("comments_p1.json")));
    fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(p1.continuation), 200, fixture(QStringLiteral("comments_p2.json")));
    // Derived: a last page behind comments_p2 (its copy with fresh ids), so a 50-thread request
    // completes on recorded data.
    fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(page("comments_p2", false).continuation), 200, bytes(derivedPage(3, QString())));
    fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(p1.comments.front().replyContinuation), 200, fixture(QStringLiteral("comments_replies_p1.json")));
    fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(r1.continuation), 200, fixture(QStringLiteral("comments_replies_p2.json")));
}

int rowOfType(const CommentsModel &m, const QString &type, int from = 0) {
    for (int i = from; i < m.rowCount(); ++i)
        if (m.data(m.index(i, 0), CommentsModel::RowTypeRole).toString() == type) return i;
    return -1;
}

QString role(const CommentsModel &m, int row, int r) { return m.data(m.index(row, 0), r).toString(); }

// ------------------------------------------------------------------ parsers

void testFirstPage() {
    const CommentsPage p = page("comments_p1", false);
    CHECK(p.outcome == Outcome::Ok);
    CHECK(p.comments.size() >= 15);
    CHECK_EQ(p.missingBodies, 0);
    CHECK(!p.continuation.empty());
    int withAuthor = 0, withText = 0, withLikes = 0, withReplies = 0, withAge = 0;
    std::set<std::string> ids;
    for (const Comment &c : p.comments) {
        ids.insert(c.id);
        if (!c.author.empty() && !c.authorChannelId.empty()) ++withAuthor;
        if (!c.text.empty()) ++withText;
        if (c.likeCount >= 0) ++withLikes;
        if (!c.replyContinuation.empty() && c.replyCount > 0) ++withReplies;
        if (c.publishedAgeSeconds > 0) ++withAge;
        CHECK(!c.isReply);
        CHECK(c.avatarUrl.compare(0, 8, "https://") == 0);
    }
    CHECK_EQ(ids.size(), p.comments.size());
    CHECK(withAuthor >= 15);
    CHECK(withText >= 15);
    CHECK(withLikes >= 15);
    CHECK(withAge >= 15);
    CHECK(withReplies >= 1);
    // The recorded first thread is the uploader's pinned comment.
    CHECK(p.comments.front().pinned);
    CHECK(p.comments.front().creator);
}

void testTotalAndSortTokens() {
    // Watch-next: the comments engagement panel carries the count and the sort menu.
    const CommentsEntry e = CommentsService::parseEntry(fixtureJson(QStringLiteral("next_en.json")), "en");
    CHECK(e.outcome == Outcome::Ok);
    CHECK(!e.token.empty());
    CHECK(!e.totalText.empty());
    CHECK(e.total >= 1000);
    CHECK_EQ(e.sorts.size(), std::size_t(2));
    if (e.sorts.size() == 2) {
        CHECK(e.sorts[0].sort == CommentSort::Top);
        CHECK(e.sorts[1].sort == CommentSort::Newest);
        CHECK(!e.sorts[0].token.empty() && !e.sorts[1].token.empty());
        CHECK(e.sorts[0].token != e.sorts[1].token);
        CHECK(e.sorts[0].selected);
    }
    // The page header's countText wins when it carries a number. Derived: the recorded panel
    // answer has a header without countText; give it the classic "7,712" + " Comments" runs and
    // a sort menu.
    QJsonObject p1 = fixtureJson(QStringLiteral("comments_p1.json"));
    QJsonArray endpoints = p1.value(QStringLiteral("onResponseReceivedEndpoints")).toArray();
    QJsonObject first = endpoints.at(0).toObject();
    QJsonObject cmd = first.value(QStringLiteral("reloadContinuationItemsCommand")).toObject();
    QJsonArray items = cmd.value(QStringLiteral("continuationItems")).toArray();
    QJsonObject header = items.at(0).toObject().value(QStringLiteral("commentsHeaderRenderer")).toObject();
    header.insert(QStringLiteral("countText"), QJsonDocument::fromJson("{\"runs\":[{\"text\":\"7,712\"},{\"text\":\" Comments\"}]}").object());
    header.insert(QStringLiteral("sortMenu"), QJsonDocument::fromJson(
        "{\"sortFilterSubMenuRenderer\":{\"subMenuItems\":["
        "{\"title\":\"Top comments\",\"selected\":true,\"serviceEndpoint\":{\"continuationCommand\":{\"token\":\"TOP-T\"}}},"
        "{\"title\":\"Newest first\",\"selected\":false,\"serviceEndpoint\":{\"continuationCommand\":{\"token\":\"NEW-T\"}}}]}}").object());
    QJsonObject wrapped;
    wrapped.insert(QStringLiteral("commentsHeaderRenderer"), header);
    items.replace(0, wrapped);
    cmd.insert(QStringLiteral("continuationItems"), items);
    first.insert(QStringLiteral("reloadContinuationItemsCommand"), cmd);
    endpoints.replace(0, first);
    p1.insert(QStringLiteral("onResponseReceivedEndpoints"), endpoints);
    const CommentsPage p = CommentsService::parsePage(p1, "en", false);
    CHECK_EQ(p.total, std::int64_t(7712));
    CHECK_EQ(p.totalText, std::string("7,712"));
    CHECK_EQ(p.sorts.size(), std::size_t(2));
    if (p.sorts.size() == 2) CHECK_EQ(p.sorts[1].token, std::string("NEW-T"));
    // The recorded header (no number) yields no total rather than a wrong one.
    CHECK_EQ(page("comments_p1", false).total, std::int64_t(-1));
    // The main-results token (what the app follows) answers with a numbered header and a sort
    // menu: recorded as comments_section_p1.
    const CommentsPage real = CommentsService::parsePage(fixtureJson(QStringLiteral("comments_section_p1.json")), "en", false);
    CHECK(real.outcome == Outcome::Ok);
    CHECK(real.total >= 1000);
    CHECK(!real.totalText.empty());
    CHECK(real.comments.size() >= 15);
    CHECK(!real.continuation.empty());
    CHECK_EQ(real.sorts.size(), std::size_t(2));
    if (real.sorts.size() == 2) {
        CHECK(real.sorts[0].sort == CommentSort::Top && real.sorts[1].sort == CommentSort::Newest);
        CHECK(!real.sorts[1].token.empty() && real.sorts[0].token != real.sorts[1].token);
    }
    CHECK_EQ(real.missingBodies, 0);
}

void testSecondPage() {
    const CommentsPage p1 = page("comments_p1", false);
    const CommentsPage p2 = page("comments_p2", false);
    CHECK(p2.outcome == Outcome::Ok);
    CHECK(p2.comments.size() >= 15);
    CHECK(!p2.continuation.empty());
    CHECK(p2.continuation != p1.continuation);
    std::set<std::string> ids;
    for (const Comment &c : p1.comments) ids.insert(c.id);
    int overlap = 0;
    for (const Comment &c : p2.comments) overlap += ids.count(c.id) ? 1 : 0;
    CHECK_EQ(overlap, 0);
}

void testRepliesPages() {
    const CommentsPage p1 = page("comments_p1", false);
    const CommentsPage r1 = page("comments_replies_p1", true);
    const CommentsPage r2 = page("comments_replies_p2", true);
    CHECK(r1.outcome == Outcome::Ok);
    CHECK(r1.comments.size() >= 5);
    CHECK(!r1.continuation.empty());
    CHECK(r2.outcome == Outcome::Ok);
    CHECK(r2.comments.size() >= 5);
    std::set<std::string> ids;
    for (const Comment &c : r1.comments) {
        CHECK(c.isReply);
        CHECK(!c.author.empty());
        CHECK(!c.text.empty());
        // Replies belong to the recorded thread: "<thread id>.<reply id>".
        CHECK(c.id.compare(0, p1.comments.front().id.size() + 1, p1.comments.front().id + ".") == 0);
        ids.insert(c.id);
    }
    int overlap = 0;
    for (const Comment &c : r2.comments) { CHECK(c.isReply); overlap += ids.count(c.id) ? 1 : 0; }
    CHECK_EQ(overlap, 0);
}

void testKeyJoin() {
    // Derived: the same answer with its mutations reversed and rotated must parse identically
    // (bodies are joined by key, never by order).
    const QJsonObject base = fixtureJson(QStringLiteral("comments_p1.json"));
    QJsonArray muts = mutationsOf(base);
    QJsonArray shuffled;
    for (int i = muts.size() - 1; i >= 0; --i) shuffled.append(muts.at((i * 7 + 3) % muts.size()));
    CHECK_EQ(shuffled.size(), muts.size());
    const CommentsPage a = CommentsService::parsePage(base, "en", false);
    const CommentsPage b = CommentsService::parsePage(withMutations(base, shuffled), "en", false);
    CHECK_EQ(a.comments.size(), b.comments.size());
    bool same = a.comments.size() == b.comments.size();
    for (std::size_t i = 0; same && i < a.comments.size(); ++i) {
        same = a.comments[i].id == b.comments[i].id && a.comments[i].text == b.comments[i].text
               && a.comments[i].author == b.comments[i].author && a.comments[i].likeText == b.comments[i].likeText
               && a.comments[i].replyCount == b.comments[i].replyCount && a.comments[i].key == b.comments[i].key;
    }
    CHECK(same);
    // Each body belongs to its own view model: the id in the body matches the view model's.
    const QJsonObject thread = base.value(QStringLiteral("onResponseReceivedEndpoints")).toArray().at(1).toObject()
        .value(QStringLiteral("reloadContinuationItemsCommand")).toObject().value(QStringLiteral("continuationItems")).toArray()
        .at(2).toObject().value(QStringLiteral("commentThreadRenderer")).toObject();
    const QString vmId = thread.value(QStringLiteral("commentViewModel")).toObject().value(QStringLiteral("commentViewModel"))
        .toObject().value(QStringLiteral("commentId")).toString();
    CHECK_EQ(b.comments.at(2).id, vmId.toStdString());

    // Derived: one entity removed -> that comment is dropped and counted, the others stay.
    QJsonArray fewer;
    bool removed = false;
    for (const QJsonValue &m : muts) {
        if (!removed && m.toObject().value(QStringLiteral("payload")).toObject().contains(QStringLiteral("commentEntityPayload"))) { removed = true; continue; }
        fewer.append(m);
    }
    const CommentsPage c = CommentsService::parsePage(withMutations(base, fewer), "en", false);
    CHECK_EQ(c.missingBodies, 1);
    CHECK_EQ(c.comments.size() + 1, a.comments.size());
    CHECK(c.outcome == Outcome::Ok);
}

void testValidEmptyVersusDrift() {
    const QJsonObject p1 = fixtureJson(QStringLiteral("comments_p1.json"));
    // Derived: no threads and no continuation, containers intact -> valid empty (zero comments).
    const QJsonObject empty = filterItems(p1, [](const QJsonObject &item) { return item.contains(QStringLiteral("commentsHeaderRenderer")); });
    const CommentsPage e = CommentsService::parsePage(empty, "en", false);
    CHECK(e.outcome == Outcome::ValidEmpty);
    CHECK(e.comments.empty());
    CHECK(e.continuation.empty());
    // Derived: the endpoints container missing -> drift.
    QJsonObject noEndpoints = p1;
    noEndpoints.remove(QStringLiteral("onResponseReceivedEndpoints"));
    CHECK(CommentsService::parsePage(noEndpoints, "en", false).outcome == Outcome::ShapeDrift);
    // Derived: view models present but the entity store missing -> drift.
    QJsonObject noBodies = p1;
    noBodies.remove(QStringLiteral("frameworkUpdates"));
    const CommentsPage nb = CommentsService::parsePage(noBodies, "en", false);
    CHECK(nb.outcome == Outcome::ShapeDrift);
    CHECK(nb.missingBodies >= 15);

    // Derived watch-next: the comments sections hold a message instead of a continuation ->
    // valid empty, comments off.
    const QJsonObject next = fixtureJson(QStringLiteral("next_en.json"));
    const CommentsEntry eoffInput = CommentsService::parseEntry(next, "en");
    CHECK(eoffInput.outcome == Outcome::Ok);
    const QJsonObject off = commentsTurnedOff(next).toObject();
    const CommentsEntry eoff = CommentsService::parseEntry(off, "en");
    CHECK(eoff.outcome == Outcome::ValidEmpty);
    CHECK(eoff.commentsOff);
    CHECK(eoff.token.empty());
    CHECK_EQ(eoff.message, std::string("Comments are turned off."));
    // Derived watch-next: no comments section and no engagement panels -> drift.
    QJsonObject bare;
    bare.insert(QStringLiteral("responseContext"), next.value(QStringLiteral("responseContext")));
    bare.insert(QStringLiteral("contents"), QJsonDocument::fromJson("{\"twoColumnWatchNextResults\":{\"results\":{\"results\":{\"contents\":[]}}}}").object());
    CHECK(CommentsService::parseEntry(bare, "en").outcome == Outcome::ShapeDrift);
}

// ------------------------------------------------------------------ domain policies

CommentsPage syntheticPage(int first, int count, const std::string &next, bool replies = false) {
    CommentsPage p;
    p.outcome = count > 0 ? Outcome::Ok : Outcome::ValidEmpty;
    for (int i = 0; i < count; ++i) {
        Comment c;
        c.id = (replies ? "r" : "t") + std::to_string(first + i);
        c.text = "x";
        c.isReply = replies;
        c.replyContinuation = replies ? std::string() : "R" + std::to_string(first + i);
        p.comments.push_back(c);
    }
    p.continuation = next;
    return p;
}

void testSessionBudgets() {
    CommentsSession s;
    s.reset("P1");
    CHECK(s.canRequestMore());
    // Top level: 20 per page, endless -> one request walks 3 pages and keeps 50 (10 buffered).
    s.beginRequest();
    int page = 0;
    while (s.requestNeedsAnotherPage()) { s.applyThreadPage(syntheticPage(page * 20, 20, "P" + std::to_string(page + 2))); ++page; }
    CHECK_EQ(page, 3);
    CHECK_EQ(s.threadCount(), std::size_t(50));
    CHECK_EQ(s.bufferedThreads(), 10);
    // The next request hands out the buffer first, then walks on.
    CHECK_EQ(s.beginRequest(), 10);
    int more = 0;
    while (s.requestNeedsAnotherPage()) { s.applyThreadPage(syntheticPage((page + more) * 20, 20, "P")); ++more; }
    CHECK_EQ(more, 2);
    CHECK_EQ(s.threadCount(), std::size_t(100));
    // Duplicates across pages are not added twice.
    s.beginRequest();
    s.applyThreadPage(syntheticPage(0, 20, "P"));
    CHECK_EQ(s.threadCount(), std::size_t(100));   // the buffer is empty and all 20 are repeats

    // 10 pages per request, even when pages come back empty with a token.
    CommentsSession e;
    e.reset("E1");
    e.beginRequest();
    int pages = 0;
    while (e.requestNeedsAnotherPage()) { e.applyThreadPage(syntheticPage(0, 0, "E")); ++pages; }
    CHECK_EQ(pages, 10);
    CHECK(e.canRequestMore());

    // Replies: 12 per request on a thread; a 43-reply page keeps 31 in the buffer.
    CommentsSession r;
    r.reset("P1");
    r.beginRequest();
    r.applyThreadPage(syntheticPage(0, 20, ""));
    CHECK(!r.requestNeedsAnotherPage());
    CHECK(r.canLoadMoreReplies(0));
    CHECK_EQ(r.replyToken(0), std::string("R0"));
    CHECK_EQ(r.beginReplyRequest(0), 0);
    CHECK(r.replyRequestNeedsAnotherPage(0));
    CHECK_EQ(r.applyReplyPage(0, syntheticPage(0, 43, "R0-2", true)), 12);
    CHECK(!r.replyRequestNeedsAnotherPage(0));
    CHECK_EQ(r.bufferedReplies(0), 31);
    CHECK_EQ(r.beginReplyRequest(0), 12);           // from the buffer, no fetch
    CHECK(!r.replyRequestNeedsAnotherPage(0));
    CHECK_EQ(r.thread(0).replies.size(), std::size_t(24));
    // One page per tap: a 10-reply page satisfies the request without walking on.
    CHECK_EQ(r.beginReplyRequest(1), 0);
    CHECK_EQ(r.applyReplyPage(1, syntheticPage(100, 10, "R1-2", true)), 10);
    CHECK(!r.replyRequestNeedsAnotherPage(1));
    CHECK(r.canLoadMoreReplies(1));
    // 150 replies in total per list.
    int thread = 2;
    while (r.totalReplies() < 150 && thread < 20) {
        r.beginReplyRequest(static_cast<std::size_t>(thread));
        r.applyReplyPage(static_cast<std::size_t>(thread), syntheticPage(1000 * thread, 43, "more", true));
        ++thread;
    }
    CHECK_EQ(r.totalReplies(), 150);
    for (std::size_t t = 0; t < 20; ++t) CHECK(!r.canLoadMoreReplies(t));
    CHECK_EQ(r.beginReplyRequest(0), 0);
    CHECK(!r.replyRequestNeedsAnotherPage(0));
}

void testRevealWindow() {
    RevealWindow w(5);
    CHECK_EQ(w.shown(), 0);
    CHECK_EQ(w.grow(3), 3);
    CHECK_EQ(w.grow(20), 8);
    CHECK_EQ(w.grow(20), 13);
    CHECK_EQ(w.grow(5), 13);   // never shrinks
    CHECK_EQ(w.grow(0), 13);
    w.reset();
    CHECK_EQ(w.shown(), 0);
}

void testAutoLoadPolicy() {
    CHECK(autoLoadModeFromName("off") == AutoLoadMode::Off);
    CHECK(autoLoadModeFromName("playback") == AutoLoadMode::AfterPlayback);
    CHECK(autoLoadModeFromName("near") == AutoLoadMode::Near);
    CHECK(autoLoadModeFromName("bogus") == AutoLoadMode::Near);
    AutoLoadPolicy off(AutoLoadMode::Off);
    CHECK(!off.onScrolledNear());
    CHECK_EQ(off.onPlaybackStarted(0), std::int64_t(-1));
    CHECK(!off.onTimer(100000));
    AutoLoadPolicy near(AutoLoadMode::Near);
    CHECK_EQ(near.onPlaybackStarted(0), std::int64_t(-1));
    CHECK(near.onScrolledNear());
    near.markLoaded();
    CHECK(!near.onScrolledNear());
    AutoLoadPolicy later(AutoLoadMode::AfterPlayback);
    CHECK_EQ(later.onPlaybackStarted(1000), std::int64_t(3000));
    CHECK_EQ(later.onPlaybackStarted(2000), std::int64_t(2000));   // a resume keeps the deadline
    CHECK(!later.onTimer(3999));
    CHECK_EQ(later.remainingMs(3999), std::int64_t(1));
    CHECK(later.onTimer(4000));
    later.markLoaded();
    CHECK(!later.onTimer(5000));
    later.reset();
    CHECK(later.onScrolledNear());   // scrolling near loads sooner than the timer
}

// ------------------------------------------------------------------ presentation model

void testModelFlow(const QString &settings) {
    Stack st;
    serveRecorded(st.fake);
    CommentsModel model(&st.service, nullptr, settings);
    CommentBudgets b;
    b.topLevel = 20;   // one recorded page per request, so comments_p2 is the second request
    model.setBudgets(b);
    model.load(QString::fromLatin1(kVideo));   // no token: the model asks watch-next itself
    CHECK(model.busy());
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(model.errorText(), QString());
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 2);
    CHECK_EQ(model.rowCount(), 5);
    CHECK(!model.totalText().isEmpty());
    CHECK_EQ(model.footerText(), QStringLiteral("5 of ") + model.totalText());
    CHECK(model.canLoadMore());
    CHECK(st.fake.requests.at(0).url.toString().contains(QStringLiteral("youtubei/v1/next")));
    CHECK(st.counters.value(Area::Comments, Outcome::Ok) == 2);
    CHECK(!role(model, 0, CommentsModel::AuthorRole).isEmpty());
    CHECK(!role(model, 0, CommentsModel::TextRole).isEmpty());
    CHECK(!role(model, 0, CommentsModel::LikesRole).isEmpty());
    CHECK(model.data(model.index(0, 0), CommentsModel::PinnedRole).toBool());
    CHECK(model.data(model.index(0, 0), CommentsModel::IsCreatorRole).toBool());
    CHECK_EQ(role(model, 0, CommentsModel::RowTypeRole), QStringLiteral("thread"));

    // Reveal in steps from what is fetched; no network until the window reaches the end.
    model.loadMore(); model.loadMore(); model.loadMore();
    CHECK_EQ(model.rowCount(), 20);
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 2);
    model.loadMore();   // the second request: comments_p2
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 3);
    CHECK_EQ(model.rowCount(), 25);
    CHECK_EQ(model.shownCount(), 25);

    // Replies on demand: expand -> replies p1 only.
    CHECK(model.data(model.index(0, 0), CommentsModel::CanLoadMoreRepliesRole).toBool());
    model.toggleReplies(0);
    CHECK(model.data(model.index(0, 0), CommentsModel::RepliesExpandedRole).toBool());
    CHECK_EQ(rowOfType(model, QStringLiteral("more")), 1);   // the loading row
    CHECK(model.data(model.index(1, 0), CommentsModel::RepliesBusyRole).toBool());
    CHECK(waitFor([&] { return !model.data(model.index(0, 0), CommentsModel::RepliesBusyRole).toBool(); }));
    const std::size_t r1 = page("comments_replies_p1", true).comments.size();
    CHECK_EQ(model.session().thread(0).replies.size(), std::min<std::size_t>(r1, 12));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 4);
    const int more = rowOfType(model, QStringLiteral("more"));
    CHECK_EQ(more, 1 + static_cast<int>(model.session().thread(0).replies.size()));
    CHECK_EQ(role(model, 1, CommentsModel::RowTypeRole), QStringLiteral("reply"));
    CHECK(model.data(model.index(1, 0), CommentsModel::IsReplyRole).toBool());
    CHECK(model.data(model.index(more, 0), CommentsModel::CanLoadMoreRepliesRole).toBool());
    // "More replies" -> replies p2 (a second reply page, on demand).
    model.loadMoreReplies(more);
    CHECK(waitFor([&] { return !model.data(model.index(0, 0), CommentsModel::RepliesBusyRole).toBool(); }));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 5);
    CHECK_EQ(model.session().thread(0).replies.size(), std::min<std::size_t>(r1, 12) + 12);
    // The rest of replies p2 is buffered: the next tap needs no request.
    const int more2 = rowOfType(model, QStringLiteral("more"));
    model.loadMoreReplies(more2);
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 5);
    CHECK_EQ(model.session().thread(0).replies.size(), std::min<std::size_t>(r1, 12) + 24);
    CHECK_EQ(model.rowCount(), 25 + static_cast<int>(model.session().thread(0).replies.size()) + 1);
    // Collapse: back to the thread rows, the reveal window unchanged.
    model.toggleReplies(0);
    CHECK_EQ(model.rowCount(), 25);
    CHECK_EQ(model.shownCount(), 25);
    CHECK(!model.data(model.index(0, 0), CommentsModel::RepliesExpandedRole).toBool());
    // Re-expanding shows the kept replies without a request.
    model.toggleReplies(0);
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), 5);
    CHECK(model.rowCount() > 25 + 24);
}

void testModelBudgetsWithRepeatedPages(const QString &settings) {
    // Derived: an endless list made of comments_p2 copies with fresh ids, PAGE-k -> PAGE-k+1.
    Stack st;
    for (int k = 1; k <= 12; ++k)
        st.fake.on(QStringLiteral("youtubei/v1/next"), QByteArray("\"PAGE-") + QByteArray::number(k) + "\"", 200,
                   bytes(derivedPage(k, QStringLiteral("PAGE-%1").arg(k + 1))));
    CommentsModel model(&st.service, nullptr, settings);
    model.load(QString::fromLatin1(kVideo), QStringLiteral("PAGE-1"));
    CHECK(waitFor([&] { return !model.busy(); }));
    const int perPage = static_cast<int>(page("comments_p2", false).comments.size());
    const int expectedPages = (50 + perPage - 1) / perPage;
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), expectedPages);
    CHECK_EQ(model.session().threadCount(), std::size_t(50));
    CHECK_EQ(model.session().bufferedThreads(), expectedPages * perPage - 50);
    for (int i = 0; i < 20 && model.shownCount() < 50; ++i) model.loadMore();
    CHECK_EQ(model.shownCount(), 50);
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), expectedPages);
    model.loadMore();   // second request: buffer first, then more pages
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(model.session().threadCount(), std::size_t(100));

    // Derived: empty pages that keep offering a continuation -> 10 pages, then stop.
    Stack empty;
    for (int k = 1; k <= 15; ++k) {
        QJsonObject p = derivedPage(k, QStringLiteral("EMPTY-%1").arg(k + 1));
        p = filterItems(p, [](const QJsonObject &item) { return !item.contains(QStringLiteral("commentThreadRenderer")); });
        empty.fake.on(QStringLiteral("youtubei/v1/next"), QByteArray("\"EMPTY-") + QByteArray::number(k) + "\"", 200, bytes(p));
    }
    CommentsModel m2(&empty.service, nullptr, settings);
    m2.load(QString::fromLatin1(kVideo), QStringLiteral("EMPTY-1"));
    CHECK(waitFor([&] { return !m2.busy(); }));
    CHECK_EQ(empty.fake.count(QStringLiteral("youtubei/v1/next")), 10);
    CHECK_EQ(m2.rowCount(), 0);
    CHECK_EQ(m2.errorText(), QString());
    CHECK(empty.counters.value(Area::Comments, Outcome::ValidEmpty) == 10);
    CHECK(empty.counters.driftTotal() == 0);

    // Derived: a finished list with zero comments -> the empty text, not an error.
    Stack none;
    none.fake.on(QStringLiteral("youtubei/v1/next"), QByteArray("\"NONE\""), 200,
                 bytes(filterItems(fixtureJson(QStringLiteral("comments_p1.json")),
                                   [](const QJsonObject &item) { return item.contains(QStringLiteral("commentsHeaderRenderer")); })));
    CommentsModel m3(&none.service, nullptr, settings);
    m3.load(QString::fromLatin1(kVideo), QStringLiteral("NONE"));
    CHECK(waitFor([&] { return !m3.busy(); }));
    CHECK(!m3.emptyText().isEmpty());
    CHECK_EQ(m3.errorText(), QString());
    CHECK(!m3.canLoadMore());
}

void testAutoLoadModes(const QString &dir) {
    const std::string token = entryToken();
    {   // Default "near": playback does nothing, scrolling near loads.
        Stack st;
        serveRecorded(st.fake);
        CommentsModel model(&st.service, nullptr, dir + QStringLiteral("/near.conf"));
        CHECK_EQ(model.autoLoadMode(), QStringLiteral("near"));
        std::vector<Scheduled> timers;
        model.setScheduler([&timers](int ms, std::function<void()> fire) { Scheduled s = { ms, fire }; timers.push_back(s); });
        model.setVideo(QString::fromLatin1(kVideo), QString::fromStdString(token));
        model.playbackStarted();
        CHECK(timers.empty());
        QCoreApplication::processEvents();
        CHECK_EQ(st.fake.requests.size(), 0);
        CHECK(!model.started());
        model.scrolledNear();
        CHECK(model.started());
        CHECK(waitFor([&] { return !model.busy(); }));
        CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), kRequestsPerLoad);   // the token was given: no watch-next
        CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next"), "videoId"), 0);
        CHECK(model.rowCount() > 0);
        CHECK_EQ(model.session().threadCount(), std::size_t(50));
        model.scrolledNear();   // once loaded, scrolling near is not a reload
        CHECK_EQ(st.fake.requests.size(), kRequestsPerLoad);
    }
    {   // Off: nothing loads by itself; an explicit start does.
        Stack st;
        serveRecorded(st.fake);
        CommentsModel model(&st.service, nullptr, dir + QStringLiteral("/off.conf"));
        model.setAutoLoadMode(QStringLiteral("off"));
        std::vector<Scheduled> timers;
        model.setScheduler([&timers](int ms, std::function<void()> fire) { Scheduled s = { ms, fire }; timers.push_back(s); });
        model.setVideo(QString::fromLatin1(kVideo), QString::fromStdString(token));
        model.playbackStarted();
        model.scrolledNear();
        model.loadMore();
        CHECK(timers.empty());
        QCoreApplication::processEvents();
        CHECK_EQ(st.fake.requests.size(), 0);
        CHECK(!model.started());
        model.start();
        CHECK(waitFor([&] { return !model.busy(); }));
        CHECK_EQ(st.fake.requests.size(), kRequestsPerLoad);
        QSettings saved(dir + QStringLiteral("/off.conf"), QSettings::IniFormat);
        CHECK_EQ(saved.value(QLatin1String(CommentsModel::settingsKey())).toString(), QStringLiteral("off"));
    }
    {   // After playback: waits 3 s (injected clock and timer), then loads.
        Stack st;
        serveRecorded(st.fake);
        const QString file = dir + QStringLiteral("/playback.conf");
        {
            CommentsModel writer(&st.service, nullptr, file);
            writer.setAutoLoadMode(QStringLiteral("playback"));
        }
        CommentsModel model(&st.service, nullptr, file);
        CHECK_EQ(model.autoLoadMode(), QStringLiteral("playback"));   // read back from the settings file
        qint64 clock = 10000;
        std::vector<Scheduled> timers;
        model.setClock([&clock] { return clock; });
        model.setScheduler([&timers](int ms, std::function<void()> fire) { Scheduled s = { ms, fire }; timers.push_back(s); });
        model.setVideo(QString::fromLatin1(kVideo), QString::fromStdString(token));
        model.playbackStarted();
        CHECK_EQ(timers.size(), std::size_t(1));
        if (timers.size() == 1) CHECK_EQ(timers[0].ms, 3000);
        clock += 1000;
        if (!timers.empty()) timers[0].fire();   // fired early: nothing loads, re-armed for the rest
        QCoreApplication::processEvents();
        CHECK(!model.started());
        CHECK_EQ(st.fake.requests.size(), 0);
        CHECK_EQ(timers.size(), std::size_t(2));
        if (timers.size() == 2) CHECK_EQ(timers[1].ms, 2000);
        clock += 2000;
        if (timers.size() == 2) timers[1].fire();
        CHECK(model.started());
        CHECK(waitFor([&] { return !model.busy(); }));
        CHECK_EQ(st.fake.requests.size(), kRequestsPerLoad);
        // A timer of a previous video never loads the next one.
        model.setVideo(QStringLiteral("otherVideo1"), QStringLiteral("X"));
        model.playbackStarted();
        const std::size_t armed = timers.size();
        model.setVideo(QStringLiteral("otherVideo2"), QStringLiteral("Y"));
        clock += 5000;
        timers[armed - 1].fire();
        CHECK(!model.started());
    }
}

void testErrorsInlineNoAutoRetry(const QString &settings) {
    Stack st;
    const std::string token = entryToken();
    const CommentsPage p1 = page("comments_p1", false);
    // First attempt fails (HTTP 503), the retry succeeds.
    st.fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(token), 503, QByteArray("unavailable"), 0, 1);
    serveRecorded(st.fake);
    CommentsModel model(&st.service, nullptr, settings);
    model.load(QString::fromLatin1(kVideo), QString::fromStdString(token));
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK(!model.errorText().isEmpty());
    CHECK_EQ(model.rowCount(), 0);
    CHECK(!model.canLoadMore());
    CHECK(st.counters.value(Area::Comments, Outcome::Http5xx) == 1);
    CHECK(st.counters.driftTotal() == 0);   // HTTP failures are never drift
    // Scrolling never retries.
    for (int i = 0; i < 5; ++i) { model.scrolledNear(); model.loadMore(); }
    QCoreApplication::processEvents();
    CHECK_EQ(st.fake.requests.size(), 1);
    // Tap to retry.
    model.retry();
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(st.fake.requests.size(), 1 + kRequestsPerLoad);
    CHECK_EQ(model.errorText(), QString());
    CHECK_EQ(model.rowCount(), 5);

    // A replies failure is inline on its thread and retried only by a tap.
    Stack rs;
    rs.fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(p1.comments.front().replyContinuation), 0, QByteArray(), 0, 1);
    serveRecorded(rs.fake);
    CommentsModel m2(&rs.service, nullptr, settings);
    m2.load(QString::fromLatin1(kVideo), QString::fromStdString(token));
    CHECK(waitFor([&] { return !m2.busy(); }));
    m2.toggleReplies(0);
    CHECK(waitFor([&] { return !m2.data(m2.index(0, 0), CommentsModel::RepliesBusyRole).toBool(); }));
    const int more = rowOfType(m2, QStringLiteral("more"));
    CHECK_EQ(more, 1);
    CHECK(!role(m2, more, CommentsModel::ReplyErrorTextRole).isEmpty());
    CHECK_EQ(m2.errorText(), QString());   // the list itself is fine
    const int before = rs.fake.requests.size();
    m2.loadMore(); m2.scrolledNear();
    QCoreApplication::processEvents();
    CHECK_EQ(rs.fake.count(QStringLiteral("youtubei/v1/next"), tokenBody(p1.comments.front().replyContinuation)), 1);
    m2.loadMoreReplies(more);
    CHECK(waitFor([&] { return !m2.data(m2.index(0, 0), CommentsModel::RepliesBusyRole).toBool(); }));
    CHECK(rs.fake.requests.size() > before);
    CHECK(m2.session().thread(0).replies.size() > 0);
    CHECK_EQ(role(m2, 1, CommentsModel::RowTypeRole), QStringLiteral("reply"));
}

void testSortSwitch(const QString &settings) {
    Stack st;
    serveRecorded(st.fake);
    const CommentsEntry entry = CommentsService::parseEntry(fixtureJson(QStringLiteral("next_en.json")), "en");
    CHECK_EQ(entry.sorts.size(), std::size_t(2));
    if (entry.sorts.size() != 2) return;
    const std::string newest = entry.sorts[1].token;
    // Derived: serve comments_p2 (ids suffixed) as the "Newest" answer.
    st.fake.on(QStringLiteral("youtubei/v1/next"), tokenBody(newest), 200, bytes(derivedPage(99, QString())));
    CommentsModel model(&st.service, nullptr, settings);
    model.setEntry(QString::fromLatin1(kVideo), entry);   // handed over by the watch-next package
    CHECK_EQ(model.totalText(), QString::fromStdString(entry.totalText));
    CHECK(model.canSort());
    model.start();
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next")), kRequestsPerLoad);
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next"), "videoId"), 0);   // no watch-next needed
    CHECK_EQ(model.sort(), QStringLiteral("top"));
    const QString firstTop = role(model, 0, CommentsModel::CommentIdRole);
    model.toggleReplies(0);
    CHECK(waitFor([&] { return !model.data(model.index(0, 0), CommentsModel::RepliesBusyRole).toBool(); }));
    model.setSort(QStringLiteral("newest"));
    CHECK_EQ(model.sort(), QStringLiteral("newest"));
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next"), tokenBody(newest)), 1);
    CHECK_EQ(model.rowCount(), 5);   // a new list: new window, replies gone
    CHECK(role(model, 0, CommentsModel::CommentIdRole) != firstTop);
    CHECK(role(model, 0, CommentsModel::CommentIdRole).endsWith(QStringLiteral("-99")));
    CHECK_EQ(model.session().totalReplies(), 0);
    CHECK_EQ(model.totalText(), QString::fromStdString(entry.totalText));   // the count stays
    model.setSort(QStringLiteral("top"));
    CHECK(waitFor([&] { return !model.busy(); }));
    CHECK_EQ(st.fake.count(QStringLiteral("youtubei/v1/next"), tokenBody(entry.sorts[0].token)), 2);
    CHECK_EQ(role(model, 0, CommentsModel::CommentIdRole), firstTop);
}

void testTiming(const QString &settings) {
    // Host side of NFR-PERF-08: parse 50 threads plus a thread's two reply pages from the raw
    // bytes (JSON decode included). The device measurement uses the same log line.
    QList<QByteArray> raw;
    raw << fixture(QStringLiteral("comments_p1.json")) << fixture(QStringLiteral("comments_p2.json"))
        << bytes(derivedPage(3, QString()));
    const QByteArray rep1 = fixture(QStringLiteral("comments_replies_p1.json"));
    const QByteArray rep2 = fixture(QStringLiteral("comments_replies_p2.json"));
    QElapsedTimer t;
    t.start();
    std::size_t threads = 0, replies = 0;
    for (const QByteArray &b : raw)
        threads += CommentsService::parsePage(QJsonDocument::fromJson(b).object(), "en", false).comments.size();
    replies += CommentsService::parsePage(QJsonDocument::fromJson(rep1).object(), "en", true).comments.size();
    replies += CommentsService::parsePage(QJsonDocument::fromJson(rep2).object(), "en", true).comments.size();
    const qint64 parseMs = t.elapsed();
    std::printf("sailtube-timing comments %lld (host parse: %d threads, %d replies)\n",
                static_cast<long long>(parseMs), static_cast<int>(threads), static_cast<int>(replies));
    CHECK(threads >= 50);
    CHECK(replies >= 20);
    CHECK(parseMs < 2000);

    // End to end through the lane: 50 threads in one request.
    Stack st;
    for (int k = 1; k <= 4; ++k)
        st.fake.on(QStringLiteral("youtubei/v1/next"), QByteArray("\"PAGE-") + QByteArray::number(k) + "\"", 200,
                   bytes(derivedPage(k, QStringLiteral("PAGE-%1").arg(k + 1))));
    CommentsModel model(&st.service, nullptr, settings);
    QElapsedTimer e;
    e.start();
    model.load(QString::fromLatin1(kVideo), QStringLiteral("PAGE-1"));
    CHECK(waitFor([&] { return !model.busy(); }));
    const qint64 endToEnd = e.elapsed();
    std::printf("sailtube-timing comments %lld (host end-to-end, 50 threads)\n", static_cast<long long>(endToEnd));
    CHECK_EQ(model.session().threadCount(), std::size_t(50));
    CHECK(endToEnd < 2000);
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    QTemporaryDir dir;
    const QString settings = dir.path() + QStringLiteral("/SailTube.conf");
    testFirstPage();
    testTotalAndSortTokens();
    testSecondPage();
    testRepliesPages();
    testKeyJoin();
    testValidEmptyVersusDrift();
    testSessionBudgets();
    testRevealWindow();
    testAutoLoadPolicy();
    testModelFlow(settings);
    testModelBudgetsWithRepeatedPages(settings);
    testAutoLoadModes(dir.path());
    testErrorsInlineNoAutoRetry(settings);
    testSortSwitch(settings);
    testTiming(settings);
    return catalog_test::finish("catalog_comments");
}
