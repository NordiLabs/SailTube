// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the pipeline plan, the audio-chain values and the
// decode-mode policy. Build: tests/player_plan/run.sh
#include "Core/Player/Domain/AudioChain.h"
#include "Core/Player/Domain/HardwareProbe.h"
#include "Core/Player/Domain/PipelinePlan.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::player;
namespace en = nordtile::player::elementnames;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool near(double a, double b, double eps = 1e-6) { return std::fabs(a - b) <= eps; }

// What a Sailfish 5 device registry offers (droid elements added separately).
static std::set<std::string> deviceElements(bool droid) {
    std::set<std::string> s;
    const char *const names[] = {"uridecodebin", "souphttpsrc", "filesrc", "qtdemux", "matroskademux", "hlsdemux",
                                 "videoconvert", "appsink", "fakesink", "audioconvert", "audioresample", "scaletempo",
                                 "equalizer-10bands", "volume", "rglimiter", "audiodynamic", "pulsesink"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i) s.insert(names[i]);
    if (droid) { s.insert("droidvdec"); s.insert("droideglsink"); }
    return s;
}

static SourceSpec src(const std::string &uri, Container c) {
    SourceSpec s;
    s.uri = uri;
    s.container = c;
    s.userAgent = "Mozilla/5.0 (test)";
    return s;
}

static PlanInput input(PlayMode mode, Container video = Container::IsoBmff, Container audio = Container::IsoBmff) {
    PlanInput in;
    in.mode = mode;
    in.available = deviceElements(false);
    in.video = src("http://127.0.0.1:8080/v", video);
    in.audio = src("http://127.0.0.1:8080/a", audio);
    switch (mode) {
    case PlayMode::Muxed: in.muxed = src("http://127.0.0.1:8080/m", Container::IsoBmff); break;
    case PlayMode::Hls: in.muxed = src("https://manifest.example/live.m3u8", Container::Unknown); break;
    case PlayMode::Local: in.muxed = src("file:///home/u/Videos/x.mp4", Container::IsoBmff); break;
    default: break;
    }
    return in;
}

static const PlayMode kModes[] = {PlayMode::Dual, PlayMode::Muxed, PlayMode::Hls, PlayMode::Local, PlayMode::AudioOnly};

static std::vector<std::string> audioChainNames(const PipelinePlan &p) {
    std::vector<std::string> out;
    const PadRoute *r = p.route(RouteKind::Audio);
    if (r) for (size_t i = 0; i < r->chain.size(); ++i) out.push_back(r->chain[i].name);
    return out;
}

static int indexOf(const std::vector<std::string> &v, const std::string &s) {
    for (size_t i = 0; i < v.size(); ++i) if (v[i] == s) return static_cast<int>(i);
    return -1;
}

// ---- PLY-01 ----------------------------------------------------------------

static void ply01_onePlanShapeForEveryMode() {
    for (size_t m = 0; m < 5; ++m) {
        const PipelinePlan p = buildPipelinePlan(input(kModes[m]));
        CHECK(p.ok);
        CHECK(p.mode == kModes[m]);
        for (size_t b = 0; b < p.branches.size(); ++b) CHECK(p.branches[b].source.factory == "uridecodebin");
        CHECK(p.route(RouteKind::Audio) != nullptr);
        CHECK(p.hasVideo() == (kModes[m] != PlayMode::AudioOnly));
        CHECK(p.unmatchedPadSink.factory == "fakesink");
    }
    const PipelinePlan dual = buildPipelinePlan(input(PlayMode::Dual));
    CHECK(dual.branches.size() == 2);
    CHECK(dual.branches[0].branch == Branch::Video && dual.branches[1].branch == Branch::Audio);
    CHECK(dual.branches[0].source.property("uri") == "http://127.0.0.1:8080/v");
    CHECK(dual.branches[1].source.property("uri") == "http://127.0.0.1:8080/a");
    CHECK(dual.branches[0].routes.size() == 1 && dual.branches[1].routes.size() == 1);

    const PipelinePlan muxed = buildPipelinePlan(input(PlayMode::Muxed));
    CHECK(muxed.branches.size() == 1 && muxed.branches[0].branch == Branch::Muxed);
    CHECK(muxed.branches[0].routes.size() == 2);

    const PipelinePlan local = buildPipelinePlan(input(PlayMode::Local));
    CHECK(!local.branches[0].network);
    CHECK(local.branches[0].sourceSetup.empty());
    PlanInput bad = input(PlayMode::Local);
    bad.muxed.uri = "http://x/y.mp4";
    CHECK(!buildPipelinePlan(bad).ok);

    // HLS: speed locked at the live edge, scaletempo still in the chain.
    const PipelinePlan hls = buildPipelinePlan(input(PlayMode::Hls));
    CHECK(hls.rateLockedAtLiveEdge);
    CHECK(!dual.rateLockedAtLiveEdge);
    CHECK(hls.find(en::kAudioTempo) != nullptr);
    CHECK(hls.branches[0].source.property("uri") == "https://manifest.example/live.m3u8");
    CHECK(hls.branches[0].source.property("use-buffering") == "true");
    PlanInput noDemux = input(PlayMode::Hls);
    noDemux.available.erase("hlsdemux");
    CHECK(!buildPipelinePlan(noDemux).ok);
    noDemux.available.insert("hlsdemux2");
    CHECK(buildPipelinePlan(noDemux).ok);

    // Missing sources are errors, not crashes.
    PlanInput noAudio = input(PlayMode::Dual);
    noAudio.audio.uri.clear();
    CHECK(!buildPipelinePlan(noAudio).ok);
    PlanInput noUridecodebin = input(PlayMode::Dual);
    noUridecodebin.available.erase("uridecodebin");
    const PipelinePlan np = buildPipelinePlan(noUridecodebin);
    CHECK(!np.ok && indexOf(np.missingRequired, "uridecodebin") >= 0);
}

