// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// One Short's details (the Shorts section's on-picture data), from the item endpoint
// reel/reel_item_watch: POST {"playerRequest":{"videoId":<id>},"disablePlayerResponse":true}
// through the interactive discovery client (anonymous WEB, no key, no params). About 12 KB on the
// wire, no new host. The answer carries the like and comment counts, the channel (id, @handle,
// avatar), the title line, the full description, the related-video chip and the sound line; the
// list answers carry none of that for most rows (see ShortDetail.h).
//
// One request per Short at a time (later callers join it); a detail is kept for 30 minutes (96
// Shorts), failures are never kept. The channel's avatar is learned into the AvatarBook, so rows
// of that channel get it too. Outcomes: Ok; Unavailable for an answer without an overlay (a
// missing, private or removed Short: final, not retried); ShapeDrift for an overlay whose parts
// are all missing; HTTP and network classes as everywhere.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/ShortDetail.h"

#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QString>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct ShortDetailResult {
    Outcome outcome = Outcome::Network;
    QString videoId;
    ShortDetail detail;
    bool fromCache = false;
};

class ShortDetailService final : public QObject {
    Q_OBJECT
public:
    enum { kCacheShorts = 96, kCacheTtlMs = 30 * 60 * 1000 };

    // `client` must use the interactive discovery transport.
    explicit ShortDetailService(InnerTubeClient *client, CatalogCounters *counters = nullptr, QObject *parent = nullptr,
                                MillisClock clock = MillisClock());

    // The request body for `videoId`.
    static QJsonObject requestBody(const QString &videoId);
    // The item answer -> outcome + details (pure; runs on the lane and from tests). `hl` is the
    // language the answer was asked in (it decides how a count's text is read).
    static ShortDetailResult parse(const QJsonObject &answer, const QString &videoId, const QString &hl);

    // Fetches (or serves from the cache) the details of `videoId`. `done` runs on `context`'s
    // thread while `context` lives, always asynchronously. Returns false (and never calls `done`)
    // for an invalid id.
    bool fetch(const QString &videoId, QObject *context, std::function<void(const ShortDetailResult &)> done);
    // The cached details of `videoId`, if fresh.
    bool peek(const QString &videoId, ShortDetail *out);
    bool isCached(const QString &videoId);
    void forget(const QString &videoId);
    // Item requests sent so far (tests and diagnostics).
    int requestsSent() const { return sent_; }

private:
    struct Waiter {
        QPointer<QObject> guard;
        bool hasContext = false;
        std::function<void(const ShortDetailResult &)> done;
    };
    void finish(const ShortDetailResult &result);

    InnerTubeClient *client_;
    CatalogCounters *counters_;
    ExpiringCache<std::string, ShortDetail> cache_;
    std::map<std::string, std::vector<Waiter>> waiting_;
    int sent_ = 0;
};

}}}
