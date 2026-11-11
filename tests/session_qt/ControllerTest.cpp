// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the watch-session decision glue (PlaybackController) with a
// fake port and injected time: open -> dual plan -> first frame, candidate
// fallback, recovery, arbiter re-attach / reclaim, audio-resource loss,
// autoplay countdown, resume position, listen-only, the interim hardware
// fallback, speed memory, deferred SponsorBlock skips, MPRIS Stop and the
// cover states; the shared-resolver request fields (RES-10/20, SEL-01/02/08), switches
// re-selected from the same set, prefetch (RES-11), late metadata and the E12 / RES-11 / RES-21
// speculation bounds (speculationAllowed, rowsVisibleDecision), the RES-19 upcoming hold; resolve
// before the old player stops (NFR-PERF-02), queue read-out, a download's link start without
// prefetch (PLY-26 / DL-07), the low-rung start A/B, the error -> setting key mapping, and the
// cover-video decisions (coverVideoActive, the freeze input coverKeepsDecoding, the decode meter),
// and "stream instead" of a playing download (PLY-26 / SEL-07); the Shorts pager decisions (DSC-17
// MAY swipe viewer): loop items, the likelyNext decision table and its first-plan prediction.
// Stall watchdog (StallPolicy in the controller): a standing position is recovered by a seek in
// place, then the candidate's streams reopened, then one fresh resolve, then a retryable failure;
// its guards (paused, audio lost or not granted, seeking, ended, switching, buffering with
// progress, the background freeze), the budget, a failed reopen, local and live items, and that a
// port without a player keeps it off.
// Build: tests/session_qt/run.sh
#include "Clients/Sailfish/Presentation/PlaybackController.h"
#include "Core/Player/Domain/ResumePolicy.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::video;
using namespace nordtile::video::media;
using namespace nordtile::video::sailfish;
namespace player = nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---- Fake port ---------------------------------------------------------------------

struct Call {
    std::string what;
    std::string text;
    std::int64_t value = 0;
};

struct FakePort : PlaybackPort {
    std::vector<Call> calls;
    std::vector<PlaybackLoad> loads;
    std::string resolveId;
    bool resolveFull = false;
    unsigned resolveGeneration = 0;
    int resolves = 0;
    ResolveRequest lastRequest;
    std::vector<ResolveRequest> prefetches;

    void add(const char *w, const std::string &t = std::string(), std::int64_t v = 0) {
        Call c; c.what = w; c.text = t; c.value = v; calls.push_back(c);
    }
    void resolve(const ResolveRequest &r) override {
        resolveId = r.videoId; resolveFull = r.fresh; resolveGeneration = r.generation; lastRequest = r; ++resolves;
        add("resolve", r.videoId, r.fresh);
    }
    void prefetch(const ResolveRequest &r) override { prefetches.push_back(r); add("prefetch", r.videoId); }
    void cancelResolve() override { add("cancelResolve"); }
    void load(const PlaybackLoad &l) override { loads.push_back(l); add("load", l.videoId, l.startPositionMs); }
    void play() override { add("play"); }
    void pause() override { add("pause"); }
    void stopPlayer() override { add("stopPlayer"); }
    void seek(std::int64_t ms) override { add("seek", std::string(), ms); }
    void postSeek(std::int64_t ms) override { add("postSeek", std::string(), ms); }
    void setRate(double r) override { add("setRate", std::string(), static_cast<std::int64_t>(r * 100)); }
    void acquireAudio() override { add("acquireAudio"); }
    void releaseAudio() override { add("releaseAudio"); }
    void savePosition(const std::string &id, std::int64_t ms) override { add("savePosition", id, ms); }
    void rememberRate(double r) override { add("rememberRate", std::string(), static_cast<std::int64_t>(r * 100)); }
    // Stall watchdog: off (as for any port without a player) unless a test turns it on.
    bool hasProgress = false;
    PlayerProgress progressNow;
    int progressAsked = 0;
    bool progress(PlayerProgress *p) override {
        ++progressAsked;
        if (!hasProgress) return false;
        *p = progressNow;
        return true;
    }

    int count(const char *w) const {
        int n = 0;
        for (std::size_t i = 0; i < calls.size(); ++i) n += calls[i].what == w;
        return n;
    }
    int index(const char *w, int from = 0) const {
        for (std::size_t i = static_cast<std::size_t>(from); i < calls.size(); ++i)
            if (calls[i].what == w) return static_cast<int>(i);
        return -1;
    }
    const Call *last(const char *w) const {
        for (std::size_t i = calls.size(); i > 0; --i)
            if (calls[i - 1].what == w) return &calls[i - 1];
        return nullptr;
    }
    void clear() { calls.clear(); loads.clear(); resolves = 0; prefetches.clear(); }
};

// ---- Fixtures ----------------------------------------------------------------------

static MediaFormat fmt(const char *id, const char *ext, const char *v, const char *a, int h, double fps = 30,
                       double kbps = 1000) {
    MediaFormat f;
    f.id = id; f.container = ext; f.protocol = Protocol::Https; f.url = std::string("https://media.example/") + id;
    f.videoCodecTag = v; f.audioCodecTag = a;
    f.videoCodec = parseVideoCodec(v); f.audioCodec = parseAudioCodec(a);
    f.height = h; f.width = h * 16 / 9; f.fps = fps; f.totalKbps = kbps;
    if (f.videoCodec == VideoCodec::None) f.audioKbps = kbps;
    f.headers["User-Agent"] = "Mozilla/5.0 test";
    f.headers["Referer"] = "https://www.youtube.com/";
    return f;
}

static catalog::VideoDetails details(const char *id) {
    catalog::VideoDetails d;
    d.summary.videoId = id;
    d.summary.title = std::string("Title ") + id;
    d.summary.durationSeconds = 600;
    d.summary.liveStatus = LiveStatus::NotLive;
    d.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500),
        fmt("247", "webm", "vp09.00.31.08", "none", 720, 30, 1300),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
        fmt("251", "webm", "none", "opus", 0, 0, 135),
    };
    return d;
}

static ResolveAnswer ok(const char *id) {
    ResolveAnswer a;
    a.details = details(id);
    return a;
}

static CodecSupport cs(bool available, bool hardware, bool software) {
    CodecSupport s; s.available = available; s.hardware = hardware; s.software = software; return s;
}

static DecoderCapabilities software() {
    DecoderCapabilities c;
    c.h264 = cs(true, false, true); c.vp9 = cs(true, false, true);
    return c;
}

struct Rig {
    FakePort port;
    PlaybackController c;
    std::int64_t now = 1000;
    Rig() : c(&port) { c.setCapabilities(software()); }
    void answer(const ResolveAnswer &a) { c.onResolved(now, 1700000000000LL + now, port.resolveId, port.resolveGeneration, a); }
    void answerOk() { answer(ok(port.resolveId.c_str())); }
    // open, resolve, grant, preroll: a playing session.
    int playing(const char *id) {
        const int ctx = c.open(now, id);
        answerOk();
        c.onAudioGranted();
        c.onPlayerStarted(now);
        c.onPlayerPlaying(true);
        return ctx;
    }
    void advance(std::int64_t ms) { now += ms; c.tick(now); }
};

// PLY-06 deep start: a resume far into the video picks the WebM rung (one-request start seek);
// a start near the beginning keeps H.264 (SEL-02 software preference).
static void ply06_deepResumePrefersWebm() {
    Rig r;
    r.c.open(r.now, "DDDDDDDDDDD");
    r.c.setStartPosition(8 * 60 * 1000 + 55 * 1000);  // the run-5 case: 8:55
    r.answerOk();
    CHECK(r.c.candidate() && r.c.candidate()->video.id == "247");
    Rig n;
    n.c.open(n.now, "EEEEEEEEEEE");
    n.c.setStartPosition(10 * 1000);
    n.answerOk();
    CHECK(n.c.candidate() && n.c.candidate()->video.id == "136");
}

// ---- Tests -------------------------------------------------------------------------

// PLY-01, PLY-23, PLT-12: open -> fast resolve -> dual plan -> resource -> PLAYING -> Ready.
static void ply01_openDualPlanFirstFrame() {
    Rig r;
    const int ctx = r.c.open(r.now, "AAAAAAAAAAA");
    CHECK(ctx > 0);
    CHECK(r.c.state() == PlaybackController::Loading);
    CHECK(r.c.status() == PlaybackController::Resolving);
    CHECK(r.port.resolves == 1 && !r.port.resolveFull);
    r.answerOk();
    CHECK(r.c.state() == PlaybackController::Preparing);
    CHECK(r.port.loads.size() == 1);
    const PlaybackLoad &l = r.port.loads[0];
    CHECK(l.mode == player::PlayMode::Dual);
    CHECK(l.video.present() && l.audio.present() && !l.muxed.present());
    CHECK(l.video.container != player::Container::Unknown && l.audio.container != player::Container::Unknown);
    CHECK(l.video.userAgent == "Mozilla/5.0 test");
    CHECK(l.video.extraHeaders.count("Referer") == 1 && l.video.extraHeaders.count("User-Agent") == 0);
    CHECK(l.decode == player::DecodeMode::Software);  // PLY-21 default
    CHECK(l.metadataDurationMs == 600000);
    // PLY-23: the resource is requested; PLAYING only after the grant.
    CHECK(r.port.index("acquireAudio") > r.port.index("load"));
    CHECK(r.port.count("play") == 0);
    r.c.onAudioGranted();
    CHECK(r.port.count("play") == 1);
    r.c.onFirstFrame(r.now);
    CHECK(r.c.state() == PlaybackController::Ready);
    CHECK(r.c.started() && r.c.hasVideo());
    CHECK(!r.c.qualityOptions().empty());       // SEL-03 dual plan has a menu
    CHECK(r.c.playingQualityIndex() >= 0);
    const unsigned changes = r.c.takeChanges();
    CHECK(changes & PlaybackController::DetailsChange);
    CHECK(r.c.takeChanges() == 0);
}

// PLY-18 attempt chain: a candidate that fails before preroll -> the next one;
// exhausted on the fast profile -> full re-extraction.
static void ply18_candidateFailureNextCandidate() {
    Rig r;
    r.c.open(r.now, "BBBBBBBBBBB");
    r.answerOk();
    const std::size_t chain = r.c.chain().size();
    CHECK(chain >= 2);
    const std::string first = r.port.loads[0].video.uri + r.port.loads[0].muxed.uri;
    r.c.onPlayerFailed(r.now, "not-negotiated", false);
    CHECK(r.port.loads.size() == 2);
    CHECK(r.c.attempt() == 1);
    CHECK(r.port.loads[1].video.uri + r.port.loads[1].muxed.uri != first);
    CHECK(r.c.recovery().triesUsed() == 0);  // chain fallbacks spend no recovery budget
    for (std::size_t i = 1; i < chain; ++i) r.c.onPlayerFailed(r.now, "boom", false);
    CHECK(r.port.resolves == 2 && r.port.resolveFull);  // core's full-profile re-extraction
    // RES-07: decode failures of every candidate say nothing certain about the source.
    CHECK(r.port.lastRequest.fault == player::SourceFault::Unspecified);
    CHECK(r.c.status() == PlaybackController::ResolvingFull);
    r.answerOk();
    CHECK(r.c.fullProfile());
    CHECK(r.c.attempt() == 0);
    // The full-profile chain fails too: recovery with 1.5 s backoff, then Retry.
    for (int round = 0; round < 3; ++round) {
        for (std::size_t i = 0; i < chain; ++i) r.c.onPlayerFailed(r.now, "boom", false);
        CHECK(r.c.recovery().state() == player::RecoveryState::Waiting);
        CHECK(r.c.status() == PlaybackController::Reconnecting);
        const int before = r.port.resolves;
        r.advance(1499);
        CHECK(r.port.resolves == before);
        r.advance(1);
        CHECK(r.port.resolves == before + 1);
        r.answerOk();
    }
    for (std::size_t i = 0; i < chain; ++i) r.c.onPlayerFailed(r.now, "boom", false);
    CHECK(r.c.state() == PlaybackController::Failed);
    CHECK(r.c.retryAvailable());
    CHECK(r.c.failure() == PlaybackController::NoPlayableStream);
    // Retry: fresh budget, full profile (signed URLs may have expired).
    const int before = r.port.resolves;
    r.c.retry(r.now);
    CHECK(r.port.resolves == before + 1 && r.port.resolveFull);
    CHECK(r.c.recovery().triesUsed() == 0);
    r.answerOk();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.state() == PlaybackController::Ready);
}

// PLY-18 fault injection: 3 failures during playback, then success; the
// budget comes back after 5 s of clean playback; background defers.
static void ply18_recoveryThreeFailuresThenSuccess() {
    Rig r;
    r.playing("CCCCCCCCCCC");
    r.c.onPosition(r.now, 60000);
    for (int i = 0; i < 3; ++i) {
        r.c.onPlayerFailed(r.now, "Could not read from resource", true);
        CHECK(r.c.state() == PlaybackController::Preparing);
        r.advance(1500);
        CHECK(r.c.state() == PlaybackController::Loading);
        CHECK(r.c.status() == PlaybackController::Reconnecting);
        r.answerOk();
        CHECK(r.port.loads.back().startPositionMs == 60000);  // resumes where it failed
        r.c.onAudioGranted();
        CHECK(r.port.calls.back().what == "play");
        r.c.onPlayerStarted(r.now);
        r.c.onPlayerPlaying(true);
    }
    CHECK(r.c.recovery().triesUsed() == 3);
    r.c.onPosition(r.now, 60000);
    r.c.onPosition(r.now, 65000);
    CHECK(r.c.recovery().triesUsed() == 0);
    CHECK(r.c.state() == PlaybackController::Ready);
    // In the background a fault spends no budget; the foreground recovers at once.
    r.c.setAppActive(r.now, false);
    r.c.onPlayerFailed(r.now, "Could not read from resource", true);
    CHECK(r.c.recovery().state() == player::RecoveryState::Deferred);
    CHECK(r.c.status() == PlaybackController::WaitingForForeground);
    const int before = r.port.resolves;
    r.c.setAppActive(r.now, true);
    CHECK(r.port.resolves == before + 1);
    // A late answer of an older generation is ignored (PLY-10).
    const unsigned current = r.port.resolveGeneration;
    r.c.onResolved(r.now, 0, r.port.resolveId, current - 1, ok("CCCCCCCCCCC"));
    CHECK(r.c.state() == PlaybackController::Loading);
    r.answerOk();
    CHECK(r.c.state() == PlaybackController::Preparing);
}

// PLY-10: leave while playing -> park; reopen -> re-attach (no resolve, no
// load); another video -> save, stop, resolve; A -> B -> back to A reclaims.
static void ply10_arbiterReattachAndReclaim() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    const int a = r.c.claimContext();
    r.answerOk();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    r.c.onPosition(r.now, 30000);
    r.port.clear();
    r.c.closeContext(r.now, a);
    CHECK(r.c.arbiter().parked());
    CHECK(r.port.count("stopPlayer") == 0);
    r.c.open(r.now, "AAAAAAAAAAA");
    r.c.claimContext();
    CHECK(r.port.resolves == 0 && r.port.loads.empty());  // re-attach
    CHECK(!r.c.arbiter().parked());
    const int ctxA = r.c.arbiter().owner();
    // A -> B: A's position is saved before it stops.
    r.c.open(r.now, "BBBBBBBBBBB");
    const int b = r.c.claimContext();
    CHECK(r.port.index("savePosition") >= 0 && r.port.index("savePosition") < r.port.index("stopPlayer"));
    CHECK(r.port.last("savePosition")->text == "AAAAAAAAAAA" && r.port.last("savePosition")->value == 30000);
    CHECK(r.port.resolveId == "BBBBBBBBBBB");
    // A late answer for A is ignored.
    r.c.onResolved(r.now, 0, "AAAAAAAAAAA", r.port.resolveGeneration, ok("AAAAAAAAAAA"));
    CHECK(r.c.state() == PlaybackController::Loading && r.port.loads.empty());
    r.answerOk();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    // Back to A: A reclaims and B is stopped; closing B afterwards does nothing.
    r.port.clear();
    r.c.activateContext(r.now, ctxA);
    CHECK(r.port.resolveId == "AAAAAAAAAAA");
    CHECK(r.c.arbiter().owner() == ctxA);
    const int stops = r.port.count("stopPlayer");
    r.c.closeContext(r.now, b);
    CHECK(r.port.count("stopPlayer") == stops);
    // Only the owner acts: with nobody owning the player a failure is ignored.
    Rig q;
    q.c.open(q.now, "DDDDDDDDDDD");
    const int d = q.c.claimContext();
    q.c.closeContext(q.now, d);  // still loading: the play intent counts as playing, so it parks
    CHECK(q.c.arbiter().parked());
    q.c.stop();
    CHECK(q.c.state() == PlaybackController::Idle);
}

// PLY-23: loss pauses at once; the grant resumes only with the setting on.
static void ply23_audioResourceLossPauses() {
    Rig r;
    r.playing("EEEEEEEEEEE");
    r.port.clear();
    r.c.onAudioLost();
    CHECK(r.port.count("pause") == 1);
    CHECK(!r.c.wantsPlay());
    r.c.onPlayerPlaying(false);
    r.c.onAudioGranted();
    CHECK(r.port.count("play") == 1);
    Rig s;
    s.c.setResumeAfterInterruption(false);
    s.playing("EEEEEEEEEEE");
    s.port.clear();
    s.c.onAudioLost();
    s.c.onAudioGranted();
    CHECK(s.port.count("pause") == 1 && s.port.count("play") == 0);
    // A user pause keeps the resource and pauses.
    Rig u;
    u.playing("EEEEEEEEEEE");
    u.port.clear();
    u.c.pause();
    CHECK(u.port.count("pause") == 1 && u.port.count("releaseAudio") == 0);
    u.c.play(u.now);
    CHECK(u.port.count("play") == 1 && u.port.count("acquireAudio") == 0);
}