// SEL-08: listen-only builds no video branch and reads the audio format only.
static void sel08_audioOnlyBuildsNoVideoBranch() {
    const PipelinePlan p = buildPipelinePlan(input(PlayMode::AudioOnly));
    CHECK(p.ok);
    CHECK(p.branches.size() == 1);
    CHECK(p.branches[0].branch == Branch::Audio);
    CHECK(p.branches[0].source.property("uri") == "http://127.0.0.1:8080/a");
    CHECK(!p.hasVideo());
    CHECK(p.find(en::kVideoSink) == nullptr);
    CHECK(p.videoSinkFactory.empty());
    // Any video in the file stops compressed (never decoded).
    CHECK(p.branches[0].source.property("caps").find("video/x-h264") != std::string::npos);
    // Hardware request is irrelevant without video.
    PlanInput hw = input(PlayMode::AudioOnly);
    hw.available = deviceElements(true);
    hw.decode = DecodeMode::Hardware;
    CHECK(!containsDroidElement(buildPipelinePlan(hw)));
}

// ---- PLY-02 ----------------------------------------------------------------

static void ply02_syncModel() {
    for (size_t m = 0; m < 5; ++m) {
        const PipelinePlan p = buildPipelinePlan(input(kModes[m]));
        CHECK(p.clockProvider == en::kAudioSink);
        const ElementPlan *sink = p.find(en::kAudioSink);
        CHECK(sink && sink->factory == "pulsesink");
    }
    const PipelinePlan p = buildPipelinePlan(input(PlayMode::Dual));
    const PadRoute *v = p.route(RouteKind::Video);
    CHECK(v && v->capsPrefixes.size() == 1 && v->capsPrefixes[0] == "video/x-raw");
    CHECK(v && v->chain.size() == 2 && v->chain[0].factory == "videoconvert" && v->chain[1].factory == "appsink");
    const ElementPlan *sink = p.find(en::kVideoSink);
    CHECK(sink != nullptr);
    if (!sink) return;
    CHECK(sink->property("sync") == "true");
    CHECK(sink->property("qos") == "true");
    CHECK(sink->property("max-buffers") == "2");
    CHECK(sink->property("drop") == "true");
    CHECK(sink->property("caps") == "video/x-raw,format=RGBA");
    CHECK(sink->property("max-lateness") == "20000000");
    PlanInput noConvert = input(PlayMode::Dual);
    noConvert.available.erase("videoconvert");
    CHECK(!buildPipelinePlan(noConvert).ok);
}

// Host tests replace the sinks (no PulseAudio, no display).
static void sinkOverrides() {
    PlanInput in = input(PlayMode::Dual);
    in.audioSink.factory = "fakesink";
    PropertySetting sync; sync.name = "sync"; sync.value = "true";
    in.audioSink.properties.push_back(sync);
    in.videoSink = in.audioSink;
    const PipelinePlan p = buildPipelinePlan(in);
    CHECK(p.ok);
    CHECK(p.find(en::kAudioSink)->factory == "fakesink");
    CHECK(p.find(en::kAudioSink)->property("sync") == "true");
    CHECK(p.find(en::kVideoSink)->factory == "fakesink");
    CHECK(p.find(en::kVideoConvert) != nullptr);
    CHECK(p.mediaRole.empty());
    in.available.erase("fakesink");
    CHECK(!buildPipelinePlan(in).ok);
    // A video override disables the hardware path for that build.
    PlanInput hw = input(PlayMode::Dual);
    hw.available = deviceElements(true);
    hw.decode = DecodeMode::Hardware;
    hw.videoSink.factory = "fakesink";
    const PipelinePlan hp = buildPipelinePlan(hw);
    CHECK(hp.decode == DecodeMode::Software && !containsDroidElement(hp) && !hp.hardwareFallbackReason.empty());
}

// ---- PLY-04 ----------------------------------------------------------------

