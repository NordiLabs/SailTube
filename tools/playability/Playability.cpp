// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Playability matrix (docs/PLAYABILITY.md): per video, the app's whole resolve chain against live
// YouTube, built from the app's own sources, and what the user would see.
//
// One child process per video (no state carried over: visitor id, breaker, caches are fresh; the
// yt-dlp player-JS cache directory is shared, as it survives app starts). In the child:
//   PlaybackController (Clients/Sailfish/Presentation, the real one) with phone capabilities
//     (software H.264 + VP9, Opus + AAC; SEL-02 720p ceiling, unmetered) and a harness port;
//   the port does what PlaybackSession::Port::resolve / onStreams do, with the real free functions
//     streamRequestFor, selectionInputFor, resolveErrorKind (PlaybackSession.cpp);
//   the resolver is the app's SharedStreamSource adapter over SailTune's pinned FastStreamResolver
//     + MediaProxy, with YtDlpStreamResolver (YtDlpVideoService, YtDlpRunner, the warm worker
//     WarmResolver and the bundled runtime: Python child, yt-dlp archive, QuickJS) as its fallback.
//     Two recording seams, no logic of their own: the Api traits' resolver forwards to the real
//     FastStreamResolver and records each outcome; a WarmBackend forwards each yt-dlp run to the
//     real WarmResolver (idle) or a second real YtDlpRunner (one-shot, as the runner itself would)
//     and records its stderr class;
//   the controller's first load (selectStreamsWithFallback inside) is opened like
//     PlaybackSession::openBranches (fetcher -> MediaProxy loopback URLs; HLS direct) and timed:
//     the real VideoPlayer to the first decoded frame when the container can decode it (VP9 +
//     Opus), else souphttpsrc ! parsebin ! fakesink to the first parsed sample;
//   a failure gives PlaybackSession::errorText()'s text (same switch, real streamErrorText /
//     errorText) and the real errorSettingKey(controller.errorSettingFacts()).
// Optionally (did not play, the list names clients): the yt-dlp run again with one explicit
// `player_client`, at most 3 clients, each checked through toStreamSet + selection + a first
// parsed sample.
//
// Output: `playability: event ...` lines while running, one `playability: result k=v ...` line per
// video, and the tables at the end (parent). No URL is ever printed: error lines are cut at the
// first URL-like token and truncated.
//
// Build and run: tools/playability.sh (container sfos-m1-sailtube, or --local).
#include "Clients/Sailfish/Presentation/ErrorText.h"
#include "Clients/Sailfish/Presentation/PlaybackController.h"
#include "Clients/Sailfish/Presentation/PlaybackSession.h"
#include "Core/Catalog/Domain/Video.h"
#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/SharedStreamSource.h"
#include "Core/Player/Infrastructure/SharedStreamSourceLadder.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"
#include "Core/Player/Infrastructure/YtDlpStreamMapping.h"
#include "Core/Player/Infrastructure/YtDlpStreamResolver.h"
#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include "Core/Resolver/Infrastructure/YtDlpRunner.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"
#include "Core/Worker/Infrastructure/PythonWorkerChild.h"
#include "Core/Worker/Infrastructure/WarmResolver.h"
#include "Shared/Playback/StreamSource.h"
#include "Shared/Runtime/Child/PythonResolverChild.h"
#include "Shared/Runtime/Child/QuickJsChild.h"
#include "Shared/SailTune/Core/SharedPlayback/Domain/StreamTypes.h"
#include "Shared/SailTune/Core/SharedPlayback/Infrastructure/FastStreamResolver.h"
#include "Shared/SailTune/Core/SharedPlayback/Infrastructure/MediaProxy.h"
#include "NordTileRuntimeLayout.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSslSocket>
#include <QStringList>
#include <QTextStream>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace sp = nordtile::music::sharedplayback;
namespace pb = nordtile::playback;
namespace media = nordtile::video::media;
namespace catalog = nordtile::video::catalog;
namespace resolver = nordtile::video::resolver;
namespace worker = nordtile::video::worker;
namespace sailfish = nordtile::video::sailfish;
namespace adapter = nordtile::video::playeradapter;
using nordtile::player::Container;
using nordtile::player::PlayMode;
using nordtile::player::PlayerSources;
using nordtile::player::PropertySetting;
using nordtile::player::VideoPlayer;

namespace {

// ---- Small helpers ------------------------------------------------------------------------------

QElapsedTimer &clock0() { static QElapsedTimer t; return t; }
long long tNow() { return clock0().isValid() ? clock0().elapsed() : 0; }

bool waitUntil(const std::function<bool()> &pred, int timeoutMs, const std::function<void()> &tick = std::function<void()>()) {
    if (pred()) return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(5);
    bool ok = false;
    QElapsedTimer t;
    t.start();
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (tick) tick();
        if (pred()) { ok = true; loop.quit(); }
        else if (t.elapsed() > timeoutMs) loop.quit();
    });
    poll.start();
    loop.exec();
    return ok;
}

void spin(int ms) { waitUntil([]() { return false; }, ms); }

// A value safe for one `key=value` token: no spaces, quotes or '|' separators inside.
std::string token(std::string s) {
    if (s.empty()) return "-";
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') s[i] = '_';
        else if (c == '"' || c == '=') s[i] = '\'';
    }
    return s;
}

// Provider text for the report: one line, every URL-like word replaced, at most 140 characters.
std::string sanitizeLine(const QString &line) {
    QString s = line.trimmed();
    static const QRegularExpression url(QStringLiteral("(https?://|www\\.|[A-Za-z0-9.-]+\\.(com|be|net)/)\\S*"));
    s.replace(url, QStringLiteral("<url>"));
    static const QRegularExpression id(QStringLiteral("\\[youtube\\] [A-Za-z0-9_-]{11}: "));
    s.replace(id, QStringLiteral("[youtube] <id>: "));
    if (s.size() > 140) s = s.left(137) + QStringLiteral("...");
    return s.toStdString();
}

// The first line with the prefix, skipping the host-locale notice (a POSIX locale in the container)
// and the token-free pass's expected "no JavaScript runtime" notice.
std::string firstLine(const QByteArray &diagnostics, const char *prefix) {
    const QList<QByteArray> lines = diagnostics.split('\n');
    for (const QByteArray &l : lines)
        if (l.startsWith(prefix) && !l.contains("--restrict-filenames") && !l.contains("No supported JavaScript runtime"))
            return sanitizeLine(QString::fromUtf8(l));
    return std::string();
}

std::string join(const std::vector<std::string> &v, const char *sep) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) out += (i ? sep : "") + v[i];
    return out;
}

void event(const std::string &what, const std::string &keys) {
    std::printf("playability: event t=%lld %s %s\n", tNow(), what.c_str(), keys.c_str());
    std::fflush(stdout);
}

const char *pathName(pb::ResolvePath p) {
    switch (p) {
    case pb::ResolvePath::Cache: return "cache";
    case pb::ResolvePath::FastPath: return "fast";
    case pb::ResolvePath::Extractor: return "ytdlp";
    case pb::ResolvePath::ExtractorWithJavaScript: return "ytdlp+js";
    case pb::ResolvePath::Unknown: break;
    }
    return "unknown";
}

const char *liveName(pb::LiveState s) {
    switch (s) {
    case pb::LiveState::NotLive: return "not_live";
    case pb::LiveState::Live: return "live";
    case pb::LiveState::Upcoming: return "upcoming";
    case pb::LiveState::PostLive: return "post_live";
    case pb::LiveState::WasLive: return "was_live";
    }
    return "?";
}

