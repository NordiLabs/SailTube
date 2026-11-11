// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Host playback performance harness (NFR-PERF-02, DIA-03 markers): measures every stage of "tap to
// picture" against live YouTube with the app's own pieces - SailTune's pinned shared component
// (FastStreamResolver + MediaProxy, Shared/SailTune/Core/SharedPlayback) and the playback component's player
// (Core/Player/Infrastructure/VideoPlayer, dual plan VP9 + Opus, fakesink audio, appsink video).
//
// Modes, per video and round:
//   cold    a new process: no visitor id, no pooled connection, no cache, no DNS cache
//   warm    the same resolver after a first resolve of another video (visitor id minted, connection
//           kept); the video's cache entry is invalidated first, so the fast path really runs
//   cached  right after a warm round of the same video: the resolver answers from its TTL cache
//           (what a prefetched / speculative resolve would give the tap)
// Every stage prints one `sailtube-timing: <event> t=<ms> ...` line (tools/experiments/perf_timing.md),
// t in ms from the tap of that round. A bot wall, an unplayable video or a network failure skips the
// round with a `sailtube-perf: result ... ok=0 reason=<class>` line; it never fails the run.
// Exit status: 0 when at least min(3, number of videos) videos produced numbers.
//
// Build and run: tools/perf_playback.sh (container sfos-m1-sailtube, or --local).
#include "Shared/SailTune/Core/SharedPlayback/Domain/StreamTypes.h"
#include "Shared/SailTune/Core/SharedPlayback/Infrastructure/FastStreamResolver.h"
#include "Shared/SailTune/Core/SharedPlayback/Infrastructure/MediaProxy.h"
#include "Core/Player/Infrastructure/GstEngine.h"
#include "Core/Player/Infrastructure/VideoPlayer.h"

#include <gst/gst.h>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QProcess>
#include <QSslSocket>
#include <QStringList>
#include <QSysInfo>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <cmath>
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
using sp::domain::FailureKind;
using sp::domain::ResolveOutcome;
using sp::domain::StreamFormat;
using sp::domain::StreamRequest;
using sp::domain::StreamSet;
using sp::infrastructure::FastStreamResolver;
using sp::infrastructure::MediaProxy;
using nordtile::player::Container;
using nordtile::player::PlayMode;
using nordtile::player::PlayerSources;
using nordtile::player::PropertySetting;
using nordtile::player::SourceSpec;
using nordtile::player::VideoPlayer;

