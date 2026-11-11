// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-ROB-01 fault injection, resolve side: the real SharedStreamSource adapter
// (Core/Player/Infrastructure/SharedStreamSource.h, the Shared/Playback/StreamSource.h producer)
// with a scripted fast path of the component's shape, falling back to the real
// YtDlpStreamResolver -> YtDlpVideoService -> YtDlpRunner chain over a fake extractor executable
// (a shell script written by the test, serving synthetic fixtures). Also the component's real
// player-answer parser (Shared/SailTune/.../PlayerResponseParser, read-only) on a synthetic answer.
// Cases: every fast-path failure that allows it reaches the extractor; a final one never falls
// back (a named Final never reaches the extractor; an unnamed one is only named, by at most one
// extractor run, and never retried); a bare "video unavailable" from the extractor is not final
// (the full-profile pass runs) while a clearly final reason is, in a single run; rate limiting is
// TryLater; extractor failures end in fixed error classes and labels (never provider text); one
// bad format in a list never drops the healthy ones. Build: tests/fault_injection/run.sh
#include "tests/fault_injection/FaultSupport.h"

#include "Core/Player/Infrastructure/SharedStreamSourceSailTune.h"
#include "Core/Player/Infrastructure/YtDlpStreamResolver.h"
#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"
#include "Shared/SailTune/Core/SharedPlayback/Infrastructure/PlayerResponseParser.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <map>
#include <memory>

using namespace nordtile;
namespace sp = nordtile::music::sharedplayback;
using playback::StreamError;
using playback::StreamErrorClass;
using playback::StreamRequest;
using playback::StreamSet;
using video::playeradapter::YtDlpStreamResolver;

namespace {

const char *const kVideo = "fiResolve01";
const std::int64_t kFarExpiryMs = 4102444800LL * 1000;

// ---- the fast path, in the shape SharedStreamSource expects of Api::Resolver ----------------

class FakeFast : public QObject {
public:
    typedef unsigned long long RequestId;
    typedef std::function<void(const sp::domain::ResolveOutcome &)> Callback;
    struct Counters {
        int fast = 0, cache = 0, fallbacks = 0, botWalls = 0, probeRejected = 0, network = 0;
        int medianTotalMs() const { return -1; }
    };
    enum class ProbeVerdict { Accepted, NoVerdict, Refused, Unusable };
    typedef std::function<void(ProbeVerdict, int, int, const std::string &)> ProbeDone;

    std::function<sp::domain::ResolveOutcome()> answer;
    int resolves = 0;
    std::vector<std::string> invalidated;

