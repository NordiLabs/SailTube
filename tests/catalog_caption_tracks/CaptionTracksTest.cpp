// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The caption track list of a streamed video (SEL-06), offline against recorded answers:
// player_visionos (manual and automatic tracks), player_live (a video without captions),
// player_refused (the answer to a request without a visitor id), visitor_id, and the cue
// documents timedtext_manual_json3 / timedtext_asr_json3. HTTP failures are served by the fake
// transport.

#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/Captions.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Core/Catalog/Infrastructure/CaptionTrackService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Clients/Sailfish/Presentation/CaptionCues.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Clients/Sailfish/Presentation/PlaybackCaptions.h"
#include "Clients/Sailfish/Presentation/WatchCaptions.h"
#include "Clients/Sailfish/Presentation/WatchInfo.h"
#include "Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QTemporaryDir>
#include <QVariantMap>
#include <set>

using namespace nordtile::video::catalog;
using namespace nordtile::video::sailfish;
using catalog_test::FakeTransport;
using catalog_test::fixture;
using catalog_test::fixtureJson;
using catalog_test::waitFor;

namespace {

namespace shared = nordtile::music::sharedplayback::domain;

const QString kVideo = QStringLiteral("9zCH37330f8");
const QString kOther = QStringLiteral("ROboUHfPUrU");
const QString kPlayerPath = QStringLiteral("/youtubei/v1/player");
const QString kVisitorPath = QStringLiteral("/youtubei/v1/visitor_id");
const QString kCuePath = QStringLiteral("api/timedtext");

std::string oc(Outcome o) { return outcomeName(o); }
int sz(std::size_t n) { return static_cast<int>(n); }

// What tells the track-list request from the watch data's WEB player request: the pinned
// shared client's name in the body.
QByteArray listMarker() {
    return QByteArray("\"clientName\":\"") + QByteArray::fromStdString(shared::visionosClient().name) + QByteArray("\"");
}

QByteArray header(const HttpRequest &r, const char *name) {
    for (const QPair<QByteArray, QByteArray> &h : r.headers)
        if (h.first.toLower() == QByteArray(name).toLower()) return h.second;
    return QByteArray();
}

bool hasHeader(const HttpRequest &r, const char *name) {
    for (const QPair<QByteArray, QByteArray> &h : r.headers)
        if (h.first.toLower() == QByteArray(name).toLower()) return true;
    return false;
}

QString fixtureVisitor(const QString &name) { return InnerTubeClient::visitorOf(fixtureJson(name)); }

// The track-list requests the transport saw, in order.
QList<HttpRequest> listRequests(const FakeTransport &net) {
    QList<HttpRequest> out;
    for (const HttpRequest &r : net.requests)
        if (r.url.toString().contains(kPlayerPath) && r.body.contains(listMarker())) out.append(r);
    return out;
}

struct Rig {
    FakeTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    CatalogCounters counters;
    qint64 now = 1000;
    CaptionTrackService tracks;
    explicit Rig(const QString &hl = QStringLiteral("en"))
        : identity(nullptr, QString()), lane(QStringLiteral("caption-tracks-test"), 1),
          client(&net, &identity, &lane, locale(hl)), tracks(&client, nullptr, [this] { return static_cast<std::int64_t>(now); }) {
        tracks.setCounters(&counters);
    }
    static DiscoveryLocale locale(const QString &hl) { DiscoveryLocale l; l.hl = hl; return l; }
    void serveVisitor() { net.on(kVisitorPath, QByteArray(), 200, fixture(QStringLiteral("visitor_id.json"))); }
    void serveList(const QString &fixtureName, int delayMs = 0, int times = -1) {
        net.on(kPlayerPath, listMarker(), 200, fixture(fixtureName), delayMs, times);
    }
    // Fetches and waits for the answer.
    CaptionTrackList fetch(const QString &videoId, bool *answered = nullptr) {
        CaptionTrackList got;
        bool done = false;
        QObject context;
        const bool accepted = tracks.fetch(videoId, &context, [&](const CaptionTrackList &l) { got = l; done = true; });
        CHECK(accepted);
        const bool ok = waitFor([&] { return done; });
        if (answered) *answered = ok;
        else CHECK(ok);
        return got;
    }
};

}  // namespace

// --- the request ---------------------------------------------------------------------------------

