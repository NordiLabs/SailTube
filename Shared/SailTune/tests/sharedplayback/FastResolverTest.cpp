// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The fast path end to end against a local TLS server that plays the provider: recorded-shape
// player answers (fixtures/), a visitor id endpoint and a media host for the probe. Qt Core and
// Network only; no real network.
#include "../../Core/SharedPlayback/Domain/InnerTubeClient.h"
#include "../../Core/SharedPlayback/Infrastructure/FastStreamResolver.h"
#include "../../Core/SharedPlayback/Infrastructure/PlayerResponseParser.h"
#include <cstdio>
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#include "FakeProvider.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSslKey>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace nordtile::music::sharedplayback;
using domain::FailureKind;
using domain::ResolveOutcome;
using domain::StreamRequest;
using infrastructure::FastStreamResolver;


// ---- harness ---------------------------------------------------------------------------------------
struct Rig {
    FakeProvider server;
    std::int64_t clock = 1700000000000LL;
    std::vector<std::string> logs;                    // before the resolver: it logs while it is torn down
    std::unique_ptr<FastStreamResolver> resolver;

    explicit Rig(bool probe = true, int reuseFirstByteMs = 3000, bool injectClock = true) {
        CHECK(server.listen(QHostAddress::LocalHost, 0));
        FastStreamResolver::Options options;
        options.playerHost = QStringLiteral("127.0.0.1");
        options.playerPort = server.serverPort();
        options.ignoreSslErrorsForTesting = true;
        options.probe = probe;
        options.reuseFirstByteTimeoutMs = reuseFirstByteMs;
        options.probeTimeoutMs = 1500;
        options.playerTimeoutMs = 3000;
        options.visitorTimeoutMs = 3000;
        if (injectClock) options.clock = [this]() { return clock; };
        options.log = [this](const std::string &line) { logs.push_back(line); };
        options.mediaEndpointOverride = [this](const QString &, QString *host, quint16 *port) {
            *host = QStringLiteral("127.0.0.1"); *port = server.serverPort(); return true;
        };
        resolver.reset(new FastStreamResolver(options));
        server.playerBodies.push_back(fixture("player_ok.json"));
    }
    ResolveOutcome resolve(const char *id, const StreamRequest &request = StreamRequest()) {
        bool done = false; ResolveOutcome out;
        resolver->resolve(id, request, [&](const ResolveOutcome &outcome) { out = outcome; done = true; });
        CHECK(waitFor([&]() { return done; }));
        return out;
    }
    bool logged(const char *needle) const {
        for (std::size_t i = 0; i < logs.size(); ++i) if (logs[i].find(needle) != std::string::npos) return true;
        return false;
    }
};

