// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DL-07 sidecar source over the catalog's fake transport and recorded answers. The fake answers
// each client the way the live service does:
//  * the catalog's anonymous WEB client asking `player`: player_web (UNPLAYABLE, no caption tracks);
//  * the pinned shared client (the track-list request, with a visitor id): player_visionos
//    (manual hr/en/it/ja and auto-generated en), player_live (no captions) or player_refused;
//  * `visitor_id`: visitor_id; `api/timedtext`: a json3 cue document, or HTTP 429 for a translation.
// CatalogSidecarSource asks through CaptionTrackService, picks the SEL-06 track for the caption
// preference and the device language among the video's own tracks, saves the json3 bodies, and maps
// the recorded SponsorBlock prefix answer; failures say "retry later".
#include "../catalog_common/TestSupport.h"

#include "Clients/Sailfish/Composition/LibraryCatalogGlue.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Core/Catalog/Infrastructure/CaptionTrackService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/SponsorBlockService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <cstdio>

using namespace nordtile::video;
using catalog_test::FakeTransport;
using catalog_test::fixture;
using catalog_test::fixtureJson;
using catalog_test::waitFor;

static const QString kPlayerPath = QStringLiteral("/youtubei/v1/player");
static const QString kCuePath = QStringLiteral("api/timedtext");

// What tells the track-list request from a WEB player request: the pinned shared client's name.
static QByteArray listMarker() {
    return QByteArray("\"clientName\":\"") + QByteArray::fromStdString(nordtile::music::sharedplayback::domain::visionosClient().name)
        + QByteArray("\"");
}

// The live service's answers, per client (see the file comment). `listFixture` answers the next
// track-list request(s); everything else is fixed.
static void serveLikeTheService(FakeTransport &net) {
    net.on(QStringLiteral("/youtubei/v1/visitor_id"), QByteArray(), 200, fixture(QStringLiteral("visitor_id.json")));
    net.on(kCuePath + QStringLiteral("?"), QByteArray(), 200, fixture(QStringLiteral("timedtext_manual_json3.json")));
    net.on(kPlayerPath, QByteArray(), 200, fixture(QStringLiteral("player_web.json")));   // any other player request: the WEB answer
    net.on(QStringLiteral("/api/skipSegments/5752"), QByteArray(), 200, fixture(QStringLiteral("sponsorblock_prefix.json")));
    net.on(QStringLiteral("/api/skipSegments/"), QByteArray(), 404, QByteArray());
}
static void prepend(FakeTransport &net, const QString &url, const QByteArray &body, int status, const QByteArray &answer, int times = -1) {
    FakeTransport::Rule r;
    r.urlContains = url; r.bodyContains = body; r.status = status; r.body = answer; r.times = times;
    net.rules.prepend(r);
}
static void serveList(FakeTransport &net, const QString &fixtureName, int times = -1) {
    prepend(net, kPlayerPath, listMarker(), 200, fixture(fixtureName), times);
}
static int listRequests(const FakeTransport &net) { return net.count(kPlayerPath, listMarker()); }
static int webPlayerRequests(const FakeTransport &net) { return net.count(kPlayerPath) - listRequests(net); }
static int translationRequests(const FakeTransport &net) {
    int n = 0;
    for (const catalog::HttpRequest &r : net.requests) if (r.url.toString().contains(QStringLiteral("tlang="))) ++n;
    return n;
}

struct Captions {
    bool called = false;
    bool ok = false;
    QList<downloads::CaptionTrackData> tracks;
};
static Captions fetch(sailfish::CatalogSidecarSource &source, const QString &videoId) {
    Captions c;
    source.fetchCaptions(videoId, [&c](bool o, const QList<downloads::CaptionTrackData> &t) { c.called = true; c.ok = o; c.tracks = t; });
    CHECK(waitFor([&] { return c.called; }, 10000));
    return c;
}

