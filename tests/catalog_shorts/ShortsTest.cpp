// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The Shorts section's feed (ShortsFeedService, ShortsModel) over recorded answers: popular
// Shorts (the "#shorts" search with the Shorts filter, es/MX), YouTube's Shorts sequence, and
// a channel's Shorts tab (NASA). Signed answers are the same recorded shapes (no account used).
#include "tests/catalog_common/TestSupport.h"

#include "Clients/Sailfish/Presentation/DislikeInfo.h"
#include "Clients/Sailfish/Presentation/ShortsModel.h"
#include "Clients/Sailfish/Presentation/TapIntent.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/ShortDetail.h"
#include "Core/Catalog/Domain/TextParse.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/DislikeService.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/ShortDetailService.h"
#include "Core/Catalog/Infrastructure/ShortsFeedService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSettings>
#include <QTemporaryDir>

using namespace nordtile::video;
using namespace nordtile::video::catalog;
using catalog_test::FakeTransport;

namespace {
const char *const kNasa = "UCLA_DiR1FfKNvjuUpBHmylQ";

InnerTubeReply okReply(const QByteArray &body) {
    InnerTubeReply r;
    r.outcome = Outcome::Ok;
    r.httpStatus = 200;
    r.json = QJsonDocument::fromJson(body).object();
    return r;
}

QString tokenOf(const QString &fixture, bool continuation) {
    return ShortsFeedService::parsePopular(okReply(catalog_test::fixture(fixture)), QStringLiteral("es"), continuation).continuation;
}

void servePopular(FakeTransport &net) {
    const QString token = tokenOf(QStringLiteral("shorts_popular_es.json"), false);
    net.on(QStringLiteral("/youtubei/v1/search"), "\"" + token.toLatin1() + "\"", 200, catalog_test::fixture(QStringLiteral("shorts_popular_es_p2.json")));
    net.on(QStringLiteral("/youtubei/v1/search"), "\"query\":\"#shorts\"", 200, catalog_test::fixture(QStringLiteral("shorts_popular_es.json")));
}
}

static void parsers() {
    const ShortsPage pop = ShortsFeedService::parsePopular(okReply(catalog_test::fixture(QStringLiteral("shorts_popular_es.json"))),
                                                           QStringLiteral("es"), false);
    CHECK(pop.outcome == Outcome::Ok);
    CHECK(pop.items.size() >= 20);
    CHECK(!pop.continuation.isEmpty());
    CHECK(!pop.sequenceParams.isEmpty());
    for (const Item &it : pop.items) {
        CHECK(it.kind == ItemKind::Short && it.id.size() == 11 && !it.title.empty());
        CHECK(it.thumbnailUrl.find("i.ytimg.com/vi/" + it.id + "/") != std::string::npos);
        CHECK(it.thumbnailUrl.find("mqdefault") == std::string::npos);    // vertical, never the 16:9 list image
    }
    int withViews = 0;
    for (const Item &it : pop.items) if (!it.viewText.empty()) ++withViews;
    CHECK(withViews > 0);
    const ShortsPage p2 = ShortsFeedService::parsePopular(okReply(catalog_test::fixture(QStringLiteral("shorts_popular_es_p2.json"))),
                                                          QStringLiteral("es"), true);
    CHECK(p2.outcome == Outcome::Ok && !p2.items.empty());
    const ShortsPage seq = ShortsFeedService::parseSequence(okReply(catalog_test::fixture(QStringLiteral("shorts_sequence_es.json"))));
    CHECK(seq.outcome == Outcome::Ok);
    CHECK_EQ(static_cast<int>(seq.items.size()), 5);
    CHECK(!seq.continuation.isEmpty());
    for (const Item &it : seq.items) CHECK(it.thumbnailUrl.find("frame0.jpg") != std::string::npos);
    CHECK(ShortsFeedService::parseSequence(okReply("{\"responseContext\":{}}")).outcome == Outcome::ShapeDrift);
    // The vertical frame and the definitions behind the requests.
    CHECK_EQ(shortsThumbnailUrl("9fYbmsmGriM"), std::string("https://i.ytimg.com/vi/9fYbmsmGriM/frame0.jpg"));
    CHECK(shortsThumbnailUrl(std::string()).empty());
    CHECK_EQ(innertube::channelTabParams(QStringLiteral("shorts")), QString::fromLatin1(innertube::kTabParamsShorts));
    CHECK(innertube::channelTabParams(QStringLiteral("releases")).isEmpty());
    CHECK_EQ(std::string(innertube::kEndpointReelSequence), std::string("reel/reel_watch_sequence"));
    const QJsonObject body = ShortsFeedService::popularBody();
    CHECK_EQ(body.value(QStringLiteral("query")).toString(), QStringLiteral("#shorts"));
    CHECK_EQ(body.value(QStringLiteral("params")).toString(), QStringLiteral("EgIQCQ=="));
}