const char *mediaLiveName(media::LiveStatus s) {
    switch (s) {
    case media::LiveStatus::NotLive: return "not_live";
    case media::LiveStatus::IsLive: return "is_live";
    case media::LiveStatus::WasLive: return "was_live";
    case media::LiveStatus::PostLive: return "post_live";
    case media::LiveStatus::Upcoming: return "upcoming";
    case media::LiveStatus::Unknown: break;
    }
    return "unknown";
}

const char *failureName(sailfish::PlaybackController::Failure f) {
    typedef sailfish::PlaybackController C;
    switch (f) {
    case C::NoFailure: return "none";
    case C::ResolveFailed: return "ResolveFailed";
    case C::NoPlayableStream: return "NoPlayableStream";
    case C::UpcomingStream: return "UpcomingStream";
    case C::NetworkFailed: return "NetworkFailed";
    case C::PlaybackFailed: return "PlaybackFailed";
    }
    return "?";
}

const char *stateName(sailfish::PlaybackController::State s) {
    typedef sailfish::PlaybackController C;
    switch (s) {
    case C::Idle: return "Idle";
    case C::Loading: return "Loading";
    case C::Preparing: return "Preparing";
    case C::Ready: return "Ready";
    case C::Failed: return "Failed";
    }
    return "?";
}

std::string codecShort(const pb::StreamFormat &f) {
    const QString fam = nordtile::player::codecFamily(f.hasVideo() ? f.videoCodec : f.audioCodec);
    return fam.isEmpty() ? std::string("?") : fam.toStdString();
}

std::string formatLabel(const pb::StreamFormat &f) {
    if (f.formatId == QLatin1String("manifest")) return "hls-manifest";
    std::string s = f.formatId.toStdString() + ":";
    if (f.isHls) s += "hls:";
    if (f.hasVideo()) {
        s += codecShort(f) + ":" + std::to_string(f.height) + "p" + std::to_string(int(f.fps + 0.5));
        if (f.hasAudio()) s += "+" + nordtile::player::codecFamily(f.audioCodec).toStdString();
    } else {
        s += codecShort(f);
        if (!f.language.isEmpty()) s += ":" + f.language.toStdString();
    }
    if (f.bitrateKbps > 0) s += ":" + std::to_string(int(f.bitrateKbps)) + "k";
    return s;
}

// Phone capabilities (R2 on the device: avdec_h264 + opusdec / avdec_aac, no droid
// decoders): software H.264 and VP9, no verified hardware decode, AV1 never (no self-test).
media::DecoderCapabilities phoneCapabilities() {
    media::DecoderCapabilities c;
    c.h264.available = c.h264.software = true;
    c.vp9.available = c.vp9.software = true;
    return c;
}

// ---- Recording seam 1: the fast path (the Api traits' resolver) ---------------------------------

struct FastRecord {
    int n = 0;
    std::string failure = "pending", reason = "-";
    bool fallback = false;
    std::string path = "-";
    int video = 0, audio = 0, muxed = 0;
    bool live = false, hls = false;
    std::string title;
    long long durationMs = 0;
    int totalMs = -1;
};

class RecordingFast {
public:
    typedef sp::infrastructure::FastStreamResolver Real;
    typedef Real::RequestId RequestId;
    typedef Real::ProbeVerdict ProbeVerdict;
    typedef Real::ProbeDone ProbeDone;
    typedef Real::Counters Counters;

    explicit RecordingFast(const Real::Options &options) : real_(options) {}

    void warmUp() { real_.warmUp(); }
    RequestId resolve(const std::string &videoId, const sp::domain::StreamRequest &request, const Real::Callback &callback) {
        const int n = int(records.size()) + 1;
        records.push_back(FastRecord());
        records.back().n = n;
        event("fast-start", "n=" + std::to_string(n) + " maxHeight=" + std::to_string(request.maxVideoHeight));
        return real_.resolve(videoId, request, [this, n, callback](const sp::domain::ResolveOutcome &o) {
            FastRecord &r = records[std::size_t(n - 1)];
            r.failure = sp::domain::failureLabel(o.failure);
            r.reason = o.reason.empty() ? "-" : o.reason;
            r.fallback = o.fallback;
            if (o.ok()) {
                r.path = sp::domain::pathLabel(o.streams.path);
                r.video = int(o.streams.video.size());
                r.audio = int(o.streams.audio.size());
                r.muxed = int(o.streams.muxed.size());
                r.live = o.streams.metadata.isLive;
                r.hls = !o.streams.hlsUrl.empty();
                r.title = o.streams.metadata.title;
                r.durationMs = o.streams.metadata.durationMs;
                r.totalMs = o.streams.timings.totalMs;
            }
            event("fast-done", "n=" + std::to_string(n) + " failure=" + r.failure + " reason=" + token(r.reason) +
                                   " fallback=" + (r.fallback ? "1" : "0") + " video=" + std::to_string(r.video) +
                                   " audio=" + std::to_string(r.audio) + " muxed=" + std::to_string(r.muxed) +
                                   " live=" + (r.live ? "1" : "0") + " hls=" + (r.hls ? "1" : "0"));
            callback(o);
        });
    }
    void cancel(RequestId id) { real_.cancel(id); }
    void invalidate(const std::string &videoId) { real_.invalidate(videoId); }
    void reportRefused(const std::string &videoId, std::int64_t msLeft) { real_.reportRefused(videoId, msLeft); }
    bool allowed() const { return real_.allowed(); }
    bool hasVisitor() const { return real_.hasVisitor(); }
    const Counters &counters() const { return real_.counters(); }
    sp::domain::CircuitBreaker &breaker() { return real_.breaker(); }
    void probeAddress(const std::string &url, const std::string &userAgent, std::int64_t contentLength, const ProbeDone &done) {
        real_.probeAddress(url, userAgent, contentLength, [this, done](ProbeVerdict v, int status, int ms, const std::string &finalUrl) {
            const char *name = v == ProbeVerdict::Accepted ? "accepted" : v == ProbeVerdict::Refused ? "refused"
                             : v == ProbeVerdict::Unusable ? "unusable" : "no_verdict";
            probes.push_back(std::string(name) + "/" + std::to_string(status));
            event("rung-probe", std::string("verdict=") + name + " status=" + std::to_string(status) + " ms=" + std::to_string(ms));
            done(v, status, ms, finalUrl);
        });
    }

    std::vector<FastRecord> records;
    std::vector<std::string> probes;

private:
    Real real_;
};

struct HarnessApi {
    typedef RecordingFast Resolver;
    typedef sp::infrastructure::MediaProxy Proxy;
    typedef sp::domain::StreamRequest Request;
    typedef sp::domain::StreamFormat Format;
    typedef sp::domain::StreamSet Set;
    typedef sp::domain::ResolveOutcome Outcome;
    typedef sp::domain::FailureKind FailureKind;
    typedef sp::domain::ResolvePath Path;
};
typedef nordtile::player::SharedStreamSource<HarnessApi> Source;

// ---- Recording seam 2: every yt-dlp run (resolver::WarmBackend) ---------------------------------