static void ply04_bufferParamsAndHintOrder() {
    const PipelinePlan mp4 = buildPipelinePlan(input(PlayMode::Dual, Container::IsoBmff, Container::IsoBmff));
    for (size_t b = 0; b < 2; ++b) {
        const ElementPlan &s = mp4.branches[b].source;
        CHECK(s.property("download") == "true");
        CHECK(s.property("buffer-duration") == "1000000000");
        CHECK(s.property("buffer-size") == "1572864");
        // Container hints before the uri, which is always last.
        CHECK(s.propertyIndex("download") < s.propertyIndex("uri"));
        CHECK(s.propertyIndex("buffer-duration") < s.propertyIndex("uri"));
        CHECK(s.propertyIndex("buffer-size") < s.propertyIndex("uri"));
        CHECK(s.propertyIndex("uri") == static_cast<int>(s.properties.size()) - 1);
    }
    const PipelinePlan webm = buildPipelinePlan(input(PlayMode::Dual, Container::Matroska, Container::IsoBmff));
    CHECK(webm.branches[0].source.property("download") == "false");
    CHECK(webm.branches[0].source.property("buffer-duration") == "3000000000");
    CHECK(webm.branches[1].source.property("download") == "true");  // m4a audio branch
    CHECK(webm.branches[0].container == Container::Matroska);
    CHECK(webm.branches[1].container == Container::IsoBmff);

    // Muxed (even an mp4 one), HLS and local: hints cleared, no disk buffer.
    const PlayMode cleared[] = {PlayMode::Muxed, PlayMode::Hls, PlayMode::Local};
    for (size_t m = 0; m < 3; ++m) {
        const PipelinePlan cp = buildPipelinePlan(input(cleared[m]));
        const ElementPlan &s = cp.branches[0].source;
        CHECK(s.property("download") == "false");
        CHECK(s.property("buffer-duration") == "-1");
        CHECK(s.property("buffer-size") == "-1");
        CHECK(s.propertyIndex("download") < s.propertyIndex("uri"));
    }
    // Listen-only over the network: the m4a DASH branch still gets the buffer.
    CHECK(buildPipelinePlan(input(PlayMode::AudioOnly)).branches[0].source.property("download") == "true");

    // Missing demuxer for a known container is a clear build error.
    PlanInput noQt = input(PlayMode::Dual);
    noQt.available.erase("qtdemux");
    const PipelinePlan np = buildPipelinePlan(noQt);
    CHECK(!np.ok && indexOf(np.missingRequired, "qtdemux") >= 0);

    // souphttpsrc settings in source-setup.
    PlanInput h = input(PlayMode::Dual);
    h.video.extraHeaders["Referer"] = "https://www.youtube.com/";
    h.video.extraHeaders["Bad Header"] = "x";
    h.video.extraHeaders["X-Quote"] = "a\"b\\c";
    const PipelinePlan hp = buildPipelinePlan(h);
    const BranchPlan &vb = hp.branches[0];
    bool ua = false, timeout = false, headers = false;
    for (size_t i = 0; i < vb.sourceSetup.size(); ++i) {
        const PropertySetting &s = vb.sourceSetup[i];
        CHECK(s.optional);
        if (s.name == "user-agent") ua = s.value == "Mozilla/5.0 (test)";
        if (s.name == "timeout") timeout = s.value == "30";
        if (s.name == "extra-headers")
            headers = s.value == "extra-headers, Referer=(string)\"https://www.youtube.com/\", "
                                 "X-Quote=(string)\"a\\\"b\\\\c\"";
    }
    CHECK(ua && timeout && headers);
    CHECK(extraHeadersStructure(std::map<std::string, std::string>()).empty());
}

// ---- PLY-21 / PLT-08 / PLY-20 ------------------------------------------------

static void plt08_noDroidElementWithHardwareOff() {
    // Every mode, every element set (with and without droid, degraded sets),
    // software decode: no droid element anywhere.
    std::vector<std::set<std::string> > sets;
    sets.push_back(deviceElements(false));
    sets.push_back(deviceElements(true));
    std::set<std::string> minimal = deviceElements(true);
    minimal.erase("scaletempo"); minimal.erase("rglimiter"); minimal.erase("audiodynamic");
    sets.push_back(minimal);
    std::set<std::string> onlyDecoder = deviceElements(false);
    onlyDecoder.insert("droidvdec");
    sets.push_back(onlyDecoder);
    for (size_t s = 0; s < sets.size(); ++s)
        for (size_t m = 0; m < 5; ++m) {
            PlanInput in = input(kModes[m]);
            in.available = sets[s];
            in.decode = DecodeMode::Software;
            const PipelinePlan p = buildPipelinePlan(in);
            CHECK(!containsDroidElement(p));
            CHECK(p.decode == DecodeMode::Software);
            // force-sw-decoders keeps autoplugging away from hardware decoders.
            for (size_t b = 0; b < p.branches.size(); ++b)
                CHECK(p.branches[b].source.property("force-sw-decoders") == "true");
        }
    // Demotion list for start-up.
    const std::vector<std::string> d = elementsToDemote();
    CHECK(d.size() == 2 && d[0] == "droidvdec" && d[1] == "droidadec"); // PLT-08: no droid element autoplugged
    // Every factory a plan uses is in the probe list.
    const std::vector<std::string> probe = elementsToProbe();
    PlanInput hw = input(PlayMode::Muxed);
    hw.available = deviceElements(true);
    hw.decode = DecodeMode::Hardware;
    const std::vector<std::string> f = buildPipelinePlan(hw).factories();
    for (size_t i = 0; i < f.size(); ++i) CHECK(indexOf(probe, f[i]) >= 0);
}