static void anonymousFeed() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts"), 1);
    FakeTransport net;
    servePopular(net);
    net.on(QStringLiteral("/youtubei/v1/browse"), "EgZzaG9ydHPyBgUKA5oBAA==", 200, catalog_test::fixture(QStringLiteral("channel_shorts.json")));
    InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
    ChannelService channels(&client);
    ShortsFeedService feed(&client, &channels);
    qint64 now = 0;
    feed.setClock([&now] { return now; });
    sailfish::TapIntent intent;
    sailfish::ShortsModel model(&feed, &intent);
    QStringList likely;
    QObject::connect(&intent, &sailfish::TapIntent::likelyVisible, [&likely](const QStringList &ids) { likely = ids; });

    ChannelList subs;
    subs.push_back(ChannelRef(kNasa, "NASA"));
    feed.setChannels(subs);
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy(); }, 8000));
    CHECK(model.count() >= ShortsFeedService::kMinPageRows);
    // The subscription's Shorts first, then popular ones filled in (NASA's tab had fewer than 20).
    const QVariantMap first = model.get(0);
    CHECK_EQ(first.value(QStringLiteral("source")).toString(), QStringLiteral("subscriptions"));
    CHECK_EQ(first.value(QStringLiteral("channelId")).toString(), QString::fromLatin1(kNasa));
    CHECK(!first.value(QStringLiteral("channel")).toString().isEmpty());
    CHECK(first.value(QStringLiteral("thumbnail")).toString().endsWith(QLatin1String("frame0.jpg")));
    CHECK_EQ(first.value(QStringLiteral("likeCount")).toDouble(), -1.0);
    // Subscription rows show the channel's avatar (from the Shorts tab answer's header).
    CHECK(first.value(QStringLiteral("channelAvatar")).toString().startsWith(QLatin1String("https://")));
    const QVariantMap hints = first.value(QStringLiteral("hints")).toMap();
    CHECK_EQ(hints.value(QStringLiteral("title")).toString(), first.value(QStringLiteral("title")).toString());
    CHECK(hints.contains(QStringLiteral("durationSeconds")));
    QSet<QString> ids;
    for (int i = 0; i < model.count(); ++i) ids.insert(model.get(i).value(QStringLiteral("videoId")).toString());
    CHECK_EQ(ids.size(), model.count());                                    // no duplicates
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/reel/")), 0);            // no sequence signed out
    // setCurrent: the next two ids for playback, and a preload near the end.
    model.setCurrent(0);
    CHECK(likely == QStringList() << model.get(1).value(QStringLiteral("videoId")).toString()
                                  << model.get(2).value(QStringLiteral("videoId")).toString());
    const int page1 = model.count();
    model.setCurrent(page1 - 2);
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > page1; }, 8000));
    // The only subscription is used up: page 2 is popular Shorts for the locale.
    CHECK_EQ(model.get(page1).value(QStringLiteral("source")).toString(), QStringLiteral("popular"));
    CHECK(model.get(page1).value(QStringLiteral("channel")).toString().isEmpty());
    // Within 10 minutes page 1 comes from the cache; after that it is asked again.
    const int searches = net.count(QStringLiteral("/youtubei/v1/browse"));
    now += 5 * 60 * 1000;
    model.refresh();
    CHECK(!model.busy());
    CHECK_EQ(model.count(), page1);
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/browse")), searches);
    now += 6 * 60 * 1000;
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return !model.busy() && model.count() > 0; }, 8000));
    CHECK(net.count(QStringLiteral("/youtubei/v1/browse")) > searches);
}

static void signedFeed() {
    // Signed in: the account's sequence (seeded by a signed Shorts search) through the signed
    // transport only; signed out again, the anonymous path.
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts-signed"), 1);
    FakeTransport anonymousNet, signedNet;
    servePopular(anonymousNet);
    servePopular(signedNet);
    const ShortsPage seq = ShortsFeedService::parseSequence(okReply(catalog_test::fixture(QStringLiteral("shorts_sequence_es.json"))));
    signedNet.on(QStringLiteral("/youtubei/v1/reel/reel_watch_sequence"), "\"" + seq.continuation.toLatin1() + "\"", 200,
                 catalog_test::fixture(QStringLiteral("shorts_sequence_es.json")));
    signedNet.on(QStringLiteral("/youtubei/v1/reel/reel_watch_sequence"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("shorts_sequence_es.json")));
    InnerTubeClient anonymousClient(&anonymousNet, &identity, &lane, DiscoveryLocale());
    InnerTubeClient signedClient(&signedNet, &identity, &lane, DiscoveryLocale());
    ShortsFeedService feed(&anonymousClient, nullptr);
    sailfish::ShortsModel model(&feed);
    feed.setSignedClient(&signedClient);
    CHECK(model.signedIn());
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy(); }, 8000));
    CHECK(model.count() > 0);
    CHECK_EQ(model.get(0).value(QStringLiteral("source")).toString(), QStringLiteral("account"));
    CHECK_EQ(model.get(0).value(QStringLiteral("videoId")).toString(), QString::fromStdString(seq.items.at(0).id));   // the sequence leads
    CHECK_EQ(signedNet.count(QStringLiteral("/youtubei/v1/reel/reel_watch_sequence")), 1);
    CHECK_EQ(anonymousNet.requests.size(), 0);
    CHECK(model.canLoadMore());
    const int page1 = model.count();
    model.loadMore();
    CHECK(catalog_test::waitFor([&] { return !model.busy(); }, 8000));
    CHECK_EQ(signedNet.count(QStringLiteral("/youtubei/v1/reel/reel_watch_sequence")), 2);
    CHECK(model.count() >= page1);
    // Sign-out: the rows go; the next refresh is anonymous.
    feed.setSignedClient(nullptr);
    CHECK_EQ(model.count(), 0);
    CHECK(!model.signedIn());
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy(); }, 8000));
    CHECK(model.count() > 0);
    CHECK_EQ(model.get(0).value(QStringLiteral("source")).toString(), QStringLiteral("popular"));
    CHECK(anonymousNet.count(QStringLiteral("/youtubei/v1/search")) >= 1);
}

// ---------------------------------------------------------------- details (one request per Short)

namespace {

const std::string kHl = "en";

ShortDetailResult parseItem(const char *fixtureName, const QString &hl = QStringLiteral("en")) {
    return ShortDetailService::parse(catalog_test::fixtureJson(QString::fromLatin1(fixtureName)), QStringLiteral("AAAAAAAAAAA"), hl);
}

bool hasDigit(const std::string &text) {
    for (char c : text) if (c >= '0' && c <= '9') return true;
    return false;
}

// A thin popular page derived from the recorded second one (a real later page holds one to five
// Shorts): only the first `keep` lockups of its first grid, the continuation kept.
QByteArray thinPopularPage(int keep) {
    QJsonObject doc = catalog_test::fixtureJson(QStringLiteral("shorts_popular_es_p2.json"));
    QJsonArray commands = doc.value(QStringLiteral("onResponseReceivedCommands")).toArray();
    QJsonObject command = commands.at(0).toObject();
    QJsonObject action = command.value(QStringLiteral("appendContinuationItemsAction")).toObject();
    QJsonArray items = action.value(QStringLiteral("continuationItems")).toArray();
    QJsonObject wrap = items.at(0).toObject();
    QJsonObject section = wrap.value(QStringLiteral("itemSectionRenderer")).toObject();
    QJsonArray contents = section.value(QStringLiteral("contents")).toArray();
    QJsonObject grid = contents.at(1).toObject();
    QJsonObject shelf = grid.value(QStringLiteral("gridShelfViewModel")).toObject();
    QJsonArray lockups = shelf.value(QStringLiteral("contents")).toArray();
    QJsonArray few;
    for (int i = 0; i < keep && i < lockups.size(); ++i) few.append(lockups.at(i));
    shelf.insert(QStringLiteral("contents"), few);
    grid.insert(QStringLiteral("gridShelfViewModel"), shelf);
    QJsonArray kept;
    kept.append(contents.at(0));
    kept.append(grid);
    section.insert(QStringLiteral("contents"), kept);
    wrap.insert(QStringLiteral("itemSectionRenderer"), section);
    items.replace(0, wrap);
    action.insert(QStringLiteral("continuationItems"), items);
    command.insert(QStringLiteral("appendContinuationItemsAction"), action);
    commands.replace(0, command);
    doc.insert(QStringLiteral("onResponseReceivedCommands"), commands);
    return QJsonDocument(doc).toJson(QJsonDocument::Compact);
}

}  // namespace