// PLY-25: related autoplay after a cancellable 5 s countdown; the queue
// advances at once; the next item replaces the page.
static void ply25_autoplayCountdown() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.c.setRelatedCandidate("RRRRRRRRRRR");
    r.c.onEnded(r.now);
    CHECK(r.c.upNextVideoId() == "RRRRRRRRRRR");
    CHECK(r.c.upNextRemainingMs(r.now) == 5000);
    CHECK(r.c.nextDeadlineMs() == r.now + 5000);
    const int before = r.port.resolves;
    r.advance(4999);
    CHECK(r.port.resolves == before);
    r.c.takeChanges();
    r.advance(1);
    CHECK(r.port.resolveId == "RRRRRRRRRRR");
    CHECK(r.c.takeChanges() & PlaybackController::ReplacePageChange);
    CHECK(r.c.replacePageVideoId() == "RRRRRRRRRRR");
    // Cancel stops it.
    Rig c;
    c.playing("AAAAAAAAAAA");
    c.c.setRelatedCandidate("RRRRRRRRRRR");
    c.c.onEnded(c.now);
    c.c.cancelUpNext();
    c.advance(6000);
    CHECK(c.port.resolveId == "AAAAAAAAAAA");
    // Setting off: no countdown.
    Rig o;
    o.c.setRelatedAutoplay(false);
    o.playing("AAAAAAAAAAA");
    o.c.setRelatedCandidate("RRRRRRRRRRR");
    o.c.onEnded(o.now);
    CHECK(o.c.upNextVideoId().empty());
    // Queue: next item at once; MPRIS CanGoNext/Previous only with a queue.
    Rig q;
    CHECK(!q.c.mprisCapabilities().canGoNext);
    q.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ", "Q2QQQQQQQQQ"}, 0);
    q.playing("Q0QQQQQQQQQ");
    CHECK(q.c.mprisCapabilities().canGoNext && q.c.mprisCapabilities().canGoPrevious);
    CHECK(q.c.coverState().right == player::CoverAction::Next);
    q.c.onEnded(q.now);
    CHECK(q.port.resolveId == "Q1QQQQQQQQQ");
    q.answerOk();
    q.c.next(q.now);
    CHECK(q.port.resolveId == "Q2QQQQQQQQQ");
    CHECK(!q.c.mprisCapabilities().canGoNext);
    // Opening a video outside the queue ends it.
    q.c.open(q.now, "XXXXXXXXXXX");
    CHECK(!q.c.autoplay().hasQueue());
}

// LIB-05 / PLY-26: the start position reaches the load; a quality switch
// resumes at the captured position; local files play in the same player.
static void ply26_resumePosition() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    r.c.setStartPosition(42000);
    r.answerOk();
    CHECK(r.port.loads.back().startPositionMs == 42000);
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.setStartPosition(1000);  // ignored once started
    r.c.onPosition(r.now, 90000);
    QualityChoice q;
    q.height = 360;
    r.c.setQuality(r.now, q);
    CHECK(r.c.switching());
    CHECK(r.port.loads.back().startPositionMs == 90000);
    r.c.takeChanges();
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.takeChanges() & PlaybackController::SwitchResumedChange);
    // A downloaded WebM pair and a single mp4 file: no resolve.
    Rig l;
    LocalMedia m;
    m.videoId = "LLLLLLLLLLL";
    m.layout = "pair";
    m.filePath = m.videoPath = "/home/u/Videos/a b.webm";
    m.audioPath = "/home/u/Videos/a b.weba";
    m.durationMs = 120000;
    l.c.openLocal(l.now, m);
    l.c.setStartPosition(30000);
    CHECK(l.port.resolves == 0);
    CHECK(l.port.loads.size() == 1);
    CHECK(l.port.loads[0].mode == player::PlayMode::Dual);
    CHECK(l.port.loads[0].video.uri == "file:///home/u/Videos/a%20b.webm");
    CHECK(l.port.loads[0].audio.container == player::Container::Matroska);
    LocalMedia single;
    single.videoId = "MMMMMMMMMMM";
    single.layout = "single";
    single.filePath = single.videoPath = "/home/u/Videos/c.mp4";
    l.c.openLocal(l.now, single);
    CHECK(l.port.loads.back().mode == player::PlayMode::Local);
    CHECK(l.port.loads.back().muxed.container == player::Container::IsoBmff);
    CHECK(l.c.hasVideo() && l.c.qualityOptions().empty());
}

// SEL-08: listen-only builds an audio-only load and releases the display.
static void sel08_listenOnly() {
    Rig r;
    r.c.setKeepAliveAvailable(true);
    r.playing("AAAAAAAAAAA");
    r.c.setAppActive(r.now, true);
    CHECK(r.c.keepDisplayOn());
    r.c.onPosition(r.now, 20000);
    r.c.setAudioOnly(r.now, true);
    const PlaybackLoad &l = r.port.loads.back();
    CHECK(l.mode == player::PlayMode::AudioOnly);
    CHECK(l.audio.present() && !l.video.present() && !l.muxed.present());
    CHECK(l.startPositionMs == 20000);
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    CHECK(!r.c.hasVideo());
    CHECK(!r.c.keepDisplayOn());
    CHECK(r.c.qualityOptions().empty());
    // The toggle applies to the next video too (per session).
    r.c.open(r.now, "BBBBBBBBBBB");
    r.answerOk();
    CHECK(r.port.loads.back().mode == player::PlayMode::AudioOnly);
}

// PLY-21 interim rule: no software decoder for the item's video -> this item
// uses the hardware decoder, shown in diagnostics.
static void ply21_interimHardwareFallback() {
    Rig r;
    DecoderCapabilities droidOnly;
    droidOnly.h264 = cs(true, true, false);
    r.c.setCapabilities(droidOnly);
    r.c.open(r.now, "HHHHHHHHHHH");
    ResolveAnswer a;
    a.details = details("HHHHHHHHHHH");
    a.details.formats = { fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360), fmt("136", "mp4", "avc1.4D401F", "none", 720),
                          fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129) };
    r.answer(a);
    CHECK(!r.port.loads.empty());
    CHECK(!r.port.loads.empty() && r.port.loads[0].decode == player::DecodeMode::Hardware);
    CHECK(r.c.hardwareDecodeFallback());
    r.c.onDecodeFallback("no video frame within 3 s");
    CHECK(r.c.decodeFallbackReason() == "no video frame within 3 s");
    // With a software decoder: software, no fallback flag.
    Rig s;
    s.c.open(s.now, "SSSSSSSSSSS");
    s.answerOk();
    CHECK(s.port.loads[0].decode == player::DecodeMode::Software && !s.c.hardwareDecodeFallback());
}

// SEL-02 (R2 finding): software decode + default settings (maxHeight 0) and an
// offer with only 60 fps rungs never plays above 720p; mobile data caps at 480p.
static void sel02_softwareCeilingWithSixtyFpsOffer() {
    ResolveAnswer a = ok("BUNNY000060");
    a.details.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("298", "mp4", "avc1.4D4020", "none", 720, 60, 2500),
        fmt("299", "mp4", "avc1.64002A", "none", 1080, 60, 5000),
        fmt("303", "webm", "vp09.00.41.08", "none", 1080, 60, 4400),
        fmt("302", "webm", "vp09.00.40.08", "none", 720, 60, 2200),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
        fmt("251", "webm", "none", "opus", 0, 0, 135),
    };
    Rig r;
    r.c.setPreferences(PlaybackPreferences());  // = AppSettings defaults: maxHeight 0, hardware off
    r.c.open(r.now, "BUNNY000060");
    r.answer(a);
    CHECK(!r.c.chain().empty());
    for (std::size_t i = 0; i < r.c.chain().size(); ++i) CHECK(r.c.chain()[i].height <= 720);
    CHECK(r.port.loads.size() == 1 && r.port.loads[0].decode == player::DecodeMode::Software);
    Rig m;
    PlaybackPreferences mobile;
    mobile.network = NetworkClass::Mobile;
    m.c.setPreferences(mobile);
    m.c.open(m.now, "BUNNY000060");
    a.details.formats.push_back(fmt("135", "mp4", "avc1.4D401E", "none", 480, 30, 900));
    m.answer(a);
    CHECK(!m.c.chain().empty() && m.c.chain()[0].height <= 480);
    // The "Maximum quality on mobile data" setting moves that ceiling (0 = the 480p default).
    Rig taller;
    mobile.mobileMaxHeight = 720;
    taller.c.setPreferences(mobile);
    taller.c.open(taller.now, "BUNNY000060");
    a.details.formats.push_back(fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500));  // a 30 fps rung at the new ceiling
    taller.answer(a);
    CHECK(!taller.c.chain().empty() && taller.c.chain()[0].height == 720);
    for (std::size_t i = 0; i < taller.c.chain().size(); ++i) CHECK(taller.c.chain()[i].height <= 720);
    Rig lower;
    mobile.mobileMaxHeight = 360;
    lower.c.setPreferences(mobile);
    lower.c.open(lower.now, "BUNNY000060");
    a.details.formats.push_back(fmt("134", "mp4", "avc1.4D401E", "none", 360, 30, 600));
    lower.answer(a);
    CHECK(!lower.c.chain().empty() && lower.c.chain()[0].height <= 360);
}

// PLY-07: the chosen speed is remembered and applied to the next load (before
// the first frame); live streams play at 1.0.
static void ply07_rememberedSpeed() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.c.setRate(1.6);
    CHECK(r.port.last("rememberRate")->value == 150);
    CHECK(r.port.last("setRate")->value == 150);
    r.c.open(r.now, "BBBBBBBBBBB");
    r.answerOk();
    CHECK(r.port.loads.back().rate == 1.5);
    ResolveAnswer live = ok("LIVELIVELIV");
    live.details.summary.liveStatus = LiveStatus::IsLive;
    live.details.manifestUrl = "https://manifest.googlevideo.com/live.m3u8";
    r.c.open(r.now, "LIVELIVELIV");
    r.answer(live);
    CHECK(!r.port.loads.empty() && r.port.loads.back().mode == player::PlayMode::Hls);
    CHECK(r.port.loads.back().rate == 1.0 && r.port.loads.back().liveHls);
    CHECK(!r.c.rateChangeable());
    r.c.setRate(2.0);  // stray choice at the live edge keeps the remembered value
    CHECK(r.c.rememberedRate() == 1.5);
}

// SB-02: an auto-skip is posted (deferred), never executed inline; owner only.
static void sb02_deferredSkip() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    std::vector<player::SponsorSegment> segs;
    segs.push_back(player::SponsorSegment(10000, 20000, player::SponsorCategory::Sponsor));
    r.c.setSponsorSegments(segs);
    r.port.clear();
    r.c.onPosition(r.now, 10500);
    CHECK(r.port.count("postSeek") == 1 && r.port.count("seek") == 0);
    CHECK(r.port.last("postSeek")->value == 20000);  // to the segment end
    CHECK(r.c.sponsorNoticeVisible());
    r.advance(1600);
    CHECK(!r.c.sponsorNoticeVisible());
}

// MPR-01 Stop really stops; MPR-02 cover states; PLY-12 keep-on.
static void mpr01_stopAndCover() {
    Rig r;
    r.c.setKeepAliveAvailable(true);
    CHECK(r.c.coverState().mode == player::CoverMode::Identity);
    r.playing("AAAAAAAAAAA");
    r.c.onSeekable(true);
    CHECK(r.c.mprisCapabilities().canSeek && r.c.mprisCapabilities().canPause);
    CHECK(r.c.coverState().left == player::CoverAction::PlayPause);
    CHECK(r.c.coverState().right == player::CoverAction::Stop);
    CHECK(r.c.keepDisplayOn());
    r.c.setAppActive(r.now, false);
    CHECK(!r.c.keepDisplayOn());  // background
    r.c.setAppActive(r.now, true);
    r.c.onPosition(r.now, 120000);
    r.port.clear();
    r.c.stop();
    CHECK(r.port.index("savePosition") == 0 && r.port.calls[0].value == 120000);
    CHECK(r.port.count("stopPlayer") == 1 && r.port.count("releaseAudio") == 1);
    CHECK(r.c.state() == PlaybackController::Idle);
    CHECK(!r.c.keepDisplayOn());
    CHECK(r.c.coverState().mode == player::CoverMode::ContinueWatching);
    CHECK(r.c.mprisCapabilities().canPlay && !r.c.mprisCapabilities().canPause);
    CHECK(r.c.continueWatching(r.now));
    CHECK(r.port.resolveId == "AAAAAAAAAAA");
    // Stop while resolving releases the job.
    Rig s;
    s.c.open(s.now, "BBBBBBBBBBB");
    s.c.stop();
    CHECK(s.port.count("cancelResolve") == 1);
    // Keep-on setting off.
    Rig k;
    k.c.setKeepAliveAvailable(true);
    k.c.setKeepDisplayOnSetting(false);
    k.playing("AAAAAAAAAAA");
    CHECK(!k.c.keepDisplayOn());
    // STORE: no KeepAlive.
    Rig st;
    st.playing("AAAAAAAAAAA");
    CHECK(!st.c.keepDisplayOn());
}

// A resolve error on the fast profile retries with the full one; a final
// error fails with the resolver's code; an empty selection reports it.
static void resolveErrors() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    ResolveAnswer e;
    e.error = ResolveAnswer::Retryable;
    e.errorCode = 6;
    r.answer(e);
    CHECK(r.port.resolves == 2 && r.port.resolveFull);
    r.answer(e);
    CHECK(r.c.state() == PlaybackController::Failed && r.c.failure() == PlaybackController::ResolveFailed);
    CHECK(r.c.failureCode() == 6);
    // Same video again after a failure: a fresh attempt.
    r.c.open(r.now, "AAAAAAAAAAA");
    CHECK(r.c.state() == PlaybackController::Loading);
    Rig u;
    u.c.open(u.now, "UUUUUUUUUUU");
    ResolveAnswer up = ok("UUUUUUUUUUU");
    up.details.summary.liveStatus = LiveStatus::Upcoming;
    up.details.formats.clear();
    u.answer(up);
    u.answer(up);
    CHECK(u.c.state() == PlaybackController::Failed && u.c.failure() == PlaybackController::UpcomingStream);
    // The start watchdog: no preroll in 25 s counts as a failed candidate.
    Rig w;
    w.c.open(w.now, "WWWWWWWWWWW");
    w.answerOk();
    w.advance(PlaybackController::kStartWatchdogMs);
    CHECK(w.port.loads.size() == 2);
    // Every candidate runs out of time: the fresh resolve says the start was abandoned, which is
    // no strike against the source (RES-07); a lost connection says Connection.
    for (int i = 0; i < 12 && w.port.resolves < 2; ++i) w.advance(PlaybackController::kStartWatchdogMs);
    CHECK(w.port.resolves == 2 && w.port.resolveFull);
    CHECK(w.port.lastRequest.fault == player::SourceFault::Abandoned);
    Rig n;
    n.c.open(n.now, "NNNNNNNNNNN");
    n.answerOk();
    for (int i = 0; i < 12 && n.port.resolves < 2; ++i) n.c.onPlayerFailed(n.now, "stream open failed: network", true);
    CHECK(n.port.resolves == 2 && n.port.resolveFull);
    CHECK(n.port.lastRequest.fault == player::SourceFault::Connection);
    CHECK(player::notTheSource(n.port.lastRequest.fault) && !player::notTheSource(player::SourceFault::Unspecified));
}

// RES-09 (cause 4): a rate-limited answer's one fresh re-resolve waits kRetryLaterMs (status
// RetryingLater, retryCode the answer's class); a second rate-limited answer fails. An open of
// another video drops the wait.
static void res09_rateLimitedRetryWaits() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    ResolveAnswer limited;
    limited.error = ResolveAnswer::Retryable;
    limited.errorCode = 15;  // opaque to the controller (playback::StreamErrorClass::TryLater)
    limited.retryLater = true;
    r.answer(limited);
    CHECK(r.port.resolves == 1 && r.c.state() == PlaybackController::Loading);
    CHECK(r.c.status() == PlaybackController::RetryingLater && r.c.retryCode() == 15);
    CHECK(r.c.nextDeadlineMs() == r.now + PlaybackController::kRetryLaterMs);
    r.c.play(r.now);
    CHECK(r.port.resolves == 1);  // play() while waiting starts nothing
    r.advance(PlaybackController::kRetryLaterMs - 1);
    CHECK(r.port.resolves == 1);
    r.advance(1);
    CHECK(r.port.resolves == 2 && r.port.resolveFull && r.c.status() == PlaybackController::ResolvingFull);
    CHECK(r.c.retryCode() == 15);
    r.answer(limited);
    CHECK(r.c.state() == PlaybackController::Failed && r.c.failure() == PlaybackController::ResolveFailed);
    CHECK(r.c.failureCode() == 15 && r.c.retryCode() == 0 && r.c.nextDeadlineMs() < 0);
    // Retryable without retryLater: at once (retryCode names the cause during the re-resolve).
    Rig n;
    n.c.open(n.now, "BBBBBBBBBBB");
    ResolveAnswer changed;
    changed.error = ResolveAnswer::Retryable;
    changed.errorCode = 21;
    n.answer(changed);
    CHECK(n.port.resolves == 2 && n.c.retryCode() == 21 && n.c.status() == PlaybackController::ResolvingFull);
    // Another open drops a pending wait.
    Rig o;
    o.c.open(o.now, "CCCCCCCCCCC");
    o.answer(limited);
    o.c.open(o.now, "DDDDDDDDDDD");
    o.advance(PlaybackController::kRetryLaterMs);
    CHECK(o.port.resolveId == "DDDDDDDDDDD" && o.port.resolves == 2 && o.c.retryCode() == 0);
}