    void warmUp() {}
    RequestId resolve(const std::string &, const sp::domain::StreamRequest &, const Callback &callback) {
        ++resolves;
        const RequestId id = next_++;
        const sp::domain::ResolveOutcome o = answer ? answer() : sp::domain::ResolveOutcome();
        pending_[id] = callback;
        QTimer::singleShot(0, this, [this, id, o]() { deliver(id, o); });
        return id;
    }
    void cancel(RequestId id) {
        if (!pending_.count(id)) return;
        sp::domain::ResolveOutcome o;
        o.failure = sp::domain::FailureKind::Cancelled;
        QTimer::singleShot(0, this, [this, id, o]() { deliver(id, o); });
    }
    void invalidate(const std::string &videoId) { invalidated.push_back(videoId); }
    void probeAddress(const std::string &url, const std::string &, std::int64_t, const ProbeDone &done) {
        QTimer::singleShot(1, this, [done, url]() { done(ProbeVerdict::Accepted, 206, 1, url); });
    }
    void reportRefused(const std::string &, std::int64_t) {}
    bool allowed() const { return true; }
    bool hasVisitor() const { return false; }
    const Counters &counters() const { return counters_; }
    sp::domain::CircuitBreaker &breaker() { return breaker_; }

private:
    void deliver(RequestId id, const sp::domain::ResolveOutcome &o) {
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        const Callback c = it->second;
        pending_.erase(it);
        c(o);
    }
    RequestId next_ = 1;
    std::map<RequestId, Callback> pending_;
    Counters counters_;
    sp::domain::CircuitBreaker breaker_;
};

struct TestApi : player::SailTuneSharedPlayback {
    typedef FakeFast Resolver;   // the proxy and the domain types are the pinned component's
};
typedef player::SharedStreamSource<TestApi> Source;

sp::domain::ResolveOutcome failed(sp::domain::FailureKind kind, bool fallback, const char *reason) {
    sp::domain::ResolveOutcome o;
    o.failure = kind;
    o.fallback = fallback;
    o.reason = reason;
    return o;
}

sp::domain::StreamFormat format(int itag, const char *mime, const char *codec, int height) {
    sp::domain::StreamFormat f;
    f.itag = itag;
    f.url = "https://rr1---sn-fault.googlevideo.com/videoplayback?expire=4102444800&itag=" + std::to_string(itag);
    f.mimeType = mime;
    f.codec = codec;
    f.hasAudio = height == 0;
    f.hasVideo = height > 0;
    f.bitrate = height ? height * 2000 : 128000;
    f.contentLength = 100000;
    f.height = height;
    f.width = height * 16 / 9;
    f.fps = height ? 30 : 0;
    f.audioLanguage = height ? "" : "en";
    f.expiresAtUnixMs = kFarExpiryMs;
    return f;
}

// A healthy fast answer plus `bad` formats mixed in.
sp::domain::ResolveOutcome fastAnswer(bool withVideo, bool withBad) {
    sp::domain::ResolveOutcome o;
    sp::domain::StreamSet &s = o.streams;
    s.videoId = kVideo;
    s.audio.push_back(format(251, "audio/webm", "opus", 0));
    if (withBad) s.audio.push_back(format(901, "audio/x-unknown", "zzz9", 0));   // no known family
    s.audio.push_back(format(140, "audio/mp4", "mp4a.40.2", 0));
    if (withVideo) {
        s.video.push_back(format(137, "video/mp4", "avc1.640028", 1080));
        if (withBad) s.video.push_back(format(902, "video/x-unknown", "qq99.1", 1080));
        if (withBad) {
            sp::domain::StreamFormat hls = format(903, "video/mp4", "avc1.4d401f", 720);
            hls.isHls = true;                                                 // never a direct video rung
            s.video.push_back(hls);
        }
        s.video.push_back(format(136, "video/mp4", "avc1.4d401f", 720));
    }
    s.userAgent = "Fault-UA/1.0";
    s.expiresAtUnixMs = kFarExpiryMs;
    s.path = sp::domain::ResolvePath::Fast;
    return o;
}

// ---- the extractor executable ---------------------------------------------------------------

struct Extractor {
    QTemporaryDir dir;
    QString script;
    explicit Extractor(const char *mode) {
        CHECK(dir.isValid());
        script = dir.path() + QStringLiteral("/fake-yt-dlp");
        QFile details(dir.path() + QStringLiteral("/details.json"));
        CHECK(details.open(QIODevice::WriteOnly));
        details.write(fault::fixture("ytdlp_details_mixed.json"));
        details.close();
        setMode(mode);
        const QByteArray d = dir.path().toUtf8();
        const QByteArray body = "#!/bin/sh\n"
            "d='" + d + "'\n"
            "{ for a in \"$@\"; do printf '%s\\n' \"$a\"; done; echo '--end--'; } >> \"$d/args.log\"\n"
            "case \"$(cat \"$d/mode\")\" in\n"
            "ok) cat \"$d/details.json\" ;;\n"
            "malformed) printf '%s' '{\"id\": \"fiResolve01\", \"formats\": [{\"format_id\": \"1' ;;\n"
            "botcheck) echo \"ERROR: [youtube] fiResolve01: Sign in to confirm you're not a bot. PROVIDER-TEXT-MARKER\" >&2; exit 1 ;;\n"
            "unavailable) echo 'ERROR: [youtube] fiResolve01: Video unavailable. This video has been removed. PROVIDER-TEXT-MARKER' >&2; exit 1 ;;\n"
            // A bare "unavailable" (no final reason) on every pass.
            "bare) echo 'ERROR: [youtube] fiResolve01: Video unavailable PROVIDER-TEXT-MARKER' >&2; exit 1 ;;\n"
            // The same on the fast (token-free) profile only; the full profile (--js-runtimes) answers.
            "barethenok) js=0; for a in \"$@\"; do [ \"$a\" = --js-runtimes ] && js=1; done\n"
            "  if [ $js = 1 ]; then cat \"$d/details.json\"; else echo 'ERROR: [youtube] fiResolve01: Video unavailable' >&2; exit 1; fi ;;\n"
            "geo) echo 'ERROR: [youtube] fiResolve01: Video unavailable. The uploader has not made this video available in your country PROVIDER-TEXT-MARKER' >&2; exit 1 ;;\n"
            "terminated) echo 'ERROR: [youtube] fiResolve01: Video unavailable. This video is no longer available because the YouTube account associated with this video has been terminated. PROVIDER-TEXT-MARKER' >&2; exit 1 ;;\n"
            "ratelimited) echo 'WARNING: [youtube] fiResolve01: Unable to download webpage: HTTP Error 429: Too Many Requests' >&2; "
                "echo 'ERROR: [youtube] fiResolve01: Video unavailable PROVIDER-TEXT-MARKER' >&2; exit 1 ;;\n"
            "storagelimit) echo 'ERROR: packaged resolver temporary storage limit exceeded' >&2; exit 2 ;;\n"
            // Metadata without formats and without a word about why.
            "empty) echo '{\"id\": \"fiResolve01\", \"title\": \"x\", \"formats\": []}' ;;\n"
            // The app clients' wording for a video that does not exist, beside metadata without formats.
            "appunavailable) echo '{\"id\": \"fiResolve01\", \"title\": \"x\", \"formats\": []}'; "
                "echo 'WARNING: [youtube] fiResolve01: This video is unavailable PROVIDER-TEXT-MARKER' >&2 ;;\n"
            "nourls) echo '{\"id\": \"fiResolve01\", \"title\": \"x\", \"formats\": [{\"format_id\": \"251\", \"ext\": \"webm\", \"protocol\": \"https\", \"vcodec\": \"none\", \"acodec\": \"opus\"}]}'; "
                "echo 'WARNING: [youtube] fiResolve01: Some tv client https formats have been skipped as they are missing a URL. PROVIDER-TEXT-MARKER' >&2 ;;\n"
            "upcoming) echo '{\"id\": \"fiResolve01\", \"title\": \"x\", \"live_status\": \"is_upcoming\", \"release_timestamp\": 4102444800, \"formats\": []}'; "
                "echo 'WARNING: [youtube] fiResolve01: This live event will begin in 3 days. PROVIDER-TEXT-MARKER' >&2 ;;\n"
            "network) echo 'ERROR: [youtube] fiResolve01: Unable to download API page: <urlopen error [Errno -3] Temporary failure in name resolution>' >&2; exit 1 ;;\n"
            "otherid) sed 's/fiResolve01/fiOtherVid1/g' \"$d/details.json\" ;;\n"
            "crash) kill -SEGV $$ ;;\n"
            "noformats) echo '{\"id\": \"fiResolve01\", \"title\": \"x\", \"formats\": []}'; "
                "echo \"WARNING: [youtube] fiResolve01: Sign in to confirm you're not a bot\" >&2 ;;\n"
            "sleep) exec sleep 5 ;;\n"
            "*) exit 3 ;;\n"
            "esac\n";
        QFile f(script);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(body);
        f.close();
        f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    }
    void setMode(const char *mode) {
        QFile m(dir.path() + QStringLiteral("/mode"));
        CHECK(m.open(QIODevice::WriteOnly));
        m.write(mode);
    }
    int runs() const {
        QFile f(dir.path() + QStringLiteral("/args.log"));
        if (!f.open(QIODevice::ReadOnly)) return 0;
        return f.readAll().count("--end--\n");
    }
};

// fast (scripted) -> SharedStreamSource -> YtDlpStreamResolver -> service -> runner -> script
struct Rig {
    Extractor extractor;
    video::resolver::RunnerOptions options;
    std::unique_ptr<video::resolver::YtDlpRunner> runner;
    std::unique_ptr<video::resolver::YtDlpVideoService> service;
    std::unique_ptr<YtDlpStreamResolver> ytdlp;
    FakeFast fast;
    std::unique_ptr<sp::infrastructure::MediaProxy> proxy;
    std::unique_ptr<Source> source;