// ---- parser -----------------------------------------------------------------------------------------
static void testParser() {
    StreamRequest request;
    const std::int64_t now = 1700000000000LL;
    infrastructure::ParsedPlayer ok = infrastructure::parsePlayerResponse(fixture("player_ok.json"), "AAAAAAAAAAA", request, now);
    CHECK(ok.failure == FailureKind::None);
    // Ranked: Opus 251 > 250 > 249, then the DRC variant, then AAC 140 > 139.
    std::vector<int> order; for (std::size_t i = 0; i < ok.set.audio.size(); ++i) order.push_back(ok.set.audio[i].itag);
    const int expected[] = {251, 250, 249, 774, 140, 139};
    CHECK(order.size() == 6);
    for (std::size_t i = 0; i < order.size() && i < 6; ++i) CHECK(order[i] == expected[i]);
    CHECK(ok.set.audio.front().codec == "opus" && ok.set.audio.front().container() == "webm");
    CHECK(ok.set.audio.front().contentLength == 8296263);
    CHECK(ok.set.video.empty());                                       // audio-only request
    CHECK(ok.droppedFormats == 5);                                     // cipher, DRM, segmented, foreign host, insecure
    CHECK(ok.set.expiresAtUnixMs == 4102444800000LL);
    CHECK(ok.set.hasLoudness && ok.set.loudnessDb > 3.44 && ok.set.loudnessDb < 3.46);   // -10.55 - (-14)
    CHECK(ok.set.metadata.title == "Synthetic Song" && ok.set.metadata.author == "Synthetic Artist - Topic");
    CHECK(ok.set.metadata.durationMs == 211000);
    CHECK(ok.set.metadata.thumbnailUrl.find("hqdefault.jpg") != std::string::npos);
    CHECK(!ok.set.hlsUrl.empty());
    for (std::size_t i = 0; i < ok.set.audio.size(); ++i) CHECK(ok.set.audio[i].url.find("https://rr1---sn-test.googlevideo.com/") == 0);

    request.audioOnly = false;
    infrastructure::ParsedPlayer video = infrastructure::parsePlayerResponse(fixture("player_ok.json"), "AAAAAAAAAAA", request, now);
    CHECK(video.failure == FailureKind::None && !video.set.video.empty());
    for (std::size_t i = 0; i < video.set.video.size(); ++i) CHECK(video.set.video[i].codecFamily() != "av1");
    CHECK(video.set.video.front().height == 1080);
    request = StreamRequest(); request.dubLanguage = "es";
    infrastructure::ParsedPlayer dub = infrastructure::parsePlayerResponse(fixture("player_dub.json"), "AAAAAAAAAAA", request, now);
    CHECK(dub.failure == FailureKind::None && dub.set.audio.front().audioLanguage == "es" && !dub.set.audio.front().isDefaultAudio);
    request = StreamRequest();
    dub = infrastructure::parsePlayerResponse(fixture("player_dub.json"), "AAAAAAAAAAA", request, now);
    CHECK(dub.set.audio.front().audioLanguage == "en" && dub.set.audio.front().itag == 251);
    CHECK(dub.set.audio.back().isDescriptive);                          // described audio last

    struct Case { const char *name; FailureKind failure; const char *id; } cases[] = {
        {"player_bot.json", FailureKind::BotWall, "AAAAAAAAAAA"},
        {"player_age.json", FailureKind::Unplayable, "AAAAAAAAAAA"},
        {"player_removed.json", FailureKind::Final, "AAAAAAAAAAA"},
        {"player_try_later.json", FailureKind::Unplayable, "AAAAAAAAAAA"},
        {"player_ciphered.json", FailureKind::NoUsableFormat, "AAAAAAAAAAA"},
        {"player_live.json", FailureKind::Unplayable, "AAAAAAAAAAA"},
        {"player_other_video.json", FailureKind::BadResponse, "AAAAAAAAAAA"}};
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const infrastructure::ParsedPlayer parsed = infrastructure::parsePlayerResponse(fixture(cases[i].name), cases[i].id, request, now);
        if (parsed.failure != cases[i].failure) { ++failures; std::fprintf(stderr, "FAIL %s: got %s\n", cases[i].name, domain::failureLabel(parsed.failure)); }
        CHECK(parsed.set.audio.empty() || parsed.failure == FailureKind::None);
    }
    CHECK(infrastructure::parsePlayerResponse(QByteArray("not json"), "AAAAAAAAAAA", request, now).failure == FailureKind::BadResponse);
    CHECK(infrastructure::parsePlayerResponse(QByteArray("{}"), "AAAAAAAAAAA", request, now).failure == FailureKind::BadResponse);
    // No per-address expiry: the declared lifetime is used.
    QJsonObject doc = QJsonDocument::fromJson(fixture("player_short_lived.json")).object();
    QJsonObject streaming = doc.value("streamingData").toObject();
    QJsonArray formats = streaming.value("adaptiveFormats").toArray();
    QJsonObject first = formats.at(0).toObject();
    first.insert("url", "https://rr1---sn-test.googlevideo.com/videoplayback?itag=251");
    formats.replace(0, first); streaming.insert("adaptiveFormats", formats); doc.insert("streamingData", streaming);
    const infrastructure::ParsedPlayer lifetime = infrastructure::parsePlayerResponse(QJsonDocument(doc).toJson(), "AAAAAAAAAAA", request, now);
    CHECK(lifetime.failure == FailureKind::None && lifetime.set.expiresAtUnixMs == now + 21540LL * 1000);
}