// RES-10 / RES-20 / SEL-01/02/05/08: the request the shared resolver gets.
static void res10_requestFields() {
    PlaybackPreferences p;
    DecoderCapabilities sw = software();
    ResolveRequest r = resolveRequestFor("AAAAAAAAAAA", sw, p, QualityChoice());
    CHECK(r.videoId == "AAAAAAAAAAA" && !r.audioOnly && !r.speculative && !r.fresh);
    CHECK(r.maxHeight == 720);                      // SEL-02 software default
    CHECK(r.videoCodecs.size() == 2 && r.videoCodecs[0] == "avc1" && r.videoCodecs[1] == "vp09");
    CHECK(r.audioCodecs.empty() && !r.hardwareDecode && !r.highFrameRate);
    p.network = NetworkClass::Mobile;
    CHECK(resolveRequestFor("AAAAAAAAAAA", sw, p, QualityChoice()).maxHeight == 480);
    p.network = NetworkClass::Unmetered;
    // Verified hardware decode: VP9 first, 1080p.
    DecoderCapabilities hw = sw;
    hw.vp9 = cs(true, true, true);
    hw.h264 = cs(true, true, true);
    hw.hardwareDecodeVerified = true;
    hw.av1SelfTestPassed = true;
    p.hardwareDecode = true;
    r = resolveRequestFor("AAAAAAAAAAA", hw, p, QualityChoice());
    CHECK(r.hardwareDecode && r.maxHeight == 1080);
    CHECK(r.videoCodecs.size() == 3 && r.videoCodecs[0] == "vp09" && r.videoCodecs[2] == "av01");
    // Codec preference, dub, listen-only, an explicit taller choice.
    p.hardwareDecode = false;
    p.codec = CodecPreference::Vp9;
    p.audioLanguage = "pl";
    p.mode = PlaybackMode::AudioOnly;
    QualityChoice tall;
    tall.height = 1080;
    tall.highFrameRate = true;
    r = resolveRequestFor("AAAAAAAAAAA", sw, p, tall);
    CHECK(r.videoCodecs[0] == "vp09" && r.dubLanguage == "pl" && r.audioOnly);
    CHECK(r.maxHeight == 1080 && r.highFrameRate);
    // The controller sends it with the generation; an audioOnly session asks for audio only.
    Rig c;
    c.c.setAudioOnly(c.now, true);
    c.c.open(c.now, "BBBBBBBBBBB");
    CHECK(c.port.resolves == 1 && c.port.lastRequest.audioOnly && c.port.lastRequest.generation == c.port.resolveGeneration);
}

// SEL-07 / SEL-08: quality and listen-only switches re-select from the same set; a listen-only
// set cannot serve video (one re-resolve with purpose Video); an explicit rung above the set's
// ceiling that it does not offer is asked for once; an expiring set is resolved fresh.
static void sel07_switchFromSameSet() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.c.onPosition(r.now, 50000);
    QualityChoice q;
    q.height = 360;
    r.c.setQuality(r.now, q);
    CHECK(r.port.resolves == 1);                     // no resolve: the set has the rung
    CHECK(r.port.loads.size() == 2 && r.port.loads.back().startPositionMs == 50000);
    r.c.onPlayerStarted(r.now);
    r.c.setAudioOnly(r.now, true);                   // to listen-only: same set
    CHECK(r.port.resolves == 1 && r.port.loads.back().mode == player::PlayMode::AudioOnly);
    r.c.onPlayerStarted(r.now);
    // A session that started listen-only: back to video needs a video set.
    Rig a;
    a.c.setAudioOnly(a.now, true);
    a.playing("BBBBBBBBBBB");
    CHECK(a.c.setAudioOnly());
    a.c.onPosition(a.now, 7000);
    a.c.setAudioOnly(a.now, false);
    CHECK(a.port.resolves == 2 && !a.port.lastRequest.audioOnly && !a.port.lastRequest.fresh);
    CHECK(a.c.state() == PlaybackController::Loading && a.c.switching());
    a.answerOk();
    CHECK(a.port.loads.back().mode == player::PlayMode::Dual && a.port.loads.back().startPositionMs == 7000);
    a.c.takeChanges();
    a.c.onPlayerStarted(a.now);
    CHECK(a.c.takeChanges() & PlaybackController::SwitchResumedChange);
    // 1080p is above the 720p set ceiling and not offered: one resolve with maxHeight 1080.
    QualityChoice big;
    big.height = 1080;
    const int before = a.port.resolves;
    a.c.setQuality(a.now, big);
    CHECK(a.port.resolves == before + 1 && a.port.lastRequest.maxHeight == 1080);
    a.answerOk();
    a.c.onPlayerStarted(a.now);
    a.c.setQuality(a.now, big);                       // the 1080 set has been asked for: re-select
    CHECK(a.port.resolves == before + 1);
    // Expired URLs: a fresh resolve.
    Rig e;
    e.c.open(e.now, "EEEEEEEEEEE");
    ResolveAnswer ans = ok("EEEEEEEEEEE");
    ans.expiresAtUnixMs = 1700000000000LL + e.now + 10 * 60000;
    e.answer(ans);
    e.c.onAudioGranted();
    e.c.onPlayerStarted(e.now);
    e.now += 9 * 60000;
    e.c.setQuality(e.now, q);
    CHECK(e.port.resolves == 2 && e.port.resolveFull);
}

// RES-11: the next queue item (else the related candidate) is prefetched once, 4 s after the
// current item's first preroll; a candidate that arrives later is prefetched when it arrives.
static void res11_prefetchAfterFirstPreroll() {
    Rig q;
    q.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ"}, 0);
    q.playing("Q0QQQQQQQQQ");
    CHECK(q.c.nextDeadlineMs() == q.now + PlaybackController::kPrefetchDelayMs);
    q.advance(3999);
    CHECK(q.port.prefetches.empty());
    q.advance(1);
    CHECK(q.port.prefetches.size() == 1 && q.port.prefetches[0].videoId == "Q1QQQQQQQQQ");
    CHECK(q.port.prefetches[0].speculative && q.port.resolves == 1);
    q.advance(10000);
    CHECK(q.port.prefetches.size() == 1);            // once per item
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.advance(5000);
    CHECK(r.port.prefetches.empty());                // no candidate yet
    r.c.setRelatedCandidate("RRRRRRRRRRR");
    CHECK(r.port.prefetches.size() == 1 && r.port.prefetches[0].videoId == "RRRRRRRRRRR");
    Rig o;
    o.c.setRelatedAutoplay(false);
    o.c.setRelatedCandidate("RRRRRRRRRRR");
    o.playing("AAAAAAAAAAA");
    o.advance(5000);
    CHECK(o.port.prefetches.empty());                // related autoplay off: nothing will play next
}

// Metadata after the start fills only what the open hints did not supply.
static void metadataAfterStart() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    ResolveAnswer a = ok("AAAAAAAAAAA");
    a.details.summary.title = "From the row";
    a.details.summary.durationSeconds = -1;
    a.details.summary.channel.clear();
    r.answer(a);
    r.c.takeChanges();
    catalog::VideoDetails m;
    m.summary.title = "From details";
    m.summary.channel = "Channel";
    m.summary.durationSeconds = 300;
    m.description = "About";
    m.chapters.resize(2);
    m.formats.push_back(fmt("22", "mp4", "avc1.64001F", "mp4a.40.2", 720));
    r.c.setMetadata("AAAAAAAAAAA", m);
    CHECK(r.c.takeChanges() & PlaybackController::DetailsChange);
    CHECK(r.c.details().summary.title == "From the row" && r.c.details().summary.channel == "Channel");
    CHECK(r.c.details().summary.durationSeconds == 300 && r.c.details().description == "About");
    CHECK(r.c.details().chapters.size() == 2 && r.c.details().formats.size() == 5);
    r.c.setMetadata("BBBBBBBBBBB", m);                // another video: ignored
    CHECK(r.c.takeChanges() == 0);
}

// E12 (RES-11 MAY): a speculative resolve on touch-down never runs in the background and never
// within 2 s of a start; on any network otherwise.
static void e12_speculationBounds() {
    SpeculationContext c;
    c.nowMs = 10000;
    c.networkClass = 1;  // mobile data: a press is likely a tap, it may speculate
    CHECK(speculationAllowed(c));
    c.appActive = false;
    CHECK(!speculationAllowed(c));                     // never in the background
    c.appActive = true;
    c.lastStartMs = c.nowMs - kSpeculationQuietMs + 1;
    CHECK(!speculationAllowed(c));                     // first 2 s of a start
    c.lastStartMs = c.nowMs - kSpeculationQuietMs;
    CHECK(speculationAllowed(c));
    // The controller exposes what the session feeds in.
    Rig r;
    CHECK(r.c.appActive() && !r.c.starting());
    r.c.open(r.now, "AAAAAAAAAAA");
    CHECK(r.c.starting());                             // resolving
    r.answerOk();
    CHECK(r.c.starting());                             // prerolling
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    CHECK(!r.c.starting());
    r.c.setAppActive(r.now, false);
    CHECK(!r.c.appActive());
}

// RES-11 / RES-21 rows: unmetered only, first row, one at a time, no repeat, keep-alive at most
// every 30 s; nothing on mobile, unknown network, in the background or while a play starts.
static void res11_rowsVisibleDecision() {
    const std::vector<std::string> ids = {"bad", "AAAAAAAAAAA", "BBBBBBBBBBB", "CCCCCCCCCCC"};
    SpeculationContext c;
    c.nowMs = 100000;
    c.networkClass = 0;
    RowPrefetchState st;
    RowsVisibleDecision d = rowsVisibleDecision(c, ids, std::string(), false, st);
    CHECK(d.prefetchVideoId == "AAAAAAAAAAA" && d.warmUp);   // first valid row
    CHECK(std::string(d.reason).empty());
    d = rowsVisibleDecision(c, ids, "AAAAAAAAAAA", false, st);
    CHECK(d.prefetchVideoId == "BBBBBBBBBBB");                // not the session's own video
    for (int net = -1; net <= 1; net += 2) {                  // mobile, unknown: nothing
        SpeculationContext m = c;
        m.networkClass = net;
        d = rowsVisibleDecision(m, ids, std::string(), false, st);
        CHECK(d.prefetchVideoId.empty() && !d.warmUp);
        CHECK(std::string(d.reason) == (net < 0 ? "unknown-network" : "metered"));
    }
    SpeculationContext bg = c;
    bg.appActive = false;
    d = rowsVisibleDecision(bg, ids, std::string(), false, st);
    CHECK(d.prefetchVideoId.empty() && !d.warmUp);            // background
    CHECK(std::string(d.reason) == "background");
    SpeculationContext starting = c;
    starting.starting = true;
    d = rowsVisibleDecision(starting, ids, std::string(), false, st);
    CHECK(d.prefetchVideoId.empty() && !d.warmUp);            // a play is starting
    SpeculationContext quiet = c;
    quiet.lastStartMs = c.nowMs - 500;
    d = rowsVisibleDecision(quiet, ids, std::string(), false, st);
    CHECK(d.prefetchVideoId.empty() && !d.warmUp);            // first 2 s of a start
    d = rowsVisibleDecision(c, ids, std::string(), true, st);
    CHECK(d.prefetchVideoId.empty() && d.warmUp);             // a press speculation runs
    // One in flight, then no repeat of the same row; the keep-alive every 30 s.
    st.lastVideoId = "AAAAAAAAAAA";
    st.lastPrefetchMs = c.nowMs;
    st.lastWarmUpMs = c.nowMs;
    SpeculationContext later = c;
    later.nowMs = c.nowMs + kRowPrefetchInFlightMs - 1;
    d = rowsVisibleDecision(later, {"BBBBBBBBBBB"}, std::string(), false, st);
    CHECK(d.prefetchVideoId.empty() && !d.warmUp);
    later.nowMs = c.nowMs + kRowPrefetchInFlightMs;
    d = rowsVisibleDecision(later, {"BBBBBBBBBBB"}, std::string(), false, st);
    CHECK(d.prefetchVideoId == "BBBBBBBBBBB" && !d.warmUp);
    d = rowsVisibleDecision(later, ids, std::string(), false, st);
    CHECK(d.prefetchVideoId.empty());                         // the top row was prefetched lately
    later.nowMs = c.nowMs + kKeepAliveIntervalMs;
    CHECK(rowsVisibleDecision(later, ids, std::string(), false, st).warmUp);
    later.nowMs = c.nowMs + kRowPrefetchRepeatMs;
    CHECK(rowsVisibleDecision(later, ids, std::string(), false, st).prefetchVideoId == "AAAAAAAAAAA");
}

// RES-19: an upcoming video shows its scheduled start and is not resolved again until then (retry,
// quality switch, autoplay, prefetch, speculation); one deadline at the start, then one resolve.
static int resolvesOf(const FakePort &p, const std::string &id) {
    int n = 0;
    for (std::size_t i = 0; i < p.calls.size(); ++i) n += p.calls[i].what == "resolve" && p.calls[i].text == id;
    return n;
}

static ResolveAnswer upcomingAnswer(const char *id, std::int64_t startUnixMs, bool asError) {
    ResolveAnswer a;
    if (asError) {
        a.error = ResolveAnswer::Final;
        a.errorCode = 7;  // playback::StreamErrorClass::Upcoming
    } else {
        a.details = details(id);
        a.details.formats.clear();
        a.details.summary.liveStatus = LiveStatus::Upcoming;
    }
    a.upcoming = true;
    a.scheduledStartUnixMs = startUnixMs;
    return a;
}

static void res19_upcomingKnownStartWaits() {
    Rig r;
    const std::int64_t offset = 1700000000000LL;
    const std::string up = "UUUUUUUUUUU";
    r.c.open(r.now, up);
    const std::int64_t start = offset + r.now + 3600000;  // in one hour
    r.answer(upcomingAnswer(up.c_str(), start, false));
    CHECK(r.c.state() == PlaybackController::Failed && r.c.failure() == PlaybackController::UpcomingStream);
    CHECK(r.c.upcoming() && r.c.scheduledStartUnixMs() == start);
    CHECK(r.c.waitingForScheduledStart() && !r.c.retryAvailable());
    CHECK(r.c.details().summary.liveStatus == LiveStatus::Upcoming && r.c.details().summary.title == "Title UUUUUUUUUUU");
    CHECK(resolvesOf(r.port, up) == 1);  // no fresh re-resolve of an empty upcoming set
    CHECK(r.c.resolveBlocked(r.now, up) && !r.c.resolveBlocked(r.now, "AAAAAAAAAAA"));
    CHECK(r.c.nextDeadlineMs() == r.now + 3600000);  // the single deadline: the start
    // Retry, play and a quality switch resolve nothing.
    r.c.retry(r.now);
    r.c.play(r.now);
    QualityChoice q; q.height = 720;
    r.c.setQuality(r.now, q);
    r.c.setAudioOnly(r.now, true);
    r.c.setAudioOnly(r.now, false);
    CHECK(resolvesOf(r.port, up) == 1 && r.c.upcoming());
    // Opening it again (the same page, or after another video) shows the start, no resolve.
    r.c.open(r.now, up);
    CHECK(resolvesOf(r.port, up) == 1 && r.c.upcoming() && r.c.scheduledStartUnixMs() == start);
    r.playing("AAAAAAAAAAA");
    r.c.open(r.now, up);
    CHECK(resolvesOf(r.port, up) == 1 && r.c.upcoming() && r.c.waitingForScheduledStart());
    // Prefetch (RES-11) and autoplay of a queue whose next item is the upcoming video.
    r.port.clear();
    r.playing("AAAAAAAAAAA");
    r.c.setQueue({"AAAAAAAAAAA", up}, 0);
    r.advance(PlaybackController::kPrefetchDelayMs);
    CHECK(r.port.prefetches.empty());
    r.c.onEnded(r.now);
    CHECK(r.c.videoId() == up && r.c.upcoming() && resolvesOf(r.port, up) == 0);
    // At the start: exactly one fresh resolve, then it plays.
    r.port.clear();
    r.advance(3600000);
    CHECK(resolvesOf(r.port, up) == 1 && r.port.resolveFull);
    CHECK(!r.c.waitingForScheduledStart());
    r.advance(1000);
    CHECK(resolvesOf(r.port, up) == 1);
    r.answerOk();
    CHECK(r.c.state() == PlaybackController::Preparing && !r.c.upcoming() && !r.port.loads.empty());
    CHECK(!r.c.resolveBlocked(r.now, up));
}

static void res19_upcomingUnknownStartRetryOnly() {
    Rig r;
    const std::string up = "VVVVVVVVVVV";
    r.c.open(r.now, up);
    r.answer(upcomingAnswer(up.c_str(), 0, true));  // StreamError Upcoming, no start known
    CHECK(r.c.upcoming() && r.c.scheduledStartUnixMs() == 0 && r.c.failureCode() == 7);
    CHECK(!r.c.waitingForScheduledStart() && r.c.retryAvailable());
    CHECK(r.c.nextDeadlineMs() < 0);  // no automatic re-resolve
    CHECK(r.c.resolveBlocked(r.now, up));
    r.advance(24LL * 3600000);
    CHECK(resolvesOf(r.port, up) == 1);
    // Autoplay into it holds; the user's Retry resolves once.
    r.playing("AAAAAAAAAAA");
    r.c.setQueue({"AAAAAAAAAAA", up}, 0);
    r.c.onEnded(r.now);
    CHECK(r.c.videoId() == up && r.c.upcoming() && resolvesOf(r.port, up) == 1);
    r.c.retry(r.now);
    CHECK(resolvesOf(r.port, up) == 2 && r.c.state() == PlaybackController::Loading);
    // Still upcoming, with a start that has already passed: no loop, Retry stays available.
    const std::int64_t past = 1700000000000LL + r.now - 60000;
    r.answer(upcomingAnswer(up.c_str(), past, false));
    CHECK(r.c.upcoming() && r.c.scheduledStartUnixMs() == past && r.c.retryAvailable());
    CHECK(!r.c.waitingForScheduledStart() && r.c.nextDeadlineMs() < 0);
    r.advance(600000);
    CHECK(resolvesOf(r.port, up) == 2);
}

// RES-19: the start follows the wall clock (the monotonic clock pauses in suspend).
static void res19_scheduledStartFollowsWallClock() {
    Rig r;
    const std::string up = "WWWWWWWWWWW";
    r.c.open(r.now, up);
    const std::int64_t start = 1700000000000LL + r.now + 7200000;
    r.answer(upcomingAnswer(up.c_str(), start, false));
    CHECK(r.c.nextDeadlineMs() == r.now + 7200000);
    // The device slept 2 h: the monotonic clock moved 1 min, the wall clock 2 h.
    r.now += 60000;
    r.c.setWallClock(r.now, start + 1000);
    CHECK(r.c.nextDeadlineMs() <= r.now);
    r.c.tick(r.now);
    CHECK(resolvesOf(r.port, up) == 2 && r.port.resolveFull);
}

// ---- Session batch 3 ----------------------------------------------------------------------