static void detailParsing() {
    // The body and the endpoint.
    const QJsonObject body = ShortDetailService::requestBody(QStringLiteral("9fYbmsmGriM"));
    CHECK_EQ(body.value(QStringLiteral("playerRequest")).toObject().value(QStringLiteral("videoId")).toString(), QStringLiteral("9fYbmsmGriM"));
    CHECK(body.value(QStringLiteral("disablePlayerResponse")).toBool());
    CHECK_EQ(body.size(), 2);
    CHECK_EQ(std::string(innertube::kEndpointReelItemWatch), std::string("reel/reel_item_watch"));

    // A Short with a related video (a watch link), counts, a channel and a sound.
    AvatarBook::instance().clear();
    const ShortDetailResult en = parseItem("shorts_item_en.json");
    CHECK(en.outcome == Outcome::Ok);
    const ShortDetail &d = en.detail;
    CHECK(d.channelHandle.size() > 1 && d.channelHandle[0] == '@');
    CHECK(isFeedChannelId(d.channelId));
    CHECK(d.channelAvatar.compare(0, 8, "https://") == 0);
    CHECK(!d.channelName.empty());
    CHECK(!d.title.empty());
    CHECK(hasDigit(d.likeText));
    CHECK(d.likeCount > 0);
    // The exact count agrees with the compact text ("4.8K" ~ 4862).
    const std::int64_t compact = parseCountText(d.likeText, kHl);
    CHECK(compact > 0 && d.likeCount * 100 >= compact * 90 && d.likeCount * 100 <= compact * 110);
    CHECK(hasDigit(d.commentText) && d.commentCount > 0 && !d.commentsDisabled);
    CHECK(ShortDetail().likeCount == -1 && ShortDetail().commentCount == -1);
    CHECK(d.relatedVideoId.size() == 11 && !d.relatedVideoTitle.empty() && !d.relatedVideoIsShort);
    CHECK(!d.publishedText.empty());
    CHECK(!d.soundText.empty());
    CHECK_EQ(d.relatedVideoThumbnail(), "https://i.ytimg.com/vi/" + d.relatedVideoId + "/mqdefault.jpg");
    CHECK(ShortDetail().relatedVideoThumbnail().empty());
    CHECK_EQ(AvatarBook::instance().lookup(d.channelId), d.channelAvatar);   // rows of the channel get it too

    // A remix: the chip links to another Short; the creator hides the like count and wrote a description.
    const ShortDetailResult remix = parseItem("shorts_item_remix_en.json");
    CHECK(remix.outcome == Outcome::Ok);
    CHECK(remix.detail.relatedVideoId.size() == 11 && remix.detail.relatedVideoIsShort);
    CHECK(remix.detail.likeText.empty() && remix.detail.likeCount == -1);
    CHECK(!remix.detail.description.empty());
    // The metadata items come in no fixed order: a sticker leads this one, the channel is still found.
    CHECK(isFeedChannelId(remix.detail.channelId) && !remix.detail.channelHandle.empty());

    // Comments off: no button, no text, no count.
    const ShortDetailResult off = parseItem("shorts_item_nocomments_en.json");
    CHECK(off.outcome == Outcome::Ok);
    CHECK(off.detail.commentsDisabled && off.detail.commentText.empty() && off.detail.commentCount == -1);
    CHECK(off.detail.likeCount > 0 && hasDigit(off.detail.likeText));
    CHECK(off.detail.relatedVideoId.empty() && !off.detail.relatedVideoIsShort);
    // Hidden likes with comments on.
    const ShortDetailResult hidden = parseItem("shorts_item_hiddenlikes_en.json");
    CHECK(hidden.outcome == Outcome::Ok);
    CHECK(hidden.detail.likeText.empty() && hidden.detail.likeCount == -1);
    CHECK(!hidden.detail.commentsDisabled && hidden.detail.commentCount > 0);

    // A Short with a real description, asked in Spanish: counts in the display language, exact
    // numbers read with that language's grouping ("62.285 Me gusta" -> 62285).
    const ShortDetailResult es = parseItem("shorts_item_description_es.json", QStringLiteral("es"));
    CHECK(es.outcome == Outcome::Ok);
    CHECK(es.detail.description.find("Mejores Tiktoks") != std::string::npos);
    CHECK(es.detail.description.find("https://") != std::string::npos);                 // links kept as text
    CHECK(es.detail.likeCount > 1000 && es.detail.likeCount < 1000000);
    CHECK(es.detail.commentCount > 0 && hasDigit(es.detail.commentText));
    CHECK(es.detail.viewCount > 1000);
    const std::string shortText = es.detail.descriptionShort();
    CHECK(!shortText.empty() && shortText.size() <= 110 && shortText.find('\n') == std::string::npos);
    CHECK(es.detail.description.compare(0, 12, shortText, 0, 12) == 0);
    // Without a description the short form is empty too.
    CHECK(en.detail.description.empty() && en.detail.descriptionShort().empty());

    // The short description: first line, a limit in code points, an ellipsis when cut.
    ShortDetail text;
    text.description = "  First line  \nsecond line";
    CHECK_EQ(text.descriptionShort(), std::string("First line\xE2\x80\xA6"));      // more lines follow
    text.description = "One line only";
    CHECK_EQ(text.descriptionShort(), std::string("One line only"));
    text.description = "d\xC3\xAD" "as de f\xC3\xBA" "tbol y m\xC3\xA1s";           // multi-byte letters count once
    CHECK_EQ(text.descriptionShort(6), std::string("d\xC3\xAD" "as d\xE2\x80\xA6"));
    text.description = std::string(150, 'x');
    CHECK_EQ(text.descriptionShort().size(), std::size_t(100 + 3));
    text.description = "   \n  ";
    CHECK(text.descriptionShort().empty());

    // A missing Short: a success status without an overlay (final, never retried).
    CHECK(parseItem("shorts_item_gone.json").outcome == Outcome::Unavailable);
    CHECK(ShortDetailService::parse(QJsonObject(), QStringLiteral("AAAAAAAAAAA"), QStringLiteral("en")).outcome == Outcome::Unavailable);
    QJsonObject failed = catalog_test::fixtureJson(QStringLiteral("shorts_item_en.json"));
    failed.insert(QStringLiteral("status"), QStringLiteral("REEL_ITEM_WATCH_STATUS_FAILED"));
    CHECK(ShortDetailService::parse(failed, QStringLiteral("AAAAAAAAAAA"), QStringLiteral("en")).outcome == Outcome::Unavailable);
    // An overlay whose parts are all gone: the shape drifted.
    QJsonObject drift;
    drift.insert(QStringLiteral("overlay"), QJsonObject());
    CHECK(ShortDetailService::parse(drift, QStringLiteral("AAAAAAAAAAA"), QStringLiteral("en")).outcome == Outcome::ShapeDrift);
    QJsonObject renamed = catalog_test::fixtureJson(QStringLiteral("shorts_item_en.json"));
    QByteArray raw = QJsonDocument(renamed).toJson(QJsonDocument::Compact);
    raw.replace("reelPlayerOverlayViewModel", "reelPlayerOverlayViewModel2");
    CHECK(ShortDetailService::parse(QJsonDocument::fromJson(raw).object(), QStringLiteral("AAAAAAAAAAA"), QStringLiteral("en")).outcome == Outcome::ShapeDrift);
}