static void requestShape() {
    Rig rig(QStringLiteral("es"));
    const shared::ClientDefinition def = shared::visionosClient();
    const HttpRequest r = rig.client.buildTrackListRequest(kVideo, QStringLiteral("VISITOR-1"), 7000);
    CHECK_EQ(r.url.toString(), QStringLiteral("https://www.youtube.com/youtubei/v1/player?prettyPrint=false"));
    CHECK_EQ(r.method, QByteArray("POST"));
    CHECK_EQ(r.timeoutMs, 7000);
    // The pinned shared definition, nothing of the catalog's own WEB identity.
    CHECK_EQ(header(r, "X-YouTube-Client-Name"), QByteArray::number(def.id));
    CHECK_EQ(header(r, "X-YouTube-Client-Version"), QByteArray::fromStdString(def.version));
    CHECK_EQ(header(r, "User-Agent"), QByteArray::fromStdString(def.userAgent));
    CHECK_EQ(header(r, "X-Goog-Visitor-Id"), QByteArray("VISITOR-1"));
    CHECK_EQ(header(r, "Content-Type"), QByteArray("application/json"));
    CHECK(!hasHeader(r, "Accept-Encoding"));   // the transport negotiates compression
    CHECK(!hasHeader(r, "Cookie"));
    CHECK(!hasHeader(r, "Authorization"));

    const QJsonObject body = QJsonDocument::fromJson(r.body).object();
    const QJsonObject client = body.value(QStringLiteral("context")).toObject().value(QStringLiteral("client")).toObject();
    CHECK_EQ(body.value(QStringLiteral("videoId")).toString(), kVideo);
    CHECK(body.value(QStringLiteral("contentCheckOk")).toBool());
    CHECK(body.value(QStringLiteral("racyCheckOk")).toBool());
    CHECK_EQ(client.value(QStringLiteral("clientName")).toString().toStdString(), def.name);
    CHECK_EQ(client.value(QStringLiteral("clientVersion")).toString().toStdString(), def.version);
    CHECK_EQ(client.value(QStringLiteral("deviceModel")).toString().toStdString(), def.deviceModel);
    CHECK_EQ(client.value(QStringLiteral("osVersion")).toString().toStdString(), def.osVersion);
    CHECK_EQ(client.value(QStringLiteral("hl")).toString(), QStringLiteral("es"));   // localised track names
    CHECK(!client.contains(QStringLiteral("gl")));
    CHECK(!r.body.contains("VISITOR-1"));     // the visitor id travels in the header only
    CHECK(!r.body.contains("visitorData"));
    // Apart from the language it is the pinned request body, member for member.
    QJsonObject pinned = QJsonDocument::fromJson(QByteArray::fromStdString(shared::playerRequestBody(def, kVideo.toStdString()))).object();
    QJsonObject pinnedClient = pinned.value(QStringLiteral("context")).toObject().value(QStringLiteral("client")).toObject();
    pinnedClient.insert(QStringLiteral("hl"), QStringLiteral("es"));
    QJsonObject pinnedContext;
    pinnedContext.insert(QStringLiteral("client"), pinnedClient);
    pinned.insert(QStringLiteral("context"), pinnedContext);
    CHECK(body == pinned);

    // No visitor id, or one that is not plain header text: the header is left out.
    CHECK(!hasHeader(rig.client.buildTrackListRequest(kVideo, QString(), 7000), "X-Goog-Visitor-Id"));
    CHECK(!hasHeader(rig.client.buildTrackListRequest(kVideo, QStringLiteral("a b\r\nX-Evil: 1"), 7000), "X-Goog-Visitor-Id"));
    // A client without a language asks in English.
    Rig bare((QString()));
    const QJsonObject bareBody = QJsonDocument::fromJson(bare.client.buildTrackListRequest(kVideo, QString(), 7000).body).object();
    CHECK_EQ(bareBody.value(QStringLiteral("context")).toObject().value(QStringLiteral("client")).toObject().value(QStringLiteral("hl")).toString(),
             QStringLiteral("en"));
}

static void visitorMemory() {
    // visitorOf: the recorded answers name one; nothing else does.
    const QString recorded = fixtureVisitor(QStringLiteral("visitor_id.json"));
    CHECK(!recorded.isEmpty());
    CHECK(InnerTubeClient::visitorOf(QJsonObject()).isEmpty());
    QJsonObject odd, ctx;
    ctx.insert(QStringLiteral("visitorData"), QStringLiteral("line\nbreak"));
    odd.insert(QStringLiteral("responseContext"), ctx);
    CHECK(InnerTubeClient::visitorOf(odd).isEmpty());

    Rig rig;
    CHECK(rig.client.visitorData().isEmpty());

    // ensureVisitor with nothing in memory asks once, in the catalog's own WEB request shape.
    rig.serveVisitor();
    bool ran = false;
    QObject context;
    rig.client.ensureVisitor(&context, [&] { ran = true; });
    CHECK(!ran);
    CHECK(waitFor([&] { return ran; }));
    CHECK_EQ(rig.client.visitorData(), recorded);
    CHECK_EQ(rig.net.count(kVisitorPath), 1);
    const HttpRequest ask = rig.net.requests.first();
    CHECK_EQ(header(ask, "X-YouTube-Client-Name"), QByteArray("1"));
    CHECK(!hasHeader(ask, "X-Goog-Visitor-Id"));
    CHECK(!hasHeader(ask, "Cookie"));

    // With one in memory nothing is sent and `then` runs at once.
    ran = false;
    rig.client.ensureVisitor(&context, [&] { ran = true; });
    CHECK(ran);
    CHECK_EQ(rig.net.count(kVisitorPath), 1);

    // forgetVisitor drops it; any later answer of this client names a new one.
    rig.client.forgetVisitor();
    CHECK(rig.client.visitorData().isEmpty());
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, fixture(QStringLiteral("next_en.json")));
    bool answered = false;
    std::function<bool(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return r.outcome == Outcome::Ok; };
    std::function<void(bool)> done = [&](bool) { answered = true; };
    rig.client.post<bool>(QStringLiteral("next"), QJsonObject(), &context, parse, done);
    CHECK(waitFor([&] { return answered; }));
    const QString fromAnswer = fixtureVisitor(QStringLiteral("next_en.json"));
    CHECK(!fromAnswer.isEmpty());
    CHECK_EQ(rig.client.visitorData(), fromAnswer);
    // The discovery request itself never carries it.
    CHECK(!hasHeader(rig.net.requests.last(), "X-Goog-Visitor-Id"));
    CHECK(!rig.net.requests.last().body.contains(fromAnswer.toUtf8()));

    // postTrackList sends the track-list request with the id in memory at that moment.
    rig.serveList(QStringLiteral("player_visionos.json"));
    bool listed = false;
    std::function<void(bool)> listDone = [&](bool ok) { listed = ok; };
    rig.client.postTrackList<bool>(kVideo, &context, parse, listDone);
    CHECK(waitFor([&] { return listed; }));
    CHECK_EQ(listRequests(rig.net).size(), 1);
    CHECK_EQ(QString::fromLatin1(header(listRequests(rig.net).first(), "X-Goog-Visitor-Id")), fromAnswer);
    CHECK_EQ(header(listRequests(rig.net).first(), "X-YouTube-Client-Name"), QByteArray::number(shared::visionosClient().id));

    // A failed visitor request still runs `then`; the memory stays empty.
    Rig offline;
    bool ranOffline = false;
    offline.client.ensureVisitor(&context, [&] { ranOffline = true; });
    CHECK(waitFor([&] { return ranOffline; }));
    CHECK(offline.client.visitorData().isEmpty());
}

// --- the answer ----------------------------------------------------------------------------------

