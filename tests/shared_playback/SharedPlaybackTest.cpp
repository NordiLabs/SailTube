// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// QtCore checks of the yt-dlp fallback adapter YtDlpStreamResolver over the shared interface
// (Shared/Playback/StreamSource.h): fixture mapping, RES-09 mapping of core's codes, RES-20 no
// cookies, RES-10 single flight / cache / bounded join / peek, RES-11 prefetch cap, RES-07 strike
// rules (a refused or just failed video starts with the full profile; a stall or a lost connection
// does not), answer tagging, the exactly-once completion contract (cancellation included) and the
// direct fetcher. The extractor
// is a fake executable written by the test that records its arguments and prints
// tests/fixtures/details_vod.json.
// Build: tests/shared_playback/run.sh (needs Qt5Core; skipped where it is absent).
#include "Core/Player/Infrastructure/YtDlpStreamResolver.h"
#include "Core/Player/Infrastructure/YtDlpStreamMapping.h"
#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <cstdio>
#include <functional>
#include <vector>

using namespace nordtile;
using playback::StreamError;
using playback::StreamErrorClass;
using playback::StreamRequest;
using playback::StreamSet;
using video::playeradapter::YtDlpStreamResolver;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const char *const fixtureId = "aBcDeFgHiJk";

static QByteArray fixture(const char *name) {
    QFile f(QStringLiteral(SAILTUBE_FIXTURES "/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("missing fixture %s\n", name); ++failures; return QByteArray(); }
    return f.readAll();
}

static bool waitUntil(const std::function<bool()> &done, int ms = 10000) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return done();
}

static void settle(int ms) { waitUntil([]() { return false; }, ms); }

static std::vector<std::string> ids(const QList<playback::StreamFormat> &list) {
    std::vector<std::string> out;
    for (const playback::StreamFormat &f : list) out.push_back(f.formatId.toStdString());
    return out;
}

// One completion as delivered.
struct Answer {
    StreamSet streams;
    StreamError error;
};
typedef std::vector<Answer> Answers;
static playback::StreamResolver::Completion into(Answers *list) {
    return [list](const StreamSet &s, const StreamError &e) { Answer a; a.streams = s; a.error = e; list->push_back(a); };
}

// A fake extractor: appends its arguments (one per line, then "--end--") to args.log, waits a
// little so that concurrent requests overlap, and prints the VOD fixture.
struct FakeExtractor {
    QTemporaryDir dir;
    QString script, log;
    bool ok = false;
    FakeExtractor() {
        if (!dir.isValid()) return;
        script = dir.path() + QStringLiteral("/fake-yt-dlp");
        log = dir.path() + QStringLiteral("/args.log");
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly)) return;
        const QByteArray body = "#!/bin/sh\n"
            "{ for a in \"$@\"; do printf '%s\\n' \"$a\"; done; echo '--end--'; } >> '" + log.toUtf8() + "'\n"
            "sleep 0.4 2>/dev/null || sleep 1\n"
            "cat '" SAILTUBE_FIXTURES "/details_vod.json'\n";
        f.write(body);
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        ok = true;
    }
    QList<QStringList> runs() const {
        QList<QStringList> out;
        QFile f(log);
        if (!f.open(QIODevice::ReadOnly)) return out;
        QStringList current;
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line == "--end--") { out << current; current.clear(); }
            else if (!line.isEmpty()) current << QString::fromUtf8(line);
        }
        return out;
    }
};

// The fixture's stream URLs carry expire=1790003600 (core's RES-22 fixture).
// The resolver's clock runs in real time from six hours before that, so the
// answers stay cacheable whatever today's date is.
// A test moves the clock forward with clockShiftMs (the strike rules' five minutes).
static qint64 clockShiftMs = 0;
static YtDlpStreamResolver::Clock fixtureClock() {
    const qint64 base = 1790003600LL * 1000 - 6LL * 3600 * 1000;
    const qint64 start = QDateTime::currentMSecsSinceEpoch();
    return [base, start]() { return base + (QDateTime::currentMSecsSinceEpoch() - start) + clockShiftMs; };
}

struct Rig {
    FakeExtractor fake;
    video::resolver::RunnerOptions options;
    video::resolver::YtDlpRunner *runner = nullptr;
    video::resolver::YtDlpVideoService *service = nullptr;
    YtDlpStreamResolver *resolver = nullptr;
    explicit Rig(const player::CachePolicy &policy = player::CachePolicy()) {
        options.executablePath = fake.script;
        options.pythonChild = false;
        options.javascriptRuntime = QStringLiteral("quickjs:/opt/test/quickjs");
        options.maximumConcurrent = 4;
        runner = new video::resolver::YtDlpRunner(options);
        service = new video::resolver::YtDlpVideoService(runner);
        resolver = new YtDlpStreamResolver(service, nullptr, policy, fixtureClock());
    }
    ~Rig() { delete resolver; delete service; delete runner; }
};

static StreamRequest videoRequest(const char *id = fixtureId) {
    StreamRequest r;
    r.videoId = QString::fromLatin1(id);
    r.maxHeight = 0;
    return r;
}

static qint64 number(const QVariantMap &d, const char *key) { return d.value(QLatin1String(key)).toLongLong(); }

// ---------------------------------------------------------------------------------------------