static void ply21_softwareIsTheDefault() {
    PlanInput in;
    CHECK(in.decode == DecodeMode::Software);
    DecodeModePolicy policy("dev|5.0|1");
    CHECK(!policy.hardwareRequested());
    policy.setHardwareElementsPresent(true);
    CHECK(policy.modeForVideo("abc") == DecodeMode::Software);
    CHECK(!policy.needsProbe());
    CHECK(!policy.preferVp9());
    PlanInput first = input(PlayMode::Dual);
    first.available = deviceElements(true);
    first.decode = policy.modeForVideo("abc");
    CHECK(!containsDroidElement(buildPipelinePlan(first)));
}

static void ply20_hardwarePath() {
    PlanInput in = input(PlayMode::Dual);
    in.available = deviceElements(true);
    in.decode = DecodeMode::Hardware;
    const PipelinePlan p = buildPipelinePlan(in);
    CHECK(p.ok && p.decode == DecodeMode::Hardware);
    CHECK(p.branches[0].source.property("caps") == "video/x-h264, stream-format=avc, alignment=au; video/x-vp9");
    CHECK(p.branches[0].source.propertyIndex("caps") < p.branches[0].source.propertyIndex("uri"));
    CHECK(p.branches[1].source.property("caps").empty());
    const PadRoute *v = p.route(RouteKind::Video);
    CHECK(v && v->chain.size() == 2 && v->chain[0].factory == "droidvdec" && v->chain[1].factory == "droideglsink");
    CHECK(v && indexOf(v->capsPrefixes, "video/x-h264") >= 0 && indexOf(v->capsPrefixes, "video/x-vp9") >= 0);
    CHECK(p.find(en::kVideoConvert) == nullptr);
    CHECK(p.clockProvider == en::kAudioSink);

    // Muxed: audio/x-raw added to the stop caps.
    PlanInput mux = input(PlayMode::Muxed);
    mux.available = in.available;
    mux.decode = DecodeMode::Hardware;
    CHECK(buildPipelinePlan(mux).branches[0].source.property("caps") ==
          "video/x-h264, stream-format=avc, alignment=au; video/x-vp9; audio/x-raw");

    // Per-build fallback when either element is missing.
    const char *const drop[] = {"droidvdec", "droideglsink"};
    for (size_t i = 0; i < 2; ++i) {
        PlanInput f = in;
        f.available.erase(drop[i]);
        const PipelinePlan fp = buildPipelinePlan(f);
        CHECK(fp.ok);
        CHECK(fp.requestedDecode == DecodeMode::Hardware && fp.decode == DecodeMode::Software);
        CHECK(!fp.hardwareFallbackReason.empty());
        CHECK(!containsDroidElement(fp));
        CHECK(fp.find(en::kVideoSink)->factory == "appsink");
        CHECK(fp.branches[0].source.property("caps").empty());
    }
}

static void ply20_decodeModePolicy() {
    DecodeModePolicy p("poco|5.0.0.67|0.20|0.1");
    p.setHardwareElementsPresent(true);
    p.setHardwareRequested(true);
    CHECK(p.needsProbe());
    CHECK(p.modeForVideo("v1") == DecodeMode::Software);  // not verified yet

    // Probe: frame within 2 s passes.
    p.onProbeStarted(1000);
    CHECK(p.probeState() == ProbeState::Running);
    CHECK(p.modeForVideo("v1") == DecodeMode::Software);
    CHECK(p.tick(2500) == DecodeEvent::None);
    CHECK(p.onProbeFrame(2900) == DecodeEvent::ProbePassed);
    CHECK(p.probeState() == ProbeState::Passed);
    CHECK(!p.needsProbe());
    CHECK(p.modeForVideo("v1") == DecodeMode::Hardware);
    CHECK(p.preferVp9());

    // Remembered per build key.
    const HardwareProbeRecord rec = p.record();
    CHECK(rec.state == ProbeState::Passed && rec.buildKey == "poco|5.0.0.67|0.20|0.1");
    DecodeModePolicy same("poco|5.0.0.67|0.20|0.1");
    same.restore(rec);
    same.setHardwareElementsPresent(true);
    same.setHardwareRequested(true);
    CHECK(!same.needsProbe() && same.modeForVideo("x") == DecodeMode::Hardware);
    DecodeModePolicy updated("poco|5.1.0.10|0.20|0.1");
    updated.restore(rec);
    updated.setHardwareElementsPresent(true);
    updated.setHardwareRequested(true);
    CHECK(updated.needsProbe() && updated.modeForVideo("x") == DecodeMode::Software);

    // Watchdog: no frame within 3 s -> software for that video, with an event.
    p.onVideoStarted("v1", DecodeMode::Hardware, 10000);
    CHECK(p.watchdogArmed());
    CHECK(p.tick(12999) == DecodeEvent::None);
    CHECK(p.tick(13000) == DecodeEvent::FallbackToSoftware);
    CHECK(!p.watchdogArmed());
    CHECK(p.tick(20000) == DecodeEvent::None);  // reported once
    CHECK(p.modeForVideo("v1") == DecodeMode::Software);
    CHECK(p.modeForVideo("v2") == DecodeMode::Hardware);  // only that video

    // A frame in time disarms; a software video never arms.
    p.onVideoStarted("v2", DecodeMode::Hardware, 30000);
    p.onVideoFrame(30400);
    CHECK(p.tick(40000) == DecodeEvent::None);
    p.onVideoStarted("v3", DecodeMode::Software, 50000);
    CHECK(!p.watchdogArmed() && p.tick(60000) == DecodeEvent::None);
    // Pause/teardown disarms; resume re-arms with a fresh deadline.
    p.onVideoStarted("v4", DecodeMode::Hardware, 70000);
    p.onVideoStopped();
    CHECK(p.tick(80000) == DecodeEvent::None);

    // Toggling off: software from the next video; the probe is not repeated.
    p.setHardwareRequested(false);
    CHECK(p.modeForVideo("v2") == DecodeMode::Software && !p.preferVp9());
    p.setHardwareRequested(true);
    CHECK(!p.needsProbe());

    // Probe failures: deadline, late frame, error.
    DecodeModePolicy late("k");
    late.setHardwareElementsPresent(true);
    late.setHardwareRequested(true);
    late.onProbeStarted(0);
    CHECK(late.tick(2001) == DecodeEvent::ProbeFailed);
    CHECK(late.probeState() == ProbeState::Failed && late.modeForVideo("a") == DecodeMode::Software);
    CHECK(late.record().state == ProbeState::Failed);
    CHECK(!late.needsProbe());
    late.forgetProbe();
    CHECK(late.needsProbe());
    late.onProbeStarted(0);
    CHECK(late.onProbeFrame(2500) == DecodeEvent::ProbeFailed);
    late.forgetProbe();
    late.onProbeStarted(0);
    CHECK(late.onProbeError() == DecodeEvent::ProbeFailed);

    // Elements missing: never hardware, never a probe.
    DecodeModePolicy none("k");
    none.setHardwareRequested(true);
    CHECK(!none.needsProbe() && none.modeForVideo("a") == DecodeMode::Software);

    // Probe plan: explicit droid chain on the bundled clip.
    const PipelinePlan probe = buildProbePlan(ProbeKind::HardwareDecode, "file:///usr/share/x/probe.mp4", deviceElements(true));
    CHECK(probe.ok && containsDroidElement(probe));
    CHECK(probe.find(en::kVideoSink)->property("sync") == "false");
    CHECK(!buildProbePlan(ProbeKind::HardwareDecode, "file:///p.mp4", deviceElements(false)).ok);
}