static void parseAnswers() {
    // Manual and automatic tracks, in the order the answer lists them, then translations.
    const CaptionTrackList full = CaptionTrackService::parse(fixtureJson(QStringLiteral("player_visionos.json")), kVideo);
    CHECK_EQ(oc(full.outcome), oc(Outcome::Ok));
    CHECK_EQ(full.videoId, kVideo);
    CHECK_EQ(full.playability, std::string("OK"));
    CHECK_EQ(sz(full.tracks.real.size()), 5);
    std::vector<std::string> keys;
    int automatic = 0;
    for (const CaptionTrack &t : full.tracks.real) {
        keys.push_back(t.key);
        if (t.autoGenerated) ++automatic;
        CHECK(!t.translation);
        CHECK(t.url.find("fmt=json3") != std::string::npos);
    }
    const std::vector<std::string> expected = {"hr", "en", "asr:en", "it", "ja"};
    CHECK(keys == expected);
    CHECK_EQ(automatic, 1);
    CHECK(full.tracks.find("asr:en") && full.tracks.find("asr:en")->autoGenerated);
    CHECK(sz(full.tracks.translations.size()) > 140);
    std::set<std::string> realCodes;
    for (const CaptionTrack &t : full.tracks.real) realCodes.insert(t.code);
    int clash = 0, notTranslation = 0;
    for (const CaptionTrack &t : full.tracks.translations) {
        if (realCodes.count(t.code)) ++clash;
        if (!t.translation || t.url.find("tlang=") == std::string::npos) ++notTranslation;
    }
    CHECK_EQ(clash, 0);
    CHECK_EQ(notTranslation, 0);

    // A playable video without captions: valid, empty, not a failure.
    const CaptionTrackList none = CaptionTrackService::parse(fixtureJson(QStringLiteral("player_live.json")), kOther);
    CHECK_EQ(oc(none.outcome), oc(Outcome::ValidEmpty));
    CHECK(none.tracks.empty());
    CHECK(!isFailure(none.outcome));

    // Refused (no visitor id) and unplayable answers list no tracks either, and are failures.
    const CaptionTrackList refused = CaptionTrackService::parse(fixtureJson(QStringLiteral("player_refused.json")), kVideo);
    CHECK_EQ(oc(refused.outcome), oc(Outcome::Unavailable));
    CHECK_EQ(refused.playability, std::string("LOGIN_REQUIRED"));
    CHECK(isFailure(refused.outcome));
    const CaptionTrackList web = CaptionTrackService::parse(fixtureJson(QStringLiteral("player_web.json")), kVideo);
    CHECK_EQ(oc(web.outcome), oc(Outcome::Unavailable));   // the anonymous WEB answer: why this request exists
    CHECK(web.tracks.empty());
    CHECK_EQ(oc(CaptionTrackService::parse(QJsonObject(), kVideo).outcome), oc(Outcome::ShapeDrift));
}

// --- the service ---------------------------------------------------------------------------------

static void fetchManualAndAutomatic() {
    Rig rig;
    rig.serveVisitor();
    rig.serveList(QStringLiteral("player_visionos.json"), 30);
    CHECK(!rig.tracks.isCached(kVideo));

    // Two callers before the answer: one request, both answered.
    QObject context;
    int answers = 0;
    CaptionTrackList first, second;
    CHECK(rig.tracks.fetch(kVideo, &context, [&](const CaptionTrackList &l) { first = l; ++answers; }));
    CHECK(rig.tracks.fetch(kVideo, &context, [&](const CaptionTrackList &l) { second = l; ++answers; }));
    CHECK_EQ(answers, 0);   // never synchronous
    CHECK(waitFor([&] { return answers == 2; }));
    CHECK_EQ(oc(first.outcome), oc(Outcome::Ok));
    CHECK_EQ(sz(first.tracks.real.size()), 5);
    CHECK_EQ(sz(second.tracks.real.size()), 5);
    CHECK(!first.fromCache);
    CHECK_EQ(rig.net.count(kVisitorPath), 1);
    const QList<HttpRequest> sent = listRequests(rig.net);
    CHECK_EQ(sent.size(), 1);
    CHECK_EQ(rig.tracks.requestsSent(), 1);
    CHECK_EQ(QString::fromLatin1(header(sent.first(), "X-Goog-Visitor-Id")), fixtureVisitor(QStringLiteral("visitor_id.json")));
    CHECK(sent.first().body.contains(kVideo.toLatin1()));
    CHECK_EQ(static_cast<int>(rig.counters.value(Area::Captions, Outcome::Ok)), 1);
    // Only the list was asked for: no cue document.
    CHECK_EQ(rig.net.count(kCuePath), 0);

    // Kept for 30 minutes, then asked again.
    CHECK(rig.tracks.isCached(kVideo));
    const CaptionTrackList cached = rig.fetch(kVideo);
    CHECK(cached.fromCache);
    CHECK_EQ(sz(cached.tracks.real.size()), 5);
    CHECK_EQ(listRequests(rig.net).size(), 1);
    rig.now += CaptionTrackService::kCacheTtlMs + 1;
    CHECK(!rig.tracks.isCached(kVideo));
    const CaptionTrackList again = rig.fetch(kVideo);
    CHECK(!again.fromCache);
    CHECK_EQ(listRequests(rig.net).size(), 2);
    rig.tracks.forget(kVideo);
    CHECK(!rig.tracks.isCached(kVideo));

    // An invalid id is refused without a request or a callback.
    bool called = false;
    CHECK(!rig.tracks.fetch(QStringLiteral("not a video"), &context, [&](const CaptionTrackList &) { called = true; }));
    QCoreApplication::processEvents();
    CHECK(!called);

    // A caller that is gone when the answer arrives is not called.
    Rig late;
    late.serveVisitor();
    late.serveList(QStringLiteral("player_visionos.json"), 30);
    bool lateCalled = false;
    {
        QObject gone;
        late.tracks.fetch(kVideo, &gone, [&](const CaptionTrackList &) { lateCalled = true; });
    }
    CHECK(waitFor([&] { return late.tracks.isCached(kVideo); }));
    CHECK(!lateCalled);
}