static void mapping_fixtureToStreamSet() {
    video::catalog::VideoDetails d;
    CHECK(video::resolver::parseDetails(fixture("details_vod.json"), fixtureId, &d) == video::resolver::ErrorCode::None);
    StreamRequest r = videoRequest();
    StreamSet s = video::playeradapter::toStreamSet(d, r);
    CHECK(s.videoId == QLatin1String(fixtureId) && s.liveState == playback::LiveState::NotLive && !s.live && s.hlsManifest.isEmpty());
    // video-only 137 (H.264 1080p) and 337 (VP9.2 HDR 2160p); no ceiling: tallest first.
    CHECK((ids(s.video) == std::vector<std::string>{"337", "137"}));
    // SEL-04: original-language 140 first, then Opus 251, then non-DRC 139, DRC last.
    CHECK((ids(s.audio) == std::vector<std::string>{"140", "251", "139", "140-drc"}));
    // muxed last resort: HLS 96 before progressive 18.
    CHECK((ids(s.muxed) == std::vector<std::string>{"96", "18"}));
    CHECK(s.muxed.size() == 2 && s.muxed[0].isHls && !s.muxed[1].isHls);
    bool ua = false;
    if (s.video.size() == 2) for (const auto &h : s.video[1].headers) ua = ua || h.first == "User-Agent";
    CHECK(ua);
    CHECK(!s.audio.isEmpty() && s.audio[0].contentLength == 3400000 && s.audio[0].language == QLatin1String("en")
          && s.audio[0].languagePreference == 10 && s.audio[0].originalAudio);
    CHECK(s.audio.size() == 4 && s.audio[3].dynamicRangeCompressed && s.video[0].hdr && s.video[0].container == QLatin1String("webm"));
    CHECK(s.expiresAtUnixMs == 1790003600LL * 1000 && !s.hasLoudness); // earliest expire= of the URLs
    r.maxHeight = 1080;
    s = video::playeradapter::toStreamSet(d, r);
    CHECK((ids(s.video) == std::vector<std::string>{"137", "337"}));
    r.maxHeight = 720;
    s = video::playeradapter::toStreamSet(d, r);
    CHECK((ids(s.muxed) == std::vector<std::string>{"18", "96"}));
    StreamRequest audioOnly;
    audioOnly.purpose = playback::StreamPurpose::AudioOnly;
    s = video::playeradapter::toStreamSet(d, audioOnly);
    CHECK(s.video.isEmpty() && s.audio.size() == 4);
}

// RES-22: formats failing core's existing URL validation (missing URL, plain http) and
// non-media formats (storyboard) never reach the answer.
static void res22_invalidUrlsDropped() {
    video::catalog::VideoDetails d;
    video::resolver::parseDetails(fixture("details_vod.json"), fixtureId, &d);
    const StreamSet s = video::playeradapter::toStreamSet(d, videoRequest());
    for (const QList<playback::StreamFormat> *list : {&s.video, &s.audio, &s.muxed}) {
        for (const playback::StreamFormat &f : *list) {
            CHECK(f.formatId != QLatin1String("http") && f.formatId != QLatin1String("nourl") && f.formatId != QLatin1String("sb0"));
            CHECK(f.url.scheme() == QLatin1String("https"));
        }
    }
    CHECK(s.video.size() + s.audio.size() + s.muxed.size() == 8);
}

static void res09_coreCodesMapped() {
    using video::resolver::ErrorCode;
    struct Row { ErrorCode code; StreamErrorClass error; bool finalSignedOut; bool finalSignedIn; };
    const Row rows[] = {
        {ErrorCode::None, StreamErrorClass::None, false, false},
        {ErrorCode::InvalidRequest, StreamErrorClass::InvalidId, true, true},
        {ErrorCode::PrivateVideo, StreamErrorClass::Private, true, false},
        // Without a reason it is the bare "unavailable": not final (RES-09; reasons: res09_reasonsMapped).
        {ErrorCode::VideoUnavailable, StreamErrorClass::Unavailable, false, false},
        {ErrorCode::Upcoming, StreamErrorClass::Upcoming, true, true},
        {ErrorCode::BotCheck, StreamErrorClass::BotCheck, false, false},
        {ErrorCode::AgeRestricted, StreamErrorClass::AgeRestricted, false, false},
        {ErrorCode::Network, StreamErrorClass::Network, false, false},
        {ErrorCode::Timeout, StreamErrorClass::Timeout, false, false},
        {ErrorCode::Busy, StreamErrorClass::Busy, false, false},
        {ErrorCode::InvalidResponse, StreamErrorClass::InvalidResponse, false, false},
        {ErrorCode::NoPlayableStream, StreamErrorClass::NoPlayableStream, false, false},
        {ErrorCode::RuntimeUnavailable, StreamErrorClass::RuntimeUnavailable, false, false},
        {ErrorCode::Cancelled, StreamErrorClass::Cancelled, false, false},
        {ErrorCode::Failed, StreamErrorClass::Failed, false, false},
    };
    for (const Row &row : rows) {
        const StreamError out = YtDlpStreamResolver::errorFor(row.code, false);
        const StreamError in = YtDlpStreamResolver::errorFor(row.code, true);
        CHECK(out.kind == row.error && out.final == row.finalSignedOut && in.final == row.finalSignedIn);
        CHECK(row.error == StreamErrorClass::None ? out.label.isEmpty() : out.label == QLatin1String(playback::errorLabel(row.error)));
    }
}

