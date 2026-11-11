// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SuggestService.h"
#include "InnerTubeDefinitions.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>

namespace nordtile { namespace video { namespace catalog {

const int SuggestService::kTimeoutMs;
const int SuggestService::kMaxEntries;

SuggestService::SuggestService(HttpTransport *transport, const DiscoveryLocale &locale, CatalogCounters *counters,
                               QObject *parent, CatalogClock now)
    : QObject(parent), transport_(transport), locale_(locale), counters_(counters), cache_(now) {}

SuggestService::~SuggestService() {
    for (auto it = pending_.constBegin(); it != pending_.constEnd(); ++it)
        if (it.value() && transport_) transport_->cancel(it.value());
}

HttpRequest SuggestService::buildRequest(const QString &query, const QString &hl) {
    // Built by hand so the query is fully percent-encoded ("+", "&", "#" keep their meaning).
    QByteArray qs(innertube::kSuggestQueryPrefix);
    qs += QUrl::toPercentEncoding(hl.isEmpty() ? QStringLiteral("en") : hl);
    qs += "&q=";
    qs += QUrl::toPercentEncoding(query);
    QUrl url(QStringLiteral("https://suggestqueries.google.com/complete/search"));
    url.setQuery(QString::fromLatin1(qs));
    HttpRequest r;
    r.url = url;
    r.method = "GET";
    r.timeoutMs = kTimeoutMs;
    r.headers.append(qMakePair(QByteArray("User-Agent"), webUserAgent()));
    return r;
}

SuggestResult SuggestService::parse(const QByteArray &body) {
    SuggestResult out;
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError) doc = QJsonDocument::fromJson(QString::fromLatin1(body).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) { out.outcome = Outcome::ShapeDrift; return out; }
    const QJsonArray top = doc.array();
    if (top.size() < 2 || !top.at(1).isArray()) { out.outcome = Outcome::ShapeDrift; return out; }
    out.query = top.at(0).toString();
    for (const QJsonValue &v : top.at(1).toArray()) {
        const QString s = v.isString() ? v.toString().trimmed()
                                       : v.isArray() ? v.toArray().at(0).toString().trimmed() : QString();
        if (s.isEmpty() || out.suggestions.contains(s)) continue;
        out.suggestions.append(s);
        if (out.suggestions.size() >= kMaxEntries) break;
    }
    out.outcome = out.suggestions.isEmpty() ? Outcome::ValidEmpty : Outcome::Ok;
    return out;
}

quint64 SuggestService::fetch(const QString &query, Callback done) {
    const quint64 id = ++next_;
    const std::string key = query.toStdString();
    std::vector<std::string> cached;
    if (cache_.lookup(key, &cached)) {
        SuggestResult r;
        r.outcome = cached.empty() ? Outcome::ValidEmpty : Outcome::Ok;
        r.query = query;
        r.fromCache = true;
        for (const std::string &s : cached) r.suggestions.append(QString::fromStdString(s));
        pending_.insert(id, 0);
        QTimer::singleShot(0, this, [this, id, r, done] {
            if (!pending_.contains(id)) return;
            pending_.remove(id);
            if (done) done(r);
        });
        return id;
    }
    if (!transport_) return 0;
    pending_.insert(id, 0);
    const quint64 tid = transport_->send(buildRequest(query, locale_.hl), this, [this, id, query, key, done](const HttpReply &http) {
        if (!pending_.contains(id) || http.cancelled) { pending_.remove(id); return; }
        pending_.remove(id);
        SuggestResult r;
        const Outcome httpOutcome = outcomeForHttp(http.status);
        if (httpOutcome != Outcome::Ok) r.outcome = httpOutcome;
        else r = parse(http.body);
        r.query = query;
        r.elapsedMs = http.elapsedMs;
        if (counters_) counters_->count(Area::Suggest, r.outcome);
        if (!isFailure(r.outcome)) {
            std::vector<std::string> list;
            for (const QString &s : r.suggestions) list.push_back(s.toStdString());
            cache_.store(key, list);
        } else {
            r.suggestions.clear();
        }
        if (done) done(r);
    });
    if (pending_.contains(id)) pending_[id] = tid;
    return id;
}

void SuggestService::cancel(quint64 id) {
    auto it = pending_.find(id);
    if (it == pending_.end()) return;
    const quint64 tid = it.value();
    pending_.erase(it);
    if (tid && transport_) transport_->cancel(tid);
}

}}}