namespace {

// ---- Settings -----------------------------------------------------------------------------------

// Long-lived official uploads, public and not age restricted (titles in docs/PERF_PLAYBACK.md).
const char *const kDefaultVideos[] = {"aqz-KE-bpKQ", "eRsGyueVLvQ", "R6MlUcmOul8", "WhWc3b3KhnY", "Y-rmzh0PI3c"};
const char *const kDefaultPrimer = "jNQXAC9IVRw";

struct Settings {
    int rounds = 3;
    std::vector<std::string> videos;
    std::string primer = kDefaultPrimer;
    int height = 720;                  // SEL-02 default ceiling: 720p unmetered, software decode
    std::set<std::string> modes;
    int resolveTimeoutMs = 20000;
    int roundTimeoutMs = 25000;        // from the play plan to the picture
    int pauseMs = 750;                 // between rounds
    int watchMs = 2500;                // shorts: how long a Short plays before the next swipe
    // Child (one cold round in a fresh process).
    std::string childVideo;
    int childRun = 0, childRound = 0;
};

bool validId(const std::string &id) {
    if (id.size() != 11) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

std::vector<std::string> splitList(const std::string &text) {
    std::vector<std::string> out;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) if (!item.empty()) out.push_back(item);
    return out;
}

// ---- Event loop helpers -------------------------------------------------------------------------

// Runs the event loop until pred() holds or the timeout passes. Timestamps are taken in the
// callbacks themselves, so the 1 ms poll does not quantise them.
bool waitUntil(const std::function<bool()> &pred, int timeoutMs) {
    if (pred()) return true;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(1);
    bool ok = false;
    QElapsedTimer t;
    t.start();
    QObject::connect(&poll, &QTimer::timeout, &loop, [&]() {
        if (pred()) { ok = true; loop.quit(); }
        else if (t.elapsed() > timeoutMs) loop.quit();
    });
    poll.start();
    loop.exec();
    return ok;
}

void spin(int ms) { waitUntil([]() { return false; }, ms); }

// ---- One round ----------------------------------------------------------------------------------

struct RoundResult {
    std::string video, mode;
    int run = 0, round = 0;
    bool ok = false;
    std::string reason = "-";
    std::map<std::string, long long> t;   // "event" or "event.branch" -> ms from the tap
    int probeRedirects = 0, rungRedirects = 0, proxyRedirects = 0;
    bool reused = false;
};

struct RoundState {
    RoundResult r;
    QElapsedTimer clock;
    bool rungProbing = false;
    int proxyRedirectsAtStart = 0;
};

std::string flagValue(const std::string &line, const std::string &word) {
    const std::size_t at = line.find(" " + word + " ");
    if (at == std::string::npos) return std::string();
    const std::size_t from = at + word.size() + 2;
    const std::size_t end = line.find(' ', from);
    return line.substr(from, end == std::string::npos ? std::string::npos : end - from);
}

void mark(RoundState &s, const std::string &event, const std::string &branch, const std::string &keys) {
    const std::string key = branch.empty() ? event : event + "." + branch;
    if (s.r.t.count(key)) return;   // first occurrence only
    const long long t = s.clock.elapsed();
    s.r.t[key] = t;
    std::printf("sailtube-timing: %s t=%lld%s%s run=%d round=%d video=%s mode=%s%s\n", event.c_str(), t,
                branch.empty() ? "" : " branch=", branch.c_str(), s.r.run, s.r.round, s.r.video.c_str(), s.r.mode.c_str(),
                keys.c_str());
    std::fflush(stdout);
}

std::string formatLabel(const StreamFormat &f) {
    std::string label = std::to_string(f.itag) + ":" + f.codecFamily() + ":" + f.container();
    if (f.hasVideo) label += ":" + std::to_string(f.height) + "p" + std::to_string(f.fps);
    label += ":" + std::to_string(f.bitrate / 1000) + "k";
    return label;
}

// SEL-02 for the host: the highest rung at or below the ceiling (the component already applied the
// ceiling), lower frame rate first at equal height, then the higher bitrate. `family` restricts the
// codec ("vp9" here: the container has no software H.264 decoder; the phone would take H.264).
const StreamFormat *pickVideo(const StreamSet &set, int ceiling, const std::string &family) {
    const StreamFormat *best = nullptr;
    for (std::size_t i = 0; i < set.video.size(); ++i) {
        const StreamFormat &f = set.video[i];
        if (!family.empty() && f.codecFamily() != family) continue;
        if (f.height > ceiling || f.contentLength <= 0) continue;
        if (!best) { best = &f; continue; }
        if (f.height != best->height) { if (f.height > best->height) best = &f; continue; }
        const bool lowA = f.fps <= 30, lowB = best->fps <= 30;
        if (lowA != lowB) { if (lowA) best = &f; continue; }
        if (f.bitrate > best->bitrate) best = &f;
    }
    return best;
}

const StreamFormat *pickAudio(const StreamSet &set, const std::string &family) {
    for (std::size_t i = 0; i < set.audio.size(); ++i)
        if (set.audio[i].contentLength > 0 && (family.empty() || set.audio[i].codecFamily() == family)) return &set.audio[i];
    return nullptr;
}

Container containerOf(const StreamFormat &f) { return f.container() == "webm" ? Container::Matroska : Container::IsoBmff; }

// "First parsed sample" (fallback when no decodable rung is offered): souphttpsrc ! parsebin !
// fakesink on the proxied address; first byte at the source pad, first buffer after the demuxer.
class ParsedProbe {
public:
    explicit ParsedProbe(const QElapsedTimer *clock) : clock_(clock), firstByte_(-1), firstSample_(-1) {}
    ~ParsedProbe() { stop(); }
    bool start(const QString &url) {
        const QString description = QStringLiteral("souphttpsrc name=src location=%1 ! parsebin ! fakesink name=sink signal-handoffs=true sync=false async=false").arg(url);
        GError *error = nullptr;
        pipeline_ = gst_parse_launch(description.toUtf8().constData(), &error);
        if (error) { g_error_free(error); if (pipeline_) gst_object_unref(pipeline_); pipeline_ = nullptr; return false; }
        if (!pipeline_) return false;
        GstElement *src = gst_bin_get_by_name(GST_BIN(pipeline_), "src");
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline_), "sink");
        if (src) {
            GstPad *pad = gst_element_get_static_pad(src, "src");
            if (pad) { gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, &ParsedProbe::onSourceBuffer, this, nullptr); gst_object_unref(pad); }
            gst_object_unref(src);
        }
        if (sink) { g_signal_connect(sink, "handoff", G_CALLBACK(&ParsedProbe::onHandoff), this); gst_object_unref(sink); }
        return gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
    }
    void stop() {
        if (!pipeline_) return;
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
    bool failed() const {
        if (!pipeline_) return true;
        GstBus *bus = gst_element_get_bus(pipeline_);
        GstMessage *m = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
        gst_object_unref(bus);
        if (!m) return false;
        gst_message_unref(m);
        return true;
    }
    long long firstByte() const { return firstByte_.load(); }
    long long firstSample() const { return firstSample_.load(); }

private:
    static GstPadProbeReturn onSourceBuffer(GstPad *, GstPadProbeInfo *, gpointer user) {
        ParsedProbe *self = static_cast<ParsedProbe *>(user);
        long long none = -1;
        self->firstByte_.compare_exchange_strong(none, self->clock_->elapsed());
        return GST_PAD_PROBE_REMOVE;
    }
    static void onHandoff(GstElement *, GstBuffer *, GstPad *, gpointer user) {
        ParsedProbe *self = static_cast<ParsedProbe *>(user);
        long long none = -1;
        self->firstSample_.compare_exchange_strong(none, self->clock_->elapsed());
    }
    const QElapsedTimer *clock_;
    GstElement *pipeline_ = nullptr;
    std::atomic<long long> firstByte_, firstSample_;
};

// The pieces of one process: one resolver (warm state lives here), one loopback proxy per branch
// (so the proxy's log lines can be told apart; the app uses one proxy, each stream has its own
// upstream connection either way) and one player.
class Harness {
public:
    explicit Harness(const Settings &settings)
        : settings_(settings), resolver_(resolverOptions(this)), proxyVideo_(proxyOptions(this, "video")),
          proxyAudio_(proxyOptions(this, "audio")) {
        request_.audioOnly = false;
        request_.maxVideoHeight = settings.height;
        request_.codecPreference.push_back("opus");
        request_.hardwareDecode = false;
        QObject::connect(&player_, &VideoPlayer::firstByte, &player_, [this](const QString &branch, qint64) {
            if (cur_) mark(*cur_, "first-byte", branch.toStdString(), "");
        });
        QObject::connect(&player_, &VideoPlayer::timing, &player_, [this](const QString &marker, qint64) {
            if (cur_ && marker == QLatin1String("preroll")) mark(*cur_, "preroll-done", "", "");
        });
        QObject::connect(&player_, &VideoPlayer::firstFrame, &player_, [this]() {
            if (cur_) mark(*cur_, "first-frame", "", " decoder=sw sink=appsink");
        });
        QObject::connect(&player_, &VideoPlayer::failed, &player_, [this](const QString &reason, bool network, int branch) {
            if (!cur_ || playerFailed_) return;
            playerFailed_ = true;
            failReason_ = network ? "player_network" : "player_error";
            std::fprintf(stderr, "perf: player failed (branch %d): %s\n", branch, qPrintable(reason.left(200)));
        });
    }