// NFR-PERF-02 (tap -> resolve-start): opening another video dispatches its resolve before the
// previous video's player is stopped; the stop follows within the same call.
static void perf02_resolveBeforeOldPlayerStops() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.port.clear();
    r.c.open(r.now, "BBBBBBBBBBB");
    const int resolve = r.port.index("resolve");
    const int stop = r.port.index("stopPlayer");
    CHECK(resolve >= 0 && stop > resolve);
    CHECK(r.port.count("stopPlayer") == 1);
    CHECK(r.port.index("savePosition") >= 0 && r.port.index("savePosition") < resolve);  // PLY-10 save first
    CHECK(r.c.state() == PlaybackController::Loading && !r.c.playerPlaying());
    r.answerOk();
    CHECK(r.port.loads.size() == 1 && r.port.loads[0].videoId == "BBBBBBBBBBB");
    // To a downloaded item: no resolve, the old player stops before the local load.
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.port.clear();
    LocalMedia m;
    m.videoId = "LLLLLLLLLLL";
    m.filePath = m.videoPath = "/home/u/Downloads/l.mp4";
    r.c.openLocal(r.now, m);
    CHECK(r.port.resolves == 0 && r.port.index("stopPlayer") >= 0 && r.port.index("stopPlayer") < r.port.index("load"));
}

// PLY-25 / MPR-01: queueLength / queueIndex, -1 without a queue; next/previous move the index.
static void ply25_queueLengthAndIndex() {
    Rig r;
    CHECK(r.c.queueLength() == -1 && r.c.queueIndex() == -1);
    r.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ", "Q2QQQQQQQQQ"}, 0);
    CHECK(r.c.queueLength() == 3 && r.c.queueIndex() == 0);
    r.playing("Q0QQQQQQQQQ");
    r.c.next(r.now);
    CHECK(r.c.queueIndex() == 1 && r.c.videoId() == "Q1QQQQQQQQQ");
    r.c.open(r.now, "Q2QQQQQQQQQ");  // from inside the queue: moves it
    CHECK(r.c.queueLength() == 3 && r.c.queueIndex() == 2);
    r.c.open(r.now, "XXXXXXXXXXX");  // from outside: ends it
    CHECK(r.c.queueLength() == -1 && r.c.queueIndex() == -1);
    r.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ"}, 1);
    r.c.clearQueue();
    CHECK(r.c.queueLength() == -1 && r.c.queueIndex() == -1);
    // canGoNext / canGoPrevious / coverMode unchanged by the read-out.
    CHECK(!r.c.mprisCapabilities().canGoNext && !r.c.mprisCapabilities().canGoPrevious);
}

// PLY-26 / DL-07 / LNK-02: a download opened with a link start starts there, once; no resolve and
// no RES-11 prefetch (no network request at all).
static void ply26_localStartAndNoPrefetch() {
    Rig r;
    r.c.setQueue({"LLLLLLLLLLL", "NNNNNNNNNNN"}, 0);
    LocalMedia m;
    m.videoId = "LLLLLLLLLLL";
    m.filePath = m.videoPath = "/home/u/Downloads/l.mp4";
    m.durationMs = 300000;
    m.startPositionMs = 90000;
    r.c.openLocal(r.now, m);
    CHECK(r.port.resolves == 0 && r.port.loads.size() == 1 && r.port.loads[0].startPositionMs == 90000);
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.advance(PlaybackController::kPrefetchDelayMs + 10);
    CHECK(r.port.prefetches.empty() && r.port.resolves == 0);
    // Stop, then "continue watching" (MPR-02) reopens the download: the link start applied once.
    r.c.onPosition(r.now, 120000);
    r.c.stop();
    CHECK(r.c.hasResumable() && r.c.continueWatching(r.now));
    CHECK(r.port.loads.size() == 2 && r.port.loads.back().startPositionMs != 90000 && r.port.resolves == 0);
    // Without a start: from the beginning.
    Rig b;
    m.startPositionMs = -1;
    b.c.openLocal(b.now, m);
    CHECK(b.port.loads.size() == 1 && b.port.loads[0].startPositionMs == 0);
}

static ResolveAnswer rungsAnswer(const char *id) {
    ResolveAnswer a;
    a.details = details(id);
    a.details.formats = {
        fmt("137", "mp4", "avc1.640028", "none", 1080, 30, 4000),
        fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500),
        fmt("247", "webm", "vp09.00.31.08", "none", 720, 30, 1300),
        fmt("135", "mp4", "avc1.4D401E", "none", 480, 30, 800),
        fmt("244", "webm", "vp09.00.30.08", "none", 480, 30, 700),
        fmt("134", "mp4", "avc1.4D4015", "none", 360, 30, 400),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
        fmt("251", "webm", "none", "opus", 0, 0, 135),
    };
    return a;
}

static bool loadsFormat(const PlaybackLoad &l, const char *id) {
    const std::string tail = std::string("/") + id;
    return l.video.uri.size() > tail.size() && l.video.uri.compare(l.video.uri.size() - tail.size(), tail.size(), tail) == 0;
}

// NFR-PERF-02 low-rung A/B (diagnostics/startLowRung): off -> the normal pick; on -> the best dual
// rung <= 480p of the same codec family first, then one same-set switch up after the first frame
// and 2 s of playing (LowRungSwitchChange -> switch-start kind=lowrung, then switch-resumed).
static void perf02_lowRungStartThenSwitchUp() {
    Rig off;
    off.c.open(off.now, "AAAAAAAAAAA");
    off.answer(rungsAnswer("AAAAAAAAAAA"));
    CHECK(off.port.loads.size() == 1 && loadsFormat(off.port.loads[0], "136"));
    CHECK(!off.c.lowRungActive());

    Rig r;
    r.c.setStartLowRung(true);
    r.c.open(r.now, "AAAAAAAAAAA");
    r.answer(rungsAnswer("AAAAAAAAAAA"));
    CHECK(r.port.resolves == 1);
    CHECK(r.port.loads.size() == 1 && loadsFormat(r.port.loads[0], "135"));  // 480p H.264, not 480p VP9
    CHECK(r.port.loads[0].mode == player::PlayMode::Dual && r.c.lowRungActive());
    CHECK(r.c.qualityChoice().height == 0);  // still Auto
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);           // preroll
    r.c.onPlayerPlaying(true);
    r.c.onPosition(r.now, 1500);          // before the first frame: does not count
    CHECK(r.c.nextDeadlineMs() < 0 || r.c.nextDeadlineMs() > r.now);
    r.c.onFirstFrame(r.now);
    r.c.onPosition(r.now + 1000, 2500);   // 1 s played
    r.now += 1000;
    r.c.tick(r.now);
    CHECK(r.port.loads.size() == 1);
    r.c.takeChanges();
    r.c.onPosition(r.now + 1000, 3600);   // 2.1 s played: due, but never inside onPosition
    r.now += 1000;
    CHECK(r.port.loads.size() == 1 && r.c.nextDeadlineMs() == r.now);
    r.c.tick(r.now);
    CHECK(r.port.loads.size() == 2 && loadsFormat(r.port.loads[1], "136"));
    CHECK(r.port.loads[1].startPositionMs == 3600 && r.port.resolves == 1);  // same set, same position
    CHECK(r.c.takeChanges() & PlaybackController::LowRungSwitchChange);
    CHECK(r.c.switching() && !r.c.lowRungActive());
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.takeChanges() & PlaybackController::SwitchResumedChange);
    r.c.onFirstFrame(r.now);
    r.c.onPosition(r.now + 5000, 9000);
    r.now += 5000;
    r.c.tick(r.now);
    CHECK(r.port.loads.size() == 2);      // one switch only

    // The VP9 preference keeps the family: 720p VP9 -> 480p VP9 first.
    Rig v;
    PlaybackPreferences p;
    p.codec = CodecPreference::Vp9;
    v.c.setPreferences(p);
    v.c.setStartLowRung(true);
    v.c.open(v.now, "VVVVVVVVVVV");
    v.answer(rungsAnswer("VVVVVVVVVVV"));
    CHECK(v.port.loads.size() == 1 && loadsFormat(v.port.loads[0], "244"));

    // Already low (mobile data, 480p ceiling), listen-only or an explicit choice: no A/B.
    Rig m;
    PlaybackPreferences mp;
    mp.network = NetworkClass::Mobile;
    m.c.setPreferences(mp);
    m.c.setStartLowRung(true);
    m.c.open(m.now, "MMMMMMMMMMM");
    m.answer(rungsAnswer("MMMMMMMMMMM"));
    CHECK(m.port.loads.size() == 1 && loadsFormat(m.port.loads[0], "135") && !m.c.lowRungActive());

    // A user quality choice during the low rung replaces the switch up.
    Rig q;
    q.c.setStartLowRung(true);
    q.c.open(q.now, "QQQQQQQQQQQ");
    q.answer(rungsAnswer("QQQQQQQQQQQ"));
    q.c.onAudioGranted();
    q.c.onPlayerStarted(q.now);
    q.c.onPlayerPlaying(true);
    q.c.onFirstFrame(q.now);
    QualityChoice c360;
    c360.height = 360;
    q.c.setQuality(q.now, c360);
    CHECK(!q.c.lowRungActive() && loadsFormat(q.port.loads.back(), "134"));
    q.c.onPlayerStarted(q.now);
    q.c.onPosition(q.now + 3000, 5000);
    q.now += 3000;
    q.c.tick(q.now);
    CHECK(q.port.loads.size() == 2);

    // The low rung fails before its first frame: the normal pick takes over (no worse than off).
    Rig f;
    f.c.setStartLowRung(true);
    f.c.open(f.now, "FFFFFFFFFFF");
    f.answer(rungsAnswer("FFFFFFFFFFF"));
    f.c.onPlayerFailed(f.now, "stream open failed", false);
    CHECK(f.port.loads.size() == 2 && loadsFormat(f.port.loads[1], "136") && !f.c.lowRungActive());
}

// PLY-18 / ux: the AppSettings key that would fix the error (output_settings.json names).
static void ply18_errorSettingKeyMapping() {
    typedef PlaybackController PC;
    ErrorSettingFacts f;
    CHECK(errorSettingKey(f).empty());                                   // no failure
    f.failure = PC::NetworkFailed;
    CHECK(errorSettingKey(f).empty());                                   // network
    f.failure = PC::UpcomingStream;
    CHECK(errorSettingKey(f).empty());
    f.failure = PC::ResolveFailed;
    f.requestMaxHeight = 720;
    CHECK(errorSettingKey(f).empty());                                   // provider / final classes
    f.noPlayableAnswer = true;
    CHECK(errorSettingKey(f) == "playback/maxHeight");                   // nothing under the ceiling
    f.mobileNetwork = true;
    CHECK(errorSettingKey(f) == "playback/maxHeightMobile");
    f.audioOnly = true;
    CHECK(errorSettingKey(f) == "playback/audioOnly");                   // listen-only asked for it
    f = ErrorSettingFacts();
    f.failure = PC::ResolveFailed;
    f.noPlayableAnswer = true;
    CHECK(errorSettingKey(f).empty());                                   // no ceiling: no setting helps
    f = ErrorSettingFacts();
    f.failure = PC::PlaybackFailed;
    CHECK(errorSettingKey(f).empty());
    f.hardwareDecode = true;
    CHECK(errorSettingKey(f) == "playback/hardwareDecode");
    f.failure = PC::NoPlayableStream;
    f.playerFault = true;
    CHECK(errorSettingKey(f) == "playback/hardwareDecode");              // every candidate failed in hardware
    f = ErrorSettingFacts();
    f.failure = PC::NoPlayableStream;
    f.emptySelection = true;
    f.noDecodableVideo = true;
    CHECK(errorSettingKey(f) == "playback/codecPreference");
    f.noDecodableVideo = false;
    CHECK(errorSettingKey(f).empty());
    f.audioOnly = true;
    f.videoModePlayable = true;
    CHECK(errorSettingKey(f) == "playback/audioOnly");
    f.local = true;
    CHECK(errorSettingKey(f).empty());                                   // a downloaded file

    // From the controller: a resolver "no playable stream" answer under the 720p ceiling.
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    ResolveAnswer e;
    e.error = ResolveAnswer::Retryable;
    e.errorCode = 9;
    r.answer(e);
    r.answer(e);
    CHECK(r.c.failure() == PC::ResolveFailed);
    CHECK(errorSettingKey(r.c.errorSettingFacts(true)) == "playback/maxHeight");
    CHECK(errorSettingKey(r.c.errorSettingFacts(false)).empty());
    // A set without a decodable video codec (AV1 without its self-test).
    Rig a;
    a.c.open(a.now, "AV1AAAAAAAA");
    ResolveAnswer av1 = ok("AV1AAAAAAAA");
    av1.details.formats = { fmt("399", "mp4", "av01.0.08M.08", "none", 1080) };
    a.answer(av1);
    a.answer(av1);
    CHECK(a.c.failure() == PC::NoPlayableStream);
    CHECK(errorSettingKey(a.c.errorSettingFacts(false)) == "playback/codecPreference");
    // Repeated player faults while the hardware-decode setting decoded.
    Rig h;
    DecoderCapabilities hw = software();
    hw.h264 = cs(true, true, true);
    hw.vp9 = cs(true, true, true);
    hw.hardwareDecodeVerified = true;
    h.c.setCapabilities(hw);
    PlaybackPreferences hp;
    hp.hardwareDecode = true;
    h.c.setPreferences(hp);
    h.playing("HHHHHHHHHHH");
    CHECK(h.port.loads.back().decode == player::DecodeMode::Hardware);
    for (int i = 0; i < 4 && h.c.state() != PC::Failed; ++i) {
        h.c.onPlayerFailed(h.now, "decoder error", false);
        h.advance(1500);
        if (h.c.state() == PC::Loading) {
            h.answerOk();
            h.c.onAudioGranted();
            h.c.onPlayerStarted(h.now);
            h.c.onPlayerPlaying(true);
        }
    }
    CHECK(h.c.state() == PC::Failed && h.c.failure() == PC::PlaybackFailed);
    CHECK(errorSettingKey(h.c.errorSettingFacts(false)) == "playback/hardwareDecode");
    // The same faults with software decode: no setting.
    Rig sw;
    sw.playing("SSSSSSSSSSS");
    for (int i = 0; i < 4 && sw.c.state() != PC::Failed; ++i) {
        sw.c.onPlayerFailed(sw.now, "decoder error", false);
        sw.advance(1500);
        if (sw.c.state() == PC::Loading) {
            sw.answerOk();
            sw.c.onAudioGranted();
            sw.c.onPlayerStarted(sw.now);
            sw.c.onPlayerPlaying(true);
        }
    }
    CHECK(sw.c.state() == PC::Failed && errorSettingKey(sw.c.errorSettingFacts(false)).empty());
}

// ---- Cover video ------------------------------------------------------------------

// Cover video: the freeze input and the item flag for a playing dual video.
static void cover_dualVideoDecisionTable() {
    Rig r;
    CHECK(!r.c.coverVideoSetting());           // off until the session applies the setting
    r.c.setCoverVideoSetting(true);
    r.playing("CCCCCCCCCCC");
    // Foreground: never active; the freeze input may be true but FreezePolicy ignores it there.
    r.c.onFirstFrame(r.now);
    CHECK(!r.c.coverVideoActive() && !r.c.coverDecoding());
    CHECK(std::string(r.c.coverOffReason()) == "foreground");
    // Background, cover visible, setting on -> active, decoding kept (not frozen).
    r.c.setAppActive(r.now, false);
    CHECK(r.c.coverVideoActive());
    CHECK(!r.c.coverKeepsDecoding());          // cover not visible yet: frozen per PLY-09
    CHECK(std::string(r.c.coverOffReason()) == "cover-hidden");
    r.c.setCoverVisible(true);
    CHECK(r.c.coverKeepsDecoding() && r.c.coverDecoding());
    CHECK(std::string(r.c.coverOffReason()).empty());
    // Any of them false -> the freeze applies as today.
    r.c.setCoverVisible(false);
    CHECK(!r.c.coverKeepsDecoding() && !r.c.coverDecoding() && r.c.coverVideoActive());
    r.c.setCoverVisible(true);
    r.c.setCoverVideoSetting(false);
    CHECK(!r.c.coverKeepsDecoding() && !r.c.coverVideoActive());
    CHECK(std::string(r.c.coverOffReason()) == "setting-off");
    r.c.setCoverVideoSetting(true);
    CHECK(r.c.coverKeepsDecoding() && r.c.coverVideoActive());
    // Foreground again: inactive item, no cover-only decoding.
    r.c.setAppActive(r.now, true);
    CHECK(!r.c.coverVideoActive() && !r.c.coverDecoding());
    // Stop ends it.
    r.c.setAppActive(r.now, false);
    r.c.stop();
    CHECK(!r.c.coverVideoActive() && !r.c.coverKeepsDecoding());
    CHECK(std::string(r.c.coverOffReason()) == "idle" || std::string(r.c.coverOffReason()) == "cover-hidden");
}

// Cover video: not before this load's first frame (but decoding is kept, so the frame can come).
static void cover_notBeforeFirstFrame() {
    Rig r;
    r.c.setCoverVideoSetting(true);
    r.c.setAppActive(r.now, false);
    r.c.setCoverVisible(true);
    r.playing("FFFFFFFFFFF");
    CHECK(!r.c.coverVideoActive());
    CHECK(r.c.coverKeepsDecoding());           // a frozen branch would never deliver the frame
    r.c.onFirstFrame(r.now);
    CHECK(r.c.coverVideoActive());
    // A new load (quality switch) waits for its own first frame.
    QualityChoice q;
    q.height = 360;
    r.c.setQuality(r.now, q);
    CHECK(!r.c.coverVideoActive());
    r.c.onPlayerStarted(r.now);
    CHECK(!r.c.coverVideoActive());            // preroll is not a frame
    r.c.onFirstFrame(r.now);
    CHECK(r.c.coverVideoActive());
}

