// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The caption track list of a streamed video (SEL-06). The anonymous WEB player answer lists no
// tracks, so the list comes from one extra player request per opened video, made through
// InnerTubeClient::postTrackList (the pinned shared client definition, the device language and
// an in-memory visitor id; see InnerTubeClient.h). Only the track list is read from the answer:
// the cues of a track are fetched by CaptionService, and only once a track is chosen.
//
// One request per video at a time (later callers join it). A list is kept for 30 minutes (the
// track addresses are signed for a few hours); failures are never kept, so the next call asks
// again. An answer that refuses the visitor id is asked once more with a new one.

#include "CaptionService.h"
#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Captions.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct CaptionTrackList {
    // Ok: `tracks` holds at least one track. ValidEmpty: the video has no captions. Anything
    // else is a failure: the list could not be had (network, HTTP, a refused or unplayable
    // answer, an answer of another shape) and `tracks` is empty.
    Outcome outcome = Outcome::Network;
    QString videoId;
    CaptionTrackSet tracks;
    std::string playability;   // playabilityStatus.status of the answer, "" when it has none
    bool fromCache = false;
};

class CaptionTrackService final : public QObject {
    Q_OBJECT
public:
    enum { kCacheVideos = 24, kCacheTtlMs = 30 * 60 * 1000 };

    // `client` decides the transport and lane: the interactive one for the playing video.
    explicit CaptionTrackService(InnerTubeClient *client, QObject *parent = nullptr, MillisClock clock = MillisClock());

    void setCounters(CatalogCounters *counters) { counters_ = counters; }

    // Player answer -> outcome + shaped tracks (pure; runs on the lane and from tests).
    static CaptionTrackList parse(const QJsonObject &playerAnswer, const QString &videoId);

    // Lists `videoId`'s tracks. `done` runs on `context`'s thread while `context` lives, always
    // asynchronously. Returns false (and never calls `done`) for an invalid id.
    bool fetch(const QString &videoId, QObject *context, std::function<void(const CaptionTrackList &)> done);

    bool isCached(const QString &videoId);
    void forget(const QString &videoId);
    // Player requests sent so far (a refused visitor id counts twice); tests and diagnostics.
    int requestsSent() const { return sent_; }

private:
    struct Waiter {
        QPointer<QObject> guard;
        bool hasContext = false;
        std::function<void(const CaptionTrackList &)> done;
    };
    void send(const QString &videoId, int attempt);
    void finish(const CaptionTrackList &list);

    InnerTubeClient *client_;
    CatalogCounters *counters_ = nullptr;
    ExpiringCache<std::string, CaptionTrackList> cache_;
    std::map<std::string, std::vector<Waiter>> waiting_;
    int sent_ = 0;
};

}}}