    // A first resolve of another video: mints the visitor id and leaves the pooled connection open.
    bool prime(const std::string &video, int *ms) {
        std::shared_ptr<ResolveOutcome> out(new ResolveOutcome);
        std::shared_ptr<bool> done(new bool(false));
        QElapsedTimer t;
        t.start();
        resolver_.resolve(video, request_, [out, done](const ResolveOutcome &o) { *out = o; *done = true; });
        waitUntil([done]() { return *done; }, settings_.resolveTimeoutMs);
        *ms = int(t.elapsed());
        return *done && out->ok();
    }

    void invalidate(const std::string &video) { resolver_.invalidate(video); }

    // ---- Shorts: a swipe is a tap on the next Short while another one plays (modes ahead / swipecold / back) ----
    // What a swipe needs to start a video that was prepared ahead: the rungs and the loopback addresses.
    struct Prepared {
        std::string video;
        bool decodable = false;
        StreamFormat videoFormat, audioFormat;
        QUrl videoUrl, audioUrl;
        long long durationMs = 0;
    };

    typedef Prepared PreparedStreams;
    // The next Short, prepared while the current one plays: resolve, rung probe, proxy opens; no player.
    bool prepareAhead(const std::string &video, int run, int round, PreparedStreams *out, long long *ms) {
        RoundState s;
        s.r.video = video; s.r.mode = "prepare"; s.r.run = run; s.r.round = round;
        s.proxyRedirectsAtStart = proxyVideo_.counters().redirects + proxyAudio_.counters().redirects;
        RoundState *previous = cur_;
        cur_ = &s;
        s.clock.start();
        mark(s, "tap", "", " net=host");
        mark(s, "resolve-start", "", "");
        const bool ok = prepareStreams(s, out);
        cur_ = previous;
        *ms = s.clock.elapsed();
        std::printf("sailtube-perf: prepared run=%d round=%d video=%s ok=%d reason=%s ms=%lld\n", run, round, video.c_str(), ok ? 1 : 0,
                    s.r.reason.c_str(), (long long)*ms);
        std::fflush(stdout);
        return ok;
    }

    // The swipe: tap now, the current Short is replaced. `prepared` null: resolved on the way (the
    // resolver's cache answers when it holds the video). The previous Short's streams are the caller's
    // to close (closeVideo) once the picture is up; the player is left playing the new Short.
    RoundResult swipe(const std::string &video, const std::string &mode, int run, int round, PreparedStreams *prepared) {
        RoundState s;
        s.r.video = video; s.r.mode = mode; s.r.run = run; s.r.round = round;
        s.proxyRedirectsAtStart = proxyVideo_.counters().redirects + proxyAudio_.counters().redirects;
        playerFailed_ = false;
        failReason_.clear();
        cur_ = &s;
        s.clock.start();
        mark(s, "tap", "", " net=host");
        if (prepared) {
            playPrepared(s, *prepared);
        } else {
            mark(s, "resolve-start", "", "");
            PreparedStreams fresh;
            if (prepareStreams(s, &fresh)) playPrepared(s, fresh);
        }
        cur_ = nullptr;
        s.r.proxyRedirects = proxyVideo_.counters().redirects + proxyAudio_.counters().redirects - s.proxyRedirectsAtStart;
        std::printf("sailtube-perf: result run=%d round=%d video=%s mode=%s ok=%d reason=%s probeRedirects=%d rungRedirects=%d proxyRedirects=%d\n",
                    run, round, video.c_str(), mode.c_str(), s.r.ok ? 1 : 0, s.r.reason.c_str(), s.r.probeRedirects,
                    s.r.rungRedirects, s.r.proxyRedirects);
        std::fflush(stdout);
        return s.r;
    }
    void closeVideo(const std::string &video) {
        proxyVideo_.closeVideo(video);
        proxyAudio_.closeVideo(video);
    }
    void stopPlayer() { player_.stop(); }

    RoundResult run(const std::string &video, const std::string &mode, int run, int round) {
        RoundState s;
        s.r.video = video; s.r.mode = mode; s.r.run = run; s.r.round = round;
        s.proxyRedirectsAtStart = proxyVideo_.counters().redirects + proxyAudio_.counters().redirects;
        playerFailed_ = false;
        failReason_.clear();
        cur_ = &s;
        s.clock.start();
        mark(s, "tap", "", " net=host");
        mark(s, "resolve-start", "", "");
        execute(s);
        // Tear down before the next round: the player, then every stream of the video.
        player_.stop();
        proxyVideo_.closeVideo(video);
        proxyAudio_.closeVideo(video);
        cur_ = nullptr;
        spin(150);
        s.r.proxyRedirects = proxyVideo_.counters().redirects + proxyAudio_.counters().redirects - s.proxyRedirectsAtStart;
        std::printf("sailtube-perf: result run=%d round=%d video=%s mode=%s ok=%d reason=%s probeRedirects=%d rungRedirects=%d proxyRedirects=%d\n",
                    run, round, video.c_str(), mode.c_str(), s.r.ok ? 1 : 0, s.r.reason.c_str(), s.r.probeRedirects,
                    s.r.rungRedirects, s.r.proxyRedirects);
        std::fflush(stdout);
        return s.r;
    }

private:
    static FastStreamResolver::Options resolverOptions(Harness *self) {
        FastStreamResolver::Options o;
        o.log = [self](const std::string &line) { self->onResolverLog(line); };
        return o;
    }
    static MediaProxy::Options proxyOptions(Harness *self, const char *branch) {
        MediaProxy::Options o;
        const std::string b(branch);
        o.log = [self, b](const std::string &line) { self->onProxyLog(b, line); };
        // A/B of the pinned proxy's start pre-buffer and tail cache (tools/perf_playback_ab.sh).
        if (qgetenv("SAILTUBE_PREBUFFER") == "0") {
            o.prebufferBytes = 0;
            o.tailBytes = 0;
        }
        return o;
    }

