// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Recommended (DSC-04, SHOULD): browse FEwhat_to_watch. Only meaningful when signed in: the
// account component hands a signed-in InnerTube client with setClient(client, true). Anonymous
// answers are valid-empty (a rich grid holding only a "try searching" nudge), which is not
// drift; the service does not even ask while signed out.
//
// Outcomes: the tab's richGridRenderer (or a continuation action) is the container marker.
// Present and holding no video rows: ValidEmpty. Missing: ShapeDrift.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"

#include <QObject>
#include <QString>
#include <set>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

struct HomePage {
    Outcome outcome = Outcome::Network;
    std::vector<Item> items;
    QString continuation;
};

// DSC-04 / DSC-04a (user-approved 2026-11-08). Signed in: the recommended home (FEwhat_to_watch).
// Signed out: "Popular" rows from the anonymous search endpoint (empty query, sorted by view
// count, uploaded this week, videos only, mixed results off) in the request's language and
// region (I18N-01); page 1 is cached for kPopularCacheMs. Same parser, RowFilter and Shorts rules.
class HomeService final : public QObject {
    Q_OBJECT
public:
    static const qint64 kPopularCacheMs = 10LL * 60 * 1000;
    explicit HomeService(InnerTubeClient *client = nullptr, bool signedIn = false, QObject *parent = nullptr);

    void setClient(InnerTubeClient *client, bool signedIn);
    bool signedIn() const { return signedIn_; }
    // Signed out the rows are the public "Popular" list (DSC-04a).
    bool popular() const { return !signedIn_; }
    // Monotonic milliseconds for the popular cache (tests).
    void setClock(const std::function<qint64()> &clock) { clock_ = clock; }
    // The search body of the popular list (exposed for tests and the recorder).
    static QJsonObject popularBody();
    void setCounters(CatalogCounters *counters) { counters_ = counters; }
    // The list filter (SET-01 hide Shorts, members-only, restricted). When a hiding switch
    // changes while rows are shown, the list is loaded again with it (the popular list from
    // its page 1 cache), so the rows on screen always match the setting.
    void setRowFilter(const RowFilterSettings &settings);
    const RowFilterSettings &rowFilter() const { return filter_; }
    // DSC-17: what is already known about a video being a Short (rows without their own evidence).
    void setShortsLookup(const ShortsLookup &lookup) { lookup_ = lookup; }

    // First page (replaces the rows), then continuation pages.
    void load();
    void loadMore();
    void clear();

    const std::vector<Item> &items() const { return items_; }
    Outcome outcome() const { return outcome_; }
    bool busy() const { return pending_ != 0; }
    bool canLoadMore() const { return !busy() && !continuation_.isEmpty(); }
    int hiddenCount() const { return hidden_; }

    static HomePage parseHome(const InnerTubeReply &reply, const QString &hl, bool continuation);

signals:
    void changed();

private:
    void request(bool more);
    void requestPopular(bool more);
    void apply(quint64 generation, const HomePage &page);
    qint64 nowMs() const;
    InnerTubeClient *client_;
    bool signedIn_;
    CatalogCounters *counters_ = nullptr;
    RowFilterSettings filter_;
    ShortsLookup lookup_;
    bool loaded_ = false;            // load() ran since the last clear()
    std::vector<Item> items_;
    std::set<std::string> seen_;
    QString continuation_;
    Outcome outcome_ = Outcome::ValidEmpty;
    quint64 pending_ = 0;
    quint64 generation_ = 0;
    int hidden_ = 0;
    std::function<qint64()> clock_;
    // Popular page 1 cache: rows, token, when, and the language / region it was asked in.
    std::vector<Item> popularRows_;
    QString popularContinuation_;
    qint64 popularStoredMs_ = -1;
    QString popularLocale_;
};

}}}
