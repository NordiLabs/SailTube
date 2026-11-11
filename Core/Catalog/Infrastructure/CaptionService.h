// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Caption tracks and cues (SEL-06, FINTUBE_SPEC §5.5). Tracks come from an InnerTube player
// answer (captions.playerCaptionsTracklistRenderer) or from a yt-dlp info dict (subtitles /
// automatic_captions). Cues are a GET of the json3 URL with a browser UA, 15 s, no cookies, on
// the *interactive* transport (RES-02: caption cues never queue behind background work). Parsed
// cue lists are cached per URL for the session (12 URLs); failures are never cached.

#include "HttpTransport.h"
#include "WorkLane.h"
#include "Core/Catalog/Domain/Captions.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

using CueList = std::shared_ptr<const std::vector<CaptionCue>>;

struct CueResult {
    Outcome outcome = Outcome::Network;
    QString url;
    CueList cues;            // never null; empty on failure
    bool fromCache = false;
};

class CaptionService final : public QObject {
    Q_OBJECT
public:
    enum { kTimeoutMs = 15000, kCacheUrls = 12 };

    // `interactive` is the interactive discovery transport. `lane` may be null (parse inline).
    CaptionService(HttpTransport *interactive, WorkLane *lane, QObject *parent = nullptr);

    void setCounters(CatalogCounters *counters) { counters_ = counters; }

    // Track shaping from the two sources, as offered to the user: auto-translations are left
    // out while kCaptionTranslationsOffered is false (Captions.h).
    static CaptionTrackSet tracksFromPlayer(const QJsonObject &playerAnswer);
    static CaptionTrackSet tracksFromYtDlp(const QJsonObject &info);

    // json3 body -> outcome + post-processed cues. Ok with cues, ValidEmpty for a valid events
    // array without text, Unavailable for an empty 200 body (timedtext refusing the request),
    // ShapeDrift when the body is not a json3 document.
    static Outcome parseJson3(const QByteArray &body, std::vector<CaptionCue> *cues);

    // Fetches (or serves from the cache) the cues of `url`. `done` runs on `context`'s thread
    // while `context` lives. Returns a request id, or 0 when answered from the cache (the
    // callback then still arrives asynchronously).
    quint64 fetchCues(const QString &url, QObject *context, std::function<void(const CueResult &)> done);
    void cancel(quint64 id);

    bool isCached(const QString &url);
    void clearCache() { cache_.clear(); }

private:
    HttpTransport *transport_;
    WorkLane *lane_;
    CatalogCounters *counters_ = nullptr;
    ExpiringCache<std::string, CueList> cache_;
};

}}}