    void onResolverLog(const std::string &line) {
        if (!cur_) return;
        if (line.find("phase visitor ok") != std::string::npos) {
            mark(*cur_, "visitor-done", "", " ok=" + flagValue(line, "ok"));
        } else if (line.find("phase player ok") != std::string::npos) {
            const std::string reused = flagValue(line, "reused");
            cur_->r.reused = reused == "true";
            mark(*cur_, "post-done", "", " ok=" + flagValue(line, "ok") + (reused.empty() ? "" : " reused=" + reused));
        } else if (line.find("phase probe redirect hop") != std::string::npos) {
            if (cur_->rungProbing) ++cur_->r.rungRedirects; else ++cur_->r.probeRedirects;
        } else if (line.find("phase probe verdict") != std::string::npos && line.find("rung other") == std::string::npos) {
            mark(*cur_, "probe-done", "", " verdict=" + flagValue(line, "verdict") + " redirects=" + std::to_string(cur_->r.probeRedirects));
        }
    }

    void onProxyLog(const std::string &branch, const std::string &line) {
        if (!cur_) return;
        if (line.find("phase serve status 20") != std::string::npos) mark(*cur_, "proxy-request", branch, "");
        else if (line.find("phase first_byte") != std::string::npos)
            mark(*cur_, "upstream-first-byte", branch, " sinceRequestMs=" + flagValue(line, "since_request_ms"));
    }

    void skip(RoundState &s, const std::string &reason) { s.r.ok = false; s.r.reason = reason; }

    void execute(RoundState &s) {
        Prepared prepared;
        if (!prepareStreams(s, &prepared)) return;
        playPrepared(s, prepared);
    }

    // The resolve, the host's rung probe and the proxy opens (everything before the player).
    bool prepareStreams(RoundState &s, Prepared *prepared) {
        const std::string video = s.r.video;
        std::shared_ptr<ResolveOutcome> outcome(new ResolveOutcome);
        std::shared_ptr<bool> done(new bool(false));
        const FastStreamResolver::RequestId id =
            resolver_.resolve(video, request_, [outcome, done](const ResolveOutcome &o) { *outcome = o; *done = true; });
        if (!waitUntil([done]() { return *done; }, settings_.resolveTimeoutMs)) {
            resolver_.cancel(id);
            skip(s, "resolve_timeout");
            return false;
        }
        if (!outcome->ok()) {
            skip(s, std::string(sp::domain::failureLabel(outcome->failure)) + "/" + (outcome->reason.empty() ? "-" : outcome->reason));
            return false;
        }
        const StreamSet &set = outcome->streams;
        const sp::domain::StreamTimings &tm = set.timings;
        mark(s, "resolve-done", "", std::string(" path=") + sp::domain::pathLabel(set.path) + " totalMs=" + std::to_string(tm.totalMs) +
                                        " visitorMs=" + std::to_string(tm.visitorMs) + " playerMs=" + std::to_string(tm.playerMs) +
                                        " parseMs=" + std::to_string(tm.parseMs) + " probeMs=" + std::to_string(tm.probeMs) +
                                        " reused=" + (tm.reusedConnection ? "1" : "0"));
        if (titles_.insert(video).second)
            std::printf("sailtube-perf: video id=%s durationMs=%lld title=\"%s\"\n", video.c_str(), (long long)set.metadata.durationMs,
                        set.metadata.title.c_str());

        // Rungs: VP9 + Opus decode in the container; otherwise the "first parsed sample" fallback.
        const StreamFormat *v = pickVideo(set, settings_.height, "vp9");
        const StreamFormat *a = pickAudio(set, "opus");
        const bool decodable = v && a && v->container() == "webm" && a->container() == "webm";
        if (!v) v = pickVideo(set, settings_.height, std::string());
        if (!a) a = pickAudio(set, std::string());
        if (!v || !a) { skip(s, !v ? "no_video_rung" : "no_audio_rung"); return false; }
        std::printf("sailtube-perf: rungs run=%d video=%s v=%s a=%s decode=%s ladderVideo=%d ladderAudio=%d\n", s.r.run, video.c_str(),
                    formatLabel(*v).c_str(), formatLabel(*a).c_str(), decodable ? "vp9+opus" : "parsed-only", int(set.video.size()),
                    int(set.audio.size()));

        // RES-06: a delivered rung other than the component's own top is probed by the host first.
        StreamFormat videoFormat = *v, audioFormat = *a;
        std::vector<StreamFormat *> toProbe;
        // A rung probed earlier in this process is not probed again; the address that answered is used.
        if (verified_.count(videoFormat.url)) videoFormat.url = verified_[videoFormat.url];
        else if (!set.video.empty() && v->url != set.video.front().url) toProbe.push_back(&videoFormat);
        if (verified_.count(audioFormat.url)) audioFormat.url = verified_[audioFormat.url];
        else if (a->url != set.probedUrl) toProbe.push_back(&audioFormat);
        if (!toProbe.empty()) {
            struct Join { int pending = 0; int refused = 0; std::string verdict = "accept"; };
            std::shared_ptr<Join> join(new Join);
            join->pending = int(toProbe.size());
            std::shared_ptr<std::vector<std::string> > finals(new std::vector<std::string>(toProbe.size()));
            s.rungProbing = true;
            for (std::size_t i = 0; i < toProbe.size(); ++i) {
                resolver_.probeAddress(toProbe[i]->url, set.userAgent, toProbe[i]->contentLength,
                    FastStreamResolver::ProbeDone([join, finals, i](FastStreamResolver::ProbeVerdict verdict, int, int, const std::string &finalUrl) {
                        typedef FastStreamResolver::ProbeVerdict PV;
                        if (verdict == PV::Refused || verdict == PV::Unusable) { ++join->refused; join->verdict = verdict == PV::Refused ? "reject" : "unusable"; }
                        else if (verdict == PV::NoVerdict && join->verdict == "accept") join->verdict = "none";
                        if (verdict == PV::Accepted) (*finals)[i] = finalUrl;
                        --join->pending;
                    }));
            }
            const bool answered = waitUntil([join]() { return join->pending == 0; }, settings_.resolveTimeoutMs);
            s.rungProbing = false;
            mark(s, "rung-probe-done", "", " verdict=" + (answered ? join->verdict : std::string("timeout")) + " rungs=" +
                                               std::to_string(toProbe.size()) + " redirects=" + std::to_string(s.r.rungRedirects));
            if (!answered) { skip(s, "rung_probe_timeout"); return false; }
            if (join->refused) { skip(s, "rung_" + join->verdict); return false; }
            for (std::size_t i = 0; i < toProbe.size(); ++i) {
                const std::string answered = (*finals)[i].empty() ? toProbe[i]->url : (*finals)[i];
                verified_[toProbe[i]->url] = answered;
                toProbe[i]->url = answered;
            }
        }

        // MediaProxy for both branches, then the player (both first fetches start in parallel).
        const QUrl videoUrl = proxyVideo_.open(video, sourceOf(videoFormat, set), refresherFor(video, videoFormat.itag));
        const QUrl audioUrl = proxyAudio_.open(video, sourceOf(audioFormat, set), refresherFor(video, audioFormat.itag));
        if (videoUrl.isEmpty() || audioUrl.isEmpty()) { skip(s, "proxy_refused"); return false; }
        mark(s, "proxy-open", "", "");
        prepared->video = video;
        prepared->decodable = decodable;
        prepared->videoFormat = videoFormat;
        prepared->audioFormat = audioFormat;
        prepared->videoUrl = videoUrl;
        prepared->audioUrl = audioUrl;
        prepared->durationMs = set.metadata.durationMs;
        return true;
    }