    explicit Rig(const char *mode, bool runtimePresent = true) : extractor(mode) {
        options.executablePath = runtimePresent ? extractor.script : extractor.dir.path() + QStringLiteral("/missing-yt-dlp");
        options.pythonChild = false;
        options.javascriptRuntime = QStringLiteral("quickjs:/opt/fault/quickjs");
        runner.reset(new video::resolver::YtDlpRunner(options));
        service.reset(new video::resolver::YtDlpVideoService(runner.get()));
        ytdlp.reset(new YtDlpStreamResolver(service.get()));
        proxy.reset(new sp::infrastructure::MediaProxy(sp::infrastructure::MediaProxy::Options()));
        source.reset(new Source(&fast, proxy.get(), ytdlp.get()));
    }
    ~Rig() { source.reset(); ytdlp.reset(); service.reset(); runner.reset(); }

    struct Answer { bool done = false; StreamSet streams; StreamError error; };
    Answer resolve(const StreamRequest &request) {
        std::shared_ptr<Answer> a = std::make_shared<Answer>();
        source->resolve(request, [a](const StreamSet &s, const StreamError &e) { a->streams = s; a->error = e; a->done = true; });
        CHECK(catalog_test::waitFor([a] { return a->done; }, 20000));
        return *a;
    }
};

StreamRequest videoRequest() {
    StreamRequest r;
    r.videoId = QString::fromLatin1(kVideo);
    r.purpose = playback::StreamPurpose::Video;
    r.maxHeight = 1080;
    return r;
}

std::string ids(const QList<playback::StreamFormat> &list) {
    std::string out;
    for (const playback::StreamFormat &f : list) out += (out.empty() ? "" : ",") + f.formatId.toStdString();
    return out;
}

bool contains(const QList<playback::StreamFormat> &list, const char *id) {
    for (const playback::StreamFormat &f : list) if (f.formatId == QLatin1String(id)) return true;
    return false;
}

qint64 number(const QVariantMap &d, const char *key) { return d.value(QLatin1String(key)).toLongLong(); }

// The extractor's healthy answer for the fixture: only the valid formats, ranked.
void checkExtractorSet(const StreamSet &s) {
    CHECK(s.videoId == QLatin1String(kVideo));
    CHECK(s.path == playback::ResolvePath::Extractor || s.path == playback::ResolvePath::ExtractorWithJavaScript);
    CHECK_EQ(ids(s.video), std::string("137"));
    CHECK(contains(s.audio, "140") && contains(s.audio, "251"));
    for (const QList<playback::StreamFormat> *list : {&s.video, &s.audio, &s.muxed}) {
        for (const playback::StreamFormat &f : *list) {
            CHECK(f.url.scheme() == QLatin1String("https"));
            CHECK(f.url.host().endsWith(QLatin1String(".googlevideo.com")));
            CHECK(f.url.userInfo().isEmpty());
            for (const char *bad : {"nourl", "plainhttp", "foreignhost", "userinfo", "weirdcodec", "sb0"})
                CHECK(f.formatId != QLatin1String(bad));
        }
    }
}

// ---- cases ----------------------------------------------------------------------------------

// A fast-path failure that allows a fallback ends in the extractor's healthy answer.
void fastFailureFallsBack(sp::domain::FailureKind kind, const char *reason) {
    Rig rig("ok");
    rig.fast.answer = [kind, reason] { return failed(kind, true, reason); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.ok());
    checkExtractorSet(a.streams);
    CHECK_EQ(rig.fast.resolves, 1);
    CHECK_EQ(rig.extractor.runs(), 1);
    const QVariantMap d = rig.source->diagnostics();
    CHECK_EQ(number(d, "fallbacks"), qint64(1));
    CHECK_EQ(number(d, "extractorAnswers"), qint64(1));
}

void fastUnpairedFallsBack() {
    Rig rig("ok");
    rig.fast.answer = [] { return fastAnswer(false, false); };   // audio only for a video request
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.ok());
    checkExtractorSet(a.streams);
    CHECK_EQ(number(rig.source->diagnostics(), "fastUnpaired"), qint64(1));
}