// ---- resolver ---------------------------------------------------------------------------------------
static void testHappyPath() {
    Rig rig;
    const ResolveOutcome first = rig.resolve("AAAAAAAAAAA");
    CHECK(first.ok() && first.streams.path == domain::ResolvePath::Fast);
    CHECK(!first.streams.audio.empty() && first.streams.audio.front().itag == 251);
    CHECK(first.streams.userAgent == domain::visionosClient().userAgent);
    CHECK(first.streams.timings.totalMs >= 0 && first.streams.timings.playerMs >= 0 && first.streams.timings.probeMs >= 0);
    CHECK(first.streams.timings.visitorMs >= 0);
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1 && rig.server.count("/youtubei/v1/player") == 1);

    const Recorded *player = rig.server.last("/youtubei/v1/player");
    CHECK(player && player->method == "POST");
    CHECK(player && player->path == "/youtubei/v1/player?prettyPrint=false");          // no key, no other query
    CHECK(player && player->body == QByteArray::fromStdString(domain::playerRequestBody(domain::visionosClient(), "AAAAAAAAAAA")));
    CHECK(player && player->headers.value("x-goog-visitor-id") == "FAKEVISITOR%3D%3D");
    CHECK(player && player->headers.value("x-youtube-client-name") == "101" && player->headers.value("x-youtube-client-version") == "1.02");
    CHECK(player && player->headers.value("user-agent") == QByteArray::fromStdString(domain::visionosClient().userAgent));
    CHECK(player && player->headers.value("accept-encoding") == "identity" && player->headers.value("content-type") == "application/json");
    CHECK(player && !player->headers.contains("cookie") && !player->headers.contains("authorization") && !player->headers.contains("x-goog-api-key"));
    CHECK(player && !player->body.contains("visitorData") && !player->body.contains("\"gl\""));

    // The probe is the first request playback makes: same headers, one bounded range from byte 0.
    const Recorded *probe = rig.server.last("/videoplayback");
    CHECK(probe && probe->method == "GET");
    CHECK(probe && probe->headers.value("range") == "bytes=0-8296262");    // min(10 MiB, content length) - 1: playback's first chunk
    CHECK(probe && probe->headers.value("user-agent") == QByteArray::fromStdString(domain::visionosClient().userAgent));
    CHECK(probe && probe->headers.value("sec-fetch-mode") == "navigate" && probe->headers.value("accept-language") == "en-us,en;q=0.5");
    CHECK(probe && probe->headers.value("accept-encoding") == "identity" && !probe->headers.contains("cookie"));

    // Another video: the visitor id and the connection are reused.
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_other_video.json"));
    const ResolveOutcome second = rig.resolve("BBBBBBBBBBB");
    CHECK(second.ok() && second.streams.timings.reusedConnection);
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1);
    CHECK(rig.server.connectionsAccepted <= 3);                         // player connection kept alive; one per probe
    CHECK(second.streams.timings.visitorMs == -1);
    CHECK(rig.logged("sailtune.fast phase resolve path fast"));
    for (std::size_t i = 0; i < rig.logs.size(); ++i)                   // log lines carry no address or identifier
        CHECK(rig.logs[i].find("googlevideo") == std::string::npos && rig.logs[i].find("AAAAAAAAAAA") == std::string::npos);
    CHECK(rig.resolver->counters().fast == 2 && rig.resolver->counters().medianTotalMs() >= 0);
}

static void testCacheAndSingleFlight() {
    Rig rig;
    bool a = false, b = false; ResolveOutcome outA, outB;
    rig.resolver->resolve("AAAAAAAAAAA", StreamRequest(), [&](const ResolveOutcome &o) { outA = o; a = true; });
    rig.resolver->resolve("AAAAAAAAAAA", StreamRequest(), [&](const ResolveOutcome &o) { outB = o; b = true; });
    CHECK(waitFor([&]() { return a && b; }));
    CHECK(outA.ok() && outB.ok());
    CHECK(rig.server.count("/youtubei/v1/player") == 1);                // concurrent callers share one request
    const ResolveOutcome again = rig.resolve("AAAAAAAAAAA");
    CHECK(again.ok() && again.streams.path == domain::ResolvePath::Cache);
    CHECK(rig.server.count("/youtubei/v1/player") == 1);
    StreamRequest other; other.maxAudioKbps = 100;                      // another key: asked again
    CHECK(rig.resolve("AAAAAAAAAAA", other).ok());
    CHECK(rig.server.count("/youtubei/v1/player") == 2);
    rig.resolver->invalidate("AAAAAAAAAAA");
    CHECK(rig.resolve("AAAAAAAAAAA").streams.path == domain::ResolvePath::Fast);
    CHECK(rig.server.count("/youtubei/v1/player") == 3);
    // The cache honours the validity rule: after twenty minutes the entry is gone.
    rig.clock += 21 * 60 * 1000;
    CHECK(rig.resolve("AAAAAAAAAAA").streams.path == domain::ResolvePath::Fast);
    CHECK(rig.server.count("/youtubei/v1/player") == 4);
}