    // The player (or the parsed-sample probes) on prepared streams; the picture time is measured here.
    void playPrepared(RoundState &s, const Prepared &prepared) {
        const bool decodable = prepared.decodable;
        const QUrl &videoUrl = prepared.videoUrl, &audioUrl = prepared.audioUrl;
        const StreamFormat &videoFormat = prepared.videoFormat, &audioFormat = prepared.audioFormat;

        if (decodable) {
            PlayerSources p;
            p.mode = PlayMode::Dual;
            p.videoId = prepared.video;
            p.video = spec(videoUrl, containerOf(videoFormat));
            p.audio = spec(audioUrl, containerOf(audioFormat));
            p.metadataDurationMs = prepared.durationMs;
            p.audioSink.factory = "fakesink";
            PropertySetting sync;
            sync.name = "sync";
            sync.value = "true";
            p.audioSink.properties.push_back(sync);
            player_.load(p);
            const bool pictured = waitUntil([this, &s]() {
                return playerFailed_ || (s.r.t.count("first-frame") && s.r.t.count("first-byte.video") && s.r.t.count("first-byte.audio"));
            }, settings_.roundTimeoutMs);
            if (playerFailed_) { skip(s, failReason_); return; }
            if (!pictured) { skip(s, s.r.t.count("first-frame") ? "first_byte_timeout" : "first_frame_timeout"); return; }
            // The first async-done can come just after the first frame: give it a moment (not part of the picture time).
            waitUntil([this, &s]() { return playerFailed_ || s.r.t.count("preroll-done") > 0; }, 1000);
        } else {
            ParsedProbe pv(&s.clock), pa(&s.clock);
            if (!pv.start(videoUrl.toString()) || !pa.start(audioUrl.toString())) { skip(s, "parse_pipeline"); return; }
            bool failed = false;
            const bool parsed = waitUntil([&]() {
                if (pv.firstByte() >= 0) markAt(s, "first-byte", "video", pv.firstByte());
                if (pa.firstByte() >= 0) markAt(s, "first-byte", "audio", pa.firstByte());
                if (pv.firstSample() >= 0) markAt(s, "first-parsed-sample", "video", pv.firstSample());
                if (pa.firstSample() >= 0) markAt(s, "first-parsed-sample", "audio", pa.firstSample());
                failed = pv.failed() || pa.failed();
                return failed || (s.r.t.count("first-parsed-sample.video") && s.r.t.count("first-parsed-sample.audio"));
            }, settings_.roundTimeoutMs);
            if (failed) { skip(s, "parse_error"); return; }
            if (!parsed) { skip(s, "first_sample_timeout"); return; }
        }
        s.r.ok = true;
    }

    // An event measured on another thread (the parsed-sample probes): printed with its own time.
    void markAt(RoundState &s, const std::string &event, const std::string &branch, long long t) {
        const std::string key = event + "." + branch;
        if (s.r.t.count(key)) return;
        s.r.t[key] = t;
        std::printf("sailtube-timing: %s t=%lld branch=%s run=%d round=%d video=%s mode=%s\n", event.c_str(), t, branch.c_str(),
                    s.r.run, s.r.round, s.r.video.c_str(), s.r.mode.c_str());
        std::fflush(stdout);
    }

    static SourceSpec spec(const QUrl &url, Container container) {
        SourceSpec s;
        s.uri = url.toString().toStdString();
        s.container = container;
        return s;
    }

    static MediaProxy::Source sourceOf(const StreamFormat &f, const StreamSet &set) {
        MediaProxy::Source s;
        s.url = f.url;
        s.userAgent = set.userAgent;
        s.contentLength = f.contentLength;
        s.mimeType = f.mimeType;
        s.expiresAtUnixMs = f.expiresAtUnixMs;
        return s;
    }

    // RES-12: a fresh address of the same format (same itag), or a failure.
    MediaProxy::Refresher refresherFor(const std::string &video, int itag) {
        FastStreamResolver *resolver = &resolver_;
        const StreamRequest request = request_;
        return [resolver, video, itag, request](const std::function<void(const MediaProxy::FreshSource &)> &deliver) {
            resolver->invalidate(video);
            resolver->resolve(video, request, [deliver, itag](const ResolveOutcome &o) {
                MediaProxy::FreshSource fresh;
                if (o.ok()) {
                    const std::vector<StreamFormat> *lists[] = {&o.streams.video, &o.streams.audio};
                    for (int l = 0; l < 2 && !fresh.ok; ++l)
                        for (std::size_t i = 0; i < lists[l]->size() && !fresh.ok; ++i)
                            if ((*lists[l])[i].itag == itag) { fresh.ok = true; fresh.source = sourceOf((*lists[l])[i], o.streams); }
                }
                deliver(fresh);
            });
        };
    }

    const Settings &settings_;
    StreamRequest request_;
    FastStreamResolver resolver_;
    MediaProxy proxyVideo_, proxyAudio_;
    VideoPlayer player_;
    RoundState *cur_ = nullptr;
    bool playerFailed_ = false;
    std::string failReason_;
    std::map<std::string, std::string> verified_;   // rung address -> address that answered (the adapter remembers verdicts)
    std::set<std::string> titles_;
};