// RES-09: a Final whose reason label names the case (S-14) never reaches the extractor, even a
// healthy one: no retry, widening or fallback.
void fastFinalNeverFallsBack() {
    Rig rig("ok");
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Final, false, "removed"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.kind == StreamErrorClass::Removed);
    CHECK(a.error.final);
    CHECK_EQ(a.error.label, QStringLiteral("removed"));
    CHECK(a.streams.video.isEmpty() && a.streams.audio.isEmpty());
    CHECK_EQ(rig.fast.resolves, 1);
    CHECK_EQ(rig.extractor.runs(), 0);
    const QVariantMap d = rig.source->diagnostics();
    CHECK_EQ(number(d, "finalFailures"), qint64(1));
    CHECK_EQ(number(d, "finalLookups"), qint64(0));
    CHECK_EQ(number(d, "fallbacks"), qint64(0));
}

// RES-09: the pinned component's unnamed "final" is named by at most one extractor run (the fast
// profile, never the full one) and is final whatever that run says: the fast path is not asked
// again and nothing is retried. `mode` is what the extractor answers.
void fastFinalNamedByOneRun(const char *mode, StreamErrorClass expected) {
    Rig rig(mode);
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Final, false, "final"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.kind == expected);
    CHECK(a.error.final);
    CHECK_EQ(a.error.label, QString::fromLatin1(playback::errorLabel(expected)));
    CHECK(!a.error.label.contains(QLatin1String("PROVIDER-TEXT-MARKER")));
    CHECK(a.streams.video.isEmpty() && a.streams.audio.isEmpty() && a.streams.muxed.isEmpty());
    CHECK_EQ(rig.fast.resolves, 1);
    CHECK_EQ(rig.extractor.runs(), 1);
    const QVariantMap d = rig.source->diagnostics();
    CHECK_EQ(number(d, "finalLookups"), qint64(1));
    CHECK_EQ(number(d, "finalFailures"), qint64(1));
    CHECK_EQ(number(d, "fallbacks"), qint64(0));
    CHECK_EQ(number(d, "extractorAnswers"), qint64(0));
}