static void testBotWallAndBreaker() {
    Rig rig;
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_bot.json"));
    ResolveOutcome first = rig.resolve("AAAAAAAAAAA");
    CHECK(first.failure == FailureKind::BotWall && first.fallback);     // the host's extractor may still succeed
    CHECK(!rig.resolver->hasVisitor());                                  // the visitor id was discarded
    CHECK(rig.resolver->allowed());
    ResolveOutcome second = rig.resolve("BBBBBBBBBBB");
    CHECK(second.failure == FailureKind::BotWall);
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 2);             // a new id for the second attempt
    CHECK(!rig.resolver->allowed());                                     // two walls in a row: off
    const int playerRequests = rig.server.count("/youtubei/v1/player");
    ResolveOutcome third = rig.resolve("CCCCCCCCCCC");
    CHECK(third.failure == FailureKind::BreakerOpen && third.fallback);
    CHECK(rig.server.count("/youtubei/v1/player") == playerRequests);    // nothing is sent while it is off
    rig.clock += 31 * 60 * 1000;
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_ok.json"));
    CHECK(rig.resolver->allowed());
    CHECK(rig.resolve("AAAAAAAAAAA").ok());                              // thirty minutes later it is used again
    CHECK(rig.resolver->counters().botWalls == 2);
}

static void testSignInThenSuccess() {
    // A common case: the provider asks for a sign-in on one answer, the next one is fine.
    // The fast path reports a fallback, and a later request succeeds without any state to repair.
    Rig rig;
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_age.json"));
    ResolveOutcome age = rig.resolve("AAAAAAAAAAA");
    CHECK(age.failure == FailureKind::Unplayable && age.fallback && age.reason == "login_required");
    CHECK(rig.resolver->hasVisitor() && rig.resolver->allowed());        // an age gate is not a bot wall
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_ok.json"));
    CHECK(rig.resolve("AAAAAAAAAAA").ok());
}

static void testFinalAndShapes() {
    Rig rig;
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_removed.json"));
    ResolveOutcome removed = rig.resolve("AAAAAAAAAAA");
    CHECK(removed.failure == FailureKind::Final && !removed.fallback);   // skipped, no second extractor
    CHECK(rig.resolver->allowed());
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_ciphered.json"));
    ResolveOutcome ciphered = rig.resolve("AAAAAAAAAAA");
    CHECK(ciphered.failure == FailureKind::NoUsableFormat && ciphered.fallback);
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_live.json"));
    CHECK(rig.resolve("AAAAAAAAAAA").failure == FailureKind::Unplayable);
    // An answer about another video is never used.
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_other_video.json"));
    CHECK(rig.resolve("DDDDDDDDDDD").failure == FailureKind::BadResponse);
    // Invalid input never leaves the process.
    const int before = int(rig.server.requests.size());
    CHECK(rig.resolve("short").failure == FailureKind::BadResponse);
    CHECK(int(rig.server.requests.size()) == before);
}

static void testProbeVerdicts() {
    {   // Any HTTP refusal rejects and counts a strike; two strikes switch the fast path off.
        Rig rig;
        rig.server.mediaStatuses.push_back(403);
        ResolveOutcome refused = rig.resolve("AAAAAAAAAAA");
        CHECK(refused.failure == FailureKind::ProbeRejected && refused.fallback && refused.reason == "http_403");
        CHECK(rig.resolver->allowed());
        rig.resolver->invalidate("AAAAAAAAAAA");
        ResolveOutcome again = rig.resolve("AAAAAAAAAAA");
        CHECK(again.failure == FailureKind::ProbeRejected);
        CHECK(!rig.resolver->allowed());
        CHECK(rig.resolver->counters().probeRejected == 2);
    }
    {   // A network failure of the probe is no verdict: the address is accepted and nothing is counted.
        Rig rig;
        rig.server.mediaStatuses.push_back(0);
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.ok());
        CHECK(rig.resolver->allowed());
        rig.server.mediaStatuses.clear(); rig.server.mediaStatuses.push_back(0);
        rig.resolver->invalidate("AAAAAAAAAAA");
        CHECK(rig.resolve("AAAAAAAAAAA").ok() && rig.resolver->allowed());
    }
    {   // The caller reports a refusal downstream: only an address with a minute or more left counts.
        Rig rig;
        CHECK(rig.resolve("AAAAAAAAAAA").ok());
        rig.resolver->reportRefused("AAAAAAAAAAA", 30 * 1000);
        CHECK(rig.resolver->allowed());
        rig.resolver->reportRefused("AAAAAAAAAAA", 5 * 3600 * 1000);
        CHECK(rig.resolver->allowed());
        rig.resolver->reportRefused("AAAAAAAAAAA", 5 * 3600 * 1000);
        CHECK(!rig.resolver->allowed());
    }
    {   // Without a probe the answer is returned as soon as it is parsed.
        Rig rig(false);
        CHECK(rig.resolve("AAAAAAAAAAA").ok());
        CHECK(rig.server.count("/videoplayback") == 0);
    }
}