static void fetchNone() {
    Rig rig;
    rig.serveVisitor();
    rig.serveList(QStringLiteral("player_live.json"));
    const CaptionTrackList none = rig.fetch(kOther);
    CHECK_EQ(oc(none.outcome), oc(Outcome::ValidEmpty));
    CHECK(none.tracks.empty());
    CHECK(rig.tracks.isCached(kOther));   // "has none" is an answer, not a failure
    CHECK_EQ(listRequests(rig.net).size(), 1);
}

static void fetchRefused() {
    // A refused visitor id: forgotten, a new one asked for, the list asked once more.
    Rig rig;
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, fixture(QStringLiteral("next_en.json")));
    rig.serveVisitor();
    rig.serveList(QStringLiteral("player_refused.json"), 0, 1);
    rig.serveList(QStringLiteral("player_visionos.json"));
    // The memory holds the id of an earlier discovery answer (so no visitor request up front).
    bool answered = false;
    QObject context;
    std::function<bool(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return r.outcome == Outcome::Ok; };
    std::function<void(bool)> done = [&](bool) { answered = true; };
    rig.client.post<bool>(QStringLiteral("next"), QJsonObject(), &context, parse, done);
    CHECK(waitFor([&] { return answered; }));
    const QString stale = rig.client.visitorData();
    CHECK(!stale.isEmpty());

    const CaptionTrackList list = rig.fetch(kVideo);
    CHECK_EQ(oc(list.outcome), oc(Outcome::Ok));
    CHECK_EQ(sz(list.tracks.real.size()), 5);
    const QList<HttpRequest> sent = listRequests(rig.net);
    CHECK_EQ(sent.size(), 2);
    CHECK_EQ(rig.tracks.requestsSent(), 2);
    CHECK_EQ(rig.net.count(kVisitorPath), 1);
    CHECK_EQ(QString::fromLatin1(header(sent.at(0), "X-Goog-Visitor-Id")), stale);
    CHECK_EQ(QString::fromLatin1(header(sent.at(1), "X-Goog-Visitor-Id")), fixtureVisitor(QStringLiteral("visitor_id.json")));
    CHECK_EQ(static_cast<int>(rig.counters.value(Area::Captions, Outcome::Ok)), 1);   // counted once

    // Refused again with the new id: a failure, two requests at most, nothing kept.
    Rig refused;
    refused.serveVisitor();
    refused.serveList(QStringLiteral("player_refused.json"));
    const CaptionTrackList failed = refused.fetch(kVideo);
    CHECK_EQ(oc(failed.outcome), oc(Outcome::Unavailable));
    CHECK(failed.tracks.empty());
    CHECK_EQ(listRequests(refused.net).size(), 2);
    CHECK(!refused.tracks.isCached(kVideo));
    CHECK_EQ(static_cast<int>(refused.counters.value(Area::Captions, Outcome::Unavailable)), 1);
    refused.fetch(kVideo);   // asked again next time
    CHECK_EQ(listRequests(refused.net).size(), 4);
}

static void fetchFails() {
    // HTTP failure, then success: failures are never kept.
    Rig rig;
    rig.serveVisitor();
    rig.net.on(kPlayerPath, listMarker(), 503, QByteArray("unavailable"), 0, 1);
    rig.serveList(QStringLiteral("player_visionos.json"));
    const CaptionTrackList failed = rig.fetch(kVideo);
    CHECK_EQ(oc(failed.outcome), oc(Outcome::Http5xx));
    CHECK(isFailure(failed.outcome));
    CHECK(failed.tracks.empty());
    CHECK_EQ(failed.videoId, kVideo);
    CHECK(!rig.tracks.isCached(kVideo));
    CHECK_EQ(listRequests(rig.net).size(), 1);   // an HTTP failure is not asked twice
    const CaptionTrackList ok = rig.fetch(kVideo);
    CHECK_EQ(oc(ok.outcome), oc(Outcome::Ok));
    CHECK_EQ(listRequests(rig.net).size(), 2);

    // Rate limited, not JSON, and no network at all.
    Rig limited;
    limited.serveVisitor();
    limited.net.on(kPlayerPath, listMarker(), 429, QByteArray());
    CHECK_EQ(oc(limited.fetch(kVideo).outcome), oc(Outcome::RateLimited));
    Rig html;
    html.serveVisitor();
    html.net.on(kPlayerPath, listMarker(), 200, QByteArray("<html>consent</html>"));
    CHECK_EQ(oc(html.fetch(kVideo).outcome), oc(Outcome::ShapeDrift));
    Rig offline;   // neither the visitor request nor the list request is answered
    const CaptionTrackList network = offline.fetch(kVideo);
    CHECK_EQ(oc(network.outcome), oc(Outcome::Network));
    CHECK_EQ(listRequests(offline.net).size(), 1);
    CHECK(!hasHeader(listRequests(offline.net).first(), "X-Goog-Visitor-Id"));
}

// --- the model's load state ----------------------------------------------------------------------