// RES-19: the one run that names an upcoming Final also brings its scheduled start.
void fastFinalUpcomingCarriesStart() {
    Rig rig("upcoming");
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Final, false, "final"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.kind == StreamErrorClass::Upcoming && a.error.final);
    CHECK(a.streams.liveState == playback::LiveState::Upcoming);
    CHECK_EQ(a.streams.scheduledStartUnixMs, qint64(4102444800LL * 1000));
    CHECK_EQ(rig.extractor.runs(), 1);
}

// RES-08 / RES-09: a bare "video unavailable" on the fast (token-free) profile is not final: the
// full-profile pass runs and its healthy answer is returned.
void extractorBareUnavailableWidens() {
    Rig rig("barethenok");
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Unplayable, true, "unplayable"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.ok());
    checkExtractorSet(a.streams);
    CHECK(a.streams.path == playback::ResolvePath::ExtractorWithJavaScript);
    CHECK_EQ(rig.extractor.runs(), 2);
    CHECK_EQ(number(rig.source->diagnostics(), "extractorAnswers"), qint64(1));
}

// The fast path fails, then the extractor fails too: one fixed class and label, no provider text.
void bothFail(const char *mode, StreamErrorClass expected, bool final, int expectedRuns, bool runtimePresent = true) {
    Rig rig(mode, runtimePresent);
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Network, true, "network"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.kind == expected);
    CHECK_EQ(a.error.final, final);
    CHECK_EQ(a.error.label, QString::fromLatin1(playback::errorLabel(expected)));
    CHECK(!a.error.label.contains(QLatin1String("PROVIDER-TEXT-MARKER")));
    CHECK(!QString::fromLatin1(playback::userReason(expected)).contains(QLatin1String("PROVIDER")));
    CHECK(a.streams.video.isEmpty() && a.streams.audio.isEmpty() && a.streams.muxed.isEmpty());
    CHECK_EQ(rig.extractor.runs(), expectedRuns);   // a non-final failure gets one full-profile pass
    const QVariantMap d = rig.source->diagnostics();
    CHECK_EQ(number(d, final ? "finalFailures" : "failed"), qint64(1));
}