// Cover video: listen-only, an audio-only plan and a local file without video never show it.
static void cover_noVideoPlanNoCover() {
    Rig l;
    l.c.setCoverVideoSetting(true);
    l.playing("LOLOLOLOLOL");
    l.c.onFirstFrame(l.now);
    l.c.setAppActive(l.now, false);
    l.c.setCoverVisible(true);
    CHECK(l.c.coverVideoActive() && l.c.coverKeepsDecoding());
    l.c.setAudioOnly(l.now, true);             // listen-only
    CHECK(!l.c.coverVideoActive() && !l.c.coverKeepsDecoding());
    l.c.onPlayerStarted(l.now);
    l.c.onFirstFrame(l.now);
    CHECK(!l.c.coverVideoActive() && !l.c.coverKeepsDecoding());
    // A set with audio formats only (video mode): no video plan (the selection finds nothing).
    Rig a;
    a.c.setCoverVideoSetting(true);
    a.c.setAppActive(a.now, false);
    a.c.setCoverVisible(true);
    a.c.open(a.now, "AUAUAUAUAUA");
    ResolveAnswer audio = ok("AUAUAUAUAUA");
    audio.details.formats = { fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
                              fmt("251", "webm", "none", "opus", 0, 0, 135) };
    a.answer(audio);
    a.c.onAudioGranted();
    a.c.onPlayerStarted(a.now);
    a.c.onFirstFrame(a.now);
    CHECK(a.port.loads.empty() || a.port.loads.back().mode == player::PlayMode::AudioOnly);
    CHECK(!a.c.coverVideoActive() && !a.c.coverKeepsDecoding());
    // A downloaded audio file (no video track).
    Rig d;
    d.c.setCoverVideoSetting(true);
    d.c.setAppActive(d.now, false);
    d.c.setCoverVisible(true);
    LocalMedia m;
    m.videoId = "DADADADADAD";
    m.filePath = m.audioPath = "/home/u/Music/d.m4a";
    d.c.openLocal(d.now, m);
    d.c.onAudioGranted();
    d.c.onPlayerStarted(d.now);
    d.c.onFirstFrame(d.now);
    CHECK(d.port.loads.back().mode == player::PlayMode::AudioOnly);
    CHECK(!d.c.coverVideoActive() && !d.c.coverKeepsDecoding());
    // A downloaded video file: shown.
    m.videoId = "DVDVDVDVDVD";
    m.filePath = m.videoPath = "/home/u/Videos/d.mp4";
    m.audioPath.clear();
    d.c.openLocal(d.now, m);
    d.c.onAudioGranted();
    d.c.onPlayerStarted(d.now);
    CHECK(!d.c.coverVideoActive() && d.c.coverKeepsDecoding());
    d.c.onFirstFrame(d.now);
    CHECK(d.c.coverVideoActive());
}

// Cover video: an error (Failed) turns both off.
static void cover_errorTurnsItOff() {
    Rig r;
    r.c.setCoverVideoSetting(true);
    r.c.open(r.now, "EEEEEEEEEEE");
    ResolveAnswer final;
    final.error = ResolveAnswer::Final;
    r.answer(final);
    r.c.setAppActive(r.now, false);
    r.c.setCoverVisible(true);
    CHECK(r.c.state() == PlaybackController::Failed);
    CHECK(!r.c.coverVideoActive() && !r.c.coverKeepsDecoding());
    // Repeated player faults after the first frame: Failed after the recoveries.
    Rig p;
    p.c.setCoverVideoSetting(true);
    p.playing("PFPFPFPFPFP");
    p.c.onFirstFrame(p.now);
    for (int i = 0; i < 4 && p.c.state() != PlaybackController::Failed; ++i) {
        p.c.onPlayerFailed(p.now, "decoder error", false);
        p.advance(1500);
        if (p.c.state() == PlaybackController::Loading) {
            p.answerOk();
            p.c.onAudioGranted();
            p.c.onPlayerStarted(p.now);
            p.c.onFirstFrame(p.now);
        }
    }
    CHECK(p.c.state() == PlaybackController::Failed);
    p.c.setAppActive(p.now, false);
    p.c.setCoverVisible(true);
    CHECK(!p.c.coverVideoActive() && !p.c.coverKeepsDecoding());
}

// videoAspect before the first frame: the chosen plan's format, the record's size, 0 otherwise.
static void cover_plannedVideoAspect() {
    Rig r;
    CHECK(r.c.plannedVideoAspect() == 0);              // nothing chosen
    r.c.open(r.now, "SHSHSHSHSHS");
    CHECK(r.c.plannedVideoAspect() == 0);              // resolving
    ResolveAnswer shorts = ok("SHSHSHSHSHS");
    shorts.details.formats[1].width = 405;   // 136 and 247: 405x720, 9:16
    shorts.details.formats[2].width = 405;
    r.answer(shorts);
    CHECK(!r.port.loads.empty() && r.port.loads.back().mode == player::PlayMode::Dual);
    CHECK(r.c.plannedVideoAspect() == 0.5625);         // set with the plan, no frame yet
    r.c.setAudioOnly(r.now, true);
    CHECK(r.c.plannedVideoAspect() == 0);              // listen-only
    // A download whose record knows its size; one that does not -> 0 until decoded.
    Rig l;
    LocalMedia m;
    m.videoId = "LALALALALAL";
    m.filePath = m.videoPath = "/home/u/Videos/l.mp4";
    m.width = 1280; m.height = 720;
    l.c.openLocal(l.now, m);
    CHECK(std::fabs(l.c.plannedVideoAspect() - 1280.0 / 720.0) < 1e-12);
    m.width = m.height = 0;
    l.c.openLocal(l.now, m);
    CHECK(l.c.plannedVideoAspect() == 0);
}

// Cover video diagnostics: coverDecodeSeconds accumulates the lifted time only.
static void cover_decodeMeter() {
    CoverDecodeMeter m;
    CHECK(m.elapsedMs(5000) == 0);
    CHECK(!m.update(false, 1000));
    CHECK(m.update(true, 1000) && m.on);
    CHECK(m.elapsedMs(1500) == 500);
    CHECK(!m.update(true, 1700));
    CHECK(m.update(false, 2000) && !m.on);
    CHECK(m.elapsedMs(9000) == 1000);
    CHECK(m.update(true, 10000));
    CHECK(m.elapsedMs(10250) == 1250);
}

// ---- PLY-26 / SEL-07 stream instead ---------------------------------------------------------

static LocalMedia streamableDownload(const char *id) {
    LocalMedia m;
    m.videoId = id;
    m.filePath = m.videoPath = "/home/u/Downloads/s.mp4";
    m.title = "Downloaded title";
    m.durationMs = 600000;
    return m;
}

// A download playing at `positionMs` (granted, prerolled, playing).
static int playingDownload(Rig &r, const char *id, std::int64_t positionMs) {
    const int ctx = r.c.openLocal(r.now, streamableDownload(id));
    r.c.claimContext();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onFirstFrame(r.now);
    r.c.onPlayerPlaying(true);
    r.c.onPosition(r.now, positionMs);
    return ctx;
}

// PLY-26 / SEL-07: local -> streamInstead -> one resolve like open()'s while the file plays on
// (no state change, no stop), then one switch to the dual stream plan at the position captured
// when the plan was ready; the session, owner and title stay; never an end.
static void ply26_streamInsteadSwitchesAtCapturedPosition() {
    Rig r;
    playingDownload(r, "SSSSSSSSSSS", 42000);
    const int owner = r.c.arbiter().owner();
    CHECK(owner != player::kNoContext);
    CHECK(r.c.local() && r.c.canStreamInstead());
    r.port.clear();
    r.c.takeChanges();
    CHECK(r.c.streamInstead(r.now));
    // One resolve, the request open() would make (RES-10 fields, Auto quality, not fresh).
    CHECK(r.port.resolves == 1 && r.port.resolveId == "SSSSSSSSSSS" && !r.port.resolveFull);
    const ResolveRequest expected = resolveRequestFor("SSSSSSSSSSS", r.c.capabilities(), r.c.preferences(), QualityChoice());
    CHECK(r.port.lastRequest.videoCodecs == expected.videoCodecs && r.port.lastRequest.maxHeight == expected.maxHeight);
    CHECK(!r.port.lastRequest.audioOnly && !r.port.lastRequest.speculative);
    // The file plays on: no stop, no load, the same state; a second call does nothing.
    CHECK(r.port.count("stopPlayer") == 0 && r.port.loads.empty());
    CHECK(r.c.state() == PlaybackController::Ready && r.c.local() && r.c.streamInsteadPending());
    CHECK((r.c.takeChanges() & (PlaybackController::StateChange | PlaybackController::DetailsChange)) == 0);
    CHECK(!r.c.canStreamInstead() && !r.c.streamInstead(r.now) && r.port.resolves == 1);
    r.now += 1500;
    r.c.onPosition(r.now, 43500);
    r.answerOk();
    const unsigned changes = r.c.takeChanges();
    CHECK(changes & PlaybackController::StreamSwitchChange);
    CHECK((changes & PlaybackController::EndedChange) == 0);
    CHECK(!r.c.local() && !r.c.streamInsteadPending());
    CHECK(r.port.loads.size() == 1);
    const PlaybackLoad &l = r.port.loads[0];
    CHECK(l.mode == player::PlayMode::Dual && l.video.present() && l.audio.present());
    CHECK(l.startPositionMs >= 42500 && l.startPositionMs <= 44500 && l.startPositionMs == 43500);
    CHECK(r.port.index("stopPlayer") >= 0 && r.port.index("stopPlayer") < r.port.index("load"));
    CHECK(r.port.index("play", r.port.index("load")) > 0);  // it was playing: it plays on
    CHECK(r.c.state() == PlaybackController::Preparing && r.c.status() == PlaybackController::Starting);
    CHECK(r.c.arbiter().owner() == owner && r.c.arbiter().hasSession());
    CHECK(r.c.details().summary.title == "Downloaded title" && r.c.videoId() == "SSSSSSSSSSS");
    CHECK(r.c.switching());
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.takeChanges() & PlaybackController::SwitchResumedChange);
    CHECK(r.c.state() == PlaybackController::Ready && !r.c.qualityOptions().empty());
    CHECK(r.c.candidate() && r.c.hasVideo() && !r.c.canStreamInstead());
    // A stream item now: the quality menu works from the same set, open() of the id re-attaches.
    QualityChoice q;
    q.height = 360;
    r.c.setQuality(r.now, q);
    CHECK(r.port.resolves == 1 && r.port.loads.size() == 2);
    r.c.onPlayerStarted(r.now);
    r.c.open(r.now, "SSSSSSSSSSS");
    CHECK(r.port.resolves == 1 && r.port.loads.size() == 2);
    // After Stop, play() resolves the stream (not the file).
    r.c.stop();
    r.c.play(r.now);
    CHECK(r.port.resolves == 2 && !r.c.local());
}

// PLY-26: a failed stream-instead resolve (error, nothing playable, upcoming) keeps the file
// playing; a transient notice for kStreamNoticeMs, no failure, no setting key.
static void ply26_streamInsteadFailureKeepsFile() {
    for (int kind = 0; kind < 3; ++kind) {
        Rig r;
        playingDownload(r, "FFFFFFFFFFF", 30000);
        r.port.clear();
        CHECK(r.c.streamInstead(r.now));
        r.c.takeChanges();
        ResolveAnswer a = ok("FFFFFFFFFFF");
        if (kind == 0) { a = ResolveAnswer(); a.error = ResolveAnswer::Retryable; a.errorCode = 1; }
        if (kind == 1) a.details.formats.clear();
        if (kind == 2) a.upcoming = true;
        r.answer(a);
        CHECK(r.c.local() && r.c.state() == PlaybackController::Ready && r.c.playerPlaying());
        CHECK(r.port.count("stopPlayer") == 0 && r.port.loads.empty() && r.port.resolves == 1);  // no fresh retry
        CHECK(r.c.streamNoticeActive() && r.c.failure() == PlaybackController::NoFailure);
        CHECK(errorSettingKey(r.c.errorSettingFacts(false)).empty());
        CHECK(r.c.takeChanges() & PlaybackController::StateChange);
        CHECK(r.c.canStreamInstead());  // may be tried again
        r.advance(PlaybackController::kStreamNoticeMs + 1);
        CHECK(!r.c.streamNoticeActive() && (r.c.takeChanges() & PlaybackController::StateChange));
        r.c.onPosition(r.now, 36000);  // still the file's position
        CHECK(r.c.positionMs() == 36000);
    }
}

// PLY-26: a no-op unless a download with a YouTube id is in the session.
static void ply26_streamInsteadOnlyForLocal() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.port.clear();
    CHECK(!r.c.canStreamInstead() && !r.c.streamInstead(r.now) && r.port.resolves == 0);
    Rig p;  // a file without a video id ("local:<path>")
    LocalMedia m = streamableDownload("");
    p.c.openLocal(p.now, m);
    CHECK(p.c.local() && !p.c.streamInstead(p.now) && p.port.resolves == 0);
    Rig i;  // after Stop
    playingDownload(i, "IIIIIIIIIII", 1000);
    i.c.stop();
    CHECK(!i.c.streamInstead(i.now) && i.port.resolves == 0);
    // Opening another video while it resolves cancels it; its late answer is stale.
    Rig o;
    playingDownload(o, "OOOOOOOOOOO", 1000);
    CHECK(o.c.streamInstead(o.now));
    const unsigned g = o.port.resolveGeneration;
    o.port.clear();
    o.c.open(o.now, "PPPPPPPPPPP");
    CHECK(o.port.count("cancelResolve") >= 1 && !o.c.streamInsteadPending());
    o.c.onResolved(o.now, 1700000000000LL, "OOOOOOOOOOO", g, ok("OOOOOOOOOOO"));
    CHECK(o.c.videoId() == "PPPPPPPPPPP" && o.port.loads.empty());
}

// PLY-26 / SEL-07 / PLY-07 / SEL-08: paused stays paused, the rate and listen-only are kept.
static void ply26_streamInsteadKeepsPauseRateListenOnly() {
    Rig r;
    playingDownload(r, "KKKKKKKKKKK", 20000);
    r.c.setRate(1.5);
    r.c.pause();
    r.c.onPlayerPlaying(false);
    r.port.clear();
    CHECK(r.c.streamInstead(r.now));
    r.answerOk();
    CHECK(r.port.loads.size() == 1 && r.port.loads[0].startPositionMs == 20000);
    CHECK(std::fabs(r.port.loads[0].rate - 1.5) < 1e-9 && std::fabs(r.c.rate() - 1.5) < 1e-9);
    CHECK(r.port.count("play") == 0 && !r.c.wantsPlay());
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.state() == PlaybackController::Ready && !r.c.wantsPlay());
    // Listen-only: the request asks for audio, the switch loads the audio-only plan.
    Rig a;
    playingDownload(a, "LLLLLLLLLLL", 5000);
    a.c.setAudioOnly(a.now, true);
    a.c.onPlayerStarted(a.now);
    a.c.onPosition(a.now, 5000);
    a.port.clear();
    CHECK(a.c.streamInstead(a.now));
    CHECK(a.port.lastRequest.audioOnly);
    a.answerOk();
    CHECK(a.port.loads.size() == 1 && a.port.loads[0].mode == player::PlayMode::AudioOnly && a.c.audioOnly());
    // Listen-only turned on while it resolves: asked again for the new purpose.
    Rig b;
    playingDownload(b, "BBBBBBBBBBB", 5000);
    CHECK(b.c.streamInstead(b.now) && !b.port.lastRequest.audioOnly);
    b.c.setAudioOnly(b.now, true);
    CHECK(b.c.streamInsteadPending() && b.port.lastRequest.audioOnly && b.port.resolves == 2);
}

// ---- Shorts pager (DSC-17 MAY swipe viewer) -------------------------------------------------

static media::SelectionInput inputOf(const ResolveAnswer &a) {
    media::SelectionInput in;
    in.formats = a.details.formats;
    in.liveStatus = a.details.summary.liveStatus;
    in.manifestUrl = a.details.manifestUrl;
    return in;
}

// Shorts loop: at the end of stream one seek to 0 and play on in the same load: no up-next, no
// related autoplay, no prefetch, the queue untouched; the end is saved once per item.
static void shorts_loopRestartsAtZero() {
    Rig r;
    r.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ"}, 0);
    const int ctx = r.c.open(r.now, "Q0QQQQQQQQQ", PlaybackController::kLoopOn);
    CHECK(ctx > 0 && r.c.loop());
    r.answerOk();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    r.c.setRelatedCandidate("RRRRRRRRRRR");
    r.c.onPosition(r.now, 599000);
    const int loads = static_cast<int>(r.port.loads.size()), resolves = r.port.resolves;
    r.port.calls.clear();
    r.c.onEnded(r.now);
    CHECK(r.port.count("seek") == 1 && r.port.last("seek") && r.port.last("seek")->value == 0);
    CHECK(r.port.index("play") > r.port.index("seek"));
    CHECK(r.port.count("stopPlayer") == 0 && static_cast<int>(r.port.loads.size()) == loads);
    CHECK(r.port.count("savePosition") == 1 && r.port.last("savePosition")->value == player::kNoResume);
    CHECK(r.c.upNextVideoId().empty() && !r.c.autoplay().countdownActive());
    CHECK(r.c.queueIndex() == 0 && r.c.videoId() == "Q0QQQQQQQQQ");
    CHECK(r.c.positionMs() == 0 && r.c.state() == PlaybackController::Ready && r.c.wantsPlay());
    // A second end: one more seek, no second save; no prefetch however long it plays.
    r.c.onPosition(r.now, 599000);
    r.c.onEnded(r.now);
    CHECK(r.port.count("seek") == 2 && r.port.count("savePosition") == 1);
    r.advance(PlaybackController::kPrefetchDelayMs + 1000);
    CHECK(r.port.prefetches.empty() && r.port.resolves == resolves);
    // Not a loop item: the queue advances as before.
    Rig n;
    n.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ"}, 0);
    n.playing("Q0QQQQQQQQQ");
    CHECK(!n.c.loop());
    n.c.onEnded(n.now);
    CHECK(n.port.resolveId == "Q1QQQQQQQQQ");
}