static void modelLoadState() {
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/state.conf"), QSettings::IniFormat);
    CaptionTracksModel model(&settings);
    int loadChanges = 0, reloads = 0;
    QObject::connect(&model, &CaptionTracksModel::loadChanged, [&] { ++loadChanges; });
    QObject::connect(&model, &CaptionTracksModel::reloadRequested, [&] { ++reloads; });
    CHECK(!model.loading());
    CHECK(!model.failed());
    CHECK(!model.local());

    model.setLoading(kVideo);
    CHECK(model.loading());
    CHECK(!model.failed());
    CHECK_EQ(model.videoId(), kVideo);
    CHECK(!model.available());
    CHECK_EQ(loadChanges, 1);
    model.setLoading(kVideo);            // already loading: no second signal
    CHECK_EQ(loadChanges, 1);

    model.setFailed(kOther);             // another video's failure is ignored
    CHECK(model.loading());
    model.setFailed(kVideo);
    CHECK(!model.loading());
    CHECK(model.failed());
    CHECK(!model.available());

    model.reload();
    CHECK_EQ(reloads, 1);
    model.setLoading(kVideo);            // asked again
    CHECK(model.loading());
    CHECK(!model.failed());

    // Tracks end the load state.
    model.setFromPlayerAnswer(kVideo, fixtureJson(QStringLiteral("player_visionos.json")));
    CHECK(!model.loading());
    CHECK(!model.failed());
    CHECK(model.available());
    CHECK_EQ(model.count(), 5);

    // Loading another video empties the list at once.
    model.setLoading(kOther);
    CHECK_EQ(model.videoId(), kOther);
    CHECK_EQ(model.count(), 0);
    CHECK(model.loading());

    // A download's saved tracks: local, and neither a later "loading" keeps them nor a failure
    // touches them.
    QVariantMap saved;
    saved.insert(QStringLiteral("languageCode"), QStringLiteral("en"));
    saved.insert(QStringLiteral("name"), QStringLiteral("English"));
    saved.insert(QStringLiteral("path"), dir.path() + QStringLiteral("/en.json3"));
    model.setLocalTracks(kVideo, QVariantList() << saved);
    CHECK(model.local());
    CHECK(!model.loading());
    CHECK_EQ(model.count(), 1);
    model.setFailed(kVideo);
    CHECK(!model.failed());
    CHECK_EQ(model.count(), 1);
    model.clear();
    CHECK(!model.local());
    CHECK(model.videoId().isEmpty());
}

// --- the playing video ---------------------------------------------------------------------------

namespace {

// The catalog objects of one watch page, with playback's caption seam on top.
struct Watch {
    Rig rig;
    QTemporaryDir dir;
    QSettings settings;
    WatchNextService watchNext;
    WatchInfo info;
    CaptionTracksModel model;
    CaptionService captions;
    CaptionCues cues;
    WatchCaptions glue;
    CatalogCaptionSource source;
    int changes = 0;

    Watch()
        : settings(dir.path() + QStringLiteral("/watch.conf"), QSettings::IniFormat),
          watchNext(&rig.client, &rig.counters), info(&watchNext), model(&settings), captions(&rig.net, &rig.lane),
          cues(&captions), glue(&info, &rig.tracks, &model), source(&model, &cues) {
        cues.follow(&model);
        source.setChangedCallback([this] { ++changes; });
    }
    // The watch data and the cue documents; the track list is served per test.
    void serveWatch() {
        rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200, fixture(QStringLiteral("next_en.json")));
        rig.serveVisitor();
        rig.net.on(QStringLiteral("tlang="), QByteArray(), 429, QByteArray());   // what the provider answers today
        rig.net.on(QStringLiteral("kind=asr"), QByteArray(), 200, fixture(QStringLiteral("timedtext_asr_json3.json")));
        rig.net.on(kCuePath, QByteArray(), 200, fixture(QStringLiteral("timedtext_manual_json3.json")));
    }
    // After the track list: the watch data's own WEB player request.
    void serveWebPlayer() { rig.net.on(kPlayerPath, QByteArray(), 200, fixture(QStringLiteral("player_web.json"))); }
    bool listed(const QString &videoId) {
        return waitFor([&] { return model.videoId() == videoId && !model.loading(); }, 8000);
    }
    QStringList ids(const QString &videoId) const {
        QStringList out;
        for (const QVariant &v : source.tracks(videoId)) out.append(v.toMap().value(QStringLiteral("id")).toString());
        return out;
    }
};

}  // namespace