// The shared runtime's temporary-file budget: the class Failed (never RuntimeUnavailable, whose
// text says "reinstall"), not final, under the fixed label storage_limit; the full-profile pass ran.
void extractorStorageLimit() {
    Rig rig("storagelimit");
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::Unplayable, true, "unplayable"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.kind == StreamErrorClass::Failed);
    CHECK(!a.error.final);
    CHECK_EQ(a.error.label, QStringLiteral("storage_limit"));
    CHECK_EQ(rig.extractor.runs(), 2);
    CHECK_EQ(number(rig.source->diagnostics(), "failed"), qint64(1));
}

void extractorTimeoutClassified() {
    Extractor ex("sleep");
    video::resolver::RunnerOptions o;
    o.executablePath = ex.script;
    o.pythonChild = false;
    video::resolver::YtDlpRunner runner(o);
    std::shared_ptr<video::resolver::RunResult> r = std::make_shared<video::resolver::RunResult>();
    std::shared_ptr<bool> done = std::make_shared<bool>(false);
    QElapsedTimer t;
    t.start();
    runner.run(QStringList() << QStringLiteral("--version"), 500, 1024, [r, done](const video::resolver::RunResult &res) { *r = res; *done = true; });
    CHECK(catalog_test::waitFor([done] { return *done; }, 8000));
    CHECK(r->timedOut);
    CHECK(t.elapsed() < 4000);
    // The service maps a timed-out run to Timeout (YtDlpVideoService runError); the adapter's class:
    const StreamError e = YtDlpStreamResolver::errorFor(video::resolver::ErrorCode::Timeout, false);
    CHECK(e.kind == StreamErrorClass::Timeout && !e.final && e.label == QLatin1String("timeout"));
}

void extractorOneBadFormat() {
    // Parser level: the bad entries never reach VideoDetails.
    video::catalog::VideoDetails d;
    CHECK(video::resolver::parseDetails(fault::fixture("ytdlp_details_mixed.json"), kVideo, &d) == video::resolver::ErrorCode::None);
    std::string kept;
    for (const video::media::MediaFormat &f : d.formats) kept += (kept.empty() ? "" : ",") + f.id;
    CHECK_EQ(kept, std::string("140,weirdcodec,137,251"));   // URL-valid ones; the ladder drops the unknown codec
    // End to end through the adapter (fast path declined).
    Rig rig("ok");
    rig.fast.answer = [] { return failed(sp::domain::FailureKind::BotWall, true, "bot_wall"); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.ok());
    checkExtractorSet(a.streams);
}

void fastOneBadFormatThroughAdapter() {
    Rig rig("ok");
    rig.fast.answer = [] { return fastAnswer(true, true); };
    const Rig::Answer a = rig.resolve(videoRequest());
    CHECK(a.error.ok());
    CHECK(a.streams.path == playback::ResolvePath::FastPath);
    CHECK_EQ(ids(a.streams.video), std::string("137,136"));
    CHECK(contains(a.streams.audio, "251") && contains(a.streams.audio, "140"));
    CHECK(!contains(a.streams.audio, "901"));
    CHECK_EQ(rig.extractor.runs(), 0);
}

