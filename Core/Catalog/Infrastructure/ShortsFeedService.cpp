// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortsFeedService.h"

#include "ChannelService.h"
#include "InnerTubeClient.h"
#include "Json.h"
#include "SearchService.h"
#include "Core/Catalog/Domain/Images.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QPointer>
#include <map>
#include <memory>

namespace nordtile { namespace video { namespace catalog {

namespace {

// The vertical frame of a Short (a lockup's own thumbnail, or the 9:16 frame by id).
void makeVertical(Item &item, const QString &lockupThumbnail = QString()) {
    item.kind = ItemKind::Short;
    if (item.shortEvidence == ShortEvidence::None) item.shortEvidence = ShortEvidence::Renderer;
    item.thumbnailUrl = !lockupThumbnail.isEmpty() ? lockupThumbnail.toStdString() : shortsThumbnailUrl(item.id);
}

}  // namespace

ShortsFeedService::ShortsFeedService(InnerTubeClient *anonymous, ChannelService *channels, CatalogCounters *counters,
                                     QObject *parent)
    : QObject(parent), anonymous_(anonymous), channels_(channels), counters_(counters) {}

qint64 ShortsFeedService::nowMs() const {
    if (clock_) return clock_();
    static QElapsedTimer t;
    if (!t.isValid()) t.start();
    return t.elapsed();
}

QString ShortsFeedService::cacheKey() const {
    QCryptographicHash h(QCryptographicHash::Sha1);
    InnerTubeClient *client = signed_ ? signed_ : anonymous_;
    if (client) h.addData((client->locale().hl + QLatin1Char('-') + client->locale().gl).toUtf8());
    h.addData(signed_ ? "signed" : "anonymous");
    for (const ChannelRef &c : subscriptions_) h.addData(c.id.c_str(), static_cast<int>(c.id.size()));
    return QString::fromLatin1(h.result().toHex());
}

void ShortsFeedService::setChannels(const ChannelList &channels) {
    subscriptions_ = channels;
    cachedAt_ = -1;   // a different list: the cached page 1 does not apply
    std::set<std::string> now;
    for (const ChannelRef &c : channels) now.insert(c.id);
    if (now == subscribed_) return;
    subscribed_ = now;
    emit subscriptionsChanged();
}

void ShortsFeedService::setSignedClient(InnerTubeClient *client) {
    if (client == signed_) return;
    signed_ = client;
    cachedAt_ = -1;   // personalised rows never outlive the account
    clear();
}

void ShortsFeedService::clear() {
    ++generation_;
    pending_ = 0;
    rows_.clear();
    seen_.clear();
    pageRows_.clear();
    nextChannel_ = 0;
    popularContinuation_.clear();
    popularStarted_ = false;
    popularEnded_ = false;
    sequenceParams_.clear();
    sequenceContinuation_.clear();
    seeds_.clear();
    nextSeed_ = 0;
    seedAttempts_ = 0;
    loaded_ = false;
    outcome_ = Outcome::ValidEmpty;
    emit changed();
}

bool ShortsFeedService::canLoadMore() const {
    if (busy() || !loaded_) return false;
    const bool seedsLeft = anonymous_ && nextSeed_ < seeds_.size();
    if (signed_) return !sequenceContinuation_.isEmpty() || !popularContinuation_.isEmpty() || seedsLeft;
    return nextChannel_ < subscriptions_.size() || !popularEnded_ || seedsLeft;
}

QJsonObject ShortsFeedService::popularBody() {
    QJsonObject body;
    body.insert(QStringLiteral("query"), QStringLiteral("#shorts"));
    body.insert(QStringLiteral("params"), QLatin1String(innertube::kSearchParamsShorts));
    return body;
}

ShortsPage ShortsFeedService::parsePopular(const InnerTubeReply &reply, const QString &hl, bool continuation) {
    ShortsPage page;
    page.outcome = reply.outcome;
    if (reply.outcome != Outcome::Ok) return page;
    const SearchResult s = continuation ? SearchService::parseContinuation(reply.json, hl)
                                        : SearchService::parseFirstPage(reply.json, hl);
    page.outcome = s.outcome;
    page.continuation = s.continuation;
    // Each lockup's vertical thumbnail, by id.
    std::map<std::string, QString> frames;
    std::function<void(const QJsonValue &)> walk = [&](const QJsonValue &v) {
        if (v.isObject()) {
            const QJsonObject o = v.toObject();
            const QJsonObject e = o.value(QStringLiteral("reelWatchEndpoint")).toObject();
            if (!e.isEmpty()) {
                const QString id = e.value(QStringLiteral("videoId")).toString();
                const QString url = json::bestImage(e.value(QStringLiteral("thumbnail")));
                if (!id.isEmpty() && !url.isEmpty()) frames[id.toStdString()] = url;
                const QString seed = e.value(QStringLiteral("sequenceParams")).toString();
                if (page.sequenceParams.isEmpty()) page.sequenceParams = seed;
                if (!id.isEmpty() && !seed.isEmpty() && !page.seeds.count(id.toStdString())) page.seeds[id.toStdString()] = seed;
            }
            for (auto it = o.constBegin(); it != o.constEnd(); ++it) walk(it.value());
        } else if (v.isArray()) {
            for (const QJsonValue &x : v.toArray()) walk(x);
        }
    };
    walk(reply.json);
    for (Item it : s.rows) {
        if (it.kind != ItemKind::Short || it.id.empty()) continue;
        const auto f = frames.find(it.id);
        makeVertical(it, f == frames.end() ? QString() : f->second);
        page.items.push_back(it);
    }
    if (page.outcome == Outcome::Ok && page.items.empty()) page.outcome = Outcome::ValidEmpty;
    return page;
}

ShortsPage ShortsFeedService::parseSequence(const InnerTubeReply &reply) {
    ShortsPage page;
    page.outcome = reply.outcome;
    if (reply.outcome != Outcome::Ok) return page;
    const QJsonValue entries = reply.json.value(QStringLiteral("entries"));
    if (!entries.isArray()) { page.outcome = Outcome::ShapeDrift; return page; }
    for (const QJsonValue &v : entries.toArray()) {
        const QJsonObject e = json::findFirst(v, QStringLiteral("reelWatchEndpoint"));
        const QString id = e.value(QStringLiteral("videoId")).toString();
        if (id.size() != 11) continue;
        Item it;
        it.id = id.toStdString();
        it.shortEvidence = ShortEvidence::Renderer;
        makeVertical(it, json::bestImage(e.value(QStringLiteral("thumbnail"))));
        page.items.push_back(it);
    }
    page.continuation = json::continuationToken(reply.json.value(QStringLiteral("continuationEndpoint")).toObject());
    page.outcome = page.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return page;
}

void ShortsFeedService::refresh() {
    const QString key = cacheKey();
    if (cachedAt_ >= 0 && cachedKey_ == key && nowMs() - cachedAt_ < kCacheMs) {
        ++generation_;
        pending_ = 0;
        rows_ = cachedRows_;
        seen_.clear();
        for (const ShortsRow &r : rows_) seen_.insert(rowSeenKey(r.item));
        seeds_ = cachedSeeds_;
        nextSeed_ = cachedNextSeed_;
        seedAttempts_ = 0;
        nextChannel_ = cachedNextChannel_;
        popularContinuation_ = cachedPopularContinuation_;
        popularStarted_ = cachedPopularStarted_;
        popularEnded_ = popularStarted_ && popularContinuation_.isEmpty();
        sequenceContinuation_ = cachedSequenceContinuation_;
        loaded_ = true;
        outcome_ = rows_.empty() ? Outcome::ValidEmpty : Outcome::Ok;
        emit changed();
        return;
    }
    clear();
    startPage(true);
}

void ShortsFeedService::loadMore() {
    if (!canLoadMore()) return;
    startPage(false);
}

void ShortsFeedService::startPage(bool first) {
    const quint64 generation = ++generation_;
    pageRows_.clear();
    seedAttempts_ = 0;
    if (signed_) {
        requestSequence(generation, first);
        return;
    }
    std::vector<ChannelRef> batch;
    while (nextChannel_ < subscriptions_.size() && static_cast<int>(batch.size()) < kChannelsPerPage)
        batch.push_back(subscriptions_[nextChannel_++]);
    if (!batch.empty() && channels_) {
        requestChannels(generation, batch);
        return;
    }
    seedAttempts_ = 0;
    if (popularEnded_) requestSeeded(generation);
    else requestPopular(generation, !popularStarted_);
}

void ShortsFeedService::requestChannels(quint64 generation, std::vector<ChannelRef> batch) {
    // One Shorts tab per channel in parallel; interleaved in subscription order when all answered.
    struct Collect {
        std::vector<std::vector<Item>> items;
        std::vector<ChannelPage> pages;
        int left = 0;
    };
    std::shared_ptr<Collect> c(new Collect);
    c->items.resize(batch.size());
    c->pages.resize(batch.size());
    c->left = static_cast<int>(batch.size());
    pending_ += c->left;
    emit changed();
    for (std::size_t i = 0; i < batch.size(); ++i) {
        channels_->browseTab(QString::fromStdString(batch[i].id), QStringLiteral("shorts"), this,
                             [this, generation, c, i, batch](ChannelPage page) {
            if (generation != generation_) return;
            --pending_;
            if (page.outcome == Outcome::Ok && page.selectedTab == QLatin1String("shorts")) {
                for (Item it : page.items) {
                    if (!isVideoLike(it) || it.id.empty()) continue;
                    if (it.channelId.empty()) it.channelId = batch[i].id;
                    if (it.channel.empty()) it.channel = page.header.title.isEmpty() ? batch[i].name : page.header.title.toStdString();
                    makeVertical(it);
                    c->items[i].push_back(it);
                }
            }
            c->pages[i] = page;
            if (--c->left > 0) return;
            // Round-robin: the newest Short of each channel first, then the next of each, ...
            for (std::size_t round = 0;; ++round) {
                bool any = false;
                for (std::size_t k = 0; k < c->items.size(); ++k) {
                    if (round >= c->items[k].size()) continue;
                    any = true;
                    ShortsRow row;
                    row.item = c->items[k][round];
                    row.source = ShortsSource::Subscriptions;
                    row.channelAvatar = c->pages[k].header.avatarUrl.toStdString();
                    pageRows_.push_back(row);
                }
                if (!any) break;
            }
            if (static_cast<int>(pageRows_.size()) < kMinPageRows) {
                if (!popularEnded_) requestPopular(generation, !popularStarted_);
                else requestSeeded(generation);
            } else {
                finishPage(generation);
            }
        });
    }
}

void ShortsFeedService::requestPopular(quint64 generation, bool first) {
    InnerTubeClient *client = signed_ ? signed_ : anonymous_;
    if (!client) { popularEnded_ = true; finishPage(generation); return; }
    if (!first && popularContinuation_.isEmpty()) { popularEnded_ = true; requestAfterPopular(generation); return; }
    QJsonObject body = first ? popularBody() : QJsonObject();
    if (!first) body.insert(QStringLiteral("continuation"), popularContinuation_);
    popularStarted_ = true;
    const QString hl = client->locale().hl;
    ++pending_;
    emit changed();
    std::function<ShortsPage(const InnerTubeReply &)> parse = [hl, first](const InnerTubeReply &r) { return parsePopular(r, hl, !first); };
    std::function<void(ShortsPage)> done = [this, generation, first](ShortsPage page) {
        if (generation != generation_) return;
        --pending_;
        if (counters_) counters_->count(Area::Search, page.outcome);
        if (page.outcome == Outcome::Ok || page.outcome == Outcome::ValidEmpty) {
            popularContinuation_ = page.continuation;
            if (popularContinuation_.isEmpty()) popularEnded_ = true;
            // The later pages of the search hold one to five Shorts: not worth a request each.
            if (!first && static_cast<int>(page.items.size()) < kThinPageRows) popularEnded_ = true;
            if (sequenceParams_.isEmpty()) sequenceParams_ = page.sequenceParams;
            for (const Item &it : page.items) {
                ShortsRow row;
                row.item = it;
                row.source = signed_ ? ShortsSource::Account : ShortsSource::Popular;
                const auto seed = page.seeds.find(it.id);
                if (seed != page.seeds.end()) {
                    row.sequenceParams = seed->second.toStdString();
                    rememberSeed(row.sequenceParams);
                }
                pageRows_.push_back(row);
            }
        } else if (pageRows_.empty()) {
            outcome_ = page.outcome;
        }
        finishPage(generation);
    };
    client->post<ShortsPage>(QLatin1String(innertube::kEndpointSearch), body, this, parse, done);
}

void ShortsFeedService::requestSequence(quint64 generation, bool first) {
    // The account's own sequence needs a seed: the first lockup of a (signed) Shorts search.
    if (first && sequenceParams_.isEmpty()) {
        InnerTubeClient *client = signed_;
        const QString hl = client->locale().hl;
        ++pending_;
        emit changed();
        std::function<ShortsPage(const InnerTubeReply &)> parse = [hl](const InnerTubeReply &r) { return parsePopular(r, hl, false); };
        std::function<void(ShortsPage)> done = [this, generation](ShortsPage page) {
            if (generation != generation_) return;
            --pending_;
            popularStarted_ = true;
            popularContinuation_ = page.continuation;
            sequenceParams_ = page.sequenceParams;
            if (sequenceParams_.isEmpty()) {
                // No seed: the signed search rows themselves.
                for (const Item &it : page.items) { ShortsRow row; row.item = it; row.source = ShortsSource::Account; pageRows_.push_back(row); }
                if (pageRows_.empty()) outcome_ = page.outcome;
                finishPage(generation);
                return;
            }
            // The search rows follow the sequence rows on the page.
            for (const Item &it : page.items) {
                ShortsRow row;
                row.item = it;
                row.source = ShortsSource::Account;
                const auto seed = page.seeds.find(it.id);
                if (seed != page.seeds.end()) { row.sequenceParams = seed->second.toStdString(); rememberSeed(row.sequenceParams); }
                pageRows_.push_back(row);
            }
            requestSequence(generation, true);
        };
        signed_->post<ShortsPage>(QLatin1String(innertube::kEndpointSearch), popularBody(), this, parse, done);
        return;
    }
    const QString token = first ? sequenceParams_ : sequenceContinuation_;
    if (token.isEmpty()) { requestPopular(generation, false); return; }
    QJsonObject body;
    body.insert(QStringLiteral("sequenceParams"), token);
    ++pending_;
    emit changed();
    std::function<ShortsPage(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return parseSequence(r); };
    std::function<void(ShortsPage)> done = [this, generation](ShortsPage page) {
        if (generation != generation_) return;
        --pending_;
        if (page.outcome == Outcome::Ok || page.outcome == Outcome::ValidEmpty) {
            sequenceContinuation_ = page.continuation;
            std::vector<ShortsRow> account;
            for (const Item &it : page.items) { ShortsRow row; row.item = it; row.source = ShortsSource::Account; account.push_back(row); }
            pageRows_.insert(pageRows_.begin(), account.begin(), account.end());   // the account's own order first
        }
        finishPage(generation);
    };
    signed_->post<ShortsPage>(QLatin1String(innertube::kEndpointReelSequence), body, this, parse, done);
}

void ShortsFeedService::rememberSeed(const std::string &seed) {
    if (seed.empty()) return;
    for (const std::string &known : seeds_) if (known == seed) return;
    seeds_.push_back(seed);
}

void ShortsFeedService::requestAfterPopular(quint64 generation) {
    seedAttempts_ = 0;
    requestSeeded(generation);
}

void ShortsFeedService::requestSeeded(quint64 generation) {
    // YouTube's own Shorts sequence seeded by a popular row: the first page of a seed holds 10 to 25
    // Shorts (its continuation repeats them). A page that adds nothing new tries the next seed.
    if (!anonymous_ || nextSeed_ >= seeds_.size() || seedAttempts_ >= kSeedRequestsPerPage) { finishPage(generation); return; }
    const std::size_t index = nextSeed_++;
    ++seedAttempts_;
    QJsonObject body;
    body.insert(QStringLiteral("sequenceParams"), QString::fromStdString(seeds_[index]));
    ++pending_;
    emit changed();
    std::function<ShortsPage(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return parseSequence(r); };
    std::function<void(ShortsPage)> done = [this, generation, index](ShortsPage page) {
        if (generation != generation_) return;
        --pending_;
        if (counters_) counters_->count(Area::Search, page.outcome);
        if (page.outcome != Outcome::Ok && page.outcome != Outcome::ValidEmpty) {
            // A failure does not use the seed up: the next page asks it again.
            if (nextSeed_ == index + 1) nextSeed_ = index;
            --seedAttempts_;
            finishPage(generation);
            return;
        }
        std::set<std::string> here;
        for (const ShortsRow &r : pageRows_) here.insert(rowSeenKey(r.item));
        int added = 0;
        for (const Item &it : page.items) {
            const std::string key = rowSeenKey(it);
            if (seen_.count(key) || !here.insert(key).second) continue;
            ShortsRow row;
            row.item = it;
            row.source = signed_ ? ShortsSource::Account : ShortsSource::Popular;
            pageRows_.push_back(row);
            ++added;
        }
        if (added == 0) requestSeeded(generation);   // nothing new: the next seed, within the page's budget
        else finishPage(generation);
    };
    anonymous_->post<ShortsPage>(QLatin1String(innertube::kEndpointReelSequence), body, this, parse, done);
}

void ShortsFeedService::finishPage(quint64 generation) {
    if (generation != generation_ || pending_ > 0) return;
    std::vector<Item> items;
    items.reserve(pageRows_.size());
    for (const ShortsRow &r : pageRows_) items.push_back(r.item);
    RowFilterSettings settings = filter_;
    settings.hideShorts = false;   // this is the Shorts section
    const RowFilterResult f = filterRows(items, settings, &seen_);
    std::map<std::string, const ShortsRow *> byId;
    for (const ShortsRow &r : pageRows_) if (!byId.count(r.item.id)) byId[r.item.id] = &r;
    const bool first = rows_.empty();
    for (const Item &it : f.visible) {
        ShortsRow row = *byId[it.id];
        row.item = it;
        if (row.item.kind != ItemKind::Short) row.item.kind = ItemKind::Short;
        rows_.push_back(row);
    }
    pageRows_.clear();
    loaded_ = true;
    if (!rows_.empty()) outcome_ = Outcome::Ok;
    else if (outcome_ == Outcome::Ok) outcome_ = Outcome::ValidEmpty;
    if (first && !rows_.empty()) {
        cachedKey_ = cacheKey();
        cachedAt_ = nowMs();
        cachedRows_ = rows_;
        cachedNextChannel_ = nextChannel_;
        cachedPopularContinuation_ = popularContinuation_;
        cachedPopularStarted_ = popularStarted_;
        cachedSequenceContinuation_ = sequenceContinuation_;
        cachedSeeds_ = seeds_;
        cachedNextSeed_ = nextSeed_;
    }
    emit changed();
}

}}}