static void testProbeVerdictKinds() {
    {   // A redirect, a 200 that ignored Range and a Content-Range with another total cannot be played the way playback fetches:
        // unusable (fall back), but not a refusal by the provider - no strike.
        for (int status : {302, 200}) {
            Rig rig;
            rig.server.mediaStatuses.push_back(status);
            ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
            CHECK(out.failure == FailureKind::ProbeRejected && out.fallback && out.reason == "unusable_" + std::to_string(status));
            CHECK(rig.resolver->allowed() && rig.resolver->counters().probeRejected == 0);
        }
        Rig rig;
        rig.server.probeTotal = 123456;                                    // not the length the player answer declared
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.failure == FailureKind::ProbeRejected && out.reason == "unusable_206");
        CHECK(rig.resolver->allowed());
    }
    {   // A server error is no verdict (accepted); a client error other than 403 is a refusal like 403.
        Rig rig;
        rig.server.mediaStatuses.push_back(503);
        CHECK(rig.resolve("AAAAAAAAAAA").ok() && rig.resolver->allowed());
        Rig refused;
        refused.server.mediaStatuses.push_back(404);
        CHECK(refused.resolve("AAAAAAAAAAA").reason == "http_404");
        refused.resolver->invalidate("AAAAAAAAAAA");
        CHECK(refused.resolve("AAAAAAAAAAA").failure == FailureKind::ProbeRejected && !refused.resolver->allowed());
    }
    {   // probeAddress() for a rung the host is about to deliver: same verdicts, same strikes.
        Rig rig;
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.ok() && out.streams.probedUrl == out.streams.audio.front().url);
        const domain::StreamFormat &other = out.streams.audio.back();
        FastStreamResolver::ProbeVerdict verdict = FastStreamResolver::ProbeVerdict::NoVerdict; bool done = false;
        rig.resolver->probeAddress(other.url, out.streams.userAgent, other.contentLength, [&](FastStreamResolver::ProbeVerdict v, int, int) { verdict = v; done = true; });
        CHECK(waitFor([&]() { return done; }) && verdict == FastStreamResolver::ProbeVerdict::Accepted);
        rig.server.mediaStatuses.clear(); rig.server.mediaStatuses.push_back(403);
        done = false;
        rig.resolver->probeAddress(other.url, out.streams.userAgent, other.contentLength, [&](FastStreamResolver::ProbeVerdict v, int, int) { verdict = v; done = true; });
        CHECK(waitFor([&]() { return done; }) && verdict == FastStreamResolver::ProbeVerdict::Refused);
        CHECK(rig.resolver->counters().probeRejected == 1);
    }
}

// The provider turning the client away, rate limiting it, or no longer giving plain addresses is about the client:
// it feeds the breaker. A server error or a network failure does not.
static void testStructuralFailuresFeedTheBreaker() {
    {
        Rig rig;
        rig.server.playerStatuses.push_back(403);
        CHECK(rig.resolve("AAAAAAAAAAA").reason == "http_403" && rig.resolver->allowed());
        rig.resolver->invalidate("AAAAAAAAAAA");
        CHECK(rig.resolve("AAAAAAAAAAA").reason == "http_403");
        CHECK(!rig.resolver->allowed());                                   // two in ten minutes: off
    }
    {
        Rig rig;
        rig.server.playerStatuses.push_back(429);
        rig.resolve("AAAAAAAAAAA"); rig.resolve("BBBBBBBBBBB");
        CHECK(!rig.resolver->allowed());
    }
    {
        Rig rig;
        rig.server.playerStatuses.push_back(500);
        for (int i = 0; i < 4; ++i) rig.resolve("AAAAAAAAAAA");
        CHECK(rig.resolver->allowed());                                    // server errors never count
    }
    {   // No plain addresses three times in a row (the client may have stopped working): strikes from the third on.
        Rig rig;
        rig.server.playerBodies.clear(); rig.server.playerBodies.push_back(fixture("player_ciphered.json"));
        for (int i = 0; i < 3; ++i) CHECK(rig.resolve("AAAAAAAAAAA").failure == FailureKind::NoUsableFormat);
        CHECK(rig.resolver->allowed());
        CHECK(rig.resolve("AAAAAAAAAAA").failure == FailureKind::NoUsableFormat);
        CHECK(!rig.resolver->allowed());
    }
    {   // One answer with plain addresses resets the streak.
        Rig rig;
        rig.server.playerBodies.clear(); rig.server.playerBodies.push_back(fixture("player_ciphered.json"));
        rig.resolve("AAAAAAAAAAA"); rig.resolve("AAAAAAAAAAA");
        rig.server.playerBodies.clear(); rig.server.playerBodies.push_back(fixture("player_ok.json"));
        CHECK(rig.resolve("AAAAAAAAAAA").ok());
        rig.server.playerBodies.clear(); rig.server.playerBodies.push_back(fixture("player_ciphered.json"));
        rig.resolver->invalidate("AAAAAAAAAAA");
        rig.resolve("AAAAAAAAAAA"); rig.resolve("AAAAAAAAAAA");
        CHECK(rig.resolver->allowed());
    }
}

