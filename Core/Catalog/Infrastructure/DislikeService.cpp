// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DislikeService.h"
#include "Json.h"
#include "Core/Catalog/Domain/Video.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>
#include <QUrlQuery>

namespace nordtile { namespace video { namespace catalog {

DislikeService::DislikeService(HttpTransport *transport, QObject *parent)
    : QObject(parent), transport_(transport), cache_(kCacheEntries, kCacheTtlMs) {}

QUrl DislikeService::requestUrl(const QString &videoId) {
    QUrl url(QStringLiteral("https://returnyoutubedislikeapi.com/votes"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("videoId"), videoId);
    url.setQuery(query);
    return url;
}

Outcome DislikeService::parseVotes(int status, const QByteArray &body, DislikeVotes *out) {
    if (out) *out = DislikeVotes();
    if (status == 404) return Outcome::Unavailable;   // unknown or removed video
    if (status < 200 || status >= 300) return outcomeForHttp(status);
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return Outcome::ShapeDrift;
    const QJsonObject o = doc.object();
    if (!o.value(QStringLiteral("dislikes")).isDouble()) return Outcome::ShapeDrift;
    DislikeVotes v;
    v.dislikes = static_cast<std::int64_t>(o.value(QStringLiteral("dislikes")).toDouble());
    if (o.value(QStringLiteral("likes")).isDouble()) v.likes = static_cast<std::int64_t>(o.value(QStringLiteral("likes")).toDouble());
    if (o.value(QStringLiteral("rating")).isDouble()) v.rating = o.value(QStringLiteral("rating")).toDouble();
    if (o.value(QStringLiteral("viewCount")).isDouble()) v.viewCount = static_cast<std::int64_t>(o.value(QStringLiteral("viewCount")).toDouble());
    v.deleted = o.value(QStringLiteral("deleted")).toBool(false);
    if (out) *out = v;
    return Outcome::Ok;
}

quint64 DislikeService::fetch(const QString &videoId, QObject *context, std::function<void(const DislikeResult &)> done) {
    const std::string id = json::s(videoId);
    if (!enabled_ || !validVideoId(id)) return 0;
    QPointer<QObject> guard(context);
    DislikeResult cached;
    if (cache_.get(id, &cached.votes)) {
        cached.outcome = Outcome::Ok;
        cached.videoId = videoId;
        cached.fromCache = true;
        QTimer::singleShot(0, this, [guard, done, cached] { if (guard && done) done(cached); });
        return 0;
    }
    HttpRequest request;
    request.url = requestUrl(videoId);
    request.timeoutMs = kTimeoutMs;
    QPointer<DislikeService> self(this);
    return transport_->send(request, this, [self, guard, done, videoId, id](const HttpReply &http) {
        if (!self || http.cancelled) return;
        DislikeResult r;
        r.videoId = videoId;
        r.outcome = parseVotes(http.timedOut ? 0 : http.status, http.body, &r.votes);
        if (self->counters_) self->counters_->count(Area::Dislikes, r.outcome);
        if (r.outcome == Outcome::Ok) self->cache_.put(id, r.votes);
        if (guard && done) done(r);
    });
}

void DislikeService::cancel(quint64 id) {
    if (id) transport_->cancel(id);
}

}}}