struct RunRecord {
    int n = 0;
    std::string pass = "?";          // nojs, js, client:<name>
    std::string route = "?";         // warm (worker lane idle) or oneshot (the runner's own child)
    int exitCode = -1;
    bool timedOut = false, started = false;
    bool rateLimited = false;        // stderr names an HTTP 429 (the watch page or an API call)
    long long ms = 0;
    std::string service = "?";       // what YtDlpVideoService::details() reports (errorLabel)
    std::string stderrClass = "-";   // classifyFailure(stderr)
    std::string error, warning;      // first ERROR / WARNING line, sanitized
    int formats = 0, video = 0, audio = 0, muxed = 0, hls = 0, audioLanguages = 0;
    std::string live = "-", title;
    long long duration = -1;
    catalog::VideoDetails details;   // when service == none
};

class RecordingBackend final : public resolver::WarmBackend {
public:
    RecordingBackend(worker::WarmResolver *warm, resolver::YtDlpRunner *oneShot, std::string videoId)
        : warm_(warm), oneShot_(oneShot), videoId_(std::move(videoId)) {}

    // The runner asks the backend first; this one always takes the run and routes it as the
    // runner would (warm lane when idle, else a one-shot child), so every run is recorded.
    bool laneAvailable(int) const override { return true; }
    bool laneIdle(int) const override { return true; }
    void wakeLane(int lane) override { if (warm_) warm_->wakeLane(lane); }
    quint64 runOnLane(int lane, const QStringList &arguments, int timeoutMs, qint64 outputLimit, Completion c) override {
        const quint64 id = next_++;
        RunRecord r;
        r.n = int(runs.size()) + 1;
        const int extractor = arguments.indexOf(QStringLiteral("--extractor-args"));
        const QString youtube = extractor >= 0 && extractor + 1 < arguments.size() ? arguments.at(extractor + 1) : QString();
        const int pc = youtube.indexOf(QStringLiteral("player_client="));
        if (pc >= 0) r.pass = "client:" + youtube.mid(pc + 14).section(QLatin1Char(';'), 0, 0).toStdString();
        else r.pass = arguments.contains(QStringLiteral("--js-runtimes")) ? "js" : "nojs";
        const bool useWarm = warm_ && warm_->laneAvailable(lane) && warm_->laneIdle(lane);
        r.route = useWarm ? "warm" : "oneshot";
        runs.push_back(r);
        event("ytdlp-start", "n=" + std::to_string(r.n) + " pass=" + r.pass + " route=" + r.route);
        const int index = r.n - 1;
        Completion done = [this, index, c](const resolver::RunResult &result) {
            finished(index, result);
            c(result);
        };
        Pending p;
        p.warm = useWarm;
        p.id = useWarm ? warm_->runOnLane(lane, arguments, timeoutMs, outputLimit, done)
                       : oneShot_->run(arguments, timeoutMs, outputLimit, done);
        pending_[id] = p;
        return id;
    }
    void cancelRun(quint64 id) override {
        auto it = pending_.find(id);
        if (it == pending_.end()) return;
        if (it->second.warm) warm_->cancelRun(it->second.id);
        else oneShot_->cancel(it->second.id);
    }

    std::vector<RunRecord> runs;

private:
    struct Pending { bool warm = false; quint64 id = 0; };

    void finished(int index, const resolver::RunResult &result) {
        RunRecord &r = runs[std::size_t(index)];
        r.exitCode = result.exitCode;
        r.timedOut = result.timedOut;
        r.started = result.started;
        r.ms = result.elapsedMs;
        r.rateLimited = result.diagnostics.contains("HTTP Error 429");
        r.error = firstLine(result.diagnostics, "ERROR:");
        r.warning = firstLine(result.diagnostics, "WARNING:");
        const resolver::ErrorCode stderrCode = resolver::classifyFailure(result.diagnostics);
        r.stderrClass = resolver::errorLabel(stderrCode);
        // What YtDlpVideoService::details() derives from this run (its runError + parse steps, for
        // the report; the chain itself uses the service's own result).
        resolver::ErrorCode code = resolver::ErrorCode::None;
        if (result.cancelled) code = resolver::ErrorCode::Cancelled;
        else if (result.timedOut) code = resolver::ErrorCode::Timeout;
        else if (!result.started) code = resolver::ErrorCode::RuntimeUnavailable;
        else if (result.overflow) code = resolver::ErrorCode::InvalidResponse;
        else if (result.crashed) code = resolver::ErrorCode::Failed;
        else if (result.exitCode != 0) code = stderrCode;
        catalog::VideoDetails details;
        if (code == resolver::ErrorCode::None) code = resolver::parseDetails(result.output, videoId_, &details);
        if (code == resolver::ErrorCode::None) {
            r.formats = int(details.formats.size());
            std::set<std::string> languages;
            for (const media::MediaFormat &f : details.formats) {
                const bool v = f.videoCodec != media::VideoCodec::None, a = f.audioCodec != media::AudioCodec::None;
                if (f.protocol == media::Protocol::Hls) ++r.hls;
                else if (v && a) ++r.muxed;
                else if (v) ++r.video;
                else if (a) { ++r.audio; if (!f.language.empty()) languages.insert(f.language); }
            }
            r.audioLanguages = int(languages.size());
            r.live = mediaLiveName(details.summary.liveStatus);
            r.title = details.summary.title;
            r.duration = details.summary.durationSeconds;
            if (details.formats.empty() && stderrCode != resolver::ErrorCode::Failed && stderrCode != resolver::ErrorCode::Network)
                code = stderrCode;
            r.details = details;
        }
        r.service = resolver::errorLabel(code);
        event("ytdlp-done", "n=" + std::to_string(r.n) + " pass=" + r.pass + " route=" + r.route + " exit=" +
                                std::to_string(r.exitCode) + " ms=" + std::to_string(r.ms) + " service=" + r.service +
                                " stderr=" + r.stderrClass + " formats=" + std::to_string(r.formats) + " v/a/m/hls=" +
                                std::to_string(r.video) + "/" + std::to_string(r.audio) + "/" + std::to_string(r.muxed) + "/" +
                                std::to_string(r.hls) + " live=" + r.live + " error=\"" + token(r.error) + "\"");
    }

    worker::WarmResolver *warm_;
    resolver::YtDlpRunner *oneShot_;
    std::string videoId_;
    quint64 next_ = 1;
    std::map<quint64, Pending> pending_;
};

// ---- First parsed sample (a format the container cannot decode) ---------------------------------

// souphttpsrc ! parsebin ! fakesink: the first buffer after the demuxer. `streams` 2 adds a second
// output for a muxed source (progressive or HLS) so its other elementary stream is consumed too.
class ParsedProbe {
public:
    explicit ParsedProbe(const QElapsedTimer *clock) : clock_(clock), firstSample_(-1) {}
    ~ParsedProbe() { stop(); }
    bool start(const QString &url, const std::string &userAgent, int streams) {
        QString description = QStringLiteral("souphttpsrc name=src location=\"%1\"").arg(url);
        if (!userAgent.empty()) description += QStringLiteral(" user-agent=\"%1\"").arg(QString::fromStdString(userAgent).replace('"', '\''));
        description += QStringLiteral(" ! parsebin name=p p. ! queue ! fakesink name=sink signal-handoffs=true sync=false async=false");
        if (streams > 1) description += QStringLiteral(" p. ! queue ! fakesink name=sink2 signal-handoffs=true sync=false async=false");
        GError *error = nullptr;
        pipeline_ = gst_parse_launch(description.toUtf8().constData(), &error);
        if (error) { g_error_free(error); if (pipeline_) gst_object_unref(pipeline_); pipeline_ = nullptr; return false; }
        if (!pipeline_) return false;
        for (const char *name : {"sink", "sink2"}) {
            GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline_), name);
            if (sink) { g_signal_connect(sink, "handoff", G_CALLBACK(&ParsedProbe::onHandoff), this); gst_object_unref(sink); }
        }
        return gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }
    void stop() {
        if (!pipeline_) return;
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    // The first error's text (GStreamer's own, may name an HTTP status), empty while none.
    std::string error() {
        if (!pipeline_) return "pipeline";
        if (!error_.empty()) return error_;
        GstBus *bus = gst_element_get_bus(pipeline_);
        GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (!m) return std::string();
        GError *e = nullptr;
        gst_message_parse_error(m, &e, nullptr);
        error_ = sanitizeLine(QString::fromUtf8(e ? e->message : "error"));
        if (e) g_error_free(e);
        gst_message_unref(m);
        return error_;
    }
    long long firstSample() const { return firstSample_.load(); }

private:
    static void onHandoff(GstElement *, GstBuffer *, GstPad *, gpointer user) {
        ParsedProbe *self = static_cast<ParsedProbe *>(user);
        long long none = -1;
        self->firstSample_.compare_exchange_strong(none, self->clock_->elapsed());
    }
    const QElapsedTimer *clock_;
    GstElement *pipeline_ = nullptr;
    std::atomic<long long> firstSample_;
    std::string error_;
};