static void detailService() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts-details"), 1);
    FakeTransport net;
    net.on(QStringLiteral("/youtubei/v1/reel/reel_item_watch"), "\"videoId\":\"GoneGoneGon\"", 200, catalog_test::fixture(QStringLiteral("shorts_item_gone.json")));
    net.on(QStringLiteral("/youtubei/v1/reel/reel_item_watch"), "\"videoId\":\"FlakyFlaky1\"", 503, QByteArray(), 0, 1);
    net.on(QStringLiteral("/youtubei/v1/reel/reel_item_watch"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("shorts_item_en.json")), 20);
    InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
    CatalogCounters counters;
    qint64 now = 1000;
    ShortDetailService service(&client, &counters, nullptr, [&now] { return now; });
    QObject context;
    auto fetch = [&](const char *id, ShortDetailResult *out) {
        bool done = false;
        const bool sent = service.fetch(QString::fromLatin1(id), &context, [&](const ShortDetailResult &r) { *out = r; done = true; });
        CHECK(sent);
        CHECK(!done);                                                 // never synchronous
        CHECK(catalog_test::waitFor([&done] { return done; }));
    };

    // Two callers before the answer: one request, both answered.
    ShortDetailResult a, b;
    bool doneB = false;
    CHECK(service.fetch(QStringLiteral("ShortShort01"), &context, [&](const ShortDetailResult &r) { a = r; }) == false);   // 12 characters: not an id
    CHECK(service.fetch(QStringLiteral("ShortShrt01"), &context, [&](const ShortDetailResult &r) { a = r; }));
    CHECK(service.fetch(QStringLiteral("ShortShrt01"), &context, [&](const ShortDetailResult &r) { b = r; doneB = true; }));
    CHECK(catalog_test::waitFor([&doneB] { return doneB; }));
    CHECK(a.outcome == Outcome::Ok && b.outcome == Outcome::Ok);
    CHECK(!a.fromCache && isFeedChannelId(a.detail.channelId));
    CHECK_EQ(service.requestsSent(), 1);
    CHECK_EQ(net.count(QStringLiteral("reel_item_watch")), 1);
    // The request: anonymous WEB client in the context, the minimal body, no cookies or key.
    const HttpRequest sent = net.requests.first();
    CHECK(sent.url.toString().contains(QStringLiteral("/youtubei/v1/reel/reel_item_watch?prettyPrint=false")));
    CHECK(sent.body.contains("\"disablePlayerResponse\":true") && sent.body.contains("\"videoId\":\"ShortShrt01\""));
    CHECK(!sent.body.contains("visitorData"));
    // Kept for 30 minutes.
    CHECK(service.isCached(QStringLiteral("ShortShrt01")));
    ShortDetail kept;
    CHECK(service.peek(QStringLiteral("ShortShrt01"), &kept) && kept.channelId == a.detail.channelId);
    ShortDetailResult c;
    fetch("ShortShrt01", &c);
    CHECK(c.fromCache && c.outcome == Outcome::Ok);
    CHECK_EQ(net.count(QStringLiteral("reel_item_watch")), 1);
    now += ShortDetailService::kCacheTtlMs + 1;
    CHECK(!service.isCached(QStringLiteral("ShortShrt01")));
    fetch("ShortShrt01", &c);
    CHECK(!c.fromCache);
    CHECK_EQ(net.count(QStringLiteral("reel_item_watch")), 2);
    service.forget(QStringLiteral("ShortShrt01"));
    CHECK(!service.isCached(QStringLiteral("ShortShrt01")));

    // A failure is not kept; the next call asks again. A missing Short answers Unavailable.
    ShortDetailResult flaky;
    fetch("FlakyFlaky1", &flaky);
    CHECK(flaky.outcome == Outcome::Http5xx);
    CHECK(!service.isCached(QStringLiteral("FlakyFlaky1")));
    fetch("FlakyFlaky1", &flaky);
    CHECK(flaky.outcome == Outcome::Ok);
    ShortDetailResult gone;
    fetch("GoneGoneGon", &gone);
    CHECK(gone.outcome == Outcome::Unavailable && !service.isCached(QStringLiteral("GoneGoneGon")));
    CHECK(counters.value(Area::WatchNext, Outcome::Ok) >= 3);
    CHECK_EQ(counters.value(Area::WatchNext, Outcome::Unavailable), std::int64_t(1));
    // A caller that is gone when the answer arrives is not called.
    bool called = false;
    {
        QObject gonecontext;
        service.fetch(QStringLiteral("LateLateLat"), &gonecontext, [&](const ShortDetailResult &) { called = true; });
    }
    CHECK(catalog_test::waitFor([&] { return service.isCached(QStringLiteral("LateLateLat")); }));
    CHECK(!called);
}

