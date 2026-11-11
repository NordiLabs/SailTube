// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SponsorBlockService.h"
#include "Json.h"
#include "Core/Catalog/Domain/Video.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

namespace nordtile { namespace video { namespace catalog {

namespace {

struct ParsedSegments {
    Outcome outcome = Outcome::Network;
    SponsorSegmentList list;
};

QByteArray sponsorUserAgent() {
    const QString version = QCoreApplication::applicationVersion();
    return version.isEmpty() ? QByteArray("SailTube") : QByteArray("SailTube/") + version.toUtf8();
}

}  // namespace

SponsorBlockService::SponsorBlockService(HttpTransport *interactive, WorkLane *lane, QObject *parent)
    : QObject(parent), transport_(interactive), lane_(lane), cache_(kCacheEntries, kCacheTtlMs) {}

void SponsorBlockService::setHighlightEnabled(bool on) {
    if (highlight_ == on) return;
    highlight_ = on;
    cache_.clear();
}

QUrl SponsorBlockService::requestUrl(const QString &videoId) const {
    const QByteArray prefix = QByteArray::fromStdString(sponsorHashPrefix(json::s(videoId)));
    const QByteArray categories = QUrl::toPercentEncoding(QString::fromStdString(sponsorCategoriesJson(highlight_)));
    return QUrl::fromEncoded("https://sponsor.ajay.app/api/skipSegments/" + prefix + "?categories=" + categories, QUrl::StrictMode);
}

Outcome SponsorBlockService::parseAnswer(int status, const QByteArray &body, const std::string &videoId, SponsorSegmentList *out) {
    if (out) *out = SponsorSegmentList();
    if (status == 404) return Outcome::ValidEmpty;   // no segments for any id with this prefix
    if (status < 200 || status >= 300) return outcomeForHttp(status);
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isArray()) return Outcome::ShapeDrift;
    std::vector<SponsorPrefixEntry> entries;
    for (const QJsonValue &v : doc.array()) {
        const QJsonObject o = v.toObject();
        SponsorPrefixEntry entry;
        entry.videoId = json::s(o.value(QStringLiteral("videoID")).toString());
        if (entry.videoId != videoId) continue;   // other ids sharing the prefix are discarded here
        for (const QJsonValue &sv : o.value(QStringLiteral("segments")).toArray()) {
            const QJsonObject so = sv.toObject();
            const QJsonArray range = so.value(QStringLiteral("segment")).toArray();
            if (range.size() < 2) continue;
            SponsorSegment s(range.at(0).toDouble(), range.at(1).toDouble(), json::s(so.value(QStringLiteral("category")).toString()));
            s.actionType = json::s(so.value(QStringLiteral("actionType")).toString());
            s.uuid = json::s(so.value(QStringLiteral("UUID")).toString());
            entry.segments.push_back(s);
        }
        entries.push_back(entry);
    }
    SponsorSegmentList list = filterSponsorSegments(entries, videoId);
    const bool any = !list.segments.empty() || list.hasHighlight;
    if (out) *out = list;
    return any ? Outcome::Ok : Outcome::ValidEmpty;
}

quint64 SponsorBlockService::fetch(const QString &videoId, QObject *context, std::function<void(const SponsorResult &)> done) {
    QPointer<QObject> guard(context);
    const std::string id = json::s(videoId);
    SponsorResult immediate;
    immediate.videoId = videoId;
    bool answerNow = false;
    if (!validVideoId(id)) {
        immediate.outcome = Outcome::Http4xx;
        answerNow = true;
    } else if (cache_.get(id, &immediate.list)) {
        immediate.outcome = immediate.list.segments.empty() && !immediate.list.hasHighlight ? Outcome::ValidEmpty : Outcome::Ok;
        immediate.fromCache = true;
        answerNow = true;
    }
    if (answerNow) {
        QTimer::singleShot(0, this, [guard, done, immediate] { if (guard && done) done(immediate); });
        return 0;
    }

    HttpRequest request;
    request.url = requestUrl(videoId);
    request.headers.append(qMakePair(QByteArray("User-Agent"), sponsorUserAgent()));
    request.timeoutMs = kTimeoutMs;
    WorkLane *lane = lane_;
    QPointer<SponsorBlockService> self(this);
    return transport_->send(request, this, [self, lane, guard, done, videoId, id](const HttpReply &http) {
        if (!self || http.cancelled) return;
        auto deliver = [self, guard, done, videoId, id](const ParsedSegments &parsed) {
            if (!self) return;
            if (self->counters_) self->counters_->count(Area::SponsorBlock, parsed.outcome);
            if (!isFailure(parsed.outcome)) self->cache_.put(id, parsed.list);
            SponsorResult r;
            r.outcome = parsed.outcome;
            r.videoId = videoId;
            r.list = parsed.list;
            if (guard && done) done(r);
        };
        const int status = http.timedOut ? 0 : http.status;
        const QByteArray body = http.body;
        std::function<ParsedSegments()> work = [status, body, id] {
            ParsedSegments p;
            p.outcome = parseAnswer(status, body, id, &p.list);
            return p;
        };
        if (lane) lane->run<ParsedSegments>(self.data(), work, deliver);
        else deliver(work());
    });
}

void SponsorBlockService::cancel(quint64 id) {
    if (id) transport_->cancel(id);
}

}}}
