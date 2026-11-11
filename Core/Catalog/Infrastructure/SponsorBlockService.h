// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SponsorBlock segments, fetched privately (SB-01): GET
// https://sponsor.ajay.app/api/skipSegments/<first 4 hex of sha256(videoId)>?categories=[...]
// and filtered for the exact id locally, so the request never carries the id. 8 s timeout, a 404
// means "no segments". Valid answers are cached 10 min / 64 videos; failures are never cached.
// Runs on the interactive transport: it must never queue behind background work (RES-02).

#include "HttpTransport.h"
#include "WorkLane.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/SponsorSegments.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

namespace nordtile { namespace video { namespace catalog {

struct SponsorResult {
    Outcome outcome = Outcome::Network;
    QString videoId;
    SponsorSegmentList list;
    bool fromCache = false;
};

class SponsorBlockService final : public QObject {
    Q_OBJECT
public:
    enum { kTimeoutMs = 8000, kCacheEntries = 64, kCacheTtlMs = 10 * 60 * 1000 };

    SponsorBlockService(HttpTransport *interactive, WorkLane *lane, QObject *parent = nullptr);

    void setCounters(CatalogCounters *counters) { counters_ = counters; }
    void setClock(MillisClock clock) { cache_.setClock(clock); }
    // Also request poi_highlight (SB-02 SHOULD). Changing it clears the cache.
    void setHighlightEnabled(bool on);
    bool highlightEnabled() const { return highlight_; }

    QUrl requestUrl(const QString &videoId) const;

    // HTTP status + body -> outcome and the segments of `videoId`. 404 is ValidEmpty.
    static Outcome parseAnswer(int status, const QByteArray &body, const std::string &videoId, SponsorSegmentList *out);

    // `done` runs on `context`'s thread while it lives. Returns 0 when served from the cache or
    // rejected (invalid id); the callback still arrives asynchronously.
    quint64 fetch(const QString &videoId, QObject *context, std::function<void(const SponsorResult &)> done);
    void cancel(quint64 id);
    void clearCache() { cache_.clear(); }

private:
    HttpTransport *transport_;
    WorkLane *lane_;
    CatalogCounters *counters_ = nullptr;
    bool highlight_ = false;
    ExpiringCache<std::string, SponsorSegmentList> cache_;
};

}}}
