// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SearchService.h"
#include "Json.h"
#include "RendererParser.h"

#include <QJsonArray>

namespace nordtile { namespace video { namespace catalog {

const int SearchService::kFallbackPageSize;

namespace {

void merge(SearchResult &out, const ParsedItems &parsed) {
    out.rows.insert(out.rows.end(), parsed.items.begin(), parsed.items.end());
    if (!parsed.continuation.isEmpty()) out.continuation = parsed.continuation;
    for (auto it = parsed.unknown.constBegin(); it != parsed.unknown.constEnd(); ++it) out.unknown[it.key()] += it.value();
}

}  // namespace

SearchService::SearchService(InnerTubeClient *client, CatalogCounters *counters, QObject *parent)
    : QObject(parent), client_(client), counters_(counters) {}

SearchService::~SearchService() {
    const QList<quint64> ids = ops_.keys();
    for (quint64 id : ids) cancel(id);
}

QJsonObject SearchService::firstPageBody(const QString &query, const SearchFilter &filter) {
    QJsonObject body;
    body.insert(QStringLiteral("query"), query);
    const std::string params = searchParams(filter);
    if (!params.empty()) body.insert(QStringLiteral("params"), QString::fromStdString(params));
    return body;
}

SearchResult SearchService::parseFirstPage(const QJsonObject &answer, const QString &hl) {
    SearchResult out;
    const QJsonValue list = json::at(answer, QStringLiteral("contents.twoColumnSearchResultsRenderer.primaryContents.sectionListRenderer"));
    if (!list.isObject() || !list.toObject().value(QStringLiteral("contents")).isArray()) {
        out.outcome = Outcome::ShapeDrift;
        return out;
    }
    merge(out, RendererParser(hl).parse(list));
    out.outcome = out.rows.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return out;
}

SearchResult SearchService::parseContinuation(const QJsonObject &answer, const QString &hl) {
    SearchResult out;
    bool marker = false;
    const RendererParser parser(hl);
    for (const QJsonValue &c : answer.value(QStringLiteral("onResponseReceivedCommands")).toArray()) {
        const QJsonObject cmd = c.toObject();
        if (cmd.contains(QStringLiteral("appendContinuationItemsAction"))) {
            marker = true;
            QJsonObject holder;
            holder.insert(QStringLiteral("continuationItems"),
                          json::at(cmd, QStringLiteral("appendContinuationItemsAction.continuationItems")));
            merge(out, parser.parse(holder));
        } else if (cmd.contains(QStringLiteral("reloadContinuationItemsCommand"))) {
            const QJsonObject reload = cmd.value(QStringLiteral("reloadContinuationItemsCommand")).toObject();
            if (reload.value(QStringLiteral("slot")).toString().contains(QLatin1String("HEADER"))) continue;
            marker = true;
            QJsonObject holder;
            holder.insert(QStringLiteral("continuationItems"), reload.value(QStringLiteral("continuationItems")));
            merge(out, parser.parse(holder));
        }
    }
    if (!marker) { out.outcome = Outcome::ShapeDrift; return out; }
    out.outcome = out.rows.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return out;
}

quint64 SearchService::begin() {
    const quint64 id = ++next_;
    ops_.insert(id, Op());
    return id;
}

quint64 SearchService::firstPage(const QString &query, const SearchFilter &filter, Callback done) {
    const quint64 op = begin();
    post(op, firstPageBody(query, filter), true, query, filter, 0, done);
    return op;
}

quint64 SearchService::nextPage(const QString &query, const SearchFilter &filter, const QString &continuation,
                                int rowsSoFar, Callback done) {
    const quint64 op = begin();
    if (isFallbackToken(continuation)) {
        bool ok = false;
        const int offset = continuation.mid(6).toInt(&ok);
        SearchResult none;
        none.outcome = Outcome::Unavailable;
        none.firstTierOutcome = Outcome::Unavailable;
        if (!ok || offset < 0 || !fallback_) { finish(op, none, done); return op; }
        runFallback(op, query, filter, offset, none, done);
        return op;
    }
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), continuation);
    post(op, body, false, query, filter, rowsSoFar, done);
    return op;
}

quint64 SearchService::prefetchPage(const QString &query, const SearchFilter &filter, const QString &continuation, Callback done) {
    const quint64 op = begin();
    ops_[op].noFallback = true;
    if (isFallbackToken(continuation) || continuation.isEmpty()) {
        SearchResult none;
        none.outcome = Outcome::Unavailable;
        none.firstTierOutcome = Outcome::Unavailable;
        finish(op, none, done);
        return op;
    }
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), continuation);
    post(op, body, false, query, filter, -1, done);
    return op;
}