// SEL-01 AV1 self-test hook.
static void sel01_av1SelfTest() {
    std::set<std::string> s = deviceElements(false);
    CHECK(av1DecoderFor(s).empty());
    CHECK(!buildProbePlan(ProbeKind::Av1Decode, "file:///av1.mp4", s).ok);
    s.insert("avdec_av1");
    CHECK(av1DecoderFor(s) == "avdec_av1");
    s.insert("dav1ddec");
    CHECK(av1DecoderFor(s) == "dav1ddec");
    const PipelinePlan p = buildProbePlan(ProbeKind::Av1Decode, "file:///av1.mp4", s);
    CHECK(p.ok && !containsDroidElement(p));
    CHECK(p.find(en::kVideoSink)->factory == "fakesink");
    CHECK(p.find(en::kVideoSink)->property("signal-handoffs") == "true");
    CHECK(p.find(en::kVideoSink)->property("sync") == "false");
    CHECK(!buildProbePlan(ProbeKind::Av1Decode, "http://x/av1.mp4", s).ok);

    CodecSelfTestResult r;
    r.expectedFrames = 60; r.framesDecoded = 60; r.elapsedMs = 1500; r.clipDurationMs = 2000;
    CHECK(codecSelfTestPassed(r));
    r.elapsedMs = 2400;
    CHECK(!codecSelfTestPassed(r));  // not real time
    r.elapsedMs = 1500; r.framesDecoded = 59;
    CHECK(!codecSelfTestPassed(r));  // dropped frames
    r.framesDecoded = 60; r.error = true;
    CHECK(!codecSelfTestPassed(r));
    CHECK(!codecSelfTestPassed(CodecSelfTestResult()));
}

// ---- PLY-22 / PLT-12 -----------------------------------------------------------

static void ply22_elementOrder() {
    for (size_t m = 0; m < 5; ++m) {
        const PipelinePlan p = buildPipelinePlan(input(kModes[m]));
        const std::vector<std::string> n = audioChainNames(p);
        const char *const expected[] = {en::kAudioConvertIn, en::kAudioTempo, en::kAudioConvertFloat, en::kAudioEq,
                                        en::kAudioLoudness, en::kAudioBoost, en::kAudioLimiter, en::kAudioConvertOut,
                                        en::kAudioResample, en::kAudioSink};
        CHECK(n.size() == 10);
        for (size_t i = 0; i < 10 && i < n.size(); ++i) CHECK(n[i] == expected[i]);
        const PadRoute *a = p.route(RouteKind::Audio);
        CHECK(a->chain[1].factory == "scaletempo");
        CHECK(a->chain[3].factory == "equalizer-10bands");
        CHECK(a->chain[4].factory == "volume" && a->chain[5].factory == "volume");
        CHECK(a->chain[6].factory == "rglimiter");
        CHECK(a->chain[9].factory == "pulsesink");
        CHECK(a->chain[2].linkCapsToNext == "audio/x-raw,format=F32LE,layout=interleaved");
        CHECK(p.skipped.empty());
    }
}