// ---- One video: the app's chain -----------------------------------------------------------------

struct ChildSettings {
    std::string video, category = "-", expect = "plays", clients;
    int maxClients = 3;
    QString runtimeRoot, workDir;
    bool warm = true;
    int resolveBudgetMs = 240000;
    int pictureTimeoutMs = 25000;
};

class Child;

class Port final : public sailfish::PlaybackPort {
public:
    explicit Port(Child *c) : c_(c) {}
    void resolve(const sailfish::ResolveRequest &r) override;
    void cancelResolve() override;
    void prefetch(const sailfish::ResolveRequest &) override {}
    void load(const sailfish::PlaybackLoad &l) override;
    void play() override {}
    void pause() override {}
    void stopPlayer() override {}
    void seek(std::int64_t) override {}
    void postSeek(std::int64_t) override {}
    void setRate(double) override {}
    void acquireAudio() override {}
    void releaseAudio() override {}
    void savePosition(const std::string &, std::int64_t) override {}
    void rememberRate(double) override {}
private:
    Child *c_;
};

struct AnswerRecord {
    int n = 0;
    bool fresh = false;
    std::string path = "-", error = "none", kind = "None";
    bool final = false;
    int video = 0, audio = 0, muxed = 0;
    bool hls = false;
    std::string live = "-";
    long long resolveMs = -1;
};

class Child {
public:
    explicit Child(const ChildSettings &s)
        : s_(s),
          runnerOptions_(makeRunnerOptions(s, nullptr)),
          oneShot_(runnerOptions_),
          warm_(workerOptions(s)),
          backend_(s.warm ? &warm_ : nullptr, &oneShot_, s.video),
          runner_(makeRunnerOptions(s, &backend_)),
          service_(&runner_),
          ytdlp_(&service_),
          fast_(fastOptions()),
          proxy_(sp::infrastructure::MediaProxy::Options()),
          source_(&fast_, &proxy_, &ytdlp_),
          port_(this),
          controller_(&port_) {}

    int run();

    // Port side (PlaybackSession::Port::resolve and onStreams, without speculation / likelyNext).
    void resolve(const sailfish::ResolveRequest &r) {
        const quint64 token = ++resolveToken_;
        const std::string videoId = r.videoId;
        const unsigned generation = r.generation;
        const pb::StreamRequest request = sailfish::streamRequestFor(r, 0 /* unmetered */, false);
        lastRequest_ = request;
        AnswerRecord a;
        a.n = int(answers_.size()) + 1;
        a.fresh = r.fresh;
        answers_.push_back(a);
        event("resolve-start", "n=" + std::to_string(a.n) + " fresh=" + (r.fresh ? "1" : "0") + " maxHeight=" +
                                   std::to_string(request.maxHeight) + " codecs=" + request.videoCodecs.join(QLatin1Char(',')).toStdString());
        if (r.fresh) source_.invalidate(request.videoId);
        const int index = a.n - 1;
        resolveOp_ = source_.resolve(request, [this, token, videoId, generation, index](const pb::StreamSet &set, const pb::StreamError &error) {
            if (token != resolveToken_) return;
            resolveOp_ = 0;
            onStreams(index, videoId, generation, set, error);
        });
    }
    void cancelResolve() {
        ++resolveToken_;
        if (resolveOp_) source_.cancel(resolveOp_);
        resolveOp_ = 0;
    }
    void loaded(const sailfish::PlaybackLoad &l) {
        if (haveLoad_) return;   // the first load is the one timed
        load_ = l;
        haveLoad_ = true;
        loadAt_ = tNow();
        event("load", std::string("mode=") + modeName(l.mode));
    }

private:
    static resolver::RunnerOptions makeRunnerOptions(const ChildSettings &s, resolver::WarmBackend *warm) {
        resolver::RunnerOptions o;
        // As main.cpp: the app binary in resolver-child mode, QuickJS when its library is installed.
        o.executablePath = QCoreApplication::applicationFilePath();
        o.pythonChild = true;
        if (QFileInfo(s.runtimeRoot + QStringLiteral("/lib/libsailtune-quickjs.so.1")).isFile())
            o.javascriptRuntime = QStringLiteral("quickjs:") + QCoreApplication::applicationFilePath();
        o.warm = warm;
        o.warmLane = 0;
        return o;
    }
    static worker::WorkerOptions workerOptions(const ChildSettings &s) {
        worker::WorkerOptions o;
        o.workerProgram = QCoreApplication::applicationFilePath();
        o.archivePath = s.runtimeRoot + QStringLiteral("/runtime/yt-dlp.pyz");
        o.workDirectory = s.workDir + QStringLiteral("/worker-") + QString::number(QCoreApplication::applicationPid());
        o.cacheDirectory = s.workDir + QStringLiteral("/yt-dlp-cache");   // survives between videos, like app starts
        return o;
    }
    // SharedStreamSourceSailTune.h componentOptions(): the component's defaults (its own probe on).
    static sp::infrastructure::FastStreamResolver::Options fastOptions() {
        return sp::infrastructure::FastStreamResolver::Options();
    }
    static const char *modeName(PlayMode m) {
        switch (m) {
        case PlayMode::Dual: return "dual";
        case PlayMode::Muxed: return "muxed";
        case PlayMode::Hls: return "hls";
        case PlayMode::Local: return "local";
        case PlayMode::AudioOnly: return "audio";
        }
        return "?";
    }

