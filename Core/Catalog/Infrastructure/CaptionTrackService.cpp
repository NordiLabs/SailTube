// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CaptionTrackService.h"
#include "Json.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"

#include <QJsonArray>
#include <QTimer>

namespace nordtile { namespace video { namespace catalog {

namespace {
const char kRefused[] = "LOGIN_REQUIRED";   // the answer to a missing or refused visitor id
}

CaptionTrackService::CaptionTrackService(InnerTubeClient *client, QObject *parent, MillisClock clock)
    : QObject(parent), client_(client), cache_(kCacheVideos, kCacheTtlMs, clock) {}

CaptionTrackList CaptionTrackService::parse(const QJsonObject &playerAnswer, const QString &videoId) {
    CaptionTrackList list;
    list.videoId = videoId;
    const QJsonObject status = playerAnswer.value(QStringLiteral("playabilityStatus")).toObject();
    list.playability = json::s(status.value(QStringLiteral("status")).toString());
    list.tracks = CaptionService::tracksFromPlayer(playerAnswer);
    if (!list.tracks.empty()) {
        list.outcome = Outcome::Ok;
        return list;
    }
    // No track: "has none" only when the answer is one about a playable video. A refused or
    // unplayable answer carries no caption list either, and must not read as "no captions".
    if (status.isEmpty()) list.outcome = Outcome::ShapeDrift;
    else if (playerAnswer.value(QStringLiteral("videoDetails")).isObject() && list.playability != kRefused &&
             list.playability != "ERROR" && list.playability != "UNPLAYABLE")
        list.outcome = Outcome::ValidEmpty;
    else list.outcome = Outcome::Unavailable;
    return list;
}

bool CaptionTrackService::isCached(const QString &videoId) { return cache_.contains(json::s(videoId)); }

void CaptionTrackService::forget(const QString &videoId) { cache_.remove(json::s(videoId)); }

bool CaptionTrackService::fetch(const QString &videoId, QObject *context, std::function<void(const CaptionTrackList &)> done) {
    const std::string key = json::s(videoId);
    if (!ThumbnailBook::isVideoId(key) || !client_) return false;
    QPointer<QObject> guard(context);
    const bool hasContext = context != nullptr;
    CaptionTrackList cached;
    if (cache_.get(key, &cached)) {
        cached.fromCache = true;
        QTimer::singleShot(0, this, [guard, hasContext, done, cached] {
            if ((!hasContext || guard) && done) done(cached);
        });
        return true;
    }
    Waiter w;
    w.guard = guard;
    w.hasContext = hasContext;
    w.done = done;
    std::vector<Waiter> &waiters = waiting_[key];
    waiters.push_back(w);
    if (waiters.size() > 1) return true;   // joins the request in flight
    client_->ensureVisitor(this, [this, videoId] { send(videoId, 0); });
    return true;
}

void CaptionTrackService::send(const QString &videoId, int attempt) {
    std::function<CaptionTrackList(const InnerTubeReply &)> parseReply = [videoId](const InnerTubeReply &r) {
        if (r.outcome != Outcome::Ok) {
            CaptionTrackList failed;
            failed.outcome = r.outcome;
            failed.videoId = videoId;
            return failed;
        }
        return parse(r.json, videoId);
    };
    std::function<void(CaptionTrackList)> done = [this, videoId, attempt](CaptionTrackList list) {
        if (attempt == 0 && list.playability == kRefused) {
            // The visitor id was refused (or there was none): once more with a new one.
            client_->forgetVisitor();
            client_->ensureVisitor(this, [this, videoId] { send(videoId, 1); });
            return;
        }
        finish(list);
    };
    ++sent_;
    client_->postTrackList<CaptionTrackList>(videoId, this, parseReply, done);
}

void CaptionTrackService::finish(const CaptionTrackList &list) {
    const std::string key = json::s(list.videoId);
    if (counters_) counters_->count(Area::Captions, list.outcome);
    if (!isFailure(list.outcome)) cache_.put(key, list);
    qInfo("sailtube-captions tracks %s outcome=%s real=%d translations=%d", qPrintable(list.videoId),
          outcomeName(list.outcome), static_cast<int>(list.tracks.real.size()), static_cast<int>(list.tracks.translations.size()));
    const auto it = waiting_.find(key);
    if (it == waiting_.end()) return;
    const std::vector<Waiter> waiters = it->second;
    waiting_.erase(it);
    for (const Waiter &w : waiters)
        if ((!w.hasContext || w.guard) && w.done) w.done(list);
}

}}}