// Shorts loop hint: a new video loops only with kLoopOn; re-opening the session's video keeps the
// mode unless the hint says otherwise, and never reloads; the next video starts unlooped.
static void shorts_loopHintPerItem() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA", PlaybackController::kLoopOn);
    r.answerOk();
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.takeChanges();
    CHECK(r.c.loop());
    const int resolves = r.port.resolves;
    const std::size_t loads = r.port.loads.size();
    r.c.open(r.now, "AAAAAAAAAAA");  // the watch page re-attaches: loop kept
    CHECK(r.c.loop());
    r.c.open(r.now, "AAAAAAAAAAA", PlaybackController::kLoopOff);  // ... and switches it off
    CHECK(!r.c.loop() && (r.c.takeChanges() & PlaybackController::ControlsChange));
    r.c.open(r.now, "AAAAAAAAAAA", PlaybackController::kLoopOn);
    CHECK(r.c.loop());
    CHECK(r.port.resolves == resolves && r.port.loads.size() == loads && r.port.count("stopPlayer") == 0);
    r.c.open(r.now, "BBBBBBBBBBB");  // another video: plays once
    CHECK(!r.c.loop());
    r.c.open(r.now, "CCCCCCCCCCC", PlaybackController::kLoopOff);
    CHECK(!r.c.loop());
    // A downloaded item takes the hint the same way.
    LocalMedia m;
    m.videoId = "DDDDDDDDDDD";
    m.filePath = "/home/user/Videos/d.webm";
    m.videoPath = m.filePath;
    r.c.openLocal(r.now, m, PlaybackController::kLoopOn);
    CHECK(r.c.loop() && r.c.local());
}

// Shorts likelyNext: the decision table (order of the reasons; Keep and Skip leave the preload).
static void shorts_likelyNextDecision() {
    LikelyNextContext c;
    c.validId = true;
    CHECK(likelyNextDecision(c).action == LikelyNextDecision::Start && !*likelyNextDecision(c).reason);
    LikelyNextContext b = c;
    b.appActive = false;
    b.alreadyPreloaded = true;
    CHECK(likelyNextDecision(b).action == LikelyNextDecision::Skip && std::string(likelyNextDecision(b).reason) == "background");
    LikelyNextContext n = c;
    n.resolverAvailable = false;
    CHECK(std::string(likelyNextDecision(n).reason) == "no-resolver");
    LikelyNextContext i = c;
    i.validId = false;
    CHECK(likelyNextDecision(i).action == LikelyNextDecision::Skip && std::string(likelyNextDecision(i).reason) == "invalid-id");
    LikelyNextContext p = c;
    p.sessionVideo = true;
    p.starting = true;
    CHECK(likelyNextDecision(p).action == LikelyNextDecision::Skip && std::string(likelyNextDecision(p).reason) == "playing");
    LikelyNextContext l = c;
    l.local = true;
    CHECK(likelyNextDecision(l).action == LikelyNextDecision::Skip && std::string(likelyNextDecision(l).reason) == "local");
    LikelyNextContext u = c;
    u.blocked = true;
    CHECK(std::string(likelyNextDecision(u).reason) == "upcoming");
    LikelyNextContext k = c;
    k.alreadyPreloaded = true;
    k.starting = true;
    CHECK(likelyNextDecision(k).action == LikelyNextDecision::Keep && std::string(likelyNextDecision(k).reason) == "preloaded");
    LikelyNextContext s = c;
    s.pressSpeculation = true;
    CHECK(likelyNextDecision(s).action == LikelyNextDecision::Skip && std::string(likelyNextDecision(s).reason) == "pressed");
    LikelyNextContext d = c;
    d.starting = true;
    CHECK(likelyNextDecision(d).action == LikelyNextDecision::Defer && std::string(likelyNextDecision(d).reason) == "starting");
    CHECK(kLikelyNextHoldMs == 60000);
}

// Shorts likelyNext: firstLoadFor predicts the first load open() makes for the same answer (dual,
// listen-only, the low-rung A/B), so the held branches are the ones the open uses.
static void shorts_firstLoadMatchesOpen() {
    for (int lowRung = 0; lowRung < 2; ++lowRung) {
        for (int listen = 0; listen < 2; ++listen) {
            Rig r;
            r.c.setStartLowRung(lowRung != 0);
            r.c.setAudioOnly(r.now, listen != 0);
            r.c.open(r.now, "AAAAAAAAAAA");
            const ResolveAnswer a = rungsAnswer("AAAAAAAAAAA");
            r.answer(a);
            CHECK(r.port.loads.size() == 1);
            if (r.port.loads.empty()) continue;
            PlaybackPreferences p;
            p.mode = listen ? PlaybackMode::AudioOnly : PlaybackMode::Video;
            PlaybackLoad predicted;
            CHECK(firstLoadFor("AAAAAAAAAAA", inputOf(a), software(), p, lowRung != 0, &predicted));
            const PlaybackLoad &l = r.port.loads[0];
            CHECK(predicted.mode == l.mode && predicted.videoId == l.videoId);
            CHECK(predicted.video.uri == l.video.uri && predicted.audio.uri == l.audio.uri && predicted.muxed.uri == l.muxed.uri);
            if (lowRung && !listen) CHECK(loadsFormat(predicted, "135"));
            if (!lowRung && !listen) CHECK(loadsFormat(predicted, "136"));
            if (listen) CHECK(predicted.mode == player::PlayMode::AudioOnly && !predicted.video.present());
        }
    }
    // Nothing playable: no plan.
    ResolveAnswer empty;
    empty.details = details("EEEEEEEEEEE");
    empty.details.formats.clear();
    CHECK(!firstLoadFor("EEEEEEEEEEE", inputOf(empty), software(), PlaybackPreferences(), false, nullptr));
    // The low-rung chain is empty for an already low or muxed pick.
    const std::vector<StreamCandidate> normal = selectStreamsWithFallback(inputOf(rungsAnswer("A")), software(),
                                                                          PlaybackPreferences(), QualityChoice());
    CHECK(!normal.empty() && !lowRungChain(inputOf(rungsAnswer("A")), software(), PlaybackPreferences(), normal[0]).empty());
    StreamCandidate muxed;
    muxed.kind = StreamKind::Progressive;
    muxed.height = 720;
    CHECK(lowRungChain(inputOf(rungsAnswer("A")), software(), PlaybackPreferences(), muxed).empty());
}


// ---- Watch overlay controls --------------------------------------------------------------------

static MediaFormat audio(const char *id, const char *ext, const char *codec, const char *language, int preference,
                         bool drc = false) {
    MediaFormat f = fmt(id, ext, "none", codec, 0, 0, ext[0] == 'w' ? 135 : 129);
    f.language = language;
    f.languagePreference = preference;
    f.dynamicRangeCompressed = drc;
    return f;
}

// A set with the original (en, Opus and AAC, each with a DRC twin), a German dub and an English
// audio-description track.
static ResolveAnswer dubbed(const char *id) {
    ResolveAnswer a = ok(id);
    a.details.formats = {
        fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500),
        audio("251-en", "webm", "opus", "en", 10), audio("251-en-drc", "webm", "opus", "en", 10, true),
        audio("140-en", "m4a", "mp4a.40.2", "en", 10), audio("140-en-drc", "m4a", "mp4a.40.2", "en", 10, true),
        audio("251-de", "webm", "opus", "de", -1), audio("140-de", "m4a", "mp4a.40.2", "de", -1),
        audio("251-ad", "webm", "opus", "en", -10),
    };
    return a;
}

static bool loadsAudio(const PlaybackLoad &l, const char *id) {
    return l.audio.uri == std::string("https://media.example/") + id;
}

// SEL-05 / SEL-07: the dub picker lists the set's entries (original first); a pick plays from the
// same set at the captured position (no resolve) and is remembered for the session's next videos;
// the original stores "no preference", an audio-description pick is not remembered.
static void sel05_audioTrackSwitchWithoutResolve() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    CHECK(r.c.audioTrackOptions().empty() && r.c.playingAudioTrackIndex() == -1);  // nothing chosen yet
    r.answer(dubbed("AAAAAAAAAAA"));
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    CHECK(loadsAudio(r.port.loads.back(), "251-en"));
    std::vector<AudioTrackOption> options = r.c.audioTrackOptions();
    CHECK(options.size() == 3);
    if (options.size() != 3) return;
    CHECK(audioTrackId(options[0]) == "en" && options[0].original);
    CHECK(audioTrackId(options[1]) == "de" && !options[1].original);
    CHECK(audioTrackId(options[2]) == "en#ad" && options[2].descriptive);
    CHECK(r.c.playingAudioTrackIndex() == 0);
    r.c.onPosition(r.now, 42000);
    r.c.takeChanges();
    CHECK(!r.c.setAudioTrack(r.now, "fr"));                       // not in the set: nothing
    CHECK(r.port.loads.size() == 1);
    CHECK(r.c.setAudioTrack(r.now, "de"));
    const unsigned changes = r.c.takeChanges();
    CHECK(changes & PlaybackController::AudioSwitchChange);
    CHECK(r.port.resolves == 1);                                  // same set
    CHECK(r.port.loads.size() == 2 && loadsAudio(r.port.loads.back(), "251-de"));
    CHECK(r.port.loads.back().startPositionMs == 42000 && r.port.loads.back().video.uri == r.port.loads[0].video.uri);
    CHECK(r.c.switching());
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.takeChanges() & PlaybackController::SwitchResumedChange);
    CHECK(r.c.playingAudioTrackIndex() == 1);
    CHECK(r.c.dubRemembered() && r.c.rememberedDub() == "de");
    // The same entry again: no switch.
    r.c.setAudioTrack(r.now, "de");
    CHECK(r.port.loads.size() == 2);
    // An audio-description pick plays but is not remembered.
    r.c.setAudioTrack(r.now, "en#ad");
    CHECK(loadsAudio(r.port.loads.back(), "251-ad") && r.c.rememberedDub() == "de");
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.playingAudioTrackIndex() == 2);
    // The next video asks for the remembered dub and starts with it.
    r.c.open(r.now, "BBBBBBBBBBB");
    CHECK(r.port.lastRequest.dubLanguage == "de");
    r.answer(dubbed("BBBBBBBBBBB"));
    CHECK(loadsAudio(r.port.loads.back(), "251-de"));
    // Choosing the original stores "no preference" (SEL-05: the Polish case).
    r.c.onPlayerStarted(r.now);
    r.c.setAudioTrack(r.now, "en");
    CHECK(loadsAudio(r.port.loads.back(), "251-en") && r.c.dubRemembered() && r.c.rememberedDub().empty());
    r.c.open(r.now, "CCCCCCCCCCC");
    CHECK(r.port.lastRequest.dubLanguage.empty());
    // A muxed or local plan has no picker.
    Rig m;
    m.playing("MMMMMMMMMMM");
    QualityChoice q;
    CHECK(!m.c.audioTrackOptions().empty());
    LocalMedia local;
    local.videoId = "LLLLLLLLLLL";
    local.filePath = "/home/user/v.mp4";
    local.videoPath = local.filePath;
    m.c.openLocal(m.now, local);
    CHECK(m.c.audioTrackOptions().empty() && !m.c.setAudioTrack(m.now, "en"));
    (void)q;
}

// Stable volume: the DRC track of the playing language, as a same-set switch, and back.
static void sel04_stableVolumeSwitch() {
    Rig r;
    r.c.open(r.now, "AAAAAAAAAAA");
    r.answer(dubbed("AAAAAAAAAAA"));
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPosition(r.now, 9000);
    CHECK(r.c.stableVolumeAvailable());                            // "en" has a levelled twin
    r.c.setStableVolume(r.now, true);
    CHECK(r.c.stableVolume() && r.port.resolves == 1);
    CHECK(r.port.loads.size() == 2 && loadsAudio(r.port.loads.back(), "251-en-drc"));
    CHECK(r.c.stableVolumeAvailable());                            // still a choice while it is on
    CHECK(r.port.loads.back().startPositionMs == 9000);
    r.c.onPlayerStarted(r.now);
    CHECK(r.c.playingAudioTrackIndex() == 0);                      // still the original entry
    // A dub without a DRC twin keeps its own track.
    r.c.setAudioTrack(r.now, "de");
    CHECK(loadsAudio(r.port.loads.back(), "251-de"));
    r.c.onPlayerStarted(r.now);
    CHECK(!r.c.stableVolumeAvailable());                           // nothing the switch could change
    r.c.setAudioTrack(r.now, "en");
    CHECK(loadsAudio(r.port.loads.back(), "251-en-drc"));
    r.c.onPlayerStarted(r.now);
    r.c.setStableVolume(r.now, false);
    CHECK(loadsAudio(r.port.loads.back(), "251-en"));
    // Kept for the next video; applied at its first selection (no extra load).
    r.c.setStableVolume(r.now, true);
    r.c.open(r.now, "BBBBBBBBBBB");
    const std::size_t loads = r.port.loads.size();
    r.answer(dubbed("BBBBBBBBBBB"));
    CHECK(r.port.loads.size() == loads + 1 && loadsAudio(r.port.loads.back(), "251-en-drc"));
    // Not offered before the details, for a set without a levelled track, or once idle.
    CHECK(r.c.stableVolumeAvailable());
    {
        Rig plain;
        plain.c.open(plain.now, "CCCCCCCCCCC");
        CHECK(!plain.c.stableVolumeAvailable());
        plain.answerOk();
        plain.c.onAudioGranted();
        plain.c.onPlayerStarted(plain.now);
        CHECK(!plain.c.stableVolumeAvailable());
        plain.c.setStableVolume(plain.now, true);
        CHECK(!plain.c.stableVolumeAvailable() && plain.port.loads.size() == 1);
    }
    r.c.stop();
    CHECK(!r.c.stableVolumeAvailable());
    // The pure step: no choice, stable volume off: the chain is untouched.
    std::vector<StreamCandidate> chain(1);
    chain[0].kind = StreamKind::Adaptive;
    chain[0].audio = audio("251-en", "webm", "opus", "en", 10);
    SelectionInput in;
    in.formats = dubbed("X").details.formats;
    applyAudioChoice(&chain, in, software(), AudioChoice());
    CHECK(chain[0].audio.id == "251-en");
    AudioChoice stable;
    stable.stableVolume = true;
    applyAudioChoice(&chain, in, software(), stable);
    CHECK(chain[0].audio.id == "251-en-drc");
    // firstLoadFor (Shorts preload) predicts the same audio.
    PlaybackLoad predicted;
    CHECK(firstLoadFor("BBBBBBBBBBB", in, software(), PlaybackPreferences(), false, &predicted, true));
    CHECK(loadsAudio(predicted, "251-en-drc"));
}

// Shorts (DSC-17): holds pause and resume; a user's choice wins; the app leaving the foreground is a
// hold even with background playback on; a new video, Stop and a non-Short item end holds; a Short
// stores no resume point (the position saved is "none"); a plain video is unaffected.
static void shorts_holdsAndRules() {
    // A hold on an item that is not a Short does nothing.
    Rig plain;
    plain.playing("AAAAAAAAAAA");
    plain.c.holdShort("tab");
    CHECK(!plain.c.shortHeld() && plain.c.wantsPlay() && plain.port.count("pause") == 0);
    plain.c.setAppActive(plain.now, false);
    CHECK(plain.c.wantsPlay());                                  // background playback on: the sound plays on
    plain.c.setAppActive(plain.now, true);

    // The first reason pauses, the last release resumes.
    Rig r;
    r.playing("BBBBBBBBBBB");
    r.c.setShortItem(true);
    CHECK(r.c.shortItem() && !r.c.shortHeld());
    r.c.holdShort("tab");
    CHECK(r.c.shortHeld() && !r.c.wantsPlay() && r.port.count("pause") == 1 && r.c.shortHolds() == 1);
    r.c.holdShort("comments");
    CHECK(r.c.shortHolds() == 2 && r.port.count("pause") == 1);   // one pause for the first reason
    r.c.releaseShort(r.now, "tab");
    CHECK(r.c.shortHeld() && !r.c.wantsPlay());                  // comments still hold it
    r.c.releaseShort(r.now, "unknown");
    CHECK(r.c.shortHeld());
    r.c.releaseShort(r.now, "comments");
    CHECK(!r.c.shortHeld() && r.c.wantsPlay() && r.c.shortHolds() == 0);
    CHECK(r.port.count("play") >= 1);

    // Paused by the user before the hold: the release leaves it paused.
    Rig u;
    u.playing("CCCCCCCCCCC");
    u.c.setShortItem(true);
    u.c.pause();
    u.c.holdShort("tab");
    u.c.releaseShort(u.now, "tab");
    CHECK(!u.c.wantsPlay() && !u.c.shortHeld());
    // The user pauses during the hold: the release does not resume it.
    Rig w;
    w.playing("DDDDDDDDDDD");
    w.c.setShortItem(true);
    w.c.holdShort("comments");
    w.c.pause();
    w.c.releaseShort(w.now, "comments");
    CHECK(!w.c.wantsPlay());
    // The user plays during the hold (a lock-screen play): it plays and the release changes nothing.
    Rig p;
    p.playing("EEEEEEEEEEE");
    p.c.setShortItem(true);
    p.c.holdShort("comments");
    p.c.play(p.now);
    CHECK(p.c.wantsPlay());
    const int pauses = p.port.count("pause");
    p.c.releaseShort(p.now, "comments");
    CHECK(p.c.wantsPlay() && p.port.count("pause") == pauses);

    // The app in the background holds a Short even with background playback on, and resumes on return.
    Rig b;
    b.playing("FFFFFFFFFFF");
    b.c.setShortItem(true);
    CHECK(b.c.backgroundPlayback());
    b.c.setAppActive(b.now, false);
    CHECK(b.c.shortHeld() && !b.c.wantsPlay());
    b.c.holdShort("comments");
    b.c.setAppActive(b.now, true);
    CHECK(b.c.shortHeld() && !b.c.wantsPlay());                  // still held by the comments
    b.c.releaseShort(b.now, "comments");
    CHECK(!b.c.shortHeld() && b.c.wantsPlay());
    // A Short that opens while the app is in the background starts held.
    Rig o;
    o.playing("GGGGGGGGGGG");
    o.c.setAppActive(o.now, false);
    CHECK(o.c.wantsPlay());
    o.c.setShortItem(true);
    CHECK(o.c.shortHeld() && !o.c.wantsPlay());
    o.c.setAppActive(o.now, true);
    CHECK(!o.c.shortHeld() && o.c.wantsPlay());

    // A new video ends every hold; so does Stop; and a non-Short item takes the flag away.
    Rig n;
    n.playing("HHHHHHHHHHH");
    n.c.setShortItem(true);
    n.c.holdShort("tab");
    n.c.open(n.now, "IIIIIIIIIII");
    CHECK(!n.c.shortHeld() && n.c.shortHolds() == 0 && n.c.wantsPlay());
    n.c.holdShort("tab");
    n.c.stop();
    CHECK(!n.c.shortHeld() && n.c.shortHolds() == 0);
    Rig k;
    k.playing("JJJJJJJJJJJ");
    k.c.setShortItem(true);
    k.c.holdShort("tab");
    k.c.setShortItem(false);
    CHECK(!k.c.shortItem() && !k.c.shortHeld());

    // No resume point for a Short: leaving it for another video saves "none"; a plain video saves
    // its position (the case at the top of the file).
    Rig s;
    s.playing("KKKKKKKKKKK");
    s.c.onPosition(s.now, 20000);
    s.c.setShortItem(true);
    s.c.open(s.now, "LLLLLLLLLLL");
    CHECK(s.port.count("savePosition") == 1 && s.port.last("savePosition")->value == player::kNoResume);
    s.answerOk();
    s.c.onAudioGranted();
    s.c.onPlayerStarted(s.now);
    s.c.setShortItem(true);
    s.c.onPosition(s.now, 25000);
    s.c.stop();
    CHECK(s.port.count("savePosition") == 2 && s.port.last("savePosition")->value == player::kNoResume);
}