    void onStreams(int index, const std::string &videoId, unsigned generation, const pb::StreamSet &set, const pb::StreamError &error) {
        sailfish::ResolveAnswer a;
        a.error = sailfish::resolveErrorKind(error);
        a.errorCode = static_cast<int>(error.kind);
        a.upcoming = error.kind == pb::StreamErrorClass::Upcoming || (error.ok() && set.liveState == pb::LiveState::Upcoming);
        a.scheduledStartUnixMs = set.scheduledStartUnixMs > 0 ? set.scheduledStartUnixMs : 0;
        if (a.error == sailfish::ResolveAnswer::None && !set.videoId.isEmpty() && set.videoId != QString::fromStdString(videoId)) {
            a.error = sailfish::ResolveAnswer::Retryable;
            a.errorCode = static_cast<int>(pb::StreamErrorClass::InvalidResponse);
        }
        AnswerRecord &r = answers_[std::size_t(index)];
        r.path = error.ok() ? pathName(set.path) : "-";
        r.error = error.ok() ? "none" : error.label.toStdString();
        r.final = error.final;
        r.kind = a.error == sailfish::ResolveAnswer::None ? "None" : a.error == sailfish::ResolveAnswer::Final ? "Final" : "Retryable";
        r.video = set.video.size();
        r.audio = set.audio.size();
        r.muxed = set.muxed.size();
        r.hls = set.hlsManifest.isValid() && !set.hlsManifest.isEmpty();
        r.live = liveName(set.liveState);
        r.resolveMs = set.resolveMs;
        if (a.error == sailfish::ResolveAnswer::None) {
            for (const QList<pb::StreamFormat> *list : {&set.video, &set.audio, &set.muxed})
                for (const pb::StreamFormat &f : *list) formats_[f.url.toEncoded().toStdString()] = f;
            if (r.hls) {
                pb::StreamFormat manifest;
                manifest.url = set.hlsManifest;
                manifest.isHls = true;
                manifest.formatId = QStringLiteral("manifest");
                formats_[set.hlsManifest.toEncoded().toStdString()] = manifest;
            }
            // PlaybackSession::detailsFor without the row hints (title, duration).
            const media::SelectionInput in = sailfish::selectionInputFor(set);
            a.details.formats = in.formats;
            a.details.manifestUrl = in.manifestUrl;
            a.details.summary.liveStatus = in.liveStatus;
            a.details.summary.videoId = videoId;
            a.expiresAtUnixMs = set.expiresAtUnixMs;
        }
        event("resolve-done", "n=" + std::to_string(r.n) + " path=" + r.path + " error=" + r.error + " final=" +
                                  (r.final ? "1" : "0") + " answer=" + r.kind + " v/a/m=" + std::to_string(r.video) + "/" +
                                  std::to_string(r.audio) + "/" + std::to_string(r.muxed) + " hls=" + (r.hls ? "1" : "0") +
                                  " live=" + r.live);
        controller_.onResolved(controllerNow(), QDateTime::currentMSecsSinceEpoch(), videoId, generation, a);
    }

    std::int64_t controllerNow() const { return 1000 + tNow(); }

    // PlaybackSession::errorText() for a failed controller (same switch, same text functions).
    QString errorText() const {
        if (controller_.state() != sailfish::PlaybackController::Failed) return QString();
        switch (controller_.failure()) {
        case sailfish::PlaybackController::ResolveFailed:
            return sailfish::streamErrorText(static_cast<pb::StreamErrorClass>(controller_.failureCode()));
        case sailfish::PlaybackController::NoPlayableStream: return sailfish::errorText(resolver::ErrorCode::NoPlayableStream);
        case sailfish::PlaybackController::UpcomingStream: {
            const QString text = sailfish::errorText(resolver::ErrorCode::Upcoming);
            const qint64 start = controller_.scheduledStartUnixMs();
            return start > 0 ? text + QStringLiteral(" Scheduled to start <time>.") : text;
        }
        case sailfish::PlaybackController::NetworkFailed: return sailfish::errorText(resolver::ErrorCode::Network);
        case sailfish::PlaybackController::PlaybackFailed: return QStringLiteral("Playback stopped after repeated errors.");
        case sailfish::PlaybackController::NoFailure: break;
        }
        return QString();
    }
    std::string errorSettingKey() const {
        const bool noPlayable = controller_.failure() == sailfish::PlaybackController::ResolveFailed &&
            static_cast<pb::StreamErrorClass>(controller_.failureCode()) == pb::StreamErrorClass::NoPlayableStream;
        return sailfish::errorSettingKey(controller_.errorSettingFacts(noPlayable));
    }

    // The format of a load's branch, from the resolved set (PlaybackSession::formats_).
    const pb::StreamFormat *formatOf(const nordtile::player::SourceSpec &spec) const {
        if (!spec.present()) return nullptr;
        auto it = formats_.find(spec.uri);
        return it == formats_.end() ? nullptr : &it->second;
    }

    struct Picture { long long ms = -1; std::string kind = "-", error = "-"; };
    Picture measure();
    std::string clientProbes();

    ChildSettings s_;
    resolver::RunnerOptions runnerOptions_;
    resolver::YtDlpRunner oneShot_;
    worker::WarmResolver warm_;
    RecordingBackend backend_;
    resolver::YtDlpRunner runner_;
    resolver::YtDlpVideoService service_;
    adapter::YtDlpStreamResolver ytdlp_;
    RecordingFast fast_;
    sp::infrastructure::MediaProxy proxy_;
    Source source_;
    Port port_;
    sailfish::PlaybackController controller_;
    quint64 resolveToken_ = 0, resolveOp_ = 0;
    pb::StreamRequest lastRequest_;
    std::vector<AnswerRecord> answers_;
    std::map<std::string, pb::StreamFormat> formats_;
    bool haveLoad_ = false;
    sailfish::PlaybackLoad load_;
    long long loadAt_ = -1;
};

void Port::resolve(const sailfish::ResolveRequest &r) { c_->resolve(r); }
void Port::cancelResolve() { c_->cancelResolve(); }
void Port::load(const sailfish::PlaybackLoad &l) { c_->loaded(l); }