static void seedsAndEndlessPaging() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts-endless"), 1);
    FakeTransport net;
    const QString p1 = QStringLiteral("shorts_popular_es.json");
    const ShortsPage first = ShortsFeedService::parsePopular(okReply(catalog_test::fixture(p1)), QStringLiteral("es"), false);
    const ShortsPage second = ShortsFeedService::parsePopular(okReply(catalog_test::fixture(QStringLiteral("shorts_popular_es_p2.json"))),
                                                              QStringLiteral("es"), true);
    // Every lockup names its own seed.
    CHECK(first.seeds.size() >= 20 && second.seeds.size() >= 10);
    std::vector<std::string> seeds;
    for (const Item &it : first.items) { const auto f = first.seeds.find(it.id); if (f != first.seeds.end()) seeds.push_back(f->second.toStdString()); }
    for (const Item &it : second.items) { const auto f = second.seeds.find(it.id); if (f != second.seeds.end()) seeds.push_back(f->second.toStdString()); }
    CHECK(seeds.size() >= 30);
    // Search: page 1, page 2, then a thin page 3 (2 Shorts, all seen) whose continuation is kept.
    const QString token1 = first.continuation;
    const QString token2 = second.continuation;
    CHECK(!token1.isEmpty() && !token2.isEmpty());
    net.on(QStringLiteral("/youtubei/v1/search"), "\"" + token2.toLatin1() + "\"", 200, thinPopularPage(2));
    net.on(QStringLiteral("/youtubei/v1/search"), "\"" + token1.toLatin1() + "\"", 200, catalog_test::fixture(QStringLiteral("shorts_popular_es_p2.json")));
    net.on(QStringLiteral("/youtubei/v1/search"), "\"query\":\"#shorts\"", 200, catalog_test::fixture(p1));
    // Sequences: seed 0 -> page A (11 Shorts), seed 1 -> page B (12, ten of them in A), a flaky
    // seed 2 (503 once), the rest -> A again (nothing new).
    const QByteArray seqA = catalog_test::fixture(QStringLiteral("shorts_sequence_en_a.json"));
    const QByteArray seqB = catalog_test::fixture(QStringLiteral("shorts_sequence_en_b.json"));
    const QString path = QStringLiteral("/youtubei/v1/reel/reel_watch_sequence");
    net.on(path, "\"sequenceParams\":\"" + QByteArray::fromStdString(seeds[0]) + "\"", 200, seqA);
    net.on(path, "\"sequenceParams\":\"" + QByteArray::fromStdString(seeds[1]) + "\"", 200, seqB);
    net.on(path, "\"sequenceParams\":\"" + QByteArray::fromStdString(seeds[2]) + "\"", 503, QByteArray(), 0, 1);
    net.on(path, QByteArray(), 200, seqA);
    InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
    ShortsFeedService feed(&client, nullptr);
    sailfish::ShortsModel model(&feed);
    auto idle = [&] { return catalog_test::waitFor([&] { return model.loaded() && !model.busy(); }, 8000); };
    auto sequences = [&] { return net.count(path); };

    model.refresh();
    CHECK(idle());
    const int page1 = model.count();
    CHECK(page1 >= 20 && page1 <= static_cast<int>(first.items.size()));
    CHECK_EQ(sequences(), 0);                                   // popular first
    model.loadMore();
    CHECK(idle());
    const int page2 = model.count();
    CHECK(page2 > page1);                                       // page 2: 19 new Shorts
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/search")), 2);
    model.loadMore();                                           // page 3 is thin: the popular pages end
    CHECK(idle());
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/search")), 3);
    CHECK_EQ(model.count(), page2);                             // its two Shorts were seen already
    CHECK(model.canLoadMore());                                 // the sequence goes on
    model.loadMore();                                           // seed 0: 11 new Shorts
    CHECK(idle());
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/search")), 3);
    CHECK_EQ(sequences(), 1);
    CHECK_EQ(model.count(), page2 + 11);
    CHECK(net.requests.last().body.contains(QByteArray::fromStdString(seeds[0])));
    CHECK_EQ(model.get(page2).value(QStringLiteral("source")).toString(), QStringLiteral("popular"));
    CHECK(model.get(page2).value(QStringLiteral("thumbnail")).toString().endsWith(QLatin1String("frame0.jpg")));
    const int afterA = model.count();
    model.loadMore();                                           // seed 1: ten of twelve are in A
    CHECK(idle());
    CHECK_EQ(sequences(), 2);
    CHECK_EQ(model.count(), afterA + 2);
    const int afterB = model.count();
    model.loadMore();                                           // seed 2 fails: nothing used up
    CHECK(idle());
    CHECK_EQ(sequences(), 3);
    CHECK_EQ(model.count(), afterB);
    CHECK(model.canLoadMore());
    model.loadMore();                                           // seed 2 again (now answered A), seeds 3, 4: nothing new, budget spent
    CHECK(idle());
    CHECK_EQ(sequences(), 3 + ShortsFeedService::kSeedRequestsPerPage);
    CHECK_EQ(model.count(), afterB);
    CHECK(net.requests.at(net.requests.size() - ShortsFeedService::kSeedRequestsPerPage).body.contains(QByteArray::fromStdString(seeds[2])));
    CHECK(model.canLoadMore());
    // No duplicates anywhere in the endless list.
    QSet<QString> ids;
    for (int i = 0; i < model.count(); ++i) ids.insert(model.get(i).value(QStringLiteral("videoId")).toString());
    CHECK_EQ(ids.size(), model.count());
    // The seeds run out: the list ends and loadMore stops asking.
    int guard = 0;
    while (model.canLoadMore() && guard++ < 60) { model.loadMore(); CHECK(idle()); }
    CHECK(!model.canLoadMore());
    CHECK(static_cast<std::size_t>(sequences()) <= seeds.size() + 1);   // the failed seed was asked twice
    const int requests = net.requests.size();
    model.loadMore();
    CHECK_EQ(net.requests.size(), requests);

    // setCurrent near the end asks for the next page once (the sequence included).
    model.refresh();                                            // page 1 again, from the 10 minute cache
    CHECK(idle());
    const int before = net.requests.size();
    model.setCurrent(model.count() - 2);
    CHECK(catalog_test::waitFor([&] { return !model.busy(); }, 8000));
    CHECK(net.requests.size() > before);

    // The key rows are deduplicated under, and the subscription set.
    Item video, shortRow;
    video.kind = ItemKind::Video; video.id = "abcdefghijk";
    shortRow.kind = ItemKind::Short; shortRow.id = "abcdefghijk";
    CHECK_EQ(rowSeenKey(video), rowSeenKey(shortRow));
    CHECK(!feed.isSubscribed(kNasa));
    int changes = 0;
    QObject::connect(&feed, &ShortsFeedService::subscriptionsChanged, [&changes] { ++changes; });
    ChannelList list;
    list.push_back(ChannelRef(kNasa, "NASA"));
    feed.setChannels(list);
    CHECK(feed.isSubscribed(kNasa) && changes == 1);
    feed.setChannels(list);                                      // the same list: no signal
    CHECK_EQ(changes, 1);
    feed.setChannels(ChannelList());
    CHECK(!feed.isSubscribed(kNasa) && changes == 2);
}