// RES-09 with the run's reason (core folds removed / terminated / country into one code).
static void res09_reasonsMapped() {
    using video::resolver::ErrorCode;
    using video::resolver::FailureReason;
    struct Row { ErrorCode code; FailureReason reason; StreamErrorClass error; bool final; };
    const Row rows[] = {
        {ErrorCode::VideoUnavailable, FailureReason::Removed, StreamErrorClass::Removed, true},
        {ErrorCode::VideoUnavailable, FailureReason::Terminated, StreamErrorClass::Terminated, true},
        {ErrorCode::VideoUnavailable, FailureReason::GeoBlocked, StreamErrorClass::GeoBlocked, true},
        {ErrorCode::VideoUnavailable, FailureReason::BareUnavailable, StreamErrorClass::Unavailable, false},
        {ErrorCode::VideoUnavailable, FailureReason::MembersOnly, StreamErrorClass::SignInRequired, true},
        {ErrorCode::Network, FailureReason::RateLimited, StreamErrorClass::TryLater, false},
        {ErrorCode::Network, FailureReason::None, StreamErrorClass::Network, false},
        {ErrorCode::Failed, FailureReason::StorageLimit, StreamErrorClass::Failed, false},
        {ErrorCode::Failed, FailureReason::DeliveryChanged, StreamErrorClass::DeliveryChanged, false},
        {ErrorCode::Failed, FailureReason::MembersOnly, StreamErrorClass::SignInRequired, true},
        {ErrorCode::NoPlayableStream, FailureReason::DeliveryChanged, StreamErrorClass::DeliveryChanged, false},
        {ErrorCode::PrivateVideo, FailureReason::Private, StreamErrorClass::Private, true},
        {ErrorCode::Upcoming, FailureReason::Upcoming, StreamErrorClass::Upcoming, true},
    };
    for (const Row &row : rows) {
        const StreamError e = YtDlpStreamResolver::errorFor(row.code, row.reason, false);
        CHECK(e.kind == row.error && e.final == row.final);
        // The runtime's temporary-file budget keeps the class Failed under its own label.
        CHECK(e.label == QLatin1String(row.reason == FailureReason::StorageLimit ? "storage_limit" : playback::errorLabel(row.error)));
    }
    // Members-only signed in: the cookie rung may still serve it, so not final there.
    CHECK(!YtDlpStreamResolver::errorFor(ErrorCode::VideoUnavailable, FailureReason::MembersOnly, true).final);
    // A run without a usable set: formats with URLs that do not fit are a device limit; none at all
    // means the delivery changed; the live states and the stderr reasons come first.
    StreamSet withUrl;
    playback::StreamFormat av1;
    av1.url = QUrl(QStringLiteral("https://rr1---sn-x.googlevideo.com/videoplayback?itag=399"));
    av1.videoCodec = QStringLiteral("av01.0.08M.08");
    av1.audioCodec = QStringLiteral("none");
    withUrl.video << av1;
    CHECK(YtDlpStreamResolver::unplayableError(withUrl, FailureReason::DeliveryChanged, true, false).kind == StreamErrorClass::NoPlayableStream);
    const StreamSet empty;
    // Nothing listed and no reason: no more than "unavailable", and not final.
    const StreamError bare = YtDlpStreamResolver::unplayableError(empty, FailureReason::None, false, false);
    CHECK(bare.kind == StreamErrorClass::Unavailable && !bare.final);
    CHECK(YtDlpStreamResolver::unplayableError(empty, FailureReason::None, true, false).kind == StreamErrorClass::DeliveryChanged);
    CHECK(YtDlpStreamResolver::unplayableError(empty, FailureReason::DeliveryChanged, false, false).kind == StreamErrorClass::DeliveryChanged);
    CHECK(YtDlpStreamResolver::unplayableError(empty, FailureReason::RateLimited, false, false).kind == StreamErrorClass::TryLater);
    const StreamError budget = YtDlpStreamResolver::unplayableError(empty, FailureReason::StorageLimit, false, false);
    CHECK(budget.kind == StreamErrorClass::Failed && !budget.final && budget.label == QLatin1String("storage_limit"));
    CHECK(YtDlpStreamResolver::unplayableError(empty, FailureReason::MembersOnly, false, false).kind == StreamErrorClass::SignInRequired);
    StreamSet upcoming;
    upcoming.liveState = playback::LiveState::Upcoming;
    CHECK(YtDlpStreamResolver::unplayableError(upcoming, FailureReason::None, false, false).kind == StreamErrorClass::Upcoming);
    CHECK(YtDlpStreamResolver::unplayableError(upcoming, FailureReason::None, false, false).final);
}

// A scripted extractor: the token-free pass (no --js-runtimes) and the widened pass (with it) each
// print their own stdout / stderr and exit code; every run's arguments are logged.
struct ScenarioExtractor {
    QTemporaryDir dir;
    QString script, log;
    bool ok = false;
    ScenarioExtractor() {
        if (!dir.isValid()) return;
        script = dir.path() + QStringLiteral("/fake-yt-dlp");
        log = dir.path() + QStringLiteral("/args.log");
        QFile f(script);
        if (!f.open(QIODevice::WriteOnly)) return;
        const QByteArray d = dir.path().toUtf8();
        const QByteArray body = "#!/bin/sh\n"
            "{ for a in \"$@\"; do printf '%s\\n' \"$a\"; done; echo '--end--'; } >> '" + log.toUtf8() + "'\n"
            "p=tf\n"
            "for a in \"$@\"; do [ \"$a\" = --js-runtimes ] && p=js; done\n"
            "[ -f '" + d + "'/$p.err ] && cat '" + d + "'/$p.err >&2\n"
            "[ -f '" + d + "'/$p.out ] && cat '" + d + "'/$p.out\n"
            "exit $(cat '" + d + "'/$p.code 2>/dev/null || echo 0)\n";
        f.write(body);
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        ok = true;
    }
    // pass: "tf" (token-free) or "js" (widened).
    void pass(const char *name, const QByteArray &out, const QByteArray &err, int code) {
        const QString base = dir.path() + QLatin1Char('/') + QLatin1String(name);
        QFile o(base + QStringLiteral(".out")), e(base + QStringLiteral(".err")), c(base + QStringLiteral(".code"));
        if (o.open(QIODevice::WriteOnly)) o.write(out);
        if (e.open(QIODevice::WriteOnly)) e.write(err);
        if (c.open(QIODevice::WriteOnly)) c.write(QByteArray::number(code));
    }
    QList<QStringList> runs() const {
        QList<QStringList> out;
        QFile f(log);
        if (!f.open(QIODevice::ReadOnly)) return out;
        QStringList current;
        for (const QByteArray &line : f.readAll().split('\n')) {
            if (line == "--end--") { out << current; current.clear(); }
            else if (!line.isEmpty()) current << QString::fromUtf8(line);
        }
        return out;
    }
};

struct ScenarioRig {
    ScenarioExtractor fake;
    video::resolver::RunnerOptions options;
    video::resolver::YtDlpRunner *runner = nullptr;
    video::resolver::YtDlpVideoService *service = nullptr;
    YtDlpStreamResolver *resolver = nullptr;
    ScenarioRig() {
        options.executablePath = fake.script;
        options.pythonChild = false;
        options.javascriptRuntime = QStringLiteral("quickjs:/opt/test/quickjs");
        runner = new video::resolver::YtDlpRunner(options);
        service = new video::resolver::YtDlpVideoService(runner);
        resolver = new YtDlpStreamResolver(service, nullptr, player::CachePolicy(), fixtureClock());
    }
    ~ScenarioRig() { delete resolver; delete service; delete runner; }
    Answer resolve() {
        Answers got;
        resolver->resolve(videoRequest(), into(&got));
        waitUntil([&]() { return !got.empty(); });
        CHECK(got.size() == 1);
        return got.empty() ? Answer() : got.front();
    }
};

static const QByteArray kTokenFreeClients = "youtube:skip=dash;player_client=android_vr,visionos";

static QByteArray noUrlJson(const char *liveStatus = "not_live", const char *extra = "") {
    return QByteArray("{\"id\":\"aBcDeFgHiJk\",\"title\":\"T\",\"live_status\":\"") + liveStatus + "\"" + extra +
           ",\"formats\":[{\"format_id\":\"251\",\"ext\":\"webm\",\"protocol\":\"https\",\"vcodec\":\"none\","
           "\"acodec\":\"opus\"}]}\n";
}