// ---- Child process output -> RoundResult ----------------------------------------------------------

std::map<std::string, std::string> keyValues(const std::string &line) {
    std::map<std::string, std::string> kv;
    std::stringstream in(line);
    std::string token;
    while (in >> token) {
        const std::size_t eq = token.find('=');
        if (eq != std::string::npos && eq > 0) kv[token.substr(0, eq)] = token.substr(eq + 1);
    }
    return kv;
}

RoundResult parseChild(const std::string &output, const std::string &video, int run, int round) {
    RoundResult r;
    r.video = video; r.mode = "cold"; r.run = run; r.round = round; r.reason = "child_no_result";
    std::stringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        const std::string timing = "sailtube-timing: ", result = "sailtube-perf: result ";
        if (line.compare(0, timing.size(), timing) == 0) {
            std::stringstream words(line.substr(timing.size()));
            std::string event;
            words >> event;
            std::map<std::string, std::string> kv = keyValues(line);
            const std::string key = kv.count("branch") ? event + "." + kv["branch"] : event;
            if (kv.count("t")) r.t[key] = std::atoll(kv["t"].c_str());
            if (event == "post-done" && kv["reused"] == "true") r.reused = true;
        } else if (line.compare(0, result.size(), result) == 0) {
            std::map<std::string, std::string> kv = keyValues(line);
            r.ok = kv["ok"] == "1";
            r.reason = kv["reason"];
            r.probeRedirects = std::atoi(kv["probeRedirects"].c_str());
            r.rungRedirects = std::atoi(kv["rungRedirects"].c_str());
            r.proxyRedirects = std::atoi(kv["proxyRedirects"].c_str());
        }
    }
    return r;
}

RoundResult runColdChild(const Settings &settings, const std::string &video, int run, int round) {
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    QStringList args;
    args << "--child" << QString::fromStdString(video) << "--run" << QString::number(run) << "--round" << QString::number(round)
         << "--height" << QString::number(settings.height);
    child.start(QCoreApplication::applicationFilePath(), args);
    const int budget = settings.resolveTimeoutMs * 2 + settings.roundTimeoutMs + 20000;
    if (!child.waitForStarted(10000)) {
        RoundResult r; r.video = video; r.mode = "cold"; r.run = run; r.round = round; r.reason = "child_start";
        return r;
    }
    if (!child.waitForFinished(budget)) { child.kill(); child.waitForFinished(5000); }
    const std::string output = child.readAllStandardOutput().toStdString();
    std::fputs(output.c_str(), stdout);
    std::fflush(stdout);
    RoundResult r = parseChild(output, video, run, round);
    if (child.exitStatus() != QProcess::NormalExit && r.reason == "child_no_result") r.reason = "child_crash_or_timeout";
    return r;
}

// ---- Summary ---------------------------------------------------------------------------------------

long long percentile(std::vector<long long> v, double p) {   // nearest rank
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    std::size_t rank = std::size_t(std::ceil(p * double(v.size())));
    if (rank < 1) rank = 1;
    if (rank > v.size()) rank = v.size();
    return v[rank - 1];
}

long long pictureOf(const RoundResult &r) {
    std::map<std::string, long long>::const_iterator it = r.t.find("first-frame");
    if (it == r.t.end()) it = r.t.find("first-parsed-sample.video");
    return it == r.t.end() ? -1 : it->second;
}

long long valueOr(const RoundResult &r, const char *key, long long fallback) {
    std::map<std::string, long long>::const_iterator it = r.t.find(key);
    return it == r.t.end() ? fallback : it->second;
}

const char *const kStages[] = {"visitor-done", "post-done", "probe-done", "resolve-done", "rung-probe-done", "proxy-open",
                               "proxy-request.video", "proxy-request.audio", "upstream-first-byte.video",
                               "upstream-first-byte.audio", "first-byte.video", "first-byte.audio", "preroll-done",
                               "first-frame", "first-parsed-sample.video", "first-parsed-sample.audio"};

// Critical-path segments; a stage that did not happen in a round (no visitor mint when warm, no
// network at all on a cache hit) takes the previous stage's time, so the segments add up to the picture.
struct Segments { long long v[7]; };
const char *const kSegmentNames[] = {"visitor id", "/player POST", "probe (top rungs)", "resolve hand-over",
                                     "rung probe (host)", "first bytes (both)", "demux+decode+preroll"};

Segments segmentsOf(const RoundResult &r) {
    const long long picture = pictureOf(r);
    long long points[8];
    points[0] = 0;
    points[1] = valueOr(r, "visitor-done", points[0]);
    points[2] = valueOr(r, "post-done", points[1]);
    points[3] = valueOr(r, "probe-done", points[2]);
    points[4] = valueOr(r, "resolve-done", points[3]);
    points[5] = valueOr(r, "proxy-open", points[4]);
    points[6] = std::max(valueOr(r, "first-byte.video", points[5]), valueOr(r, "first-byte.audio", points[5]));
    points[7] = picture;
    Segments s;
    for (int i = 0; i < 7; ++i) s.v[i] = std::max(0LL, points[i + 1] - points[i]);
    return s;
}