static void testFailedVisitorMintBacksOff() {
    Rig rig;
    rig.server.visitorStatuses.push_back(500);
    ResolveOutcome first = rig.resolve("AAAAAAAAAAA");
    CHECK(first.failure == FailureKind::Network && first.fallback && first.reason == "no_visitor");
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1 && rig.server.count("/youtubei/v1/player") == 0);   // no blind player request
    // Taps within the back-off go straight to the extractor without asking again.
    for (int i = 0; i < 3; ++i) CHECK(rig.resolve("AAAAAAAAAAA").reason == "no_visitor");
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1);
    CHECK(rig.resolver->allowed());                                        // a server error is no strike
    rig.clock += 16 * 1000;                                                // after 15 s it tries again
    CHECK(rig.resolve("AAAAAAAAAAA").reason == "no_visitor");
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 2);
    rig.clock += 30 * 1000;                                                // the next wait is a minute
    rig.resolve("AAAAAAAAAAA");
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 2);
    rig.clock += 40 * 1000;
    rig.server.visitorStatuses.clear();                                    // the provider recovers
    CHECK(rig.resolve("AAAAAAAAAAA").ok() && rig.server.count("/youtubei/v1/visitor_id") == 3);
    {   // A refusal of the mint itself is a strike (twice: off).
        Rig refused;
        refused.server.visitorStatuses.push_back(403);
        refused.resolve("AAAAAAAAAAA");
        CHECK(refused.resolver->allowed());
        refused.clock += 16 * 1000;
        refused.resolve("AAAAAAAAAAA");
        CHECK(!refused.resolver->allowed());
    }
}

static void testConcurrentBotWallsAreOneVerdict() {
    Rig rig;
    rig.server.answerAnyVideoId = true;
    rig.server.playerBodies.clear();
    rig.server.playerBodies.push_back(fixture("player_bot.json"));
    ResolveOutcome a, b; bool doneA = false, doneB = false;
    rig.resolver->resolve("AAAAAAAAAAA", StreamRequest(), [&](const ResolveOutcome &o) { a = o; doneA = true; });
    rig.resolver->resolve("BBBBBBBBBBB", StreamRequest(), [&](const ResolveOutcome &o) { b = o; doneB = true; });   // the prefetch of the next track
    CHECK(waitFor([&]() { return doneA && doneB; }));
    CHECK(a.failure == FailureKind::BotWall && b.failure == FailureKind::BotWall);
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1);               // they shared one visitor id
    CHECK(rig.resolver->counters().botWalls == 1);                         // one verdict on it
    CHECK(rig.resolver->allowed());                                        // not yet two walls in a row
    CHECK(!rig.resolver->hasVisitor());
}

static void testNetworkChangeForgetsEverythingBoundToTheOldNetwork() {
    Rig rig;
    CHECK(rig.resolve("AAAAAAAAAAA").ok());
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 1 && rig.server.count("/youtubei/v1/player") == 1);
    CHECK(rig.resolve("AAAAAAAAAAA").streams.path == domain::ResolvePath::Cache);
    rig.resolver->networkChanged();
    CHECK(!rig.resolver->hasVisitor() && rig.resolver->allowed());         // not a strike
    ResolveOutcome after = rig.resolve("AAAAAAAAAAA");
    CHECK(after.ok() && after.streams.path == domain::ResolvePath::Fast);   // not the cached address
    CHECK(rig.server.count("/youtubei/v1/visitor_id") == 2 && rig.server.count("/youtubei/v1/player") == 2);
    CHECK(rig.logged("network_changed"));
}