// The "Background playback" setting: off pauses when the app leaves the foreground and stays
// paused on return; a play asked for in the background still plays; on (default) nothing changes.
static void set01_backgroundPlaybackSwitch() {
    Rig on;
    on.playing("AAAAAAAAAAA");
    CHECK(on.c.backgroundPlayback());
    on.c.setAppActive(on.now, false);
    CHECK(on.c.wantsPlay() && on.port.count("pause") == 0);
    Rig off;
    off.c.setBackgroundPlayback(false);
    off.playing("AAAAAAAAAAA");
    off.c.setAppActive(off.now, false);
    CHECK(!off.c.wantsPlay() && off.port.count("pause") == 1);
    off.c.setAppActive(off.now, true);
    CHECK(!off.c.wantsPlay() && off.port.count("play") <= 1);          // stays paused until the user plays
    off.c.setAppActive(off.now, false);
    CHECK(off.port.count("pause") == 1);                               // nothing to pause twice
    off.c.play(off.now);                                               // cover action / lock-screen controls
    CHECK(off.c.wantsPlay());
    // A video still loading when the app is left starts paused.
    Rig loading;
    loading.c.setBackgroundPlayback(false);
    loading.c.open(loading.now, "BBBBBBBBBBB");
    CHECK(loading.c.wantsPlay());
    loading.c.setAppActive(loading.now, false);
    CHECK(!loading.c.wantsPlay());
    // Idle: nothing happens.
    Rig idle;
    idle.c.setBackgroundPlayback(false);
    idle.c.setAppActive(idle.now, false);
    CHECK(idle.port.count("pause") == 0);
}

// The overlay's loop toggle: same semantics as the loop hint, on the playing item.
static void ovl_writableLoop() {
    Rig r;
    r.c.setRelatedAutoplay(true);
    r.playing("AAAAAAAAAAA");
    r.c.setRelatedCandidate("RRRRRRRRRRR");
    CHECK(!r.c.loop());
    r.c.takeChanges();
    r.c.setLoop(true);
    CHECK(r.c.loop() && (r.c.takeChanges() & PlaybackController::ControlsChange));
    r.port.calls.clear();
    r.c.onPosition(r.now, 599000);
    r.c.onEnded(r.now);
    CHECK(r.port.count("seek") == 1 && r.port.last("seek")->value == 0 && r.port.count("play") == 1);
    CHECK(r.c.upNextVideoId().empty() && !r.c.autoplay().countdownActive());
    CHECK(r.port.count("savePosition") == 1);
    r.c.onPosition(r.now, 599000);
    r.c.onEnded(r.now);
    CHECK(r.port.count("savePosition") == 1);  // the end is saved once
    r.c.setLoop(false);
    CHECK(!r.c.loop());
    r.c.onPosition(r.now, 599000);
    r.c.onEnded(r.now);
    CHECK(r.c.upNextVideoId() == "RRRRRRRRRRR" && r.c.autoplay().countdownActive());
    r.c.setLoop(true);                              // turning it on cancels the countdown
    CHECK(r.c.upNextVideoId().empty() && !r.c.autoplay().countdownActive());
    // A new video plays once.
    r.c.open(r.now, "BBBBBBBBBBB");
    CHECK(!r.c.loop());
}

// Hold-to-speed: 2x while held, the previous rate back on release; never remembered.
static void ovl_holdSpeedRestores() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    r.c.setRate(1.5);
    r.port.clear();
    r.c.holdSpeed(true);
    CHECK(r.c.speedHeld() && r.c.rate() == kHoldSpeedRate);
    CHECK(r.port.count("setRate") == 1 && r.port.last("setRate")->value == 200 && r.port.count("rememberRate") == 0);
    CHECK(r.c.rememberedRate() == 1.5);
    r.c.holdSpeed(true);                            // again: nothing
    CHECK(r.port.count("setRate") == 1);
    r.c.holdSpeed(false);
    CHECK(!r.c.speedHeld() && r.c.rate() == 1.5 && r.port.last("setRate")->value == 150);
    CHECK(r.port.count("rememberRate") == 0);
    // A rate picked while held is remembered and applies at the release.
    r.c.holdSpeed(true);
    r.c.setRate(0.5);
    CHECK(r.c.rate() == kHoldSpeedRate && r.c.rememberedRate() == 0.5);
    r.c.holdSpeed(false);
    CHECK(r.c.rate() == 0.5);
    // A switch while held keeps 2x; a new video releases it.
    r.c.holdSpeed(true);
    QualityChoice q;
    q.height = 360;
    r.c.setQuality(r.now, q);
    CHECK(r.port.loads.back().rate == kHoldSpeedRate);
    r.c.open(r.now, "BBBBBBBBBBB");
    r.answerOk();
    CHECK(!r.c.speedHeld() && r.port.loads.back().rate == 0.5);
}

// Sleep timer: minutes, end of the video, off. Firing pauses and keeps the session.
static void ovl_sleepTimer() {
    Rig r;
    r.c.setQueue({"Q0QQQQQQQQQ", "Q1QQQQQQQQQ"}, 0);
    r.playing("Q0QQQQQQQQQ");
    CHECK(!r.c.sleepTimerActive() && r.c.sleepTimerSecondsLeft(r.now) == -1);
    r.c.takeChanges();
    r.c.setSleepTimer(r.now, 1);
    CHECK(r.c.sleepTimerActive() && !r.c.sleepTimerAtEnd());
    CHECK(r.c.takeChanges() & PlaybackController::SleepTimerChange);
    CHECK(r.c.sleepTimerSecondsLeft(r.now) == 60 && r.c.sleepDeadlineMs() == r.now + 60000);
    r.advance(30500);
    CHECK(r.c.sleepTimerSecondsLeft(r.now) == 30 && r.c.wantsPlay());
    CHECK(r.c.nextDeadlineMs() == r.c.sleepDeadlineMs());  // one deadline, no ticker
    r.port.calls.clear();
    r.advance(29500);
    CHECK(!r.c.sleepTimerActive() && r.c.sleepTimerSecondsLeft(r.now) == -1);
    CHECK(r.c.takeChanges() & PlaybackController::SleepTimerChange);
    CHECK(!r.c.wantsPlay() && r.port.count("pause") == 1 && r.port.count("stopPlayer") == 0);
    CHECK(r.c.state() == PlaybackController::Ready && r.c.arbiter().hasSession() && r.c.videoId() == "Q0QQQQQQQQQ");
    // Off.
    r.c.play(r.now);
    r.c.setSleepTimer(r.now, 5);
    r.c.setSleepTimer(r.now, 0);
    CHECK(!r.c.sleepTimerActive() && r.c.nextDeadlineMs() < 0);
    // At the end of the video: the media time left, then a pause instead of the next queue item.
    r.c.onPlayerPlaying(true);
    r.c.setSleepTimer(r.now, kSleepAtEndOfVideo);
    r.c.onPosition(r.now, 590000);
    CHECK(r.c.sleepTimerAtEnd() && r.c.sleepTimerSecondsLeft(r.now) == 10);
    r.c.setRate(2.0);
    CHECK(r.c.sleepTimerSecondsLeft(r.now) == 5);
    const int resolves = r.port.resolves;
    r.port.calls.clear();
    r.c.onEnded(r.now);
    CHECK(r.port.resolves == resolves && r.c.videoId() == "Q0QQQQQQQQQ" && r.c.queueIndex() == 0);
    CHECK(!r.c.wantsPlay() && !r.c.sleepTimerActive() && r.c.upNextVideoId().empty());
    CHECK(r.c.arbiter().hasSession());
    // A loop item returns to 0 paused.
    Rig l;
    l.c.open(l.now, "LLLLLLLLLLL", PlaybackController::kLoopOn);
    l.answerOk();
    l.c.onAudioGranted();
    l.c.onPlayerStarted(l.now);
    l.c.setSleepTimer(l.now, kSleepAtEndOfVideo);
    l.port.calls.clear();
    l.c.onEnded(l.now);
    CHECK(l.port.count("seek") == 1 && l.port.count("play") == 0 && !l.c.wantsPlay());
    // Stop clears it; an unknown value is off.
    l.c.setSleepTimer(l.now, 30);
    l.c.stop();
    CHECK(!l.c.sleepTimerActive());
    l.c.setSleepTimer(l.now, -7);
    CHECK(!l.c.sleepTimerActive());
    l.c.setSleepTimer(l.now, 100000);
    CHECK(l.c.sleepTimerSecondsLeft(l.now) == kSleepTimerMaxMinutes * 60);
}

// SB-02 seek bar: the skipper's segments are exposed, and cleared for the next video.
static void sb02_sponsorSegmentsExposed() {
    Rig r;
    r.playing("AAAAAAAAAAA");
    CHECK(r.c.sponsorSegments().empty());
    std::vector<player::SponsorSegment> segs;
    segs.push_back(player::SponsorSegment(10000, 20000, player::SponsorCategory::Sponsor));
    segs.push_back(player::SponsorSegment(30000, 30000, player::SponsorCategory::PoiHighlight));
    r.c.takeChanges();
    r.c.setSponsorSegments(segs);
    CHECK(r.c.takeChanges() & PlaybackController::SegmentsChange);
    CHECK(r.c.sponsorSegments().size() == 2 && r.c.sponsorSegments()[0].endMs == 20000);
    CHECK(r.c.sponsorSegments()[1].category == player::SponsorCategory::PoiHighlight);
    r.c.open(r.now, "BBBBBBBBBBB");
    CHECK(r.c.sponsorSegments().empty() && (r.c.takeChanges() & PlaybackController::SegmentsChange));
}

// Scrub preview: the storyboard cell of a position (pure).
static void ovl_storyboardCell() {
    Storyboard b;
    b.width = 160; b.height = 90; b.columns = 5; b.rows = 5;
    StoryboardFragment f;
    f.url = "https://i.example/sb/M0.jpg"; f.durationMs = 50000; b.fragments.push_back(f);   // 2 s a cell
    f.url = "https://i.example/sb/M1.jpg"; b.fragments.push_back(f);
    f.url = "https://i.example/sb/M2.jpg"; f.durationMs = 10000; b.fragments.push_back(f);   // 5 cells used
    StoryboardCell c;
    CHECK(storyboardCell(b, 0, &c) && c.url == "https://i.example/sb/M0.jpg" && c.x == 0 && c.y == 0);
    CHECK(c.width == 160 && c.height == 90);
    CHECK(storyboardCell(b, 2500, &c) && c.x == 160 && c.y == 0);
    CHECK(storyboardCell(b, 12000, &c) && c.x == 160 && c.y == 90);            // cell 6
    CHECK(storyboardCell(b, 49999, &c) && c.x == 640 && c.y == 360);           // cell 24
    CHECK(storyboardCell(b, 51000, &c) && c.url == "https://i.example/sb/M1.jpg" && c.x == 0);
    CHECK(storyboardCell(b, 108500, &c) && c.url == "https://i.example/sb/M2.jpg" && c.x == 640 && c.y == 0);
    CHECK(storyboardCell(b, 999999, &c) && c.url == "https://i.example/sb/M2.jpg" && c.x == 640);  // clamped
    CHECK(storyboardCell(b, -5, &c) && c.x == 0 && c.url == "https://i.example/sb/M0.jpg");
    Storyboard bad = b;
    bad.columns = 0;
    CHECK(!storyboardCell(bad, 0, &c) && !storyboardCell(Storyboard(), 0, &c));
}

// ---- Stall watchdog ----------------------------------------------------------------------------

// A playing, seekable session whose player answers progress(), at `positionMs`.
struct StallRig : Rig {
    explicit StallRig(const char *id = "SSSSSSSSSSS", std::int64_t positionMs = 60000) {
        port.hasProgress = true;
        playing(id);
        c.onSeekable(true);
        at(positionMs);
        c.takeChanges();
        c.takeStallEvents();
    }
    // The player reports this position (the poll's report and the fresh answer).
    void at(std::int64_t positionMs) {
        port.progressNow.positionMs = positionMs;
        c.onPosition(now, positionMs);
    }
    // Playback advances for `ms` in poll steps.
    void play(std::int64_t ms) {
        for (std::int64_t t = 0; t < ms; t += 250) {
            now += 250;
            at(port.progressNow.positionMs + 250);
            c.tick(now);
        }
    }
    // The position stands still until the watchdog acts: returns the step it ran (0 none within `limit`).
    int stallUntilStep(std::int64_t limit = 30000) {
        const int before = c.stall().lastStep();
        const std::size_t seeks = static_cast<std::size_t>(port.count("seek"));
        const std::size_t loads = port.loads.size();
        const int resolves = port.resolves;
        for (std::int64_t t = 0; t < limit; t += 250) {
            advance(250);
            if (static_cast<std::size_t>(port.count("seek")) != seeks || port.loads.size() != loads ||
                port.resolves != resolves || c.state() == PlaybackController::Failed)
                return c.state() == PlaybackController::Failed ? -1 : c.stall().lastStep();
        }
        return before == c.stall().lastStep() ? 0 : c.stall().lastStep();
    }
    std::vector<PlaybackController::StallEvent> events() { return c.takeStallEvents(); }
};

// PLY-18 stall: the position stands still with no error -> step 1 seeks in place on the same
// streams and asks for PLAYING again; playback advancing half a second ends the episode.
static void ply18_stallSeekInPlaceRecovers() {
    StallRig r;
    CHECK(r.c.stall().armed() && r.c.stall().nextDeadlineMs() == r.now + 8000);
    r.play(3000);
    CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0);
    CHECK(r.c.stall().nextDeadlineMs() == r.now + 8000);  // the one deadline moved with the position
    const std::int64_t stalledAt = r.now;
    r.advance(7999);
    CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0);
    r.port.clear();
    r.advance(1);
    CHECK(r.c.stall().stalls() == 1 && r.c.stall().recovering() && r.c.stall().step() == player::StallStep::Seek);
    CHECK(r.port.count("seek") == 1 && r.port.last("seek")->value == 63000);
    CHECK(r.port.index("play") > r.port.index("seek"));
    CHECK(r.port.count("stopPlayer") == 0 && r.port.loads.empty() && r.port.resolves == 0);  // the same streams
    CHECK(r.c.state() == PlaybackController::Ready && !r.c.keepsPicture());
    CHECK(r.c.takeChanges() & PlaybackController::StallChange);
    std::vector<PlaybackController::StallEvent> e = r.events();
    CHECK(e.size() == 2 && e[0].kind == PlaybackController::StallEvent::Detected && e[0].reason == "frozen");
    CHECK(e[0].ms == 8000 && e[0].positionMs == 63000 && r.now == stalledAt + 8000);
    CHECK(e[1].kind == PlaybackController::StallEvent::Step && e[1].step == 1);
    CHECK(r.c.nextDeadlineMs() == 0);  // looks again right after the step
    r.advance(0);
    r.advance(700);
    r.at(63400);  // not yet half a second
    CHECK(r.c.stall().recovering());
    r.advance(100);
    r.at(63700);
    CHECK(!r.c.stall().recovering() && r.c.stall().recoveries() == 1 && r.c.stall().lastStep() == 1);
    e = r.events();
    CHECK(e.size() == 1 && e[0].kind == PlaybackController::StallEvent::Recovered && e[0].step == 1 && e[0].ms == 800);
    CHECK(r.c.state() == PlaybackController::Ready && r.port.count("seek") == 1 && r.port.loads.empty());
    CHECK(r.c.recovery().triesUsed() == 0);  // not a PLY-18 error recovery
}