void SearchService::post(quint64 op, const QJsonObject &body, bool first, const QString &query,
                         const SearchFilter &filter, int offset, Callback done) {
    const QString hl = client_->locale().hl;
    std::function<SearchResult(const InnerTubeReply &)> parse = [first, hl](const InnerTubeReply &reply) {
        SearchResult r;
        if (reply.outcome == Outcome::Ok) r = first ? parseFirstPage(reply.json, hl) : parseContinuation(reply.json, hl);
        else r.outcome = reply.outcome;
        r.httpStatus = reply.httpStatus;
        r.elapsedMs = reply.elapsedMs;
        r.firstTierOutcome = r.outcome;
        return r;
    };
    std::function<void(SearchResult)> finished = [this, op, query, filter, offset, done](SearchResult r) {
        if (!ops_.contains(op)) return;   // cancelled
        if (counters_) {
            counters_->count(Area::Search, r.outcome);
            for (auto it = r.unknown.constBegin(); it != r.unknown.constEnd(); ++it) counters_->unknownRenderer(it.key().toStdString(), it.value());
        }
        if (isFailure(r.outcome) && fallback_ && !ops_.value(op).noFallback) { runFallback(op, query, filter, offset, r, done); return; }
        finish(op, r, done);
    };
    const quint64 inner = client_->post<SearchResult>(QLatin1String(innertube::kEndpointSearch), body, this, parse, finished);
    auto it = ops_.find(op);
    if (it != ops_.end()) it->innerId = inner;
}

void SearchService::runFallback(quint64 op, const QString &query, const SearchFilter &filter, int offset,
                                const SearchResult &firstTier, Callback done) {
    const int count = kFallbackPageSize;
    SearchFallback::Done answer = [this, op, offset, count, firstTier, done](Outcome outcome, const std::vector<Item> &rows) {
        if (!ops_.contains(op)) return;
        if (isFailure(outcome)) {
            SearchResult failed = firstTier;
            // Paging inside the fallback has no first-tier answer: report the fallback's own class.
            if (firstTier.outcome == Outcome::Unavailable) failed.outcome = outcome;
            finish(op, failed, done);
            return;
        }
        SearchResult r;
        r.rows = rows;
        r.outcome = rows.empty() ? Outcome::ValidEmpty : Outcome::Ok;
        r.fallback = true;
        r.firstTierOutcome = firstTier.firstTierOutcome;
        r.httpStatus = firstTier.httpStatus;
        if (static_cast<int>(rows.size()) >= count) r.continuation = fallbackToken(offset + static_cast<int>(rows.size()));
        finish(op, r, done);
    };
    const quint64 id = fallback_.search(query, filter, offset, count, this, answer);
    if (id == 0) {   // declined (unsupported filter, runtime missing): the first-tier answer stands
        if (ops_.contains(op)) finish(op, firstTier, done);
        return;
    }
    auto it = ops_.find(op);
    if (it != ops_.end()) it->fallbackId = id;
}

void SearchService::finish(quint64 op, const SearchResult &result, const Callback &done) {
    ops_.remove(op);
    if (done) done(result);
}

void SearchService::cancel(quint64 id) {
    auto it = ops_.find(id);
    if (it == ops_.end()) return;
    const Op o = it.value();
    ops_.erase(it);
    if (o.innerId) client_->cancel(o.innerId);
    if (o.fallbackId && fallback_.cancel) fallback_.cancel(o.fallbackId);
}

}}}