static void ply22_degradesNeverFails() {
    const char *const optionalFactories[] = {"scaletempo", "equalizer-10bands", "volume", "rglimiter",
                                             "audioconvert", "audioresample"};
    for (size_t m = 0; m < 5; ++m)
        for (size_t i = 0; i < sizeof(optionalFactories) / sizeof(optionalFactories[0]); ++i) {
            PlanInput in = input(kModes[m]);
            in.available.erase(optionalFactories[i]);
            const PipelinePlan p = buildPipelinePlan(in);
            CHECK(p.ok);
            const PadRoute *a = p.route(RouteKind::Audio);
            CHECK(a && a->chain.back().factory == "pulsesink");
            for (size_t e = 0; a && e < a->chain.size(); ++e) {
                const bool fallbackLimiter = std::string(optionalFactories[i]) == "rglimiter" &&
                                             a->chain[e].factory == "audiodynamic";
                CHECK(a->chain[e].factory != optionalFactories[i] || fallbackLimiter);
            }
            CHECK(!p.skipped.empty() || std::string(optionalFactories[i]) == "rglimiter");
        }
    // audioconvert missing: float-only stages go too (nothing could feed them).
    PlanInput noConvert = input(PlayMode::Dual);
    noConvert.available.erase("audioconvert");
    const PipelinePlan nc = buildPipelinePlan(noConvert);
    CHECK(nc.ok && nc.find(en::kAudioEq) == nullptr && nc.limiterFactory == "audiodynamic");
    CHECK(nc.find(en::kAudioTempo) != nullptr && nc.find(en::kAudioBoost) != nullptr);
    // rglimiter missing -> audiodynamic; both missing -> no limiter, still plays.
    PlanInput in = input(PlayMode::Dual);
    in.available.erase("rglimiter");
    PipelinePlan p = buildPipelinePlan(in);
    CHECK(p.limiterFactory == "audiodynamic" && p.find(en::kAudioLimiter)->property("ratio") == "0");
    in.available.erase("audiodynamic");
    p = buildPipelinePlan(in);
    CHECK(p.ok && p.limiterFactory.empty() && p.find(en::kAudioLimiter) == nullptr);
    // Everything optional gone: sink only.
    in.available.erase("scaletempo"); in.available.erase("equalizer-10bands"); in.available.erase("volume");
    in.available.erase("audioconvert"); in.available.erase("audioresample");
    p = buildPipelinePlan(in);
    CHECK(p.ok && audioChainNames(p).size() == 1);
    // No pulsesink: autoaudiosink (dev hosts); no sink at all: error.
    PlanInput dev = input(PlayMode::Dual);
    dev.available.erase("pulsesink");
    dev.available.insert("autoaudiosink");
    CHECK(buildPipelinePlan(dev).ok && buildPipelinePlan(dev).audioSinkFactory == "autoaudiosink");
    dev.available.erase("autoaudiosink");
    CHECK(!buildPipelinePlan(dev).ok);
}

static void plt12_mediaRoleOnEverySink() {
    for (size_t m = 0; m < 5; ++m) {
        const PipelinePlan p = buildPipelinePlan(input(kModes[m]));
        CHECK(p.find(en::kAudioSink)->property("stream-properties") == "props, media.role=(string)\"x-maemo\"");
        CHECK(p.mediaRole == "x-maemo");
        PlanInput custom = input(kModes[m]);
        custom.mediaRole = "music";
        CHECK(buildPipelinePlan(custom).find(en::kAudioSink)->property("stream-properties") ==
              "props, media.role=(string)\"music\"");
    }
}

static void ply22_eqBandsAndClamp() {
    const int hz[] = {29, 59, 119, 237, 474, 947, 1889, 3770, 7523, 15011};
    for (int i = 0; i < kEqBandCount; ++i) CHECK(eqBandFrequencies()[i] == hz[i]);
    CHECK(clampEqGain(15) == 12 && clampEqGain(-15) == -12 && clampEqGain(3.5) == 3.5);
    CHECK(clampEqGain(std::nan("")) == 0);
    AudioSettings s;
    s.eqGainsDb[0] = 15; s.eqGainsDb[9] = -15;
    const AudioChainValues v = computeAudioChainValues(s, StreamLoudness());
    CHECK(v.eqGainsDb[0] == 12 && v.eqGainsDb[9] == -12);
    const std::vector<ElementProperties> props = audioChainProperties(v, "rglimiter");
    CHECK(props[0].element == en::kAudioEq && props[0].properties.size() == 10);
    CHECK(props[0].properties[0].name == "band0" && props[0].properties[0].value == "12");
    CHECK(props[0].properties[9].name == "band9" && props[0].properties[9].value == "-12");
    // Initial values reach the plan.
    PlanInput in = input(PlayMode::Dual);
    in.audioValues = v;
    CHECK(buildPipelinePlan(in).find(en::kAudioEq)->property("band0") == "12");

    // Presets: stable keys, all within range, flat is flat.
    const std::vector<EqPreset> &ps = eqPresets();
    CHECK(ps.size() >= 4);
    const char *const required[] = {"flat", "bass", "treble", "vocal"};
    for (size_t i = 0; i < 4; ++i) { EqGains g; CHECK(eqPresetGains(required[i], g)); }
    for (size_t i = 0; i < ps.size(); ++i)
        for (int b = 0; b < kEqBandCount; ++b) CHECK(ps[i].gainsDb[b] >= kEqMinDb && ps[i].gainsDb[b] <= kEqMaxDb);
    EqGains g;
    CHECK(eqPresetGains("flat", g) && g == flatEq());
    CHECK(!eqPresetGains("nope", g));
}