// Opens the load's branches like PlaybackSession::openBranches (all opens issued first; HLS
// direct) and times the first picture.
Child::Picture Child::measure() {
    Picture p;
    const nordtile::player::SourceSpec *specs[3] = {&load_.video, &load_.audio, &load_.muxed};
    std::vector<pb::StreamFormat> formats(3);
    // Shared with the fetcher's completions, which may arrive after a timeout.
    struct Opens { QUrl local[3]; std::string errors[3]; int waiting = 0; };
    std::shared_ptr<Opens> opens = std::make_shared<Opens>();
    std::vector<quint64> handles;
    for (int slot = 0; slot < 3; ++slot) {
        if (!specs[slot]->present()) continue;
        const pb::StreamFormat *f = formatOf(*specs[slot]);
        if (!f) { p.error = "stream_not_in_set"; return p; }
        formats[std::size_t(slot)] = *f;
        if (f->isHls) { opens->local[slot] = f->url; continue; }
        ++opens->waiting;
        handles.push_back(source_.fetcher()->open(*f, [slot, opens](const QUrl &url, const pb::StreamError &e) {
            opens->local[slot] = url;
            if (!e.ok() || url.isEmpty()) opens->errors[slot] = e.ok() ? "empty" : e.label.toStdString();
            --opens->waiting;
        }));
    }
    struct Closer {
        pb::StreamFetcher *fetcher; std::vector<quint64> *handles;
        ~Closer() { for (quint64 h : *handles) fetcher->close(h); }
    } closer = {source_.fetcher(), &handles};
    if (!waitUntil([opens]() { return opens->waiting == 0; }, 20000)) { p.error = "open_timeout"; return p; }
    for (int slot = 0; slot < 3; ++slot)
        if (!opens->errors[slot].empty()) { p.error = "open_" + opens->errors[slot]; return p; }
    const QUrl *local = opens->local;
    event("streams-open", "");

    const QString video = nordtile::player::codecFamily(formats[0].videoCodec);
    const QString audio = nordtile::player::codecFamily(formats[1].audioCodec);
    const bool decodable = load_.mode == PlayMode::Dual && video == QLatin1String("vp9") && audio == QLatin1String("opus");
    QElapsedTimer &clock = clock0();
    if (decodable) {
        // The container decodes VP9 + Opus: the real player, as PlaybackSession::startPlayer loads it.
        VideoPlayer player;
        bool failed = false, framed = false;
        std::string reason;
        QObject::connect(&player, &VideoPlayer::firstFrame, &player, [&framed]() { framed = true; });
        QObject::connect(&player, &VideoPlayer::failed, &player, [&failed, &reason](const QString &why, bool, int, int status) {
            if (failed) return;
            failed = true;
            reason = sanitizeLine(why) + (status > 0 ? " http=" + std::to_string(status) : std::string());
        });
        PlayerSources src;
        src.mode = load_.mode;
        src.video = load_.video;
        src.audio = load_.audio;
        src.video.uri = local[0].toEncoded().toStdString();
        src.audio.uri = local[1].toEncoded().toStdString();
        src.video.userAgent.clear(); src.video.extraHeaders.clear();
        src.audio.userAgent.clear(); src.audio.extraHeaders.clear();
        src.videoId = load_.videoId;
        src.decode = load_.decode;
        src.metadataDurationMs = load_.metadataDurationMs;
        src.mediaRole = nordtile::player::kMediaRole;
        src.audioSink.factory = "fakesink";
        PropertySetting sync;
        sync.name = "sync";
        sync.value = "true";
        src.audioSink.properties.push_back(sync);
        player.load(src);
        waitUntil([&]() { return failed || framed; }, s_.pictureTimeoutMs);
        if (framed) { p.ms = clock.elapsed(); p.kind = "decoded"; }
        else p.error = failed ? "player:" + token(reason) : "first_frame_timeout";
        player.stop();
        return p;
    }
    // Not decodable here (H.264 / AAC, HLS): the first parsed sample of the picture branch.
    const int slot = load_.mode == PlayMode::Dual ? 0 : load_.mode == PlayMode::AudioOnly ? 1 : 2;
    const nordtile::player::SourceSpec &spec = *specs[slot];
    const bool loopback = local[std::size_t(slot)].host() == QLatin1String("127.0.0.1") || local[std::size_t(slot)].host() == QLatin1String("localhost");
    ParsedProbe probe(&clock);
    const int streams = load_.mode == PlayMode::Dual || load_.mode == PlayMode::AudioOnly ? 1 : 2;
    if (!probe.start(local[std::size_t(slot)].toEncoded(), loopback ? std::string() : spec.userAgent, streams)) {
        p.error = "parse_pipeline";
        return p;
    }
    std::string err;
    waitUntil([&]() { err = probe.error(); return !err.empty() || probe.firstSample() >= 0; }, s_.pictureTimeoutMs);
    if (probe.firstSample() >= 0) { p.ms = probe.firstSample(); p.kind = "parsed"; }
    else p.error = err.empty() ? "first_sample_timeout" : "parse:" + token(err);
    probe.stop();
    return p;
}

// The yt-dlp run with one explicit player_client (full profile, otherwise the service's own
// arguments), then the app's mapping, ladder and selection, then a first parsed sample.
std::string Child::clientProbes() {
    QStringList clients = QString::fromStdString(s_.clients).split(QLatin1Char(','), QString::SkipEmptyParts);
    while (clients.size() > s_.maxClients) clients.removeLast();
    std::vector<std::string> out;
    for (const QString &client : clients) {
        spin(3000);   // request budget: a pause before each extra run
        QStringList common = runner_.commonArguments(true);
        common.removeAll(QStringLiteral("--no-warnings"));
        QStringList args = resolver::YtDlpVideoService::detailsArguments(common, QString::fromStdString(s_.video));
        const int at = args.indexOf(QStringLiteral("--extractor-args"));
        if (at >= 0 && at + 1 < args.size()) args[at + 1] += QStringLiteral(";player_client=") + client;
        bool finished = false;
        resolver::RunResult result;
        const std::size_t before = backend_.runs.size();
        runner_.run(args, 90000, 6 * 1024 * 1024, [&finished, &result](const resolver::RunResult &r) { result = r; finished = true; });
        waitUntil([&finished]() { return finished; }, 100000);
        std::string verdict = "no_result";
        if (finished && backend_.runs.size() > before) {
            const RunRecord &rec = backend_.runs[before];
            if (rec.service != "none") {
                verdict = rec.service + (rec.error.empty() ? std::string() : "(" + token(rec.error.substr(0, 60)) + ")");
            } else {
                const pb::StreamSet set = adapter::toStreamSet(rec.details, lastRequest_);
                if (!nordtile::player::playable(set, lastRequest_)) {
                    verdict = std::string("not_playable(v/a/m=") + std::to_string(set.video.size()) + "/" + std::to_string(set.audio.size()) +
                              "/" + std::to_string(set.muxed.size()) + ")";
                } else {
                    const media::SelectionInput in = sailfish::selectionInputFor(set);
                    media::PlaybackPreferences prefs;
                    const std::vector<media::StreamCandidate> chain =
                        media::selectStreamsWithFallback(in, phoneCapabilities(), prefs, media::QualityChoice());
                    if (chain.empty()) verdict = "empty_selection";
                    else {
                        const media::StreamCandidate &c = chain[0];
                        const media::MediaFormat &m = c.kind == media::StreamKind::Adaptive ? c.video : c.kind == media::StreamKind::AudioOnly ? c.audio : c.muxed;
                        // The candidate's picture branch: first parsed sample, direct with its headers.
                        ParsedProbe probe(&clock0());
                        const long long t0 = tNow();
                        std::string ua;
                        auto h = m.headers.find("User-Agent");
                        if (h != m.headers.end()) ua = h->second;
                        const QString url = c.kind == media::StreamKind::Live && !in.manifestUrl.empty() ? QString::fromStdString(in.manifestUrl)
                                                                                                         : QString::fromStdString(m.url);
                        std::string sample = "no_sample";
                        if (probe.start(url, ua, c.kind == media::StreamKind::Adaptive ? 1 : 2)) {
                            std::string err;
                            waitUntil([&]() { err = probe.error(); return !err.empty() || probe.firstSample() >= 0; }, 20000);
                            if (probe.firstSample() >= 0) sample = "sample_" + std::to_string(probe.firstSample() - t0) + "ms";
                            else if (!err.empty()) sample = "fetch_error(" + token(err.substr(0, 50)) + ")";
                        }
                        probe.stop();
                        verdict = "plays:" + token(c.label) + ":" + sample;
                    }
                }
            }
        }
        out.push_back(client.toStdString() + "=" + verdict);
        event("client-probe", "client=" + client.toStdString() + " verdict=" + token(verdict));
    }
    return join(out, ";");
}