static void streamedVideoWithCaptions() {
    Watch w;
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();

    CHECK(w.source.tracks(kVideo).isEmpty());
    w.info.setVideoId(kVideo);
    CHECK(w.model.loading());                      // asked for with the watch data
    CHECK(w.source.tracks(kVideo).isEmpty());
    CHECK(w.listed(kVideo));
    CHECK(w.model.available());
    CHECK(!w.model.failed());
    CHECK_EQ(w.glue.fetches(), 1);
    CHECK_EQ(listRequests(w.rig.net).size(), 1);

    // playback.captionTracks: the real tracks as the answer lists them, then the translations.
    const QVariantList tracks = w.source.tracks(kVideo);
    const int real = w.model.count();
    CHECK_EQ(real, 5);
    CHECK_EQ(tracks.size(), real + w.model.translationsModel()->total());
    CHECK(tracks.size() > 140);
    const QStringList expected = QStringList() << QStringLiteral("hr") << QStringLiteral("en") << QStringLiteral("asr:en")
                                               << QStringLiteral("it") << QStringLiteral("ja");
    CHECK(w.ids(kVideo).mid(0, real) == expected);
    int misplaced = 0;
    for (int i = 0; i < tracks.size(); ++i) {
        const QVariantMap t = tracks.at(i).toMap();
        if (t.value(QStringLiteral("translated")).toBool() != (i >= real)) ++misplaced;
        if (t.value(QStringLiteral("label")).toString().isEmpty() || t.value(QStringLiteral("language")).toString().isEmpty()) ++misplaced;
    }
    CHECK_EQ(misplaced, 0);
    CHECK_EQ(tracks.at(1).toMap().value(QStringLiteral("label")).toString(), QStringLiteral("English"));
    CHECK(tracks.at(2).toMap().value(QStringLiteral("autoGenerated")).toBool());
    CHECK(!tracks.at(1).toMap().value(QStringLiteral("autoGenerated")).toBool());
    CHECK(w.source.tracks(kOther).isEmpty());      // another video's question is answered with nothing

    // Nothing was ever chosen: captions are off, and no cue document was asked for.
    CHECK(w.source.currentTrackId(kVideo).isEmpty());
    CHECK(w.model.off());
    CHECK(w.source.textAt(5000).isEmpty());
    QCoreApplication::processEvents();
    CHECK_EQ(w.rig.net.count(kCuePath), 0);

    // The user turns captions on (CaptionsPage or the overlay's CC button): the text follows the
    // position, and the language is remembered.
    CHECK(!w.source.select(kOther, QStringLiteral("en")));
    CHECK(!w.source.select(kVideo, QStringLiteral("nope")));
    CHECK(w.source.select(kVideo, QStringLiteral("en")));
    CHECK_EQ(w.source.currentTrackId(kVideo), QStringLiteral("en"));
    CHECK(waitFor([&] { return w.cues.ready(); }));
    CHECK_EQ(w.rig.net.count(kCuePath), 1);
    const QString cueUrl = w.rig.net.requests.last().url.toString();
    CHECK(cueUrl.contains(QStringLiteral("lang=en")));
    CHECK(cueUrl.contains(QStringLiteral("fmt=json3")));
    CHECK(!cueUrl.contains(QStringLiteral("kind=asr")));
    const std::vector<CaptionCue> &cues = w.cues.cues();
    CHECK(cues.size() > 20);
    if (cues.size() > 20) {
        std::size_t other = 1;
        while (other < cues.size() && cues[other].text == cues[0].text) ++other;
        const QString first = w.source.textAt(cues[0].startMs + 1);
        const QString later = w.source.textAt(cues[other].startMs + 1);
        CHECK_EQ(first, QString::fromStdString(cues[0].text));
        CHECK_EQ(later, QString::fromStdString(cues[other].text));
        CHECK(first != later);
        CHECK(!first.isEmpty());
        CHECK(w.source.textAt(cues.back().endMs + 60000).isEmpty());   // past the last cue
    }
    CHECK_EQ(w.settings.value(QLatin1String(CaptionTracksModel::kSettingsKey)).toString(), QStringLiteral("en"));
    CHECK(w.changes > 0);

    // The automatic track is a different document.
    CHECK(w.source.select(kVideo, QStringLiteral("asr:en")));
    CHECK(waitFor([&] { return w.cues.ready() && w.cues.url() == w.model.currentUrl(); }));
    CHECK_EQ(w.rig.net.count(QStringLiteral("kind=asr")), 1);
    CHECK(!w.source.textAt(w.cues.cues().front().startMs + 1).isEmpty());

    // Off clears the text and is remembered as "off".
    CHECK(w.source.select(kVideo, QString()));
    CHECK(w.source.currentTrackId(kVideo).isEmpty());
    CHECK(w.source.textAt(cues.empty() ? 0 : cues[0].startMs + 1).isEmpty());
    CHECK(!w.cues.ready());
    CHECK_EQ(w.settings.value(QLatin1String(CaptionTracksModel::kSettingsKey)).toString(), QString::fromLatin1(kCaptionsOff));

    // Asking for the watch data again does not ask for the list again, nor undo the choice.
    w.info.reload();
    QCoreApplication::processEvents();
    CHECK_EQ(w.glue.fetches(), 1);
    CHECK(w.model.off());
}

static void languageRemembered() {
    Watch w;
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();
    w.info.setVideoId(kVideo);
    CHECK(w.listed(kVideo));
    CHECK(w.source.select(kVideo, QStringLiteral("en")));
    CHECK(waitFor([&] { return w.cues.ready(); }));

    // The next video: the previous tracks are gone at once, then its own list arrives and the
    // remembered language is on without a tap.
    w.info.setVideoId(kOther);
    CHECK_EQ(w.model.count(), 0);
    CHECK(w.source.tracks(kVideo).isEmpty());
    CHECK(w.source.tracks(kOther).isEmpty());
    CHECK(w.source.textAt(5000).isEmpty());
    CHECK(w.listed(kOther));
    CHECK_EQ(w.glue.fetches(), 2);
    CHECK_EQ(w.source.currentTrackId(kOther), QStringLiteral("en"));
    CHECK(waitFor([&] { return w.cues.ready(); }));
    CHECK(!w.source.textAt(w.cues.cues().front().startMs + 1).isEmpty());
    // The auto-pick wrote nothing; the stored value is still the user's.
    CHECK_EQ(w.settings.value(QLatin1String(CaptionTracksModel::kSettingsKey)).toString(), QStringLiteral("en"));

    // Back to the first video: its list is still fresh, so no request.
    const int sent = listRequests(w.rig.net).size();
    w.info.setVideoId(kVideo);
    CHECK(w.listed(kVideo));
    CHECK_EQ(listRequests(w.rig.net).size(), sent);
    CHECK_EQ(w.source.currentTrackId(kVideo), QStringLiteral("en"));

    // A new session starts from the stored language.
    CaptionTracksModel restarted(&w.settings);
    CHECK_EQ(restarted.preferred(), QStringLiteral("en"));
    restarted.setFromPlayerAnswer(kVideo, fixtureJson(QStringLiteral("player_visionos.json")));
    CHECK_EQ(restarted.currentKey(), QStringLiteral("en"));
}

static void streamedVideoWithoutCaptions() {
    Watch w;
    w.settings.setValue(QLatin1String(CaptionTracksModel::kSettingsKey), QStringLiteral("en"));
    CaptionTracksModel model(&w.settings);            // a session that has a stored language
    WatchCaptions glue(&w.info, &w.rig.tracks, &model);
    CaptionCues cues(&w.captions);
    cues.follow(&model);
    CatalogCaptionSource source(&model, &cues);
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_live.json"));
    w.serveWebPlayer();

    w.info.setVideoId(kOther);
    CHECK(waitFor([&] { return model.videoId() == kOther && !model.loading(); }, 8000));
    CHECK(!model.available());
    CHECK(!model.failed());                        // "has none", not "could not be loaded"
    CHECK_EQ(model.count(), 0);
    CHECK(source.tracks(kOther).isEmpty());
    CHECK(source.currentTrackId(kOther).isEmpty());
    CHECK(!source.select(kOther, QStringLiteral("en")));
    CHECK(source.select(kOther, QString()));       // "off" is always accepted
    CHECK(source.textAt(1000).isEmpty());
    QCoreApplication::processEvents();
    CHECK_EQ(w.rig.net.count(kCuePath), 0);
    CHECK(w.info.loaded());
}

