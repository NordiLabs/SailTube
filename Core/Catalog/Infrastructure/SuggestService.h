// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Search suggestions (DSC-11):
// GET https://suggestqueries.google.com/complete/search?client=firefox&ds=yt&hl=<hl>&q=<query>
// with hl from the device locale, a 5 s timeout and at most 10 entries. The answer is a JSON
// array whose second element is the list. Answers are cached 10 min per exact query (128
// entries); failures are never cached and never raise an error: they yield no suggestions.

#include "ClientIdentity.h"
#include "HttpTransport.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/SearchCache.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <functional>

namespace nordtile { namespace video { namespace catalog {

struct SuggestResult {
    Outcome outcome = Outcome::Network;
    QString query;
    QStringList suggestions;
    bool fromCache = false;
    qint64 elapsedMs = 0;
};

class SuggestService final : public QObject {
    Q_OBJECT
public:
    using Callback = std::function<void(const SuggestResult &)>;
    static const int kTimeoutMs = 5000;
    static const int kMaxEntries = 10;

    SuggestService(HttpTransport *transport, const DiscoveryLocale &locale, CatalogCounters *counters = nullptr,
                   QObject *parent = nullptr, CatalogClock now = CatalogClock());
    ~SuggestService() override;

    DiscoveryLocale locale() const { return locale_; }
    void setLocale(const DiscoveryLocale &locale) { locale_ = locale; }

    // Answers asynchronously on this object's thread (also for cache hits). Returns an id.
    quint64 fetch(const QString &query, Callback done);
    void cancel(quint64 id);
    SuggestCache &cache() { return cache_; }

    static HttpRequest buildRequest(const QString &query, const QString &hl);
    // Parses the answer body (UTF-8, or Latin-1 when the server picked a legacy charset).
    static SuggestResult parse(const QByteArray &body);

private:
    HttpTransport *transport_;
    DiscoveryLocale locale_;
    CatalogCounters *counters_;
    SuggestCache cache_;
    QHash<quint64, quint64> pending_;   // our id -> transport id (0 for cache answers)
    quint64 next_ = 0;
};

}}}