void summary(const std::vector<RoundResult> &results, const std::vector<std::string> &modes) {
    std::printf("\nsailtube-perf: summary (ms from the tap; nearest-rank median / p90 over the rounds that reached the picture)\n");
    for (std::size_t m = 0; m < modes.size(); ++m) {
        std::vector<const RoundResult *> ok;
        int skipped = 0;
        std::map<std::string, int> reasons;
        int probeRedirects = 0, rungRedirects = 0, proxyRedirects = 0, reused = 0;
        for (std::size_t i = 0; i < results.size(); ++i) {
            if (results[i].mode != modes[m]) continue;
            if (results[i].ok) {
                ok.push_back(&results[i]);
                probeRedirects += results[i].probeRedirects; rungRedirects += results[i].rungRedirects;
                proxyRedirects += results[i].proxyRedirects; reused += results[i].reused ? 1 : 0;
            } else { ++skipped; ++reasons[results[i].reason]; }
        }
        std::printf("\n== mode=%s  rounds ok=%d skipped=%d", modes[m].c_str(), int(ok.size()), skipped);
        for (std::map<std::string, int>::const_iterator it = reasons.begin(); it != reasons.end(); ++it)
            std::printf("  [%s x%d]", it->first.c_str(), it->second);
        std::printf("\n   redirects followed: probe %d, rung probe %d, proxy %d; /player on a reused connection: %d of %d\n",
                    probeRedirects, rungRedirects, proxyRedirects, reused, int(ok.size()));
        if (ok.empty()) continue;
        std::printf("   %-28s %4s %8s %8s\n", "stage", "n", "median", "p90");
        for (std::size_t k = 0; k < sizeof(kStages) / sizeof(kStages[0]); ++k) {
            std::vector<long long> values;
            for (std::size_t i = 0; i < ok.size(); ++i) {
                std::map<std::string, long long>::const_iterator it = ok[i]->t.find(kStages[k]);
                if (it != ok[i]->t.end()) values.push_back(it->second);
            }
            if (values.empty()) continue;
            std::printf("   %-28s %4d %8lld %8lld\n", kStages[k], int(values.size()), percentile(values, 0.5), percentile(values, 0.9));
        }
        std::vector<long long> totals;
        std::vector<std::vector<long long> > segs(7);
        long long sumTotal = 0, sumSeg[7] = {0, 0, 0, 0, 0, 0, 0};
        for (std::size_t i = 0; i < ok.size(); ++i) {
            const long long picture = pictureOf(*ok[i]);
            totals.push_back(picture);
            sumTotal += picture;
            const Segments s = segmentsOf(*ok[i]);
            for (int j = 0; j < 7; ++j) { segs[j].push_back(s.v[j]); sumSeg[j] += s.v[j]; }
        }
        std::printf("   %-28s %4d %8lld %8lld\n", "TAP TO PICTURE", int(totals.size()), percentile(totals, 0.5), percentile(totals, 0.9));
        std::printf("   %-28s %8s %8s %7s\n", "critical-path segment", "median", "p90", "share");
        for (int j = 0; j < 7; ++j)
            std::printf("   %-28s %8lld %8lld %6.1f%%\n", kSegmentNames[j], percentile(segs[j], 0.5), percentile(segs[j], 0.9),
                        sumTotal > 0 ? 100.0 * double(sumSeg[j]) / double(sumTotal) : 0.0);
        // Per video.
        std::map<std::string, std::vector<long long> > perVideo;
        for (std::size_t i = 0; i < ok.size(); ++i) perVideo[ok[i]->video].push_back(pictureOf(*ok[i]));
        std::printf("   per video (tap to picture):");
        for (std::map<std::string, std::vector<long long> >::const_iterator it = perVideo.begin(); it != perVideo.end(); ++it)
            std::printf("  %s n=%d med=%lld", it->first.c_str(), int(it->second.size()), percentile(it->second, 0.5));
        std::printf("\n");
    }
}