static void sidecars(const QString &dir) {
    CHECK_EQ(sailfish::CatalogSidecarSource::wantedCodes(QString(), QStringLiteral("en_US")).join(QLatin1Char(',')), QStringLiteral("en"));
    CHECK_EQ(sailfish::CatalogSidecarSource::wantedCodes(QStringLiteral("it"), QStringLiteral("en_US")).join(QLatin1Char(',')), QStringLiteral("it,en"));
    CHECK_EQ(sailfish::CatalogSidecarSource::wantedCodes(QStringLiteral("off"), QStringLiteral("de_DE")).join(QLatin1Char(',')), QStringLiteral("de"));
    CHECK_EQ(sailfish::CatalogSidecarSource::wantedCodes(QStringLiteral("pt-BR"), QStringLiteral("pt_PT")).join(QLatin1Char(',')), QStringLiteral("pt-BR"));
    CHECK(sailfish::CatalogSidecarSource::wantedCodes(QString(), QStringLiteral("C")).isEmpty());

    // Why the track list cannot come from the WEB player answer: the anonymous one lists no track
    // (it does not even call the video playable); the pinned client's answer lists five.
    CHECK(catalog::CaptionService::tracksFromPlayer(fixtureJson(QStringLiteral("player_web.json"))).empty());
    CHECK_EQ(static_cast<int>(catalog::CaptionService::tracksFromPlayer(fixtureJson(QStringLiteral("player_visionos.json"))).real.size()), 5);

    const QString video = QStringLiteral("9zCH37330f8");
    catalog::ClientIdentity identity(nullptr, QString());
    catalog::WorkLane lane(QStringLiteral("glue-sidecars"), 1);
    catalog::DiscoveryLocale locale;
    FakeTransport net;
    serveLikeTheService(net);
    serveList(net, QStringLiteral("player_visionos.json"));
    catalog::InnerTubeClient client(&net, &identity, &lane, locale);
    catalog::SponsorBlockService sponsor(&net, &lane);
    QSettings settings(dir + QStringLiteral("/captions.ini"), QSettings::IniFormat);
    sailfish::CaptionTracksModel tracks(&settings);
    sailfish::CatalogSidecarSource source(&client, &net, &sponsor, &tracks);
    source.setDeviceLocaleForTests(QStringLiteral("en_US"));

    // captions: preference never chosen -> the device language only
    Captions got = fetch(source, video);
    CHECK(got.ok);
    CHECK_EQ(got.tracks.size(), 1);
    if (!got.tracks.isEmpty()) {
        CHECK_EQ(got.tracks.first().languageCode, QStringLiteral("en"));
        CHECK(!got.tracks.first().autoGenerated); // the manual track beats ASR (SEL-06)
        CHECK(got.tracks.first().json3 == fixture(QStringLiteral("timedtext_manual_json3.json")));
    }
    CHECK_EQ(listRequests(net), 1);          // asked with the pinned client ...
    CHECK_EQ(webPlayerRequests(net), 0);     // ... never the WEB player, whose answer has no tracks
    CHECK_EQ(net.count(QStringLiteral("/youtubei/v1/visitor_id")), 1);
    bool visitorSent = false;
    for (const catalog::HttpRequest &r : net.requests) {
        if (!r.url.toString().contains(kPlayerPath)) continue;
        for (const QPair<QByteArray, QByteArray> &h : r.headers) visitorSent = visitorSent || (h.first.toLower() == "x-goog-visitor-id" && !h.second.isEmpty());
    }
    CHECK(visitorSent);
    CHECK_EQ(net.count(kCuePath), 1);
    bool json3 = false;
    for (const catalog::HttpRequest &r : net.requests) json3 = json3 || r.url.toString().contains(QStringLiteral("fmt=json3"));
    CHECK(json3);

    // an explicit Italian pick keeps Italian and the device language (both are the video's own tracks)
    settings.setValue(QString::fromLatin1(sailfish::CaptionTracksModel::kSettingsKey), QStringLiteral("it"));
    sailfish::CaptionTracksModel italian(&settings);
    sailfish::CatalogSidecarSource source2(&client, &net, &sponsor, &italian);
    source2.setDeviceLocaleForTests(QStringLiteral("en_US"));
    got = fetch(source2, video);
    CHECK(got.ok && got.tracks.size() == 2);
    if (got.tracks.size() == 2) {
        CHECK_EQ(got.tracks[0].languageCode, QStringLiteral("it"));
        CHECK_EQ(got.tracks[1].languageCode, QStringLiteral("en"));
    }

    // A language the video has only as an automatic translation (French: the answer offers 156
    // translation languages) is not saved: anonymous requests for a translation get HTTP 429.
    // The device language is still kept, and no translation is ever asked for.
    prepend(net, QStringLiteral("tlang="), QByteArray(), 429, QByteArray());
    settings.setValue(QString::fromLatin1(sailfish::CaptionTracksModel::kSettingsKey), QStringLiteral("fr"));
    sailfish::CaptionTracksModel french(&settings);
    sailfish::CatalogSidecarSource source3(&client, &net, &sponsor, &french);
    source3.setDeviceLocaleForTests(QStringLiteral("en_US"));
    got = fetch(source3, video);
    CHECK(got.ok);
    CHECK_EQ(got.tracks.size(), 1);
    if (!got.tracks.isEmpty()) CHECK_EQ(got.tracks.first().languageCode, QStringLiteral("en"));
    // Only a translation would match (French preference, French device): nothing kept, final.
    source3.setDeviceLocaleForTests(QStringLiteral("fr_FR"));
    got = fetch(source3, video);
    CHECK(got.ok && got.tracks.isEmpty());
    CHECK_EQ(translationRequests(net), 0);

    // An auto-generated track is one of the video's own: kept when it is the only one in the language.
    // (German is offered as a translation only; with the device in English the ASR stays behind the manual track.)
    {
        FakeTransport asrNet;
        serveLikeTheService(asrNet);
        QJsonObject answer = fixtureJson(QStringLiteral("player_visionos.json"));
        QJsonObject captions = answer.value(QStringLiteral("captions")).toObject();
        QJsonObject renderer = captions.value(QStringLiteral("playerCaptionsTracklistRenderer")).toObject();
        QJsonArray own;
        for (const QJsonValue &t : renderer.value(QStringLiteral("captionTracks")).toArray())
            if (t.toObject().value(QStringLiteral("kind")).toString() == QLatin1String("asr")) own.append(t);
        CHECK_EQ(own.size(), 1);
        renderer.insert(QStringLiteral("captionTracks"), own);
        captions.insert(QStringLiteral("playerCaptionsTracklistRenderer"), renderer);
        answer.insert(QStringLiteral("captions"), captions);
        prepend(asrNet, kPlayerPath, listMarker(), 200, QJsonDocument(answer).toJson(QJsonDocument::Compact));
        catalog::InnerTubeClient asrClient(&asrNet, &identity, &lane, locale);
        sailfish::CatalogSidecarSource asrSource(&asrClient, &asrNet, nullptr, &tracks);
        asrSource.setDeviceLocaleForTests(QStringLiteral("en_US"));
        got = fetch(asrSource, video);
        CHECK(got.ok);
        CHECK_EQ(got.tracks.size(), 1);
        if (!got.tracks.isEmpty()) CHECK(got.tracks.first().autoGenerated && got.tracks.first().languageCode == QLatin1String("en"));
    }

    // A playable video without captions (a live stream's answer): final "none", no cue request.
    {
        FakeTransport liveNet;
        serveLikeTheService(liveNet);
        serveList(liveNet, QStringLiteral("player_live.json"));
        catalog::InnerTubeClient liveClient(&liveNet, &identity, &lane, locale);
        sailfish::CatalogSidecarSource liveSource(&liveClient, &liveNet, nullptr, &tracks);
        liveSource.setDeviceLocaleForTests(QStringLiteral("en_US"));
        got = fetch(liveSource, QStringLiteral("ROboUHfPUrU"));
        CHECK(got.ok && got.tracks.isEmpty());
        CHECK_EQ(liveNet.count(kCuePath), 0);
    }

    // The provider refuses the visitor id (asked once more with a new one), calls the video
    // unplayable (what it answers a client it does not serve), or the request fails: "retry later".
    for (int kind = 0; kind < 3; ++kind) {
        FakeTransport badNet;
        serveLikeTheService(badNet);
        if (kind == 0) serveList(badNet, QStringLiteral("player_refused.json"));
        else if (kind == 1) serveList(badNet, QStringLiteral("player_web.json"));
        else prepend(badNet, kPlayerPath, listMarker(), 500, QByteArray());
        catalog::InnerTubeClient badClient(&badNet, &identity, &lane, locale);
        sailfish::CatalogSidecarSource badSource(&badClient, &badNet, nullptr, &tracks);
        badSource.setDeviceLocaleForTests(QStringLiteral("en_US"));
        got = fetch(badSource, video);
        CHECK(!got.ok && got.tracks.isEmpty());
        CHECK_EQ(listRequests(badNet), kind == 0 ? 2 : 1);
        CHECK_EQ(badNet.count(kCuePath), 0);
        // Nothing was kept from the failure: the next attempt asks again and succeeds.
        serveList(badNet, QStringLiteral("player_visionos.json"));
        got = fetch(badSource, video);
        CHECK(got.ok && got.tracks.size() == 1);
    }

    // a failing cue GET means "retry later" (HTTP 500, and the 429 the service uses for refusals)
    for (int status : { 500, 429 }) {
        prepend(net, kCuePath, QByteArray(), status, QByteArray(), 1);
        got = fetch(source, video);
        CHECK(!got.ok && got.tracks.isEmpty());
    }
    got = fetch(source, video);
    CHECK(got.ok && got.tracks.size() == 1);
    // not a video id: nothing to keep, nothing asked
    const int before = net.requests.size();
    got = fetch(source, QStringLiteral("not an id"));
    CHECK(got.ok && got.tracks.isEmpty());
    CHECK_EQ(net.requests.size(), before);

    // segments: the recorded prefix answer, filtered to the id; 404 is a final "none"
    bool segCalled = false, segOk = false;
    QList<downloads::SkipSegmentData> segs;
    source.fetchSegments(QStringLiteral("9zCH37330f8"), [&](bool o, const QList<downloads::SkipSegmentData> &s) { segCalled = true; segOk = o; segs = s; });
    CHECK(waitFor([&] { return segCalled; }, 10000));
    CHECK(segOk && !segs.isEmpty());
    for (const downloads::SkipSegmentData &s : segs) {
        CHECK(!s.category.isEmpty());
        CHECK(s.category == QLatin1String("poi_highlight") ? s.endSeconds >= s.startSeconds : s.endSeconds > s.startSeconds);
    }
    bool requestHasId = false;
    for (const catalog::HttpRequest &r : net.requests)
        requestHasId = requestHasId || (r.url.host() == QLatin1String("sponsor.ajay.app") && r.url.toString().contains(QStringLiteral("9zCH37330f8")));
    CHECK(!requestHasId); // SB-01: the id never leaves the device
    segCalled = false;
    source.fetchSegments(QStringLiteral("AAAAAAAAAAA"), [&](bool o, const QList<downloads::SkipSegmentData> &s) { segCalled = true; segOk = o; segs = s; });
    CHECK(waitFor([&] { return segCalled; }, 10000));
    CHECK(segOk && segs.isEmpty());

    // no services: final "none"
    sailfish::CatalogSidecarSource empty(nullptr, nullptr, nullptr, nullptr);
    bool called = false, ok = false;
    segCalled = false;
    empty.fetchCaptions(QStringLiteral("9zCH37330f8"), [&](bool o, const QList<downloads::CaptionTrackData> &) { called = true; ok = o; });
    empty.fetchSegments(QStringLiteral("9zCH37330f8"), [&](bool o, const QList<downloads::SkipSegmentData> &) { segCalled = true; segOk = o; });
    CHECK(called && ok && segCalled && segOk);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QDir::tempPath() + QStringLiteral("/sailtube-glue-%1").arg(QCoreApplication::applicationPid());
    QDir(dir).removeRecursively();
    QDir().mkpath(dir);
    sidecars(dir);
    QDir(dir).removeRecursively();
    return catalog_test::finish("library catalog glue tests");
}