static void detailsOnTheModel() {
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts-model"), 1);
    FakeTransport net;
    servePopular(net);
    const ShortsPage first = ShortsFeedService::parsePopular(okReply(catalog_test::fixture(QStringLiteral("shorts_popular_es.json"))),
                                                             QStringLiteral("es"), false);
    CHECK(first.items.size() >= 8);
    const QString path = QStringLiteral("/youtubei/v1/reel/reel_item_watch");
    auto idOf = [&](std::size_t i) { return QString::fromStdString(first.items.at(i).id); };
    auto only = [&](std::size_t i) { return "\"videoId\":\"" + idOf(i).toLatin1() + "\""; };
    net.on(path, only(0), 200, catalog_test::fixture(QStringLiteral("shorts_item_en.json")), 30);
    net.on(path, only(1), 200, catalog_test::fixture(QStringLiteral("shorts_item_remix_en.json")), 30);
    net.on(path, only(2), 200, catalog_test::fixture(QStringLiteral("shorts_item_nocomments_en.json")));
    net.on(path, only(3), 503, QByteArray(), 0, 1);
    net.on(path, only(4), 200, catalog_test::fixture(QStringLiteral("shorts_item_gone.json")));
    net.on(path, QByteArray(), 200, catalog_test::fixture(QStringLiteral("shorts_item_hiddenlikes_en.json")));
    net.on(QStringLiteral("returnyoutubedislikeapi.com/votes"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("ryd_votes.json")));
    InnerTubeClient client(&net, &identity, &lane, DiscoveryLocale());
    ShortsFeedService feed(&client, nullptr);
    ShortDetailService details(&client);
    DislikeService votes(&net);
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/ryd.conf"), QSettings::IniFormat);
    sailfish::DislikeInfo dislikeInfo(&votes, &settings);
    sailfish::ShortsModel model(&feed);
    model.setDetails(&details);
    model.setDislikes(&votes, &dislikeInfo);
    model.setGraceMs(250);
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy(); }, 8000));
    CHECK(model.count() >= 8);
    auto role = [&](int row, const char *name) { return model.get(row).value(QString::fromLatin1(name)); };
    auto items = [&] { return net.count(QStringLiteral("reel_item_watch")); };

    // revision: get(row) is a snapshot, so bindings over it depend on this property.
    int revisions = 0;
    QObject::connect(&model, &sailfish::ShortsModel::revisionChanged, [&revisions] { ++revisions; });
    const int revision0 = model.revision();
    CHECK(revision0 > 0);                                         // the rows arrived

    // Before anything: the popular row has its title and views, and nothing about the channel.
    CHECK(!role(0, "title").toString().isEmpty());
    CHECK(role(0, "channel").toString().isEmpty() && role(0, "channelId").toString().isEmpty());
    CHECK_EQ(role(0, "likeCount").toDouble(), -1.0);
    CHECK_EQ(role(0, "commentCount").toDouble(), -1.0);
    CHECK(role(0, "likeText").toString().isEmpty() && role(0, "description").toString().isEmpty());
    CHECK(role(0, "relatedVideoId").toString().isEmpty() && role(0, "soundText").toString().isEmpty());
    CHECK(!role(0, "detailsLoaded").toBool() && !role(0, "detailsLoading").toBool() && !role(0, "detailsFailed").toBool());
    CHECK(!role(0, "subscribed").toBool() && !role(0, "commentsDisabled").toBool());
    CHECK_EQ(role(0, "dislikeCount").toDouble(), -1.0);

    // The hold: setCurrent alone asks for nothing while playback has not started...
    int rowChanges = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&rowChanges](const QModelIndex &, const QModelIndex &, const QVector<int> &) { ++rowChanges; });
    model.setCurrent(0);
    CHECK(!model.released());
    catalog_test::waitFor([] { return false; }, 80);
    CHECK_EQ(items(), 0);
    // ...and playing releases it: the Short on screen first, one request, then the next one.
    model.setPlaybackRunning(true);
    CHECK(model.released());
    CHECK(role(0, "detailsLoading").toBool());
    CHECK_EQ(items(), 1);
    CHECK(catalog_test::waitFor([&] { return role(0, "detailsLoaded").toBool(); }));
    CHECK(!role(0, "detailsLoading").toBool());
    CHECK(rowChanges > 0);
    CHECK(role(0, "likeCount").toDouble() > 0 && !role(0, "likeText").toString().isEmpty());
    CHECK(role(0, "commentCount").toDouble() > 0 && !role(0, "commentText").toString().isEmpty());
    CHECK(role(0, "channelHandle").toString().startsWith(QLatin1Char('@')));
    CHECK(role(0, "channelId").toString().size() == 24 && !role(0, "channel").toString().isEmpty());
    CHECK(role(0, "channelAvatar").toString().startsWith(QLatin1String("https://")));
    CHECK(role(0, "relatedVideoId").toString().size() == 11 && !role(0, "relatedVideoTitle").toString().isEmpty());
    CHECK(!role(0, "relatedVideoIsShort").toBool() && !role(0, "soundText").toString().isEmpty());
    CHECK(model.revision() > revision0 && revisions > 0);        // the snapshot of row 0 changed: bindings re-read it
    CHECK_EQ(role(0, "relatedVideoThumbnail").toString(),
             QStringLiteral("https://i.ytimg.com/vi/%1/mqdefault.jpg").arg(role(0, "relatedVideoId").toString()));
    CHECK(!role(0, "published").toString().isEmpty());
    CHECK_EQ(model.get(0).value(QStringLiteral("hints")).toMap().value(QStringLiteral("channelId")).toString(), role(0, "channelId").toString());
    // The next Short (row 1) follows, after the first answer, in the same hold.
    CHECK(catalog_test::waitFor([&] { return role(1, "detailsLoaded").toBool(); }));
    CHECK_EQ(items(), 2);
    CHECK(role(1, "relatedVideoIsShort").toBool() && role(1, "likeCount").toDouble() == -1.0 && role(1, "likeText").toString().isEmpty());
    CHECK_EQ(role(1, "relatedVideoThumbnail").toString(),
             QStringLiteral("https://i.ytimg.com/vi/%1/frame0.jpg").arg(role(1, "relatedVideoId").toString()));   // a Short: its vertical frame
    CHECK(role(2, "relatedVideoThumbnail").toString().isEmpty() && role(2, "relatedVideoId").toString().isEmpty());
    CHECK(!role(1, "description").toString().isEmpty() && role(1, "descriptionShort").toString().size() <= 101);
    // Never the Shorts after that.
    catalog_test::waitFor([] { return false; }, 120);
    CHECK_EQ(items(), 2);
    CHECK(!role(2, "detailsLoaded").toBool() && !role(2, "detailsLoading").toBool());

    // subscribed follows the library's list, by the channel id the details named.
    const QString channel = role(0, "channelId").toString();
    int flips = 0;
    QObject::connect(&model, &QAbstractItemModel::dataChanged, [&flips](const QModelIndex &, const QModelIndex &, const QVector<int> &roles) {
        if (roles.contains(sailfish::ShortsModel::SubscribedRole)) ++flips;
    });
    CHECK(!role(0, "subscribed").toBool());
    ChannelList subscriptions;
    subscriptions.push_back(ChannelRef(channel.toStdString(), "A channel"));
    feed.setChannels(subscriptions);
    CHECK(role(0, "subscribed").toBool() && !role(1, "subscribed").toBool());
    CHECK_EQ(flips, 1);
    const int revisionBefore = model.revision();
    feed.setChannels(ChannelList());                              // back, and a revision per change
    CHECK(model.revision() > revisionBefore);
    feed.setChannels(subscriptions);
    feed.setChannels(ChannelList());
    CHECK(!role(0, "subscribed").toBool());
    CHECK_EQ(flips, 4);                                           // on, off (the revision check), on, off

    // Swipe to row 1: already known, so only row 2 is asked for, after a new hold.
    model.setPlaybackRunning(false);                              // the player switches Shorts
    model.setCurrent(1);
    CHECK(!model.released());
    CHECK_EQ(items(), 2);
    model.setPlaybackRunning(true);
    CHECK(catalog_test::waitFor([&] { return role(2, "detailsLoaded").toBool(); }));
    CHECK_EQ(items(), 3);
    CHECK(role(2, "commentsDisabled").toBool() && role(2, "commentText").toString().isEmpty() && role(2, "commentCount").toDouble() == -1.0);
    // Swiping back asks for nothing: the details of rows 0 and 1 are known.
    model.setPlaybackRunning(false);
    model.setCurrent(0);
    model.setPlaybackRunning(true);
    catalog_test::waitFor([] { return false; }, 100);
    CHECK_EQ(items(), 3);

    // A failed Short: marked, not asked again in the same visit, asked again on the next one.
    model.setPlaybackRunning(false);
    model.setCurrent(3);                                          // row 3 fails once (503), row 4 is a removed Short
    model.setPlaybackRunning(true);
    CHECK(catalog_test::waitFor([&] { return role(3, "detailsFailed").toBool(); }));
    CHECK(catalog_test::waitFor([&] { return role(4, "detailsFailed").toBool(); }));    // Unavailable: failed for good
    catalog_test::waitFor([] { return false; }, 100);
    CHECK_EQ(items(), 5);
    CHECK(!role(3, "detailsLoaded").toBool() && role(3, "likeCount").toDouble() == -1.0);
    model.setPlaybackRunning(false);
    model.setCurrent(5);
    model.setCurrent(3);                                          // a new visit to row 3
    model.setPlaybackRunning(true);
    CHECK(catalog_test::waitFor([&] { return role(3, "detailsLoaded").toBool(); }));
    CHECK(!role(3, "detailsFailed").toBool());

    // Unbound playbackRunning: the grace releases the hold after the grace time.
    CHECK(catalog_test::waitFor([&] { return role(4, "detailsFailed").toBool() && !role(4, "detailsLoading").toBool(); }));
    model.setPlaybackRunning(false);
    const int sentBefore = items();
    model.setCurrent(6);
    CHECK(!model.released());
    catalog_test::waitFor([] { return false; }, 60);
    CHECK_EQ(items(), sentBefore);
    CHECK(catalog_test::waitFor([&] { return model.released(); }, 2000));
    CHECK(catalog_test::waitFor([&] { return role(6, "detailsLoaded").toBool(); }));
    // A new list (the feed starts over) starts the hold over.
    feed.clear();
    CHECK_EQ(model.currentRow(), -1);
    CHECK(!model.released());

    // The dislike estimate. Shipped state first: kShortsDislikeEstimate is false (YouTube stopped
    // reporting dislikes for Shorts), so even with the setting on nothing is asked and no number shows.
    CHECK(!kShortsDislikeEstimate && !shortsDislikeEstimate());
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 0);
    {
        model.refresh();
        CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy() && model.count() > 0; }, 8000));
        dislikeInfo.setEnabled(true);
        model.setCurrent(0);
        model.setPlaybackRunning(false);
        model.setPlaybackRunning(true);
        catalog_test::waitFor([] { return false; }, 100);
        CHECK(catalog_test::waitFor([&] { return model.released(); }, 2000));
        catalog_test::waitFor([] { return false; }, 100);
        CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 0);
        CHECK_EQ(role(0, "dislikeCount").toDouble(), -1.0);
        CHECK(role(0, "dislikeText").toString().isEmpty());
        dislikeInfo.setEnabled(false);
        feed.clear();                                             // the hold starts over for what follows
        CHECK(!model.released());
    }
    // Switched on (what flipping the constant does): off by default as a setting, nothing asked. On: the Short on screen only.
    setShortsDislikeEstimate(true);
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 0);
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy() && model.count() > 0; }, 8000));
    model.setPlaybackRunning(true);
    model.setCurrent(0);                                          // the details of rows 0 and 1 are known already
    CHECK(role(0, "detailsLoaded").toBool());
    CHECK(!model.released());                                     // nothing to ask for, so no hold to wait out
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 0);
    CHECK_EQ(role(0, "dislikeCount").toDouble(), -1.0);
    dislikeInfo.setEnabled(true);                                 // the estimate waits for the same hold
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 0);
    CHECK(catalog_test::waitFor([&] { return model.released(); }, 2000));
    CHECK(catalog_test::waitFor([&] { return role(0, "dislikeCount").toDouble() > 0; }));
    CHECK_EQ(role(0, "dislikeCount").toDouble(), 1533.0);
    CHECK_EQ(role(0, "dislikeText").toString(), QStringLiteral("1.5K"));
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), 1);                // the Short on screen only
    CHECK_EQ(role(1, "dislikeCount").toDouble(), -1.0);
    CHECK(net.requests.last().url.toString().contains(idOf(0)));
    dislikeInfo.setEnabled(false);                                // the number goes with the setting
    CHECK_EQ(role(0, "dislikeCount").toDouble(), -1.0);
    CHECK(role(0, "dislikeText").toString().isEmpty());
    const int voteRequests = net.count(QStringLiteral("returnyoutubedislikeapi.com"));
    model.setCurrent(1);
    catalog_test::waitFor([] { return false; }, 100);
    CHECK_EQ(net.count(QStringLiteral("returnyoutubedislikeapi.com")), voteRequests);

    setShortsDislikeEstimate(false);                              // back to the shipped state
    // Without a details service nothing is asked and the roles stay unknown.
    ShortsFeedService bareFeed(&client, nullptr);
    sailfish::ShortsModel bare(&bareFeed);
    bare.refresh();
    CHECK(catalog_test::waitFor([&] { return bare.loaded() && !bare.busy() && bare.count() > 0; }, 8000));
    const int asked = items();
    bare.setPlaybackRunning(true);
    bare.setCurrent(0);
    catalog_test::waitFor([] { return false; }, 80);
    CHECK_EQ(items(), asked);
    CHECK_EQ(bare.get(0).value(QStringLiteral("likeCount")).toDouble(), -1.0);
    CHECK_EQ(bare.detailRequests(), 0);
}