// A connection a carrier dropped silently accepts the request and never answers: the repeat on a fresh connection must
// not wait out the whole request timeout.
static void testSilentlyDeadPooledConnection() {
    Rig rig(true, 400);
    rig.server.silentAfterFirstAnswer = true;
    QElapsedTimer timer; timer.start();
    ResolveOutcome out = rig.resolve("AAAAAAAAAAA");          // visitor id on a new connection; the player request reuses it: silent
    CHECK(out.ok());
    CHECK(timer.elapsed() < 2500);                             // the request timeout is 3 s
    CHECK(rig.server.connectionsAccepted >= 2);
}

// An answer with no audio-only address but a progressive stream (video and audio together) is usable as the last resort
// for an audio request, instead of sending the track to the slow extractor.
static void testMuxedIsTheLastResort() {
    {
        StreamRequest request;
        infrastructure::ParsedPlayer parsed = infrastructure::parsePlayerResponse(fixture("player_muxed_only.json"), "AAAAAAAAAAA", request, 1700000000000LL);
        CHECK(parsed.failure == FailureKind::None && parsed.set.audio.empty() && parsed.set.muxed.size() == 1);
        CHECK(parsed.set.top() && parsed.set.top()->itag == 18 && parsed.set.top()->hasAudio && parsed.set.top()->hasVideo);
        // A video request still needs video-only addresses; this answer has them, so it is fine too; but no audio and no muxed is not.
        infrastructure::ParsedPlayer ciphered = infrastructure::parsePlayerResponse(fixture("player_ciphered.json"), "AAAAAAAAAAA", request, 1700000000000LL);
        CHECK(ciphered.failure == FailureKind::NoUsableFormat);
    }
    {
        Rig rig;
        rig.server.playerBodies.clear(); rig.server.playerBodies.push_back(fixture("player_muxed_only.json"));
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.ok() && out.streams.audio.empty() && out.streams.muxed.size() == 1);
        CHECK(out.streams.probedUrl == out.streams.muxed.front().url);     // the progressive stream is what was probed
        CHECK(rig.server.count("/videoplayback") == 1);
        CHECK(rig.logged("top_itag 18"));
    }
}

// Without an injected clock the breaker and the visitor id's age run on the monotonic clock: the same behaviour, no
// dependence on the time of day.
static void testDefaultClockUsesTheMonotonicClockForDurations() {
    Rig rig(true, 3000, false);
    rig.server.mediaStatuses.push_back(403);
    CHECK(rig.resolve("AAAAAAAAAAA").reason == "http_403" && rig.resolver->allowed());
    rig.resolver->invalidate("AAAAAAAAAAA");
    CHECK(rig.resolve("AAAAAAAAAAA").reason == "http_403");
    CHECK(!rig.resolver->allowed());                                   // two strikes within ten minutes, whatever the wall clock says
    Rig fresh(true, 3000, false);
    CHECK(fresh.resolve("AAAAAAAAAAA").ok() && fresh.resolver->hasVisitor());
    CHECK(fresh.resolve("BBBBBBBBBBB").failure == FailureKind::BadResponse);      // the answer is about another video; nothing breaks
    CHECK(fresh.server.count("/youtubei/v1/visitor_id") == 1);        // the visitor id was reused (its age is small)
}