// Cause 1 (RES-08/09): a bare "Video unavailable" from the token-free clients is not final: the
// widened pass runs (on yt-dlp's default clients) and plays; when it says the same, the error
// stays non-final so that the session's one re-resolve is allowed.
static void res09_bareUnavailableWidens() {
    {
        ScenarioRig rig;
        CHECK(rig.fake.ok);
        rig.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
        rig.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
        const Answer a = rig.resolve();
        CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
        const QList<QStringList> runs = rig.fake.runs();
        CHECK(runs.size() == 2);
        if (runs.size() == 2) {
            CHECK(runs[0].contains(QString::fromLatin1(kTokenFreeClients)) && !runs[0].contains(QStringLiteral("--js-runtimes")));
            CHECK(runs[1].contains(QStringLiteral("youtube:skip=dash")) && runs[1].contains(QStringLiteral("--js-runtimes")));
            CHECK(!runs[1].join(QLatin1Char(' ')).contains(QLatin1String("player_client")));
        }
    }
    ScenarioRig both;
    both.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
    both.fake.pass("js", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable. This video is not available\n", 1);
    const Answer b = both.resolve();
    CHECK(b.error.kind == StreamErrorClass::Unavailable && !b.error.final);
    CHECK(both.fake.runs().size() == 2);
}

// RES-09: removed, terminated, country-blocked, members-only (signed out) and upcoming are final:
// one run, no widening.
static void res09_finalReasonsOneRun() {
    struct Case { QByteArray out, err; int code; StreamErrorClass error; };
    const Case cases[] = {
        {QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable. This video has been removed by the uploader\n", 1,
         StreamErrorClass::Removed},
        {QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable. This video is no longer available because the YouTube "
         "account associated with this video has been terminated.\n", 1, StreamErrorClass::Terminated},
        {QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable. The uploader has not made this video available in your "
         "country\n", 1, StreamErrorClass::GeoBlocked},
        // --ignore-no-formats-error: the reason is a warning beside metadata without formats.
        {noUrlJson(), "WARNING: [youtube] aBcDeFgHiJk: Join this channel to get access to members-only content like this video\n",
         0, StreamErrorClass::SignInRequired},
    };
    for (const Case &c : cases) {
        ScenarioRig rig;
        rig.fake.pass("tf", c.out, c.err, c.code);
        rig.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
        const Answer a = rig.resolve();
        CHECK(a.error.kind == c.error && a.error.final);
        CHECK(rig.fake.runs().size() == 1);
    }
}

// RES-09: nameFinal (the reason of a fast-path Final) is one token-free run whatever it answers:
// a bare "unavailable" is not widened there, and a named reason comes back as it is.
static void res09_nameFinalSinglePass() {
    {
        ScenarioRig rig;
        rig.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
        rig.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
        Answers got;
        rig.resolver->nameFinal(videoRequest(), into(&got));
        CHECK(waitUntil([&]() { return !got.empty(); }));
        CHECK(got.size() == 1 && got[0].error.kind == StreamErrorClass::Unavailable && !got[0].error.final);
        const QList<QStringList> runs = rig.fake.runs();
        CHECK(runs.size() == 1 && !runs[0].contains(QStringLiteral("--js-runtimes")));
        // A resolve of the same request afterwards is its own flight and widens as usual.
        const Answer a = rig.resolve();
        CHECK(a.error.ok() && rig.fake.runs().size() == 3);
    }
    ScenarioRig named;
    named.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable. The uploader has not made this video "
                    "available in your country\n", 1);
    Answers got;
    named.resolver->nameFinal(videoRequest(), into(&got));
    CHECK(waitUntil([&]() { return !got.empty(); }));
    CHECK(got.size() == 1 && got[0].error.kind == StreamErrorClass::GeoBlocked && got[0].error.final);
    CHECK(named.fake.runs().size() == 1);
}

// RES-19: an upcoming answer names its scheduled start (release_timestamp) on the failed set.
static void res19_upcomingStartReachesSession() {
    ScenarioRig rig;
    rig.fake.pass("tf", noUrlJson("is_upcoming", ",\"release_timestamp\":1790000000"),
                  "WARNING: [youtube] aBcDeFgHiJk: This live event will begin in 3 days.\n", 0);
    const Answer a = rig.resolve();
    CHECK(a.error.kind == StreamErrorClass::Upcoming && a.error.final);
    CHECK(a.streams.liveState == playback::LiveState::Upcoming && a.streams.scheduledStartUnixMs == 1790000000LL * 1000);
    CHECK(rig.fake.runs().size() == 1);
    // The service's report as the adapter reads it.
    bool done = false;
    video::resolver::YtDlpVideoService::DetailsReport report;
    rig.service->detailsReport(QString::fromLatin1(fixtureId), false, video::resolver::YtDlpVideoService::DetailsOptions(),
        [&](const video::resolver::YtDlpVideoService::DetailsReport &r, const video::catalog::VideoDetails &) {
            report = r;
            done = true;
        });
    CHECK(waitUntil([&]() { return done; }));
    CHECK(report.error == video::resolver::ErrorCode::Upcoming && report.reason == video::resolver::FailureReason::Upcoming);
    CHECK(report.scheduledStartUnix == 1790000000);
}

// Scrub preview: the storyboard level of a yt-dlp answer reaches the stream set, and stays with
// the cached answer.
static void sb_storyboardReachesTheStreamSet() {
    ScenarioRig rig;
    QByteArray json = fixture("details_vod.json");
    const QByteArray key = "\"formats\": [";
    const int at = json.indexOf(key);
    CHECK(at > 0);
    json.insert(at + key.size(),
                "{\"format_id\":\"sb1\",\"format_note\":\"storyboard\",\"ext\":\"mhtml\",\"protocol\":\"mhtml\",\"vcodec\":\"none\","
                "\"acodec\":\"none\",\"url\":\"https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M$M.jpg?sigh=y\",\"width\":160,"
                "\"height\":90,\"columns\":5,\"rows\":5,\"fragments\":["
                "{\"url\":\"https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M0.jpg?sigh=y\",\"duration\":200.0},"
                "{\"url\":\"https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M1.jpg?sigh=y\",\"duration\":12.0}]}, ");
    rig.fake.pass("tf", json, QByteArray(), 0);
    const Answer a = rig.resolve();
    CHECK(a.error.ok());
    const playback::StreamStoryboard &board = a.streams.storyboard;
    CHECK(!board.isEmpty() && board.width == 160 && board.height == 90 && board.columns == 5 && board.rows == 5);
    CHECK(board.sheets.size() == 2);
    CHECK(board.sheets.size() == 2 && board.sheets[0].durationMs == 200000 && board.sheets[1].durationMs == 12000);
    CHECK(board.sheets.size() == 2 && board.sheets[1].url.host() == QLatin1String("i.ytimg.com")
          && board.sheets[1].url.path() == QLatin1String("/sb/aBcDeFgHiJk/storyboard3_L2/M1.jpg"));
    const Answer cached = rig.resolve();
    CHECK(cached.error.ok() && cached.streams.storyboard.sheets.size() == 2 && rig.fake.runs().size() == 1);
    // An answer without storyboard formats carries none.
    ScenarioRig plain;
    plain.fake.pass("tf", fixture("details_vod.json"), QByteArray(), 0);
    CHECK(plain.resolve().streams.storyboard.isEmpty());
    CHECK(YtDlpStreamResolver::storyboardOf(video::resolver::StoryboardLevel()).isEmpty());
}