int Child::run() {
    clock0().start();
    event("child-start", "video=" + s_.video + " category=" + s_.category + " expect=" + s_.expect);
    // App start-up: the warm worker's latency lane and the fast path's visitor id are warmed before
    // the first tap (main.cpp warms both once the first page is up).
    if (s_.warm) {
        warm_.warmUp(worker::Lane::Latency);
        const bool ready = waitUntil([this]() { return warm_.idle(worker::Lane::Latency); }, 30000);
        event("warm-worker", std::string("ready=") + (ready ? "1" : "0"));
    }
    source_.warmUp();
    const bool visitor = waitUntil([this]() { return fast_.hasVisitor(); }, 9000);
    event("visitor", std::string("ready=") + (visitor ? "1" : "0"));

    controller_.setCapabilities(phoneCapabilities());
    controller_.setPreferences(media::PlaybackPreferences());
    clock0().restart();   // t = 0: the tap
    event("tap", "");
    controller_.open(controllerNow(), s_.video);
    const bool settled = waitUntil([this]() {
        return haveLoad_ || controller_.state() == sailfish::PlaybackController::Failed;
    }, s_.resolveBudgetMs, [this]() { if (!haveLoad_) controller_.tick(controllerNow()); });
    const long long settledAt = tNow();

    // Rung, format and picture.
    std::string rung = "-", format = "-", decode = "sw";
    Picture picture;
    if (haveLoad_) {
        for (std::size_t i = answers_.size(); i > 0; --i)
            if (answers_[i - 1].error == "none") { rung = answers_[i - 1].path + (i > 1 ? "@" + std::to_string(i) : std::string()); break; }
        std::vector<std::string> parts;
        parts.push_back(modeName(load_.mode));
        for (const nordtile::player::SourceSpec *spec : {&load_.video, &load_.audio, &load_.muxed})
            if (const pb::StreamFormat *f = formatOf(*spec)) parts.push_back(formatLabel(*f));
        format = join(parts, "+");
        decode = load_.decode == nordtile::player::DecodeMode::Hardware ? "hw_fallback" : "sw";
        picture = measure();
        event("picture", "ms=" + std::to_string(picture.ms) + " kind=" + picture.kind + " error=" + picture.error);
    }
    const bool plays = haveLoad_ && picture.ms >= 0;

    // The fast path and yt-dlp summaries.
    std::vector<std::string> fast, passes;
    std::string ytError = "-";
    int rateLimited = 0;
    for (const FastRecord &f : fast_.records) fast.push_back(f.failure + (f.reason != "-" ? "/" + f.reason : std::string()) + (f.fallback ? "/fb" : ""));
    for (const RunRecord &r : backend_.runs) {
        passes.push_back(r.pass + ":" + r.route + ":" + r.service + (r.stderrClass != "failed" && r.stderrClass != r.service ? "(" + r.stderrClass + ")" : ""));
        rateLimited += r.rateLimited ? 1 : 0;
        if (ytError == "-" && !r.error.empty()) ytError = r.error;
        else if (ytError == "-" && r.service != "none" && !r.warning.empty()) ytError = r.warning;
    }
    std::string title = "-", live = "-";
    long long duration = -1;
    int audioLanguages = 0;
    for (const FastRecord &f : fast_.records) if (!f.title.empty()) { title = f.title; duration = f.durationMs / 1000; live = f.live ? "is_live" : "not_live"; }
    for (const RunRecord &r : backend_.runs)
        if (r.service == "none") { title = r.title; duration = r.duration; live = r.live; audioLanguages = std::max(audioLanguages, r.audioLanguages); }

    // What the user sees.
    const std::string message = haveLoad_ ? (plays ? "-" : "(player) " + picture.error) : errorText().toStdString();
    const std::string setting = haveLoad_ ? "-" : errorSettingKey();
    const QVariantMap d = source_.diagnostics();

    // Did not play and the list names clients to try: explicit player_client runs.
    std::string clients = "-";
    if (!plays && !s_.clients.empty() && s_.clients != "-" && s_.maxClients > 0) clients = clientProbes();

    std::printf("playability: result video=%s category=%s expect=%s plays=%d rung=%s format=%s decode=%s ttff=%lld ttffKind=%s "
                "settledMs=%lld state=%s failure=%s errorClass=%s fast=%s passes=%s ytdlpError=\"%s\" message=\"%s\" setting=%s "
                "title=\"%s\" durationS=%lld live=%s audioLangs=%d fastUnpaired=%lld probeRefused=%lld probeUnusable=%lld "
                "breakerOpen=%lld rungProbes=%s answers=%d clients=%s settled=%d rateLimitedRuns=%d\n",
                s_.video.c_str(), s_.category.c_str(), s_.expect.c_str(), plays ? 1 : 0, rung.c_str(), token(format).c_str(),
                decode.c_str(), picture.ms, picture.kind.c_str(), settledAt, stateName(controller_.state()),
                failureName(controller_.failure()),
                controller_.failure() == sailfish::PlaybackController::ResolveFailed
                    ? pb::errorLabel(static_cast<pb::StreamErrorClass>(controller_.failureCode())) : "-",
                token(join(fast, ",")).c_str(), token(join(passes, ",")).c_str(), token(ytError).c_str(), token(message).c_str(),
                token(setting).c_str(), token(title.substr(0, 60)).c_str(), duration, live.c_str(), audioLanguages,
                d.value(QStringLiteral("fastUnpaired")).toLongLong(), d.value(QStringLiteral("probeRefused")).toLongLong(),
                d.value(QStringLiteral("probeUnusable")).toLongLong(), d.value(QStringLiteral("breakerOpen")).toLongLong(),
                token(join(fast_.probes, ",")).c_str(), int(answers_.size()), token(clients).c_str(), settled ? 1 : 0, rateLimited);
    std::fflush(stdout);
    proxy_.closeVideo(s_.video);
    return 0;
}

// ---- Parent: the list, one child per video, the tables ------------------------------------------

struct Entry {
    std::string category, id, expect, clients, why;
};

std::vector<Entry> readList(const QString &path) {
    std::vector<Entry> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return out;
    QTextStream in(&f);
    in.setCodec("UTF-8");
    while (!in.atEnd()) {
        const QString line = in.readLine();
        if (line.trimmed().isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList cols = line.split(QLatin1Char('\t'));
        if (cols.size() < 3) continue;
        Entry e;
        e.category = cols.at(0).trimmed().toStdString();
        e.id = cols.at(1).trimmed().toStdString();
        e.expect = cols.at(2).trimmed().toStdString();
        e.clients = cols.size() > 3 ? cols.at(3).trimmed().toStdString() : std::string();
        e.why = cols.size() > 4 ? cols.at(4).trimmed().toStdString() : std::string();
        if (catalog::validVideoId(e.id)) out.push_back(e);
    }
    return out;
}

std::map<std::string, std::string> keyValues(const std::string &line) {
    std::map<std::string, std::string> kv;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && line[i] == ' ') ++i;
        const std::size_t eq = line.find('=', i);
        if (eq == std::string::npos) break;
        const std::string key = line.substr(i, eq - i);
        std::size_t j = eq + 1;
        std::string value;
        if (j < line.size() && line[j] == '"') {
            const std::size_t close = line.find('"', j + 1);
            value = line.substr(j + 1, close == std::string::npos ? std::string::npos : close - j - 1);
            i = close == std::string::npos ? line.size() : close + 1;
        } else {
            const std::size_t sp = line.find(' ', j);
            value = line.substr(j, sp == std::string::npos ? std::string::npos : sp - j);
            i = sp == std::string::npos ? line.size() : sp;
        }
        if (key.find(' ') == std::string::npos) kv[key] = value;
    }
    return kv;
}

std::string verdictOf(const std::map<std::string, std::string> &r) {
    const bool plays = r.count("plays") && r.at("plays") == "1";
    const std::string expect = r.count("expect") ? r.at("expect") : "plays";
    if (plays) return expect == "plays" ? "plays" : "plays (expected to fail)";
    return expect == "plays" ? "fails-should-play" : "fails-expected";
}

std::string cell(const std::map<std::string, std::string> &r, const char *key) {
    auto it = r.find(key);
    if (it == r.end() || it->second.empty()) return "-";
    std::string v = it->second;
    for (std::size_t i = 0; i < v.size(); ++i) { if (v[i] == '_') v[i] = ' '; if (v[i] == '|') v[i] = '/'; }
    return v;
}