void playerParserOneBadFormat() {
    sp::domain::StreamRequest request;
    request.audioOnly = false;
    request.maxVideoHeight = 1080;
    const sp::infrastructure::ParsedPlayer p =
        sp::infrastructure::parsePlayerResponse(fault::fixture("player_formats_mixed.json"), kVideo, request, kFarExpiryMs - 3600 * 1000);
    CHECK(p.failure == sp::domain::FailureKind::None);
    CHECK_EQ(p.keptFormats, 2);
    CHECK_EQ(p.droppedFormats, 9);
    CHECK(p.set.audio.size() == 1 && p.set.audio[0].itag == 251);
    CHECK(p.set.video.size() == 1 && p.set.video[0].itag == 137);
}

void playerParserFaultsClassified() {
    sp::domain::StreamRequest request;
    request.audioOnly = false;
    const auto parse = [&request](const QByteArray &body) {
        return sp::infrastructure::parsePlayerResponse(body, kVideo, request, kFarExpiryMs - 3600 * 1000);
    };
    const QByteArray healthy = fault::fixture("player_formats_mixed.json");
    QByteArray otherVideo = healthy;
    otherVideo.replace("\"videoId\": \"fiResolve01\"", "\"videoId\": \"fiOtherVid1\"");
    QByteArray live = healthy;
    live.replace("\"isLive\": false", "\"isLive\": true");
    QByteArray noFormats = healthy;
    noFormats.replace("\"adaptiveFormats\"", "\"renamedFormats\"");
    const struct { QByteArray body; sp::domain::FailureKind kind; } rows[] = {
        {fault::fixture("malformed.json"), sp::domain::FailureKind::BadResponse},
        {"{\"responseContext\":{}}", sp::domain::FailureKind::BadResponse},
        {"{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm you're not a bot\"}}", sp::domain::FailureKind::BotWall},
        {"{\"playabilityStatus\":{\"status\":\"ERROR\",\"reason\":\"This video has been removed by the uploader\"}}", sp::domain::FailureKind::Final},
        {"{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\",\"reason\":\"Sign in to confirm your age\"}}", sp::domain::FailureKind::Unplayable},
        {otherVideo, sp::domain::FailureKind::BadResponse},
        {live, sp::domain::FailureKind::Unplayable},
        {noFormats, sp::domain::FailureKind::NoUsableFormat},
    };
    for (const auto &row : rows) {
        const sp::infrastructure::ParsedPlayer p = parse(row.body);
        CHECK(p.failure == row.kind);
        CHECK(p.reason.find("PROVIDER") == std::string::npos);
        CHECK(p.set.audio.empty() && p.set.video.empty());
        // The adapter's class for it, and whether the host may fall back (Final never).
        const StreamError e = Source::errorFor(failed(p.failure, p.failure != sp::domain::FailureKind::Final, p.reason.c_str()), false);
        CHECK(e.kind != StreamErrorClass::None && !e.label.isEmpty());
        CHECK_EQ(e.final, p.failure == sp::domain::FailureKind::Final);
    }
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    fault::run("fast_network_falls_back_to_extractor", [] { fastFailureFallsBack(sp::domain::FailureKind::Network, "network"); });
    fault::run("fast_bot_wall_falls_back_to_extractor", [] { fastFailureFallsBack(sp::domain::FailureKind::BotWall, "bot_wall"); });
    fault::run("fast_bad_response_falls_back_to_extractor", [] { fastFailureFallsBack(sp::domain::FailureKind::BadResponse, "not_json"); });
    fault::run("fast_breaker_open_falls_back_to_extractor", [] { fastFailureFallsBack(sp::domain::FailureKind::BreakerOpen, "breaker_open"); });
    fault::run("fast_unpaired_answer_falls_back_to_extractor", fastUnpairedFallsBack);
    fault::run("fast_final_never_falls_back", fastFinalNeverFallsBack);
    fault::run("fast_final_unnamed_named_by_one_run", [] { fastFinalNamedByOneRun("unavailable", StreamErrorClass::Removed); });
    fault::run("fast_final_unnamed_geo_named_by_one_run", [] { fastFinalNamedByOneRun("geo", StreamErrorClass::GeoBlocked); });
    fault::run("fast_final_unnamed_bare_answer_stays_final_one_run", [] { fastFinalNamedByOneRun("bare", StreamErrorClass::Unavailable); });
    fault::run("fast_final_unnamed_network_answer_stays_final_one_run", [] { fastFinalNamedByOneRun("network", StreamErrorClass::Unavailable); });
    fault::run("fast_final_upcoming_carries_start", fastFinalUpcomingCarriesStart);
    fault::run("fast_one_bad_format_keeps_set", fastOneBadFormatThroughAdapter);
    fault::run("fast_player_answer_one_bad_format_keeps_set", playerParserOneBadFormat);
    fault::run("fast_player_answer_faults_classified", playerParserFaultsClassified);

    fault::run("extractor_bot_check_fixed_class", [] { bothFail("botcheck", StreamErrorClass::BotCheck, false, 2); });
    fault::run("extractor_malformed_json_invalid_response", [] { bothFail("malformed", StreamErrorClass::InvalidResponse, false, 2); });
    // RES-09: a clearly final reason is final in a single run and keeps its class ...
    fault::run("extractor_unavailable_final_single_run", [] { bothFail("unavailable", StreamErrorClass::Removed, true, 1); });
    fault::run("extractor_geo_blocked_final_single_run", [] { bothFail("geo", StreamErrorClass::GeoBlocked, true, 1); });
    fault::run("extractor_terminated_final_single_run", [] { bothFail("terminated", StreamErrorClass::Terminated, true, 1); });
    fault::run("extractor_upcoming_final_single_run", [] { bothFail("upcoming", StreamErrorClass::Upcoming, true, 1); });
    // ... a bare "unavailable" is not: the full-profile pass runs (and may answer).
    fault::run("extractor_bare_unavailable_not_final_two_runs", [] { bothFail("bare", StreamErrorClass::Unavailable, false, 2); });
    fault::run("extractor_bare_unavailable_full_profile_answers", extractorBareUnavailableWidens);
    // Rate limiting is TryLater: never final, and no second pass into the same limit.
    fault::run("extractor_rate_limited_try_later_single_run", [] { bothFail("ratelimited", StreamErrorClass::TryLater, false, 1); });
    // The runtime's temporary-file budget is a retryable failure, not a missing runtime.
    fault::run("extractor_storage_limit_retryable", extractorStorageLimit);
    // No formats and no known reason (or the app clients' "This video is unavailable"): the bare,
    // non-final Unavailable after both passes; never a claim about the delivery or the device.
    fault::run("extractor_no_formats_no_reason_bare_unavailable", [] { bothFail("empty", StreamErrorClass::Unavailable, false, 2); });
    fault::run("extractor_app_client_unavailable_wording_bare", [] { bothFail("appunavailable", StreamErrorClass::Unavailable, false, 2); });
    // Formats without URLs (SABR-only): the delivery changed; not a device limit.
    fault::run("extractor_formats_without_urls_delivery_changed", [] { bothFail("nourls", StreamErrorClass::DeliveryChanged, false, 2); });
    fault::run("extractor_network_error_classified", [] { bothFail("network", StreamErrorClass::Network, false, 2); });
    fault::run("extractor_other_video_rejected", [] { bothFail("otherid", StreamErrorClass::InvalidResponse, false, 2); });
    fault::run("extractor_crash_classified", [] { bothFail("crash", StreamErrorClass::Failed, false, 2); });
    fault::run("extractor_metadata_without_streams_named", [] { bothFail("noformats", StreamErrorClass::BotCheck, false, 2); });
    fault::run("extractor_runtime_missing_classified", [] { bothFail("ok", StreamErrorClass::RuntimeUnavailable, false, 0, false); });
    fault::run("extractor_timeout_classified", extractorTimeoutClassified);
    fault::run("extractor_one_bad_format_keeps_set", extractorOneBadFormat);
    return fault::finish("fault_injection resolve");
}