// PLY-18 stall: seek -> the same candidate through new stream jobs -> one fresh resolve -> a
// retryable failure; each step once, every reload at the position with the picture kept.
static void ply18_stallStepsThenRetryableFailure() {
    StallRig r;
    const std::string video = r.port.loads[0].video.uri, audio = r.port.loads[0].audio.uri;
    CHECK(!r.port.loads[0].freshStreams);
    r.port.clear();
    CHECK(r.stallUntilStep() == 1);
    r.advance(0);
    r.advance(4999);
    CHECK(r.port.loads.empty());
    r.advance(1);  // 5 s after the seek: nothing moved
    CHECK(r.c.stall().step() == player::StallStep::Reopen && r.port.loads.size() == 1 && r.port.resolves == 0);
    const PlaybackLoad &reopened = r.port.loads[0];
    CHECK(reopened.freshStreams && reopened.startPositionMs == 60000);
    CHECK(reopened.video.uri == video && reopened.audio.uri == audio);  // the same candidate
    CHECK(r.port.index("stopPlayer") >= 0 && r.port.index("stopPlayer") < r.port.index("load"));
    CHECK(r.c.state() == PlaybackController::Preparing && r.c.status() == PlaybackController::Reconnecting);
    CHECK(r.c.keepsPicture() && r.c.attempt() == 0);
    r.advance(0);
    r.advance(11999);
    CHECK(r.port.resolves == 0);
    r.advance(1);  // the reload did not preroll within 12 s
    CHECK(r.c.stall().step() == player::StallStep::Reresolve);
    CHECK(r.port.resolves == 1 && r.port.resolveFull);  // fresh: the cached answer is dropped first
    CHECK(r.c.state() == PlaybackController::Loading && r.c.status() == PlaybackController::Reconnecting);
    r.advance(30000);  // the resolver has its own limits: the watchdog does not time it
    CHECK(r.c.state() == PlaybackController::Loading && r.c.stall().recovering());
    r.answerOk();
    CHECK(r.port.loads.size() == 2 && r.port.loads[1].freshStreams && r.port.loads[1].startPositionMs == 60000);
    CHECK(r.c.status() == PlaybackController::Reconnecting && r.c.keepsPicture());
    r.c.onPlayerStarted(r.now);
    CHECK(!r.c.keepsPicture() && r.c.state() == PlaybackController::Ready);
    r.c.takeStallEvents();
    r.advance(4999);
    CHECK(r.c.state() == PlaybackController::Ready);
    r.advance(1);
    CHECK(r.c.state() == PlaybackController::Failed && r.c.failure() == PlaybackController::PlaybackFailed);
    CHECK(r.c.stalledFailure() && r.c.retryAvailable() && !r.c.stall().recovering());
    CHECK(errorSettingKey(r.c.errorSettingFacts(false)).empty());
    const std::vector<PlaybackController::StallEvent> e = r.events();
    CHECK(e.size() == 1 && e[0].kind == PlaybackController::StallEvent::Failed && e[0].reason == "exhausted" && e[0].step == 3);
    CHECK(r.c.stall().stalls() == 1 && r.c.stall().recoveries() == 0 && r.c.stall().lastStep() == 3);
    // Retry: a fresh resolve, at the position, with a full stall budget.
    const int before = r.port.resolves;
    r.c.retry(r.now);
    CHECK(r.port.resolves == before + 1 && r.port.resolveFull && !r.c.stalledFailure());
    CHECK(r.c.stall().episodesInBudget() == 0);
    r.answerOk();
    CHECK(r.port.loads.back().startPositionMs == 60000 && !r.port.loads.back().freshStreams);
}

// Not a stall: a user pause, an audio loss, an audio resource not granted yet, a seek in flight,
// the end, a switch on its way, buffering that makes progress, the PLY-09 background freeze.
static void ply18_stallGuards() {
    {
        StallRig r;  // paused
        r.c.pause();
        r.advance(60000);
        CHECK(r.c.stall().stalls() == 0 && r.c.stall().nextDeadlineMs() == -1 && r.port.count("seek") == 0);
        r.c.play(r.now);  // playing again: a fresh window, never an instant stall
        r.advance(0);
        CHECK(r.c.stall().stalls() == 0 && r.c.stall().nextDeadlineMs() == r.now + 8000);
    }
    {
        StallRig r;  // PLY-23: lost to a call
        r.c.onAudioLost();
        r.advance(60000);
        CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0);
    }
    {
        Rig r;  // PLY-23: prerolled, the resource not granted yet
        r.port.hasProgress = true;
        r.c.open(r.now, "GGGGGGGGGGG");
        r.answerOk();
        r.c.onPlayerStarted(r.now);
        r.advance(60000);
        CHECK(r.c.stall().stalls() == 0 && r.c.stall().nextDeadlineMs() == -1);
    }
    {
        StallRig r;  // a seek in flight: the reported position is its target
        r.port.progressNow.seeking = true;
        r.advance(7000);
        CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0 && !r.c.stall().armed());
        r.port.progressNow.seeking = false;  // landed on a dead stream: found from there
        r.advance(7999);
        CHECK(r.c.stall().stalls() == 0);
        CHECK(r.stallUntilStep() == 1);
    }
    {
        StallRig r;  // end of stream
        r.port.progressNow.ended = true;
        r.advance(60000);
        CHECK(r.c.stall().stalls() == 0 && r.c.stall().nextDeadlineMs() == -1);
    }
    {
        StallRig r;  // SEL-07: a quality switch on its way (not prerolled)
        QualityChoice choice;
        choice.height = 360;
        r.c.setQuality(r.now, choice);
        CHECK(r.c.switching());
        r.advance(20000);  // below the 25 s start watchdog
        CHECK(r.c.stall().stalls() == 0 && r.c.stall().nextDeadlineMs() == -1);
    }
    {
        StallRig r;  // PLY-08: buffering, the level rises (a slow link)
        r.port.progressNow.buffering = true;
        r.port.progressNow.bufferingPercent = 0;
        r.c.onPlayerPlaying(false);
        for (int i = 1; i <= 20; ++i) {
            r.port.progressNow.bufferingPercent = i;
            r.advance(3000);
        }
        CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0);
        r.advance(3999);  // no more progress: starved, 4 s after the last
        CHECK(r.c.stall().stalls() == 0);
        r.advance(1001);
        CHECK(r.c.stall().stalls() == 1 && r.events().front().reason == "starved");
    }
    {
        StallRig r;  // PLY-09: the picture frozen in the background, the audio position advancing
        r.c.setAppActive(r.now, false);
        r.port.progressNow.videoFrozen = true;
        r.play(60000);
        CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0);
    }
}

// PLY-18: nothing fails in the background. With the picture frozen and the audio position
// standing still the steps run there too (listening with the screen off); when they are used up
// the fault is deferred, and the app shown again recovers at the position with a full budget.
static void ply18_stallFailureIsDeferredInBackground() {
    StallRig r;
    r.c.setAppActive(r.now, false);
    r.port.progressNow.videoFrozen = true;
    r.play(2000);  // the audio goes on: the frozen plan has a clock to judge by
    CHECK(r.c.stall().stalls() == 0);
    CHECK(r.stallUntilStep() == 1);
    r.advance(0);
    CHECK(r.stallUntilStep() == 2);
    r.advance(0);
    CHECK(r.stallUntilStep() == 3);
    r.answerOk();
    r.c.onPlayerStarted(r.now);
    r.c.takeStallEvents();
    r.advance(5000);  // the last step did not help either
    CHECK(r.c.state() == PlaybackController::Preparing && r.c.status() == PlaybackController::WaitingForForeground);
    CHECK(!r.c.stalledFailure() && !r.c.retryAvailable());
    CHECK(r.c.recovery().state() == player::RecoveryState::Deferred && r.c.recovery().triesUsed() == 0);
    const std::vector<PlaybackController::StallEvent> e = r.events();
    CHECK(e.size() == 1 && e[0].kind == PlaybackController::StallEvent::Failed && e[0].reason == "exhausted");
    CHECK(!r.c.stall().recovering() && r.c.stall().episodesInBudget() == 0);
    const int before = r.port.resolves;
    r.c.setAppActive(r.now, true);
    CHECK(r.port.resolves == before + 1 && r.c.status() == PlaybackController::Reconnecting);
    r.answerOk();
    CHECK(r.port.loads.back().startPositionMs == 62000);
}

// A port without a player (the default answer): the watchdog stays off, no deadline of its own.
static void ply18_stallWatchdogNeedsAPlayer() {
    Rig r;
    r.playing("NNNNNNNNNNN");
    r.c.onSeekable(true);
    r.c.onPosition(r.now, 60000);
    CHECK(r.port.progressAsked > 0 && !r.c.stall().armed() && r.c.stall().nextDeadlineMs() == -1);
    r.advance(PlaybackController::kPrefetchDelayMs);
    CHECK(r.c.nextDeadlineMs() < 0);
    r.advance(120000);
    CHECK(r.c.stall().stalls() == 0 && r.port.count("seek") == 0 && r.c.state() == PlaybackController::Ready);
}

// Budget: three episodes in ten minutes are recovered, the fourth fails at once; a user seek
// clears it.
static void ply18_stallBudget() {
    StallRig r;
    for (int i = 0; i < 3; ++i) {
        CHECK(r.stallUntilStep() == 1);
        r.advance(0);
        r.play(1000);
        CHECK(!r.c.stall().recovering() && r.c.stall().recoveries() == i + 1);
    }
    CHECK(r.c.stall().episodesInBudget() == 3 && r.c.state() == PlaybackController::Ready);
    r.c.takeStallEvents();
    const int seeks = r.port.count("seek");
    CHECK(r.stallUntilStep() == -1);
    CHECK(r.c.state() == PlaybackController::Failed && r.c.stalledFailure() && r.c.retryAvailable());
    CHECK(r.port.count("seek") == seeks);  // no step for the fourth
    const std::vector<PlaybackController::StallEvent> e = r.events();
    CHECK(e.size() == 2 && e[0].kind == PlaybackController::StallEvent::Detected);
    CHECK(e[1].kind == PlaybackController::StallEvent::Failed && e[1].reason == "budget");
    StallRig u;
    for (int i = 0; i < 3; ++i) {
        CHECK(u.stallUntilStep() == 1);
        u.advance(0);
        u.play(1000);
    }
    u.c.seek(120000);  // the user
    CHECK(u.c.stall().episodesInBudget() == 0);
    u.port.progressNow.positionMs = 120000;
    CHECK(u.stallUntilStep() == 1 && u.c.state() == PlaybackController::Ready);
}

// The reopened candidate fails before its preroll (its new stream job cannot open): the fresh
// resolve is next, at once, not the stale set's other candidates; no PLY-18 budget is spent.
static void ply18_stallReopenFailureGoesToFreshResolve() {
    StallRig r;
    CHECK(r.c.chain().size() >= 2);
    r.port.clear();
    CHECK(r.stallUntilStep() == 1);
    r.advance(0);
    CHECK(r.stallUntilStep() == 2 && r.port.loads.size() == 1);
    r.advance(0);
    r.c.onPlayerFailed(r.now, "stream open failed: network", true);
    CHECK(r.port.loads.size() == 1 && r.c.attempt() == 0);  // not the next candidate
    CHECK(r.port.resolves == 1 && r.port.resolveFull && r.c.stall().step() == player::StallStep::Reresolve);
    CHECK(r.port.lastRequest.fault == player::SourceFault::Stalled);  // RES-07: a stall is not a strike
    CHECK(r.c.recovery().triesUsed() == 0 && r.c.status() == PlaybackController::Reconnecting);
    r.answerOk();
    r.c.onPlayerStarted(r.now);
    r.advance(0);
    r.play(1000);
    CHECK(!r.c.stall().recovering() && r.c.stall().recoveries() == 1 && r.c.stall().lastStep() == 3);
    CHECK(r.c.state() == PlaybackController::Ready && r.c.fullProfile());
    // A failed fresh resolve is a PLY-18 fault (backoff, then another try), not an instant failure.
    StallRig f;
    CHECK(f.stallUntilStep() == 1);
    f.advance(0);
    CHECK(f.stallUntilStep() == 2);
    f.advance(0);
    f.c.onPlayerFailed(f.now, "stream open failed: network", true);
    ResolveAnswer down;
    down.error = ResolveAnswer::Retryable;
    const std::size_t loads = f.port.loads.size();
    f.answer(down);
    CHECK(f.c.state() != PlaybackController::Failed && f.port.loads.size() >= loads);
}

// PLY-26: a downloaded file has no resolve to repeat: seek, reload the file, then the failure.
static void ply18_stallLocalFileSkipsTheResolve() {
    Rig r;
    r.port.hasProgress = true;
    LocalMedia m;
    m.videoId = "LLLLLLLLLLL";
    m.filePath = m.videoPath = "/home/u/Downloads/l.mp4";
    m.durationMs = 300000;
    r.c.openLocal(r.now, m);
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    r.c.onSeekable(true);
    r.port.progressNow.positionMs = 30000;
    r.c.onPosition(r.now, 30000);
    r.advance(8000);
    CHECK(r.c.stall().step() == player::StallStep::Seek && r.port.last("seek")->value == 30000);
    r.advance(0);
    r.advance(5000);
    CHECK(r.c.stall().step() == player::StallStep::Reopen && r.port.loads.size() == 2);
    CHECK(r.port.loads[1].startPositionMs == 30000 && r.port.loads[1].mode == player::PlayMode::Local);
    r.advance(0);
    r.advance(12000);
    CHECK(r.c.state() == PlaybackController::Failed && r.c.stalledFailure() && r.port.resolves == 0);
}

// Live HLS: no seek in place (not seekable); waiting at the edge for the next segment is not a
// stall; a standing position without buffering reloads the manifest.
static void ply18_stallLiveSkipsTheSeek() {
    Rig r;
    r.port.hasProgress = true;
    r.c.open(r.now, "VVVVVVVVVVV");
    ResolveAnswer live = ok("VVVVVVVVVVV");
    live.details.summary.liveStatus = LiveStatus::IsLive;
    live.details.manifestUrl = "https://manifest.googlevideo.com/live.m3u8";
    r.answer(live);
    r.c.onAudioGranted();
    r.c.onPlayerStarted(r.now);
    r.c.onPlayerPlaying(true);
    CHECK(r.c.live() && r.port.loads.size() == 1 && r.port.loads[0].liveHls);
    r.port.progressNow.positionMs = 5000;
    r.c.onPosition(r.now, 5000);
    r.port.progressNow.buffering = true;  // at the edge, waiting
    r.port.progressNow.bufferingPercent = 0;
    r.c.onPlayerPlaying(false);
    r.advance(60000);
    CHECK(r.c.stall().stalls() == 0 && r.port.loads.size() == 1);
    r.port.progressNow.buffering = false;  // "playing", yet nothing moves
    r.c.onPlayerPlaying(true);
    r.advance(0);
    r.advance(8000);
    CHECK(r.c.stall().stalls() == 1 && r.c.stall().step() == player::StallStep::Reopen);
    CHECK(r.port.count("seek") == 0 && r.port.loads.size() == 2 && r.port.loads[1].liveHls);
    CHECK(r.port.loads[1].startPositionMs == 0);  // back at the live edge
}

// A jump the session makes itself is not progress and not a user seek: a SponsorBlock skip
// during an episode does not end it, a loop restart keeps the budget.
static void ply18_stallInternalJumpsAreNotProgress() {
    StallRig r("JJJJJJJJJJJ", 69800);
    std::vector<player::SponsorSegment> segments;
    segments.push_back(player::SponsorSegment(70000, 90000, player::SponsorCategory::Sponsor));
    r.c.setSponsorSegments(segments);
    CHECK(r.stallUntilStep() == 1);
    r.advance(0);
    r.now += 100;
    r.at(70100);  // 0.3 s on, into the segment: the skipper posts its seek
    CHECK(r.port.count("postSeek") == 1 && r.port.last("postSeek")->value == 90000 && r.c.stall().recovering());
    r.now += 100;
    r.at(90000);  // the skip landed: 20 s further, yet nothing played
    r.advance(0);
    CHECK(r.c.stall().recovering() && r.c.stall().recoveries() == 0);
    r.play(1000);
    CHECK(!r.c.stall().recovering() && r.c.stall().recoveries() == 1);
    CHECK(r.c.stall().episodesInBudget() == 1);
    // Shorts: the loop restart at the end is the session's own seek; the budget stays.
    StallRig l("KKKKKKKKKKK", 20000);
    l.c.setLoop(true);
    CHECK(l.stallUntilStep() == 1);
    l.advance(0);
    l.play(1000);
    CHECK(l.c.stall().episodesInBudget() == 1);
    l.c.onEnded(l.now);
    CHECK(l.port.last("seek")->value == 0 && l.c.stall().episodesInBudget() == 1);
    l.c.seek(5000);  // the user's seek clears it
    CHECK(l.c.stall().episodesInBudget() == 0);
}

int main() {
    ply01_openDualPlanFirstFrame();
    ply06_deepResumePrefersWebm();
    ply18_candidateFailureNextCandidate();
    ply18_recoveryThreeFailuresThenSuccess();
    ply10_arbiterReattachAndReclaim();
    ply23_audioResourceLossPauses();
    ply25_autoplayCountdown();
    ply26_resumePosition();
    sel08_listenOnly();
    ply21_interimHardwareFallback();
    sel02_softwareCeilingWithSixtyFpsOffer();
    ply07_rememberedSpeed();
    sb02_deferredSkip();
    mpr01_stopAndCover();
    resolveErrors();
    res09_rateLimitedRetryWaits();
    res10_requestFields();
    sel07_switchFromSameSet();
    res11_prefetchAfterFirstPreroll();
    metadataAfterStart();
    e12_speculationBounds();
    res11_rowsVisibleDecision();
    res19_upcomingKnownStartWaits();
    res19_upcomingUnknownStartRetryOnly();
    res19_scheduledStartFollowsWallClock();
    perf02_resolveBeforeOldPlayerStops();
    ply25_queueLengthAndIndex();
    ply26_localStartAndNoPrefetch();
    perf02_lowRungStartThenSwitchUp();
    ply18_errorSettingKeyMapping();
    cover_dualVideoDecisionTable();
    cover_notBeforeFirstFrame();
    cover_noVideoPlanNoCover();
    cover_errorTurnsItOff();
    cover_decodeMeter();
    cover_plannedVideoAspect();
    ply26_streamInsteadSwitchesAtCapturedPosition();
    ply26_streamInsteadFailureKeepsFile();
    ply26_streamInsteadOnlyForLocal();
    ply26_streamInsteadKeepsPauseRateListenOnly();
    shorts_loopRestartsAtZero();
    shorts_loopHintPerItem();
    shorts_likelyNextDecision();
    shorts_firstLoadMatchesOpen();
    sel05_audioTrackSwitchWithoutResolve();
    sel04_stableVolumeSwitch();
    set01_backgroundPlaybackSwitch();
    shorts_holdsAndRules();
    ovl_writableLoop();
    ovl_holdSpeedRestores();
    ovl_sleepTimer();
    sb02_sponsorSegmentsExposed();
    ovl_storyboardCell();
    ply18_stallSeekInPlaceRecovers();
    ply18_stallStepsThenRetryableFailure();
    ply18_stallGuards();
    ply18_stallWatchdogNeedsAPlayer();
    ply18_stallFailureIsDeferredInBackground();
    ply18_stallBudget();
    ply18_stallReopenFailureGoesToFreshResolve();
    ply18_stallLocalFileSkipsTheResolve();
    ply18_stallLiveSkipsTheSeek();
    ply18_stallInternalJumpsAreNotProgress();
    std::printf("session controller tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