static void ply22_loudnessBoostLimiter() {
    StreamLoudness l;
    l.known = true; l.loudnessDb = 6;
    // Off by default.
    AudioSettings s;
    CHECK(!s.normalizationEnabled);
    CHECK(computeAudioChainValues(s, l).loudnessGain == 1.0);
    // 10^(-6/20) ~= 0.501, with the floor and the cap in play.
    CHECK(near(loudnessGain(true, l, 100), std::pow(10.0, -6.0 / 20.0)));
    CHECK(near(loudnessGain(true, l, 500), 0.501187234));
    // Floor at -20 dB.
    l.loudnessDb = 35;
    CHECK(near(loudnessGain(true, l, 100), 0.1));
    // Quiet stream: raised only up to the user cap.
    l.loudnessDb = -10;
    CHECK(near(loudnessGain(true, l, 100), 1.0));
    CHECK(near(loudnessGain(true, l, 200), 2.0));
    CHECK(near(loudnessGain(true, l, 500), std::pow(10.0, 0.5)));
    CHECK(near(loudnessGain(true, l, 50), 1.0));  // cap clamped to >= 100 %
    // Unknown / non-finite metadata: unity.
    CHECK(loudnessGain(true, StreamLoudness(), 500) == 1.0);
    l.loudnessDb = std::nan("");
    CHECK(loudnessGain(true, l, 500) == 1.0);

    // Boost 100..500 %, never attenuates.
    CHECK(boostGain(100) == 1.0 && boostGain(500) == 5.0 && boostGain(250) == 2.5);
    CHECK(boostGain(50) == 1.0 && boostGain(900) == 5.0);

    // Limiter engages whenever the chain can exceed unity.
    AudioSettings neutral;
    CHECK(!computeAudioChainValues(neutral, StreamLoudness()).limiterEnabled);
    AudioSettings loud;
    loud.boostPercent = 500;
    AudioChainValues v = computeAudioChainValues(loud, StreamLoudness());
    CHECK(v.boostGain == 5.0 && v.limiterEnabled);
    AudioSettings eqUp;
    eqUp.eqGainsDb[2] = 1;
    CHECK(computeAudioChainValues(eqUp, StreamLoudness()).limiterEnabled);

    // Live property blocks.
    std::vector<ElementProperties> p = audioChainProperties(v, "rglimiter");
    CHECK(p.size() == 4);
    CHECK(p[1].element == en::kAudioLoudness && p[1].properties[0].name == "volume" && p[1].properties[0].value == "1");
    CHECK(p[2].element == en::kAudioBoost && p[2].properties[0].value == "5");
    CHECK(p[3].element == en::kAudioLimiter && p[3].properties[0].name == "enabled" && p[3].properties[0].value == "true");
    p = audioChainProperties(v, "audiodynamic");
    CHECK(p[3].properties.size() == 4 && p[3].properties[3].name == "threshold" && p[3].properties[3].value == "0.977");
    CHECK(audioChainProperties(v, "").size() == 3);
    p = audioChainProperties(computeAudioChainValues(neutral, StreamLoudness()), "rglimiter");
    CHECK(p[3].properties[0].value == "false");

    // Locale-independent formatting.
    CHECK(formatDouble(0.5) == "0.5");
    CHECK(formatDouble(-2.25) == "-2.25");
}

