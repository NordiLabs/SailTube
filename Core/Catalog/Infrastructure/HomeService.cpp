// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HomeService.h"
#include <QElapsedTimer>
#include "Core/Catalog/Domain/SearchFilter.h"
#include "SearchService.h"

#include "Json.h"
#include "RendererParser.h"

#include <QJsonArray>

namespace nordtile { namespace video { namespace catalog {

HomeService::HomeService(InnerTubeClient *client, bool signedIn, QObject *parent)
    : QObject(parent), client_(client), signedIn_(signedIn) {}

void HomeService::setClient(InnerTubeClient *client, bool signedIn) {
    if (client == client_ && signedIn == signedIn_) return;
    client_ = client;
    signedIn_ = signedIn;
    clear();
}

void HomeService::clear() {
    ++generation_;
    pending_ = 0;
    loaded_ = false;
    items_.clear();
    seen_.clear();
    continuation_.clear();
    hidden_ = 0;
    outcome_ = Outcome::ValidEmpty;
    emit changed();
}

void HomeService::load() {
    clear();
    loaded_ = true;
    request(false);
}

void HomeService::setRowFilter(const RowFilterSettings &settings) {
    const bool hidingChanged = settings.hideShorts != filter_.hideShorts ||
                               settings.hideMembersOnly != filter_.hideMembersOnly ||
                               settings.hideRestricted != filter_.hideRestricted;
    filter_ = settings;
    if (hidingChanged && loaded_ && client_) load();
}

void HomeService::loadMore() {
    if (!canLoadMore()) return;
    request(true);
}

qint64 HomeService::nowMs() const {
    if (clock_) return clock_();
    static QElapsedTimer t;
    if (!t.isValid()) t.start();
    return t.elapsed();
}

QJsonObject HomeService::popularBody() {
    SearchFilter f;
    f.sort = SearchSort::Views;
    f.uploadDate = UploadDateFilter::Week;
    f.type = ResultTypeFilter::Video;   // mixed results off
    return SearchService::firstPageBody(QString(), f);
}

void HomeService::apply(quint64 generation, const HomePage &page) {
    if (generation != generation_) return;
    pending_ = 0;
    if (counters_) counters_->count(Area::Home, page.outcome);
    outcome_ = page.outcome;
    if (page.outcome == Outcome::Ok || page.outcome == Outcome::ValidEmpty) {
        RowFilterResult filtered = filterRows(page.items, filter_, &seen_, lookup_);
        hidden_ += filtered.hiddenTotal();
        items_.insert(items_.end(), filtered.visible.begin(), filtered.visible.end());
        continuation_ = page.continuation;
    }
    emit changed();
}

void HomeService::requestPopular(bool more) {
    const QString locale = client_->locale().hl + QLatin1Char('-') + client_->locale().gl;
    if (!more && popularStoredMs_ >= 0 && popularLocale_ == locale && nowMs() - popularStoredMs_ < kPopularCacheMs) {
        HomePage cached;
        cached.outcome = popularRows_.empty() ? Outcome::ValidEmpty : Outcome::Ok;
        cached.items = popularRows_;
        cached.continuation = popularContinuation_;
        apply(++generation_, cached);
        return;
    }
    QJsonObject body = popularBody();
    if (more) {
        body = QJsonObject();
        body.insert(QStringLiteral("continuation"), continuation_);
    }
    const QString hl = client_->locale().hl;
    const quint64 generation = ++generation_;
    pending_ = generation;
    std::function<HomePage(const InnerTubeReply &)> parse = [hl, more](const InnerTubeReply &r) {
        HomePage page;
        page.outcome = r.outcome;
        if (r.outcome != Outcome::Ok) return page;
        const SearchResult s = more ? SearchService::parseContinuation(r.json, hl) : SearchService::parseFirstPage(r.json, hl);
        page.outcome = s.outcome;
        for (const Item &it : s.rows) if (isVideoLike(it) && !it.id.empty()) page.items.push_back(it);
        page.continuation = s.continuation;
        if (page.outcome == Outcome::Ok && page.items.empty()) page.outcome = Outcome::ValidEmpty;
        return page;
    };
    std::function<void(HomePage)> done = [this, generation, more, locale](HomePage page) {
        if (generation != generation_) return;
        if (!more && page.outcome == Outcome::Ok) {
            popularRows_ = page.items;
            popularContinuation_ = page.continuation;
            popularStoredMs_ = nowMs();
            popularLocale_ = locale;
        }
        apply(generation, page);
    };
    emit changed();
    client_->post<HomePage>(QLatin1String(innertube::kEndpointSearch), body, this, parse, done);
}

void HomeService::request(bool more) {
    if (!client_) {
        outcome_ = Outcome::ValidEmpty;
        emit changed();
        return;
    }
    if (!signedIn_) { requestPopular(more); return; }
    QJsonObject body;
    if (more) body.insert(QStringLiteral("continuation"), continuation_);
    else body.insert(QStringLiteral("browseId"), QLatin1String(innertube::kBrowseIdHome));
    const QString hl = client_->locale().hl;
    const quint64 generation = ++generation_;
    pending_ = generation;
    std::function<HomePage(const InnerTubeReply &)> parse = [hl, more](const InnerTubeReply &r) { return parseHome(r, hl, more); };
    std::function<void(HomePage)> done = [this, generation](HomePage page) { apply(generation, page); };
    emit changed();
    client_->post<HomePage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, done);
}

HomePage HomeService::parseHome(const InnerTubeReply &reply, const QString &hl, bool continuation) {
    HomePage page;
    page.outcome = reply.outcome;
    if (reply.outcome != Outcome::Ok) return page;
    const RendererParser parser(hl);
    QJsonValue container;
    if (continuation) {
        QJsonArray items;
        bool marker = false;
        for (const QJsonValue &v : reply.json.value(QStringLiteral("onResponseReceivedActions")).toArray()) {
            const QJsonValue action = v.toObject().value(QStringLiteral("appendContinuationItemsAction"));
            if (!action.isObject()) continue;
            marker = true;
            for (const QJsonValue &x : action.toObject().value(QStringLiteral("continuationItems")).toArray()) items.append(x);
        }
        if (!marker) { page.outcome = Outcome::ShapeDrift; return page; }
        container = QJsonValue(items);
    } else {
        const QJsonObject grid = json::findFirst(json::at(reply.json, QStringLiteral("contents.twoColumnBrowseResultsRenderer")),
                                                 QStringLiteral("richGridRenderer"));
        if (grid.isEmpty()) { page.outcome = Outcome::ShapeDrift; return page; }
        container = QJsonValue(grid);
    }
    const ParsedItems parsed = parser.parse(container);
    for (const Item &it : parsed.items) if (isVideoLike(it) && !it.id.empty()) page.items.push_back(it);
    page.continuation = parsed.continuation;
    page.outcome = page.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return page;
}

}}}