void usage() {
    std::printf("usage: perf_playback [--rounds N] [--videos id,id,...] [--primer id] [--height H]\n"
                "                     [--modes cold,warm,cached,shorts] [--round-timeout MS] [--pause MS] [--watch MS]\n"
                "  shorts: swipes between Shorts with the previous one playing: ahead (the next Short prepared while the\n"
                "          current one played), swipecold (a Short nobody prepared) and back (to the one just left, resolver cache).\n"
                "          Needs 3 or more videos. SAILTUBE_PREBUFFER=0 starts the proxy without its start pre-buffer.\n");
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    Settings settings;
    std::string modeList = "cold,warm,cached";
    std::string videoList;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        const bool hasValue = i + 1 < argc;
        if (a == "--rounds" && hasValue) settings.rounds = std::max(1, std::atoi(argv[++i]));
        else if (a == "--videos" && hasValue) videoList = argv[++i];
        else if (a == "--primer" && hasValue) settings.primer = argv[++i];
        else if (a == "--height" && hasValue) settings.height = std::max(144, std::atoi(argv[++i]));
        else if (a == "--modes" && hasValue) modeList = argv[++i];
        else if (a == "--round-timeout" && hasValue) settings.roundTimeoutMs = std::max(1000, std::atoi(argv[++i]));
        else if (a == "--pause" && hasValue) settings.pauseMs = std::max(0, std::atoi(argv[++i]));
        else if (a == "--watch" && hasValue) settings.watchMs = std::max(0, std::atoi(argv[++i]));
        else if (a == "--child" && hasValue) settings.childVideo = argv[++i];
        else if (a == "--run" && hasValue) settings.childRun = std::atoi(argv[++i]);
        else if (a == "--round" && hasValue) settings.childRound = std::atoi(argv[++i]);
        else if (a == "--help" || a == "-h") { usage(); return 0; }
        else { std::printf("unknown argument: %s\n", a.c_str()); usage(); return 2; }
    }

    QString gstError;
    if (!nordtile::player::GstEngine::initialize(&gstError)) {
        std::printf("perf_playback: GStreamer unavailable: %s\n", qPrintable(gstError));
        return 2;
    }
    if (!QSslSocket::supportsSsl()) { std::printf("perf_playback: no TLS support in Qt Network\n"); return 2; }

    // Child: exactly one cold round in this fresh process.
    if (!settings.childVideo.empty()) {
        Harness harness(settings);
        harness.run(settings.childVideo, "cold", settings.childRun, settings.childRound);
        return 0;
    }

    settings.videos = videoList.empty() ? std::vector<std::string>(kDefaultVideos, kDefaultVideos + 5) : splitList(videoList);
    for (std::size_t i = 0; i < settings.videos.size(); ++i)
        if (!validId(settings.videos[i])) { std::printf("invalid video id: %s\n", settings.videos[i].c_str()); return 2; }
    if (std::find(settings.videos.begin(), settings.videos.end(), settings.primer) != settings.videos.end() || !validId(settings.primer)) {
        std::printf("the primer must be a valid id that is not in the measured list\n");
        return 2;
    }
    const std::vector<std::string> requested = splitList(modeList);
    std::vector<std::string> modes;
    const char *const known[] = {"cold", "warm", "cached"};
    for (int k = 0; k < 3; ++k)
        if (std::find(requested.begin(), requested.end(), known[k]) != requested.end()) modes.push_back(known[k]);
    const bool wantShorts = std::find(requested.begin(), requested.end(), "shorts") != requested.end();
    if (wantShorts) {
        if (settings.videos.size() < 3) { std::printf("the shorts mode needs at least 3 videos\n"); return 2; }
        modes.push_back("ahead"); modes.push_back("swipecold"); modes.push_back("back");
    }
    if (modes.empty()) { std::printf("no known mode in --modes\n"); return 2; }

    std::string videosJoined;
    for (std::size_t i = 0; i < settings.videos.size(); ++i) videosJoined += (i ? "," : "") + settings.videos[i];
    std::string modesJoined;
    for (std::size_t i = 0; i < modes.size(); ++i) modesJoined += (i ? "," : "") + modes[i];
    std::printf("sailtube-perf: start rounds=%d height=%d videos=%s primer=%s modes=%s gst=%s qt=%s arch=%s kernel=%s\n",
                settings.rounds, settings.height, videosJoined.c_str(), settings.primer.c_str(), modesJoined.c_str(),
                gst_version_string(), qVersion(), qPrintable(QSysInfo::currentCpuArchitecture()), qPrintable(QSysInfo::kernelVersion()));
    std::fflush(stdout);

    std::vector<RoundResult> results;
    int run = 0;
    const bool wantCold = std::find(modes.begin(), modes.end(), "cold") != modes.end();
    const bool wantWarm = std::find(modes.begin(), modes.end(), "warm") != modes.end();
    const bool wantCached = std::find(modes.begin(), modes.end(), "cached") != modes.end();

    // Phase A: cold rounds, one fresh process each.
    if (wantCold) {
        for (int round = 1; round <= settings.rounds; ++round)
            for (std::size_t v = 0; v < settings.videos.size(); ++v) {
                results.push_back(runColdChild(settings, settings.videos[v], ++run, round));
                spin(settings.pauseMs);
            }
    }

    // Phase B: one process-long resolver, primed with another video.
    if (wantWarm || wantCached) {
        Harness harness(settings);
        int primeMs = 0;
        const bool primed = harness.prime(settings.primer, &primeMs);
        std::printf("sailtube-perf: primed video=%s ok=%d ms=%d\n", settings.primer.c_str(), primed ? 1 : 0, primeMs);
        std::fflush(stdout);
        for (int round = 1; round <= settings.rounds; ++round)
            for (std::size_t v = 0; v < settings.videos.size(); ++v) {
                const std::string &video = settings.videos[v];
                harness.invalidate(video);
                if (wantWarm) {
                    RoundResult r = harness.run(video, "warm", ++run, round);
                    if (!primed && r.ok) { r.ok = false; r.reason = "primer_failed"; }
                    results.push_back(r);
                    spin(settings.pauseMs);
                } else {
                    int ms = 0;
                    harness.prime(video, &ms);   // fill the cache without measuring
                }
                if (wantCached) {
                    results.push_back(harness.run(video, "cached", ++run, round));
                    spin(settings.pauseMs);
                }
            }
    }

    // Phase C: Shorts. One process-long resolver and one player, as the app has: a Short plays, the
    // next one is prepared meanwhile (ahead), then swiped to; another one nobody prepared (swipecold);
    // then back to the one before (back: its answer is in the resolver's cache).
    if (wantShorts) {
        Harness harness(settings);
        int primeMs = 0;
        const bool primed = harness.prime(settings.primer, &primeMs);
        std::printf("sailtube-perf: primed video=%s ok=%d ms=%d\n", settings.primer.c_str(), primed ? 1 : 0, primeMs);
        std::fflush(stdout);
        const std::size_t n = settings.videos.size();
        for (int round = 1; round <= settings.rounds; ++round)
            for (std::size_t v = 0; v < n; ++v) {
                const std::string &a = settings.videos[v], &b = settings.videos[(v + 1) % n], &c = settings.videos[(v + 2) % n];
                harness.invalidate(a); harness.invalidate(b); harness.invalidate(c);
                // The Short that plays when the sequence begins (not measured).
                const RoundResult start = harness.swipe(a, "start", ++run, round, nullptr);
                if (!start.ok) { harness.stopPlayer(); harness.closeVideo(a); spin(settings.pauseMs); continue; }
                spin(settings.watchMs);
                // ahead: B prepared while A plays, then the swipe.
                Harness::PreparedStreams preparedB;
                long long prepareMs = 0;
                const bool preparedOk = harness.prepareAhead(b, ++run, round, &preparedB, &prepareMs);
                spin(settings.watchMs);
                if (preparedOk) {
                    results.push_back(harness.swipe(b, "ahead", ++run, round, &preparedB));
                    harness.closeVideo(a);
                    spin(settings.watchMs);
                }
                // swipecold: C, which nobody prepared.
                results.push_back(harness.swipe(c, "swipecold", ++run, round, nullptr));
                harness.closeVideo(preparedOk ? b : a);
                spin(settings.watchMs);
                // back: to B, the Short just left (A when B could not be prepared).
                const std::string &previous = preparedOk ? b : a;
                results.push_back(harness.swipe(previous, "back", ++run, round, nullptr));
                harness.closeVideo(c);
                harness.stopPlayer();
                harness.closeVideo(previous);
                spin(settings.pauseMs);
            }
    }

    summary(results, modes);
    std::set<std::string> withNumbers;
    for (std::size_t i = 0; i < results.size(); ++i) if (results[i].ok) withNumbers.insert(results[i].video);
    const std::size_t needed = std::min<std::size_t>(3, settings.videos.size());
    std::printf("\nsailtube-perf: videos-with-numbers=%d of %d (need %d)\n", int(withNumbers.size()), int(settings.videos.size()), int(needed));
    return withNumbers.size() >= needed ? 0 : 1;
}
