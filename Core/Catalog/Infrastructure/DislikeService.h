// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Return YouTube Dislike (RYD-01, MAY): opt-in and off by default. While disabled, no request
// is sent at all. Enabled: GET https://returnyoutubedislikeapi.com/votes?videoId=<id>, cached
// 10 min per video; failures are never cached. The numbers are third-party estimates.

#include "HttpTransport.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/Outcome.h"

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QUrl>
#include <cstdint>
#include <functional>

namespace nordtile { namespace video { namespace catalog {

struct DislikeVotes {
    std::int64_t likes = -1;
    std::int64_t dislikes = -1;   // the estimate shown to the user
    double rating = -1;
    std::int64_t viewCount = -1;
    bool deleted = false;
};

struct DislikeResult {
    Outcome outcome = Outcome::Network;
    QString videoId;
    DislikeVotes votes;
    bool fromCache = false;
};

class DislikeService final : public QObject {
    Q_OBJECT
public:
    enum { kTimeoutMs = 8000, kCacheEntries = 64, kCacheTtlMs = 10 * 60 * 1000 };

    explicit DislikeService(HttpTransport *transport, QObject *parent = nullptr);

    void setCounters(CatalogCounters *counters) { counters_ = counters; }
    void setClock(MillisClock clock) { cache_.setClock(clock); }
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    static QUrl requestUrl(const QString &videoId);
    static Outcome parseVotes(int status, const QByteArray &body, DislikeVotes *out);

    // Disabled or invalid id: returns 0, sends nothing and never calls `done`. A cache hit
    // returns 0 and calls `done` asynchronously.
    quint64 fetch(const QString &videoId, QObject *context, std::function<void(const DislikeResult &)> done);
    void cancel(quint64 id);
    void clearCache() { cache_.clear(); }

private:
    HttpTransport *transport_;
    CatalogCounters *counters_ = nullptr;
    bool enabled_ = false;
    ExpiringCache<std::string, DislikeVotes> cache_;
};

}}}