static void trackListFails() {
    Watch w;
    w.serveWatch();
    w.rig.net.on(kPlayerPath, listMarker(), 503, QByteArray(), 0, 1);
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();

    w.info.setVideoId(kVideo);
    CHECK(waitFor([&] { return w.model.failed(); }, 8000));
    CHECK(!w.model.loading());
    CHECK(!w.model.available());
    CHECK_EQ(w.model.videoId(), kVideo);
    CHECK(w.source.tracks(kVideo).isEmpty());
    CHECK(w.source.currentTrackId(kVideo).isEmpty());
    CHECK(!w.source.select(kVideo, QStringLiteral("en")));
    CHECK(w.source.textAt(1000).isEmpty());
    CHECK_EQ(w.rig.net.count(kCuePath), 0);
    // The rest of the watch page does not depend on it.
    CHECK(waitFor([&] { return w.info.loaded(); }, 8000));
    CHECK(!w.info.title().isEmpty());

    // "Retry" on the captions page asks again.
    w.model.reload();
    CHECK(w.model.loading());
    CHECK(!w.model.failed());
    CHECK(w.listed(kVideo));
    CHECK(w.model.available());
    CHECK_EQ(w.glue.fetches(), 2);
    CHECK_EQ(w.source.tracks(kVideo).size(), w.model.count() + w.model.translationsModel()->total());

    // Refused twice (the provider wants a sign-in for this video): the same failed state.
    Watch refused;
    refused.serveWatch();
    refused.rig.serveList(QStringLiteral("player_refused.json"));
    refused.serveWebPlayer();
    refused.info.setVideoId(kVideo);
    CHECK(waitFor([&] { return refused.model.failed(); }, 8000));
    CHECK(refused.source.tracks(kVideo).isEmpty());
    CHECK_EQ(listRequests(refused.rig.net).size(), 2);
    // Reloading the watch data asks for a failed list again.
    refused.info.reload();
    CHECK(refused.model.loading());
    CHECK(waitFor([&] { return refused.model.failed(); }, 8000));
    CHECK_EQ(refused.glue.fetches(), 2);
}

static void cueFetchFails() {
    Watch w;
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();
    w.info.setVideoId(kVideo);
    CHECK(w.listed(kVideo));

    // A translated track is offered, and its cue document is refused (HTTP 429).
    const QString translation = QStringLiteral("tlang:de");
    CHECK(w.ids(kVideo).contains(translation));
    CHECK(w.source.select(kVideo, translation));
    CHECK(waitFor([&] { return w.cues.failed(); }));
    CHECK_EQ(oc(w.cues.lastOutcome()), oc(Outcome::RateLimited));
    CHECK_EQ(w.source.currentTrackId(kVideo), translation);   // the choice stays
    CHECK(w.source.textAt(5000).isEmpty());                   // and nothing is drawn
    CHECK_EQ(w.cues.url(), w.model.currentUrl());              // what CaptionsPage's "could not be loaded" reads
    const int asked = w.rig.net.count(QStringLiteral("tlang="));
    CHECK_EQ(asked, 1);
    // Choosing it again asks again (failures are not kept); a real track still works.
    CHECK(w.source.select(kVideo, translation));
    CHECK(waitFor([&] { return w.rig.net.count(QStringLiteral("tlang=")) == asked + 1 && w.cues.failed(); }));
    CHECK(w.source.select(kVideo, QStringLiteral("en")));
    CHECK(waitFor([&] { return w.cues.ready(); }));
    CHECK(!w.cues.failed());
    CHECK(!w.source.textAt(w.cues.cues().front().startMs + 1).isEmpty());
}

static void holdAndStaleAnswers() {
    // The list waits with the watch data until the video plays (never before the first bytes).
    Watch held;
    held.serveWatch();
    held.rig.serveList(QStringLiteral("player_visionos.json"));
    held.serveWebPlayer();
    held.info.setWaitForPlayback(true);
    held.info.setVideoId(kVideo);
    QCoreApplication::processEvents();
    CHECK(held.info.deferred());
    CHECK_EQ(held.rig.net.requests.size(), 0);
    CHECK_EQ(held.glue.fetches(), 0);
    CHECK(!held.model.loading());
    held.info.setPlaybackRunning(true);
    CHECK(held.listed(kVideo));
    CHECK_EQ(held.glue.fetches(), 1);
    CHECK(held.model.available());
    // The next video while its watch data is still held back: the previous video's tracks are
    // gone at once (the CC button must not offer them), and nothing is asked for yet.
    held.info.setPlaybackRunning(false);
    held.info.setVideoId(kOther);
    CHECK(held.info.deferred());
    CHECK_EQ(held.model.count(), 0);
    CHECK(held.model.videoId().isEmpty());
    CHECK(!held.model.loading());
    CHECK(held.source.tracks(kVideo).isEmpty());
    CHECK_EQ(held.glue.fetches(), 1);
    held.info.setPlaybackRunning(true);
    CHECK(held.listed(kOther));
    CHECK_EQ(held.glue.fetches(), 2);

    // The user moves on before the list arrives: the late answer is not shown on the next video.
    Watch w;
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"), 150, 1);   // the first video's list, slow
    w.rig.serveList(QStringLiteral("player_live.json"));               // the second video has none
    w.serveWebPlayer();
    w.info.setVideoId(kVideo);
    CHECK(w.model.loading());
    w.info.setVideoId(kOther);
    CHECK(w.listed(kOther));
    CHECK(waitFor([&] { return w.rig.tracks.isCached(kVideo); }));   // the slow answer did arrive
    QCoreApplication::processEvents();
    CHECK_EQ(w.model.videoId(), kOther);
    CHECK_EQ(w.model.count(), 0);
    CHECK(w.source.tracks(kVideo).isEmpty());
    CHECK(w.source.tracks(kOther).isEmpty());

    // Closing the video empties the model.
    w.info.setVideoId(kVideo);
    CHECK(w.listed(kVideo));
    CHECK(w.model.available());
    w.info.setVideoId(QString());
    CHECK_EQ(w.model.count(), 0);
    CHECK(w.model.videoId().isEmpty());
}