static void accountRowsFillFromDetails() {
    // Signed in the rows are ids and thumbnails only; the title and the rest come with the details.
    ClientIdentity identity(nullptr, QString());
    WorkLane lane(QStringLiteral("shorts-account"), 1);
    FakeTransport anonymousNet, signedNet;
    servePopular(anonymousNet);
    servePopular(signedNet);
    signedNet.on(QStringLiteral("/youtubei/v1/reel/reel_watch_sequence"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("shorts_sequence_es.json")));
    anonymousNet.on(QStringLiteral("/youtubei/v1/reel/reel_item_watch"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("shorts_item_en.json")));
    InnerTubeClient anonymousClient(&anonymousNet, &identity, &lane, DiscoveryLocale());
    InnerTubeClient signedClient(&signedNet, &identity, &lane, DiscoveryLocale());
    ShortsFeedService feed(&anonymousClient, nullptr);
    ShortDetailService details(&anonymousClient);
    sailfish::ShortsModel model(&feed);
    model.setDetails(&details);
    model.setGraceMs(40);
    feed.setSignedClient(&signedClient);
    model.refresh();
    CHECK(catalog_test::waitFor([&] { return model.loaded() && !model.busy() && model.count() > 0; }, 8000));
    CHECK_EQ(model.get(0).value(QStringLiteral("source")).toString(), QStringLiteral("account"));
    CHECK(model.get(0).value(QStringLiteral("title")).toString().isEmpty());
    CHECK(model.get(0).value(QStringLiteral("channelId")).toString().isEmpty());
    model.setCurrent(0);
    CHECK(catalog_test::waitFor([&] { return model.get(0).value(QStringLiteral("detailsLoaded")).toBool(); }, 3000));
    CHECK(!model.get(0).value(QStringLiteral("title")).toString().isEmpty());
    CHECK_EQ(model.get(0).value(QStringLiteral("hints")).toMap().value(QStringLiteral("title")).toString(),
             model.get(0).value(QStringLiteral("title")).toString());
    CHECK_EQ(model.get(0).value(QStringLiteral("channelId")).toString().size(), 24);
    CHECK(!model.get(0).value(QStringLiteral("channel")).toString().isEmpty());
    // The details were asked of the anonymous client: public data, never the account's transport.
    CHECK_EQ(signedNet.count(QStringLiteral("reel_item_watch")), 0);
    CHECK(anonymousNet.count(QStringLiteral("reel_item_watch")) >= 1);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    parsers();
    anonymousFeed();
    signedFeed();
    detailParsing();
    detailService();
    seedsAndEndlessPaging();
    detailsOnTheModel();
    accountRowsFillFromDetails();
    return catalog_test::finish("catalog_shorts");
}