// The media host answers the first ranged request with a redirect to another node of its CDN (observed on the device: the
// probe read the 302 as unusable and sent the track to the slow extractor - 22 s to first audio).
static void testProbeFollowsRedirectsWithinTheCdn() {
    const QByteArray other = "https://rr2---sn-other.googlevideo.com/videoplayback?expire=4102444800&ei=SYNTHETIC&ip=0.0.0.0&id=o-SYNTHETIC&itag=251&source=youtube&sig=SYNTHETIC&redirect_counter=1";
    {
        Rig rig;
        rig.server.redirectLocation = other;
        rig.server.mediaStatuses = {302, 206};
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.ok() && out.streams.audio.front().url.find(other.toStdString()) == 0 && out.streams.probedUrl == out.streams.audio.front().url);
        CHECK(rig.server.count("/videoplayback") == 2 && rig.resolver->allowed() && rig.resolver->counters().probeRejected == 0);
        CHECK(out.streams.audio.size() > 1 && out.streams.audio[1].url.find("redirect_counter") == std::string::npos);   // only the probed rung changes
        // The cache hands out the address that answered.
        CHECK(rig.resolve("AAAAAAAAAAA").streams.audio.front().url.find(other.toStdString()) == 0);
        CHECK(rig.logged("probe redirect hop"));
    }
    {   // Up to three hops; a fourth is not followed.
        Rig rig;
        rig.server.redirectLocation = other;
        rig.server.mediaStatuses = {302, 302, 302, 206};
        CHECK(rig.resolve("AAAAAAAAAAA").ok());
        Rig loop;
        loop.server.redirectLocation = other;
        loop.server.mediaStatuses = {302};                               // always
        ResolveOutcome out = loop.resolve("AAAAAAAAAAA");
        CHECK(out.failure == FailureKind::ProbeRejected && out.reason == "unusable_302" && out.fallback);
        CHECK(loop.server.count("/videoplayback") == 4 && loop.resolver->allowed());   // the first answer and three followed
    }
    {   // Only the CDN's own hosts, over HTTPS, absolute: anything else is not followed (and is no strike).
        for (const char *bad : {"https://evil.example.com/videoplayback?x=1", "http://rr2---sn-other.googlevideo.com/videoplayback?x=1",
                                "https://rr2---sn-other.googlevideo.com:8443/videoplayback?x=1", "/videoplayback?x=1",
                                "https://user@rr2---sn-other.googlevideo.com/videoplayback", "https://googlevideo.com.evil.com/videoplayback", ""}) {
            Rig rig;
            rig.server.redirectLocation = bad;
            rig.server.mediaStatuses = {302, 206};
            ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
            CHECK(out.failure == FailureKind::ProbeRejected && out.reason == "unusable_302");
            CHECK(rig.server.count("/videoplayback") == 1 && rig.resolver->allowed());
        }
    }
    {   // After a redirect, a refusal by the new node is still a refusal (strike).
        Rig rig;
        rig.server.redirectLocation = other;
        rig.server.mediaStatuses = {302, 403};
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.failure == FailureKind::ProbeRejected && out.reason == "http_403" && rig.resolver->counters().probeRejected == 1);
    }
}

static void testTransportFailures() {
    {   // The provider answers with a server error: no strike, a fallback is suggested.
        Rig rig;
        rig.server.playerStatuses.push_back(500);
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.failure == FailureKind::BadResponse && out.fallback && out.reason == "http_500");
        CHECK(rig.resolver->allowed());
    }
    {   // Chunked answers and a connection closed after every answer both work.
        Rig rig;
        rig.server.chunkedPlayer = true;
        rig.server.closeAfterEveryAnswer = true;
        CHECK(rig.resolve("AAAAAAAAAAA").ok());
        rig.resolver->invalidate("AAAAAAAAAAA");
        StreamRequest other; other.maxAudioKbps = 140;
        CHECK(rig.resolve("AAAAAAAAAAA", other).ok());
    }
    {   // Nobody listens: a network class, never a strike.
        Rig rig;
        rig.server.close();
        ResolveOutcome out = rig.resolve("AAAAAAAAAAA");
        CHECK(out.failure == FailureKind::Network && out.fallback);
        CHECK(rig.resolver->allowed());
        CHECK(rig.resolver->counters().network == 1);
    }
    {   // Cancelling a waiter delivers Cancelled once and leaves the others alone.
        Rig rig;
        bool a = false, b = false; ResolveOutcome outA, outB;
        const FastStreamResolver::RequestId first = rig.resolver->resolve("AAAAAAAAAAA", StreamRequest(), [&](const ResolveOutcome &o) { outA = o; a = true; });
        rig.resolver->resolve("AAAAAAAAAAA", StreamRequest(), [&](const ResolveOutcome &o) { outB = o; b = true; });
        rig.resolver->cancel(first);
        CHECK(waitFor([&]() { return a && b; }));
        CHECK(outA.failure == FailureKind::Cancelled && outB.ok());
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    testParser();
    testHappyPath();
    testCacheAndSingleFlight();
    testBotWallAndBreaker();
    testSignInThenSuccess();
    testFinalAndShapes();
    testProbeVerdicts();
    testProbeVerdictKinds();
    testStructuralFailuresFeedTheBreaker();
    testFailedVisitorMintBacksOff();
    testConcurrentBotWallsAreOneVerdict();
    testNetworkChangeForgetsEverythingBoundToTheOldNetwork();
    testSilentlyDeadPooledConnection();
    testMuxedIsTheLastResort();
    testProbeFollowsRedirectsWithinTheCdn();
    testDefaultClockUsesTheMonotonicClockForDurations();
    testTransportFailures();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("sharedplayback FastResolverTest: all checks passed\n");
    return 0;
}