static void downloadsKeepSavedTracks() {
    Watch w;
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();
    QVariantMap saved;
    saved.insert(QStringLiteral("languageCode"), QStringLiteral("en"));
    saved.insert(QStringLiteral("name"), QStringLiteral("English"));
    saved.insert(QStringLiteral("path"), w.dir.path() + QStringLiteral("/en.json3"));
    const QVariantList local = QVariantList() << saved;

    // A download plays offline: its saved tracks stay and nothing is asked for.
    w.info.setOffline(true);
    w.source.setLocalTracks(kVideo, local);        // the session, when it opens the download
    w.info.setVideoId(kVideo);
    QCoreApplication::processEvents();
    CHECK(w.model.local());
    CHECK_EQ(w.model.count(), 1);
    CHECK_EQ(w.source.tracks(kVideo).size(), 1);
    CHECK_EQ(w.glue.fetches(), 0);
    CHECK_EQ(w.rig.net.requests.size(), 0);

    // The saved tracks may arrive after the video id (either order): still no list request
    // replaces them, even once the watch data is asked for.
    w.info.setVideoId(QString());
    w.info.setOffline(false);
    w.info.setVideoId(kOther);
    CHECK(w.model.loading());
    w.source.setLocalTracks(kOther, local);
    CHECK(w.model.local());
    CHECK(waitFor([&] { return w.rig.tracks.isCached(kOther); }));
    QCoreApplication::processEvents();
    CHECK(w.model.local());
    CHECK_EQ(w.model.count(), 1);
    CHECK_EQ(w.source.tracks(kOther).size(), 1);
    w.info.reload();
    QCoreApplication::processEvents();
    CHECK(w.model.local());
    CHECK_EQ(w.glue.fetches(), 1);

    // A download without saved captions, played offline after the same video was streamed: its
    // watch data and its list are still cached, and still nothing is listed or asked for.
    Watch cached;
    cached.serveWatch();
    cached.rig.serveList(QStringLiteral("player_visionos.json"));
    cached.serveWebPlayer();
    cached.info.setVideoId(kVideo);
    CHECK(cached.listed(kVideo));
    CHECK(cached.model.available());
    CHECK(waitFor([&] { return cached.info.loaded() && !cached.info.loading(); }, 8000));
    cached.info.setVideoId(QString());
    cached.info.setOffline(true);
    const int sentBefore = cached.rig.net.requests.size();
    cached.info.setVideoId(kVideo);
    CHECK(!cached.info.deferred());              // the cached page is shown at once
    waitFor([] { return false; }, 100);
    CHECK_EQ(cached.glue.fetches(), 1);
    CHECK_EQ(cached.model.count(), 0);
    CHECK(!cached.model.loading());
    CHECK(cached.source.tracks(kVideo).isEmpty());
    CHECK_EQ(cached.rig.net.requests.size(), sentBefore);
    // Played from the network instead: the list is asked for then (here from the fresh cache).
    cached.info.setOffline(false);
    CHECK(cached.listed(kVideo));
    CHECK(cached.model.available());
    CHECK_EQ(cached.glue.fetches(), 2);
    CHECK_EQ(cached.rig.net.requests.size(), sentBefore);
}

// The shipped state (kCaptionTranslationsOffered is false for beta 3: the provider refuses
// translated tracks): playback.captionTracks lists the real tracks only, a translation cannot be
// chosen, and no translated cue document is ever asked for. Runs before main() turns the switch
// on for the other tests.
static void translationsNotOffered() {
    CHECK(!captionTranslationsOffered());
    Watch w;
    w.settings.setValue(QLatin1String(CaptionTracksModel::kSettingsKey), QStringLiteral("de"));   // only a translation has it
    CaptionTracksModel model(&w.settings);
    CaptionCues cues(&w.captions);
    cues.follow(&model);
    WatchCaptions glue(&w.info, &w.rig.tracks, &model);
    CatalogCaptionSource source(&model, &cues);
    w.serveWatch();
    w.rig.serveList(QStringLiteral("player_visionos.json"));
    w.serveWebPlayer();

    w.info.setVideoId(kVideo);
    CHECK(waitFor([&] { return model.videoId() == kVideo && !model.loading(); }, 8000));
    CHECK(model.available());
    CHECK_EQ(model.count(), 5);
    CHECK_EQ(model.translationsModel()->total(), 0);
    const QVariantList tracks = source.tracks(kVideo);
    CHECK_EQ(tracks.size(), 5);
    int translated = 0;
    for (const QVariant &t : tracks) if (t.toMap().value(QStringLiteral("translated")).toBool()) ++translated;
    CHECK_EQ(translated, 0);
    // The stored language exists only as a translation: captions stay off, nothing is fetched.
    CHECK(source.currentTrackId(kVideo).isEmpty());
    CHECK(!source.select(kVideo, QStringLiteral("tlang:de")));
    QCoreApplication::processEvents();
    CHECK_EQ(w.rig.net.count(kCuePath), 0);
    // Real and auto-generated tracks work as before.
    CHECK(source.select(kVideo, QStringLiteral("asr:en")));
    CHECK(waitFor([&] { return cues.ready(); }));
    CHECK_EQ(w.rig.net.count(QStringLiteral("tlang=")), 0);
    CHECK(!source.textAt(cues.cues().front().startMs + 1).isEmpty());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    translationsNotOffered();
    // From here on the switch is on: the hand-over of translations stays tested although the
    // shipped lists leave them out.
    setCaptionTranslationsOffered(true);
    requestShape();
    visitorMemory();
    parseAnswers();
    fetchManualAndAutomatic();
    fetchNone();
    fetchRefused();
    fetchFails();
    modelLoadState();
    streamedVideoWithCaptions();
    languageRemembered();
    streamedVideoWithoutCaptions();
    trackListFails();
    cueFetchFails();
    holdAndStaleAnswers();
    downloadsKeepSavedTracks();
    return catalog_test::finish("catalog_caption_tracks");
}