// PLY-20 probe verdict: build key, persisted line, result line (HardwareProbe.h).
static void ply20_probeVerdictPerBuild() {
    CHECK(hardwareProbeBuildKey("Xperia 10 III", "5.0.0.62", "0.20240101", "1.2") == "Xperia 10 III|5.0.0.62|0.20240101|1.2");
    CHECK(hardwareProbeBuildKey("a|b", "", "x\ty", "1\n") == "a b|unknown|x y|1 ");
    const std::string hw = "# comment\nNAME=\"Sony Xperia 10 III\"\nMER_HA_DEVICE=pdx213\r\nVERSION_ID='5.0'\n";
    CHECK(releaseFileValue(hw, "NAME") == "Sony Xperia 10 III");
    CHECK(releaseFileValue(hw, "MER_HA_DEVICE") == "pdx213");
    CHECK(releaseFileValue(hw, "VERSION_ID") == "5.0");
    CHECK(releaseFileValue(hw, "NAM").empty() && releaseFileValue(hw, "ID").empty());

    HardwareProbeVerdict v;
    v.record.buildKey = "dev|5.0|1.0|1.2";
    v.record.state = ProbeState::Passed;
    v.firstFrameMs = 412;
    const std::string line = encodeProbeVerdict(v);
    HardwareProbeVerdict back;
    CHECK(decodeProbeVerdict(line, &back));
    CHECK(back.record.state == ProbeState::Passed && back.record.buildKey == v.record.buildKey && back.firstFrameMs == 412);
    HardwareProbeVerdict f;
    f.record.buildKey = "dev|5.0|1.0|1.2";
    f.record.state = ProbeState::Failed;
    f.detail = "droidvdec: could not\tload\ncodec";
    f.firstFrameMs = 99;  // a failure keeps no frame time
    CHECK(decodeProbeVerdict(encodeProbeVerdict(f), &back));
    CHECK(back.record.state == ProbeState::Failed && back.detail == "droidvdec: could not load codec" && back.firstFrameMs == -1);
    HardwareProbeVerdict none;
    CHECK(encodeProbeVerdict(none).empty());  // NotRun: nothing stored
    back = HardwareProbeVerdict();
    CHECK(!decodeProbeVerdict("", &back) && !decodeProbeVerdict("v2\tpassed\t1\tk\t", &back));
    CHECK(!decodeProbeVerdict("v1\tmaybe\t1\tk\t", &back) && !decodeProbeVerdict("v1\tpassed\tx\tk\t", &back));
    CHECK(!decodeProbeVerdict("v1\tpassed\t1\t\t", &back) && back.record.state == ProbeState::NotRun);

    // A stored verdict of another build is ignored by DecodeModePolicy::restore (re-probe).
    DecodeModePolicy p("dev|5.1|1.0|1.2");
    p.restore(v.record);
    CHECK(p.probeState() == ProbeState::NotRun);
    DecodeModePolicy same("dev|5.0|1.0|1.2");
    same.restore(v.record);
    CHECK(same.probeState() == ProbeState::Passed);

    CHECK(probeUnavailableReason(true, true).empty());
    CHECK(probeUnavailableReason(false, true) == "droidvdec missing");
    CHECK(probeUnavailableReason(true, false) == "droideglsink missing");
    CHECK(probeResultLine(ProbeState::Passed, "", 412, true, "") == "passed (first frame 412 ms)");
    CHECK(probeResultLine(ProbeState::Failed, "no frame within 2 s", -1, true, "") == "failed: no frame within 2 s");
    CHECK(probeResultLine(ProbeState::Running, "", -1, true, "") == "running");
    CHECK(probeResultLine(ProbeState::NotRun, "", -1, false, "") == "not run (hardware decode off)");
    CHECK(probeResultLine(ProbeState::NotRun, "", -1, true, "droidvdec missing") == "unavailable: droidvdec missing");
}

// PLY-04 / NFR-PERF-02 (R2 run 5): exactly the disk-backed (pull-mode) ISO-BMFF branches hide the
// upstream size from their demuxer until its first pad (no fragmented-mp4 end-of-file probe).
static void ply04_hideSizeUntilDemuxReadyOnDiskBackedMp4Only() {
    const PipelinePlan mp4 = buildPipelinePlan(input(PlayMode::Dual, Container::IsoBmff, Container::Matroska));
    CHECK(mp4.branches.size() == 2);
    CHECK(mp4.branches[0].hideSizeUntilDemuxReady);    // mp4 video, download=true
    CHECK(!mp4.branches[1].hideSizeUntilDemuxReady);   // WebM audio, push mode
    const PipelinePlan both = buildPipelinePlan(input(PlayMode::Dual, Container::IsoBmff, Container::IsoBmff));
    for (size_t b = 0; b < both.branches.size(); ++b)
        CHECK(both.branches[b].hideSizeUntilDemuxReady == (both.branches[b].source.property("download") == "true"));
    const PipelinePlan webm = buildPipelinePlan(input(PlayMode::Dual, Container::Matroska, Container::Matroska));
    CHECK(!webm.branches[0].hideSizeUntilDemuxReady && !webm.branches[1].hideSizeUntilDemuxReady);
    CHECK(buildPipelinePlan(input(PlayMode::AudioOnly)).branches[0].hideSizeUntilDemuxReady);  // m4a, disk-backed
    // Muxed (push), HLS and local files (filesrc: the end-of-file read is a cheap file seek): never.
    const PlayMode others[] = {PlayMode::Muxed, PlayMode::Hls, PlayMode::Local};
    for (size_t m = 0; m < 3; ++m) CHECK(!buildPipelinePlan(input(others[m])).branches[0].hideSizeUntilDemuxReady);
}

int main() {
    ply01_onePlanShapeForEveryMode();
    sel08_audioOnlyBuildsNoVideoBranch();
    ply02_syncModel();
    sinkOverrides();
    ply04_bufferParamsAndHintOrder();
    ply04_hideSizeUntilDemuxReadyOnDiskBackedMp4Only();
    plt08_noDroidElementWithHardwareOff();
    ply21_softwareIsTheDefault();
    ply20_hardwarePath();
    ply20_decodeModePolicy();
    ply20_probeVerdictPerBuild();
    sel01_av1SelfTest();
    ply22_elementOrder();
    ply22_degradesNeverFails();
    plt12_mediaRoleOnEverySink();
    ply22_eqBandsAndClamp();
    ply22_loudnessBoostLimiter();
    std::printf("player plan tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