// Cause 4 (RES-09): HTTP 429 is TryLater, not final and not widened (the next pass meets the same
// limit); the session makes the one delayed retry.
static void res09_rateLimitedTryLater() {
    ScenarioRig rig;
    rig.fake.pass("tf", QByteArray(), "WARNING: [youtube] aBcDeFgHiJk: Unable to download webpage: HTTP Error 429: Too Many "
                  "Requests\nERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
    const Answer a = rig.resolve();
    CHECK(a.error.kind == StreamErrorClass::TryLater && !a.error.final);
    CHECK(rig.fake.runs().size() == 1);
}

// Cause 3: the runtime's temporary-file budget is a retryable extraction failure, never the
// "reinstall" RuntimeUnavailable.
static void res09_storageLimitRetryable() {
    ScenarioRig rig;
    rig.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
    rig.fake.pass("js", QByteArray(), "ERROR: packaged resolver temporary storage limit exceeded\n", 2);
    const Answer a = rig.resolve();
    CHECK(a.error.kind == StreamErrorClass::Failed && !a.error.final);
    CHECK(rig.fake.runs().size() == 2);
}

// Cause 6: formats without URLs (SABR-only) or a failed challenge: DeliveryChanged after the
// widened pass ran; when the widened pass finds URLs, it plays.
static void res08_deliveryChangedWidens() {
    {
        ScenarioRig rig;
        rig.fake.pass("tf", noUrlJson(), "WARNING: [youtube] aBcDeFgHiJk: Some android_vr client https formats have been skipped "
                      "as they are missing a URL. YouTube may have enabled the SABR-only streaming experiment\n", 0);
        rig.fake.pass("js", noUrlJson(), "WARNING: [youtube] aBcDeFgHiJk: n challenge solving failed: Some formats may be missing.\n", 0);
        const Answer a = rig.resolve();
        CHECK(a.error.kind == StreamErrorClass::DeliveryChanged && !a.error.final);
        CHECK(rig.fake.runs().size() == 2);
    }
    {
        ScenarioRig ok;
        ok.fake.pass("tf", noUrlJson(), QByteArray(), 0);
        ok.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
        const Answer b = ok.resolve();
        CHECK(b.error.ok() && b.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
    }
    // android_vr's https formats are skipped without a GVS PO token: the same class.
    {
        ScenarioRig pot;
        const QByteArray none = "{\"id\":\"aBcDeFgHiJk\",\"title\":\"T\",\"live_status\":\"not_live\",\"formats\":[]}\n";
        const QByteArray warning = "WARNING: [youtube] aBcDeFgHiJk: android_vr client https formats require a GVS PO Token which "
                                   "was not provided. They will be skipped as they may yield HTTP Error 403.\n";
        pot.fake.pass("tf", none, warning, 0);
        pot.fake.pass("js", none, warning, 0);
        const Answer c = pot.resolve();
        CHECK(c.error.kind == StreamErrorClass::DeliveryChanged && !c.error.final && pot.fake.runs().size() == 2);
    }
    // No formats and no reason at all (the app clients' wording differs, or nothing is said): no
    // more than a bare, non-final "unavailable"; never "the delivery changed".
    ScenarioRig silent;
    const QByteArray none = "{\"id\":\"aBcDeFgHiJk\",\"title\":\"T\",\"live_status\":\"not_live\",\"formats\":[]}\n";
    silent.fake.pass("tf", none, "WARNING: [youtube] aBcDeFgHiJk: This video is unavailable\n", 0);
    silent.fake.pass("js", none, QByteArray(), 0);
    const Answer d = silent.resolve();
    CHECK(d.error.kind == StreamErrorClass::Unavailable && !d.error.final && silent.fake.runs().size() == 2);
}

static bool mentionsCredentials(const QStringList &args) {
    for (const QString &a : args) {
        const QString l = a.toLower();
        if (l.contains(QLatin1String("cookie")) || l.contains(QLatin1String("netrc")) || l.contains(QLatin1String("username"))
                || l.contains(QLatin1String("password")) || l.contains(QLatin1String("add-header"))
                || l.contains(QLatin1String("authorization")) || l.contains(QLatin1String("sapisid")))
            return true;
    }
    return false;
}

// RES-20: no resolve carries cookies or credentials, for either extraction profile, even with
// the request marked signed in.
static void res20_noCookiesInResolveArguments() {
    video::resolver::RunnerOptions o;
    o.executablePath = QStringLiteral("/nonexistent");
    o.javascriptRuntime = QStringLiteral("quickjs:/opt/test/quickjs");
    video::resolver::YtDlpRunner runner(o);
    for (bool js : {false, true}) {
        const QStringList args = video::resolver::YtDlpVideoService::detailsArguments(
            runner.commonArguments(js), QString::fromLatin1(fixtureId));
        CHECK(!mentionsCredentials(args));
        CHECK(args.contains(QStringLiteral("--ignore-config"))); // no user config can add cookies
        CHECK(args.contains(QStringLiteral("--js-runtimes")) == js);
    }

    Rig rig;
    CHECK(rig.fake.ok);
    StreamRequest r = videoRequest("zzzzzzzzzzz");
    r.signedIn = true;
    r.poProvider = true;
    Answers got;
    rig.resolver->resolve(r, into(&got)); // both profiles run
    CHECK(waitUntil([&]() { return !got.empty(); }));
    const QList<QStringList> runs = rig.fake.runs();
    CHECK(runs.size() == 2);
    for (const QStringList &args : runs) CHECK(!mentionsCredentials(args));
}

// Answers are tagged with the requested id: an extraction answering for another video is a
// failure, never that video's streams. A cancelled operation completes once, as Cancelled.
static void answerTagging_otherIdIgnored() {
    Rig rig;
    Answers got;
    const quint64 op = rig.resolver->resolve(videoRequest("zzzzzzzzzzz"), into(&got));
    CHECK(op != 0);
    CHECK(waitUntil([&]() { return !got.empty(); }));
    settle(100);
    CHECK(got.size() == 1);
    if (!got.empty()) {
        CHECK(got[0].streams.videoId == QLatin1String("zzzzzzzzzzz"));
        CHECK(got[0].error.kind == StreamErrorClass::InvalidResponse && !got[0].error.final);
        CHECK(got[0].streams.video.isEmpty() && got[0].streams.audio.isEmpty() && got[0].streams.muxed.isEmpty());
    }

    // A cancelled operation gets exactly one Cancelled completion, never the late answer; the
    // next request gets its own answer.
    Answers first, second;
    const quint64 a = rig.resolver->resolve(videoRequest(), into(&first));
    rig.resolver->cancel(a);
    rig.resolver->cancel(a); // twice: still once
    CHECK(first.empty());    // never synchronous
    StreamRequest other = videoRequest();
    other.maxHeight = 720;
    rig.resolver->resolve(other, into(&second));
    CHECK(waitUntil([&]() { return !second.empty(); }));
    settle(600);
    CHECK(first.size() == 1 && first[0].error.kind == StreamErrorClass::Cancelled && first[0].streams.audio.isEmpty());
    CHECK(second.size() == 1 && second[0].error.ok() && second[0].streams.videoId == QLatin1String(fixtureId));
    CHECK(!second.empty() && ids(second[0].streams.muxed).front() == "18"); // its own request (ceiling 720)

    // Invalid ids answer asynchronously with a final error and start no run.
    const int before = rig.fake.runs().size();
    Answers bad;
    rig.resolver->resolve(videoRequest("not-an-id"), into(&bad));
    CHECK(bad.empty());
    CHECK(waitUntil([&]() { return !bad.empty(); }));
    CHECK(!bad.empty() && bad[0].error.kind == StreamErrorClass::InvalidId && bad[0].error.final);
    CHECK(rig.fake.runs().size() == before);
}

// RES-10: single flight per key, cache hit with path Cache, invalidate, distinct keys.
static void res10_singleFlightAndCache() {
    Rig rig;
    Answers got;
    rig.resolver->resolve(videoRequest(), into(&got));
    rig.resolver->resolve(videoRequest(), into(&got));
    CHECK(waitUntil([&]() { return got.size() == 2; }));
    CHECK(rig.fake.runs().size() == 1);
    for (const Answer &r : got) {
        CHECK(r.error.ok() && r.streams.path == playback::ResolvePath::Extractor && r.streams.resolveMs >= 0);
        CHECK(r.streams.video.size() == 2 && r.streams.audio.size() == 4 && r.streams.muxed.size() == 2);
    }

    got.clear();
    rig.resolver->resolve(videoRequest(), into(&got));
    CHECK(got.empty()); // never synchronous, even from the cache
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(!got.empty() && got[0].streams.path == playback::ResolvePath::Cache && got[0].streams.audio.size() == 4
          && got[0].streams.resolveMs == 0);
    CHECK(rig.fake.runs().size() == 1);

    // A prefetch-flagged request shares the answer (speculative is not in the key).
    StreamRequest speculative = videoRequest();
    speculative.speculative = true;
    got.clear();
    rig.resolver->resolve(speculative, into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(!got.empty() && got[0].streams.path == playback::ResolvePath::Cache);

    StreamRequest signedIn = videoRequest(); // another key: resolves again
    signedIn.signedIn = true;
    got.clear();
    rig.resolver->resolve(signedIn, into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(rig.fake.runs().size() == 2);

    rig.resolver->invalidate(QString::fromLatin1(fixtureId)); // byte-0 refusal: every entry for the video goes
    got.clear();
    rig.resolver->resolve(videoRequest(), into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(!got.empty() && got[0].streams.path == playback::ResolvePath::Extractor);
    CHECK(rig.fake.runs().size() == 3);

    // peekCached reads the cache: the answer for a cached key, nothing for another key or video;
    // no run, no hit counted, nothing changed.
    StreamSet peeked;
    CHECK(rig.resolver->peekCached(videoRequest(), &peeked));
    CHECK(peeked.videoId == QLatin1String(fixtureId) && peeked.path == playback::ResolvePath::Cache && peeked.resolveMs == 0
          && peeked.audio.size() == 4 && peeked.phases.requestMs == -1);
    CHECK(rig.resolver->peekCached(speculative, nullptr));   // the same key
    CHECK(!rig.resolver->peekCached(signedIn, nullptr));     // its answer went with the invalidate above
    StreamRequest uncached = videoRequest();
    uncached.maxHeight = 144;
    CHECK(!rig.resolver->peekCached(uncached, &peeked) && !rig.resolver->peekCached(videoRequest("zzzzzzzzzzz"), nullptr));
    CHECK(rig.fake.runs().size() == 3 && number(rig.resolver->diagnostics(), "cacheHits") == 2);

    // reportRefused (RES-07): the cached answers go, and the next extraction of the video starts
    // with the full profile (res07_refusedVideoStartsWithTheFullProfile).
    playback::StreamFormat refused;
    rig.resolver->reportRefused(QString::fromLatin1(fixtureId), refused);
    CHECK(!rig.resolver->peekCached(videoRequest(), nullptr));
    got.clear();
    rig.resolver->resolve(videoRequest(), into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(rig.fake.runs().size() == 4);
    CHECK(!got.empty() && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::ExtractorWithJavaScript);

    const QVariantMap d = rig.resolver->diagnostics();
    CHECK(number(d, "extractor") == 3 && number(d, "extractorWithJavaScript") == 1 && number(d, "cacheHits") == 2
          && number(d, "failed") == 0);
    CHECK(number(d, "startedFull") == 1 && number(d, "fullProfileVideos") == 1);
    CHECK(number(d, "medianExtractorMs") >= 0 && d.contains(QStringLiteral("cacheEntries")));
    for (auto it = d.constBegin(); it != d.constEnd(); ++it) {
        bool isNumber = false;
        it.value().toLongLong(&isNumber);
        CHECK(isNumber); // fixed keys, numbers only
    }

    // Failures are never cached: the mismatched id resolves every time. The first time both
    // passes run; the token-free pass that just failed is not tried first again (one pass).
    got.clear();
    rig.resolver->resolve(videoRequest("zzzzzzzzzzz"), into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    rig.resolver->resolve(videoRequest("zzzzzzzzzzz"), into(&got));
    CHECK(waitUntil([&]() { return got.size() == 2; }));
    CHECK(rig.fake.runs().size() == 7);
    CHECK(got.size() == 2 && got[1].error.kind == StreamErrorClass::InvalidResponse);
}

static bool withJavaScript(const QStringList &run) { return run.contains(QStringLiteral("--js-runtimes")); }

// RES-07 / RES-12, SailTune's strike rules on the extractor: a refused source (reportRefused, or
// reportFault with Rejected) makes the video's next extractions start with the full profile for
// five minutes; a lost connection, a stall, an abandoned opening, an unexplained failure and a
// plain invalidate only drop the cached answers; nameFinal stays the token-free pass.
static void res07_refusedVideoStartsWithTheFullProfile() {
    clockShiftMs = 0;
    CHECK(YtDlpStreamResolver::fullProfileMs == 5 * 60 * 1000);
    ScenarioRig rig;
    rig.fake.pass("tf", fixture("details_vod.json"), QByteArray(), 0);
    rig.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
    const QString id = QString::fromLatin1(fixtureId);
    Answer a = rig.resolve();
    CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::Extractor);
    // No verdict on the source: the cache is dropped, the token-free pass stays first.
    for (player::SourceFault fault : {player::SourceFault::Connection, player::SourceFault::Stalled,
                                      player::SourceFault::Abandoned, player::SourceFault::Unspecified}) {
        player::SourceFaultSink *sink = rig.resolver;
        sink->reportFault(fixtureId, fault);
        CHECK(!rig.resolver->peekCached(videoRequest(), nullptr));
        a = rig.resolve();
        CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::Extractor);
    }
    rig.resolver->invalidate(id);
    a = rig.resolve();
    CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::Extractor);
    QList<QStringList> runs = rig.fake.runs();
    CHECK(runs.size() == 6);
    for (const QStringList &run : runs) CHECK(!withJavaScript(run));
    CHECK(number(rig.resolver->diagnostics(), "startedFull") == 0 && number(rig.resolver->diagnostics(), "fullProfileVideos") == 0);

    // Refused: the cached answer goes and the full profile runs first, for every request of the video.
    rig.resolver->reportRefused(id, playback::StreamFormat());
    a = rig.resolve();
    CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
    StreamRequest other = videoRequest();
    other.maxHeight = 720;
    Answers got;
    rig.resolver->resolve(other, into(&got));
    CHECK(waitUntil([&]() { return !got.empty(); }));
    CHECK(!got.empty() && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::ExtractorWithJavaScript);
    runs = rig.fake.runs();
    CHECK(runs.size() == 8 && withJavaScript(runs[6]) && withJavaScript(runs[7]));
    CHECK(number(rig.resolver->diagnostics(), "startedFull") == 2 && number(rig.resolver->diagnostics(), "fullProfileVideos") == 1);
    // A prefetch of the video starts with it too; nameFinal never does (RES-09: the token-free pass only).
    StreamRequest ahead = videoRequest();
    ahead.maxHeight = 480;
    ahead.speculative = true;
    rig.resolver->prefetch(ahead);
    CHECK(waitUntil([&]() { return rig.resolver->peekCached(ahead, nullptr); }));
    Answers named;
    rig.resolver->nameFinal(videoRequest(), into(&named));
    CHECK(waitUntil([&]() { return !named.empty(); }));
    runs = rig.fake.runs();
    CHECK(runs.size() == 10 && withJavaScript(runs[8]) && !withJavaScript(runs[9]));

    // Five minutes later the token-free pass is first again.
    clockShiftMs = YtDlpStreamResolver::fullProfileMs + 2000;
    rig.resolver->invalidate(id);
    a = rig.resolve();
    CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::Extractor);
    CHECK(number(rig.resolver->diagnostics(), "fullProfileVideos") == 0);
    // The typed report of a refusal does the same as reportRefused.
    rig.resolver->reportFault(fixtureId, player::SourceFault::Rejected);
    a = rig.resolve();
    CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
    runs = rig.fake.runs();
    CHECK(runs.size() == 12 && !withJavaScript(runs[10]) && withJavaScript(runs[11]));
    // Not a video id: nothing is remembered.
    rig.resolver->reportFault("nope", player::SourceFault::Rejected);
    CHECK(number(rig.resolver->diagnostics(), "fullProfileVideos") == 1);
    clockShiftMs = 0;
}

// SailTune's "a failed extraction makes the next one use the full profile": a token-free pass that
// answered without anything playable is not tried first again for five minutes; one that got no
// answer at all (no network) is, because that says nothing about the clients.
static void res07_failedTokenFreePassIsNotTriedFirstAgain() {
    clockShiftMs = 0;
    const QString id = QString::fromLatin1(fixtureId);
    {
        ScenarioRig rig;
        rig.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Video unavailable\n", 1);
        rig.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
        Answer a = rig.resolve();
        CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::ExtractorWithJavaScript && rig.fake.runs().size() == 2);
        rig.resolver->invalidate(id);                  // a retry, a recovery
        a = rig.resolve();
        CHECK(a.error.ok() && a.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
        QList<QStringList> runs = rig.fake.runs();
        CHECK(runs.size() == 3 && !withJavaScript(runs[0]) && withJavaScript(runs[1]) && withJavaScript(runs[2]));
        CHECK(number(rig.resolver->diagnostics(), "startedFull") == 1);
        clockShiftMs = YtDlpStreamResolver::fullProfileMs + 2000;
        rig.resolver->invalidate(id);
        a = rig.resolve();
        runs = rig.fake.runs();
        CHECK(runs.size() == 5 && !withJavaScript(runs[3]) && withJavaScript(runs[4]));
        clockShiftMs = 0;
    }
    ScenarioRig offline;
    offline.fake.pass("tf", QByteArray(), "ERROR: [youtube] aBcDeFgHiJk: Unable to download API page: <urlopen error [Errno -3] "
                      "Temporary failure in name resolution>\n", 1);
    offline.fake.pass("js", fixture("details_vod.json"), QByteArray(), 0);
    Answer b = offline.resolve();
    CHECK(b.error.ok() && offline.fake.runs().size() == 2);
    offline.resolver->invalidate(id);
    b = offline.resolve();
    const QList<QStringList> runs = offline.fake.runs();
    CHECK(runs.size() == 4 && !withJavaScript(runs[2]) && withJavaScript(runs[3]));
    CHECK(number(offline.resolver->diagnostics(), "startedFull") == 0 && number(offline.resolver->diagnostics(), "fullProfileVideos") == 0);
}

// RES-10 bounded join: a joiner that waits longer than the bound gets a transient Timeout; the
// leader still gets the answer.
static void res10_boundedJoin() {
    player::CachePolicy p;
    p.joinLimitMs = 100;
    Rig rig(p);
    Answers lead, join;
    rig.resolver->resolve(videoRequest(), into(&lead));
    rig.resolver->resolve(videoRequest(), into(&join));
    CHECK(waitUntil([&]() { return !lead.empty() && !join.empty(); }));
    CHECK(!join.empty() && join[0].error.kind == StreamErrorClass::Timeout && !join[0].error.final);
    CHECK(!lead.empty() && lead[0].error.ok());
    CHECK(rig.fake.runs().size() == 1);
}

// RES-11: at most two speculative resolves; the newest replaces the oldest.
static void res11_prefetchCap() {
    Rig rig;
    StreamRequest a = videoRequest(), b = videoRequest(), c = videoRequest();
    a.maxHeight = 360; b.maxHeight = 480; c.maxHeight = 720;
    a.speculative = b.speculative = c.speculative = true;
    rig.resolver->prefetch(a);
    rig.resolver->prefetch(b);
    rig.resolver->prefetch(c); // cancels a
    settle(1500);
    Answers got;
    rig.resolver->resolve(b, into(&got));
    rig.resolver->resolve(c, into(&got));
    CHECK(waitUntil([&]() { return got.size() == 2; }));
    for (const Answer &r : got) CHECK(r.streams.path == playback::ResolvePath::Cache);
    got.clear();
    rig.resolver->resolve(a, into(&got));
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    CHECK(!got.empty() && got[0].error.ok() && got[0].streams.path == playback::ResolvePath::Extractor);
    // A prefetch for an answer already cached starts nothing.
    const int before = rig.fake.runs().size();
    rig.resolver->prefetch(b);
    settle(100);
    CHECK(rig.fake.runs().size() == before);
}

// Cancelling the only waiter stops the run; a joiner keeps a run alive when its leader cancels.
static void cancelSemantics() {
    Rig rig;
    Answers got, cancelled;
    const quint64 only = rig.resolver->resolve(videoRequest(), into(&cancelled));
    rig.resolver->cancel(only);
    settle(800);
    CHECK(cancelled.size() == 1 && cancelled[0].error.kind == StreamErrorClass::Cancelled);
    cancelled.clear();
    const quint64 lead = rig.resolver->resolve(videoRequest(), into(&cancelled));
    rig.resolver->resolve(videoRequest(), into(&got));
    rig.resolver->cancel(lead);
    CHECK(waitUntil([&]() { return got.size() == 1; }));
    settle(100);
    CHECK(got.size() == 1 && got[0].error.ok());
    CHECK(cancelled.size() == 1 && cancelled[0].error.kind == StreamErrorClass::Cancelled);
    CHECK(number(rig.resolver->diagnostics(), "cancelled") == 2);
}

// No completion after the resolver is destroyed.
static void noCompletionAfterDestruction() {
    Answers got;
    {
        Rig rig;
        rig.resolver->resolve(videoRequest(), into(&got));
        rig.resolver->resolve(videoRequest("not-an-id"), into(&got)); // queued answer
    }
    settle(800);
    CHECK(got.empty());
}

// Direct fetcher: remote URL, asynchronous, exactly once; closed before delivery -> Cancelled.
static void directFetcher() {
    video::playeradapter::DirectStreamFetcher fetcher;
    playback::StreamFormat f;
    f.formatId = QStringLiteral("137");
    f.url = QUrl(QStringLiteral("https://media.example/137"));
    f.headers << qMakePair(QByteArray("User-Agent"), QByteArray("Mozilla/5.0 (test)"));
    std::vector<std::pair<QUrl, StreamError>> got, early;
    const auto collect = [](std::vector<std::pair<QUrl, StreamError>> *list) {
        return [list](const QUrl &u, const StreamError &e) { list->push_back(std::make_pair(u, e)); };
    };
    fetcher.open(f, collect(&got));
    const quint64 h2 = fetcher.open(f, collect(&early));
    fetcher.close(h2); // closed before delivery
    CHECK(got.empty() && early.empty());
    CHECK(waitUntil([&]() { return !got.empty() && !early.empty(); }));
    settle(50);
    CHECK(got.size() == 1 && got[0].first == f.url && got[0].second.ok());
    CHECK(early.size() == 1 && early[0].first.isEmpty() && early[0].second.kind == StreamErrorClass::Cancelled);
    playback::StreamFormat plain = f;
    plain.url = QUrl(QStringLiteral("http://media.example/137"));
    std::vector<std::pair<QUrl, StreamError>> bad;
    fetcher.open(plain, collect(&bad));
    CHECK(waitUntil([&]() { return !bad.empty(); }));
    CHECK(!bad.empty() && bad[0].second.kind == StreamErrorClass::InvalidResponse);
    const QVariantMap d = fetcher.diagnostics();
    CHECK(number(d, "opened") == 2 && number(d, "refused") == 1 && number(d, "open") == 1);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    mapping_fixtureToStreamSet();
    res22_invalidUrlsDropped();
    res09_coreCodesMapped();
    res09_reasonsMapped();
    res09_bareUnavailableWidens();
    res09_finalReasonsOneRun();
    res09_nameFinalSinglePass();
    res19_upcomingStartReachesSession();
    sb_storyboardReachesTheStreamSet();
    res09_rateLimitedTryLater();
    res09_storageLimitRetryable();
    res08_deliveryChangedWidens();
    res20_noCookiesInResolveArguments();
    answerTagging_otherIdIgnored();
    res10_singleFlightAndCache();
    res10_boundedJoin();
    res07_refusedVideoStartsWithTheFullProfile();
    res07_failedTokenFreePassIsNotTriedFirstAgain();
    res11_prefetchCap();
    cancelSemantics();
    noCompletionAfterDestruction();
    directFetcher();
    std::printf("shared playback tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