void usage() {
    std::printf("usage: playability [--list FILE] [--videos id,id] [--pause MS] [--no-clients] [--max-clients N]\n"
                "                   [--no-warm] [--runtime-root DIR] [--work DIR]\n");
}

}  // namespace

int main(int argc, char **argv) {
    // Child modes of the app binary (main.cpp order): QuickJS, the one-shot resolver, the warm worker.
    if (nordtile::music::sailfish::infrastructure::quickJsChildRequested())
        return nordtile::music::sailfish::infrastructure::runQuickJsChild(argc, argv);
    if (nordtile::music::sailfish::infrastructure::pythonResolverChildRequested())
        return nordtile::music::sailfish::infrastructure::runPythonResolverChild(argc, argv);
    if (worker::pythonWorkerRequested())
        return worker::runPythonWorkerChild(argc, argv, NORDTILE_RUNTIME_ROOT "/runtime");

    QCoreApplication app(argc, argv);
    ChildSettings child;
    child.runtimeRoot = QStringLiteral(NORDTILE_RUNTIME_ROOT);
    child.workDir = QDir::tempPath() + QStringLiteral("/sailtube-playability-work");
    QString list = QStringLiteral(NORDTILE_RUNTIME_ROOT "/../videos.tsv");
    std::string only;
    int pauseMs = 4000;
    bool clients = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool v = i + 1 < argc;
        if (a == "--child" && v) child.video = argv[++i];
        else if (a == "--category" && v) child.category = argv[++i];
        else if (a == "--expect" && v) child.expect = argv[++i];
        else if (a == "--clients" && v) child.clients = argv[++i];
        else if (a == "--max-clients" && v) child.maxClients = std::max(0, std::min(3, std::atoi(argv[++i])));
        else if (a == "--no-warm") child.warm = false;
        else if (a == "--no-clients") clients = false;
        else if (a == "--list" && v) list = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--videos" && v) only = argv[++i];
        else if (a == "--pause" && v) pauseMs = std::max(3000, std::atoi(argv[++i]));
        else if (a == "--work" && v) child.workDir = QString::fromLocal8Bit(argv[++i]);
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }
    QDir().mkpath(child.workDir);
    QFile::setPermissions(child.workDir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    if (!child.video.empty()) {
        if (!catalog::validVideoId(child.video)) { std::printf("invalid video id\n"); return 2; }
        QString gstError;
        if (!nordtile::player::GstEngine::initialize(&gstError)) { std::printf("playability: GStreamer unavailable: %s\n", qPrintable(gstError)); return 2; }
        if (!QSslSocket::supportsSsl()) { std::printf("playability: no TLS in Qt Network\n"); return 2; }
        Child c(child);
        return c.run();
    }

    std::vector<Entry> entries = readList(list);
    if (!only.empty()) {
        std::set<std::string> keep;
        std::stringstream in(only);
        std::string id;
        while (std::getline(in, id, ',')) keep.insert(id);
        std::vector<Entry> kept;
        for (const Entry &e : entries) if (keep.count(e.id)) kept.push_back(e);
        for (const std::string &k : keep) {
            bool known = false;
            for (const Entry &e : kept) known = known || e.id == k;
            if (!known && catalog::validVideoId(k)) { Entry e; e.category = "adhoc"; e.id = k; e.expect = "plays"; kept.push_back(e); }
        }
        entries = kept;
    }
    if (entries.empty()) { std::printf("playability: no videos (list %s)\n", qPrintable(list)); return 2; }
    std::printf("playability: start videos=%d pause=%d gst-runtime-root=%s\n", int(entries.size()), pauseMs, NORDTILE_RUNTIME_ROOT);
    std::fflush(stdout);

    std::vector<std::map<std::string, std::string> > results;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (i) spin(pauseMs);   // request budget: one pass, a pause between videos
        const Entry &e = entries[i];
        QProcess p;
        p.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        QStringList args;
        args << "--child" << QString::fromStdString(e.id) << "--category" << QString::fromStdString(e.category)
             << "--expect" << QString::fromStdString(e.expect) << "--work" << child.workDir;
        if (clients && !e.clients.empty() && e.clients != "-") args << "--clients" << QString::fromStdString(e.clients);
        if (!child.warm) args << "--no-warm";
        p.start(QCoreApplication::applicationFilePath(), args);
        p.waitForStarted(10000);
        if (!p.waitForFinished(600000)) { p.kill(); p.waitForFinished(5000); }
        const std::string output = p.readAllStandardOutput().toStdString();
        std::fputs(output.c_str(), stdout);
        std::fflush(stdout);
        std::map<std::string, std::string> r;
        std::stringstream lines(output);
        std::string line;
        const std::string prefix = "playability: result ";
        while (std::getline(lines, line))
            if (line.compare(0, prefix.size(), prefix) == 0) r = keyValues(line.substr(prefix.size()));
        if (r.empty()) { r["video"] = e.id; r["category"] = e.category; r["expect"] = e.expect; r["plays"] = "0"; r["message"] = "child_crash_or_timeout"; }
        results.push_back(r);
    }

    std::printf("\n## Results\n\n| Category | Id | Rung | Format (SEL-02 pick) | TTFF ms | Failure (fast / yt-dlp passes / first error) | App message | Setting | Verdict |\n"
                "|---|---|---|---|---:|---|---|---|---|\n");
    std::map<std::string, std::vector<std::string> > perCategory;
    for (const auto &r : results) {
        const std::string v = verdictOf(r);
        perCategory[cell(r, "category")].push_back(v);
        const std::string ttff = cell(r, "ttff") == "-1" ? "-" : cell(r, "ttff") + (cell(r, "ttffKind") == "parsed" ? " (parsed)" : "");
        const bool failed = cell(r, "plays") != "1";
        const std::string limited = cell(r, "rateLimitedRuns") != "-" && cell(r, "rateLimitedRuns") != "0"
            ? " (HTTP 429 in " + cell(r, "rateLimitedRuns") + " yt-dlp runs)" : std::string();
        std::printf("| %s | `%s` | %s | %s | %s | %s | %s | %s | %s |\n", cell(r, "category").c_str(), cell(r, "video").c_str(),
                    cell(r, "rung").c_str(), cell(r, "format").c_str(), ttff.c_str(),
                    failed ? (cell(r, "fast") + " / " + cell(r, "passes") + " / " + cell(r, "ytdlpError") + limited).c_str()
                           : (cell(r, "fast") + " / " + cell(r, "passes")).c_str(),
                    cell(r, "message").c_str(), cell(r, "setting").c_str(), v.c_str());
    }
    std::printf("\n## Per category\n\n| Category | Verdict |\n|---|---|\n");
    for (const auto &c : perCategory) {
        std::map<std::string, int> n;
        for (const std::string &v : c.second) ++n[v];
        std::vector<std::string> parts;
        for (const auto &x : n) parts.push_back(x.first + " x" + std::to_string(x.second));
        std::printf("| %s | %s |\n", c.first.c_str(), join(parts, ", ").c_str());
    }
    std::printf("\n## player_client tries (did not play)\n\n");
    for (const auto &r : results)
        if (cell(r, "clients") != "-") std::printf("- `%s` (%s): %s\n", cell(r, "video").c_str(), cell(r, "category").c_str(), cell(r, "clients").c_str());
    int playing = 0;
    for (const auto &r : results) playing += cell(r, "plays") == "1";
    std::printf("\nplayability: done videos=%d plays=%d\n", int(results.size()), playing);
    return 0;
}
