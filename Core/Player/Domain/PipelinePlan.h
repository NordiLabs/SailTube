// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free, GStreamer-free description of the pipeline SailTube builds
// (PLY-01, PLY-02, PLY-04, PLY-20, PLY-21, PLY-22, PLT-08, PLT-12, SEL-01
// self-test hook), plus the decode-mode state machine (PLY-20/21).
//
// The engine instantiates a PipelinePlan mechanically:
//   0. Once at start-up, after gst_init: for every name in elementsToDemote(),
//      gst_registry_lookup_feature() and gst_plugin_feature_set_rank(NONE)
//      (PLT-08). Probe elementsToProbe() with gst_element_factory_find() and
//      pass the present names as PlanInput::available.
//   1. pipeline = gst_pipeline_new(). For every BranchPlan:
//      a. src = gst_element_factory_make(source.factory, source.name); apply
//         source.properties IN ORDER (see "Applying properties"); add to the
//         pipeline. Order matters: container hints precede "uri" (PLY-04).
//      b. Connect "source-setup": apply sourceSetup to the created source
//         element (souphttpsrc; each setting is optional, so a filesrc simply
//         skips them).
//      c. For every PadRoute: make each chain element, apply its properties,
//         add it, and link chain[i] -> chain[i+1] (gst_element_link_filtered
//         with chain[i].linkCapsToNext when non-empty).
//      d. On "pad-added": take the structure name of the pad's current caps;
//         link the pad to chain.front() of the first route whose capsPrefixes
//         match (prefix compare) and which is not linked yet. Otherwise make
//         a fresh unmatchedPadSink, add it, sync its state and link the pad
//         (an unlinked pad would stop the demuxer with not-linked).
//      e. On "no-more-pads": a route that is still unlinked never gets data;
//         set its elements to NULL and remove them, or preroll never ends.
//   2. Do not set a clock: the audio sink (clockProvider) provides it
//      (PLY-02). appsink and droideglsink provide none.
//
// Applying properties: for each PropertySetting, look the pspec up with
// g_object_class_find_property(). Missing + optional -> skip (log once).
// Missing + required -> build error. G_TYPE_STRING properties: g_object_set()
// with the literal value (the deserializer treats a leading quote and "NULL"
// specially). Everything else: gst_util_set_object_arg() (booleans
// "true"/"false", integers, enums by nick, GstCaps and GstStructure in their
// serialised form). Numbers are formatted locale-independently.

#include "AudioChain.h"
#include "PlayerTypes.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nordtile { namespace player {

// PLY-21: Software is the default everywhere.
enum class DecodeMode { Software, Hardware };

// Element names used in every plan; live updates (AudioChain.h) and the
// engine's lookups use them with gst_bin_get_by_name().
namespace elementnames {
const char *const kVideoSource = "video-src";
const char *const kAudioSource = "audio-src";
const char *const kMuxedSource = "muxed-src";
const char *const kVideoConvert = "video-convert";
const char *const kVideoDecoder = "video-decoder";  // hardware path only
const char *const kVideoSink = "video-sink";
const char *const kAudioConvertIn = "audio-convert-in";
const char *const kAudioTempo = "audio-tempo";
const char *const kAudioConvertFloat = "audio-convert-float";
const char *const kAudioEq = "audio-eq";
const char *const kAudioLoudness = "audio-loudness";
const char *const kAudioBoost = "audio-boost";
const char *const kAudioLimiter = "audio-limiter";
const char *const kAudioConvertOut = "audio-convert-out";
const char *const kAudioResample = "audio-resample";
const char *const kAudioSink = "audio-sink";
}

// PLT-12: default PulseAudio media.role. AudioResourcePolicy owns
// the final value (E8); callers pass theirs in PlanInput::mediaRole.
const char *const kDefaultMediaRole = "x-maemo";

// PLY-02: caps of the software video sink. The renderer maps the appsink
// buffer and uploads it straight into one GL texture (one CPU->GPU copy, no
// QImage deep copy), so it wants packed RGBA; videoconvert does the YUV->RGB
// conversion on the CPU. An I420 path (videoconvert passthrough for the
// usual I420 decoder output, 1.5 instead of 4 bytes per pixel to upload, YUV
// to RGB in a fragment shader) is a later optimisation; it needs a
// three-plane shader in the renderer first.
const char *const kSoftwareVideoCaps = "video/x-raw,format=RGBA";

// PLY-20: decodebin stop caps for the hardware path (plus audio/x-raw in
// muxed layouts so the audio still decodes).
const char *const kHardwareStopCaps = "video/x-h264, stream-format=avc, alignment=au; video/x-vp9";

// PLY-04 buffering hints (uridecodebin units: nanoseconds, bytes).
const int64_t kIsoBmffBufferDurationNs = 1000000000LL;     // 1 s
const int kIsoBmffBufferSizeBytes = 1536 * 1024;           // 1.5 MiB
const int64_t kMatroskaBufferDurationNs = 3000000000LL;    // 3 s
const int kHttpTimeoutSeconds = 30;

// One source as the resolver / proxy hands it over.
struct SourceSpec {
    std::string uri;  // http(s):// (proxy or direct), file:// for local files
    Container container = Container::Unknown;
    std::string userAgent;                            // empty = souphttpsrc default
    std::map<std::string, std::string> extraHeaders;  // e.g. Referer, Origin
    bool present() const { return !uri.empty(); }
};

// A sink replacement, for host tests without PulseAudio or a display (e.g.
// fakesink sync=true). Empty factory = no override.
struct SinkOverride {
    std::string factory;
    std::vector<PropertySetting> properties;
    bool active() const { return !factory.empty(); }
};

struct PlanInput {
    PlayMode mode = PlayMode::Dual;
    // Which sources each mode reads:
    //   Dual:      video + audio
    //   Muxed:     muxed
    //   Hls:       muxed (the original HLS manifest URL, never proxied)
    //   Local:     muxed (file:// required)
    //   AudioOnly: audio (or muxed when audio is empty, e.g. a local file)
    SourceSpec video, audio, muxed;
    // Requested decode mode, normally DecodeModePolicy::modeForVideo().
    DecodeMode decode = DecodeMode::Software;
    // Element factories present in the registry (elementsToProbe() subset).
    std::set<std::string> available;
    // Initial audio values, so the first buffer already has the user's EQ /
    // boost (no audible jump when the live push arrives).
    AudioChainValues audioValues;
    std::string mediaRole = kDefaultMediaRole;  // PLT-12, pulsesink only
    SinkOverride audioSink;  // replaces pulsesink
    SinkOverride videoSink;  // replaces appsink (software path only)
};

// One element: gst_element_factory_make(factory, name) + properties in order.
struct ElementPlan {
    std::string factory;
    std::string name;
    std::vector<PropertySetting> properties;
    // Caps for the link from this element to the next one in its chain
    // (gst_element_link_filtered); empty = plain gst_element_link.
    std::string linkCapsToNext;

    // Value of property `key`, or "" if not set. Index in properties, or -1.
    std::string property(const std::string &key) const;
    int propertyIndex(const std::string &key) const;
};

enum class RouteKind { Video, Audio };

// Where decoded (or, on the hardware path, parsed) pads of a source go.
struct PadRoute {
    RouteKind kind = RouteKind::Audio;
    std::vector<std::string> capsPrefixes;  // e.g. "video/x-raw", "video/x-h264"
    std::vector<ElementPlan> chain;         // chain.front() receives the pad
};

struct BranchPlan {
    Branch branch = Branch::Muxed;
    Container container = Container::Unknown;  // for seek flags (PLY-03)
    bool network = false;                      // feeds BufferingPolicy::reset
    ElementPlan source;                        // uridecodebin
    std::vector<PropertySetting> sourceSetup;  // applied in "source-setup"
    std::vector<PadRoute> routes;
    // PLY-04 / NFR-PERF-02: set on the disk-backed (download=true, pull-mode) ISO-BMFF branch.
    // Until the branch's demuxer added its first pad, the engine leaves the demuxer's BYTES size
    // (duration) queries unanswered. qtdemux then skips its mfro/mfra probe at the end of a
    // fragmented file, which the download buffer serves with two extra range requests (end of
    // file, then back) before the first pad: R2 run 5, video demux-ready 400-820 ms after its
    // first byte. DASH mp4 carries no mfra, so nothing is lost; key-unit seeks still work (pull
    // mode). Later size queries are answered normally.
    bool hideSizeUntilDemuxReady = false;
};

struct PipelinePlan {
    bool ok = false;
    std::vector<std::string> errors;           // why ok is false
    std::vector<std::string> missingRequired;  // factories that made it fail
    std::vector<std::string> skipped;          // optional elements absent (degraded)

    PlayMode mode = PlayMode::Dual;
    DecodeMode requestedDecode = DecodeMode::Software;
    DecodeMode decode = DecodeMode::Software;  // effective
    std::string hardwareFallbackReason;        // non-empty: per-build fallback

    std::vector<BranchPlan> branches;
    ElementPlan unmatchedPadSink;  // template; empty factory if fakesink absent
    std::string clockProvider;     // element name of the clock-providing sink
    std::string audioSinkFactory;
    std::string videoSinkFactory;  // empty when there is no video route
    std::string mediaRole;         // as applied on the sink ("" if not pulsesink)
    std::string limiterFactory;    // "rglimiter", "audiodynamic" or ""
    // HLS: playback rate stays 1.0 at the live edge (PLY-01, SpeedPolicy).
    bool rateLockedAtLiveEdge = false;

    bool hasVideo() const;
    // Every factory name the plan instantiates, sources first, in order.
    std::vector<std::string> factories() const;
    // The element with `name`, or null.
    const ElementPlan *find(const std::string &name) const;
    // The route of `kind` (or null); audio chain = route(RouteKind::Audio)->chain.
    const PadRoute *route(RouteKind kind) const;
};

// PLY-01/02/04/20/21/22, PLT-08/12: the plan for one playback.
PipelinePlan buildPipelinePlan(const PlanInput &input);

// PLT-08: droid decoders (video and audio) whose autoplug rank is set to NONE at start-up.
std::vector<std::string> elementsToDemote();

// Every factory buildPipelinePlan / buildProbePlan may use; probe these.
std::vector<std::string> elementsToProbe();

// PLT-08 accept helper: true if any factory in the plan starts with "droid".
bool containsDroidElement(const PipelinePlan &plan);

// One-line gst-launch-like rendering, for diagnostics and logs.
std::string describePlan(const PipelinePlan &plan);

// Serialised GstStructure for souphttpsrc "extra-headers". Header names that
// GstStructure cannot carry (anything but letters, digits, '-', '_', or not
// starting with a letter) are dropped; values are quoted and escaped.
std::string extraHeadersStructure(const std::map<std::string, std::string> &headers);

// PLT-12: serialised GstStructure for pulsesink "stream-properties".
std::string streamPropertiesStructure(const std::string &mediaRole);

// ---- Self-tests: PLY-20 hardware probe, SEL-01 AV1 ---------------------------

enum class ProbeKind {
    HardwareDecode,  // PLY-20: bundled 2 s H.264 clip through droidvdec -> droideglsink
    Av1Decode        // SEL-01: bundled 2 s AV1 clip through the best AV1 software decoder
};

// Plan for a self-test pipeline on a local clip (file:// uri). Sinks run with
// sync=false so the measurement is decode speed, not the clock. The AV1 test
// ends in fakesink signal-handoffs=true (count "handoff"). ok == false when
// the needed elements are absent (the test then counts as failed).
PipelinePlan buildProbePlan(ProbeKind kind, const std::string &clipUri, const std::set<std::string> &available);

// SEL-01: the AV1 decoder present, by preference ("dav1ddec", "av1dec",
// "avdec_av1"), or "" if none.
std::string av1DecoderFor(const std::set<std::string> &available);

struct CodecSelfTestResult {
    bool error = false;            // pipeline error or plan not ok
    int framesDecoded = 0;
    int expectedFrames = 0;        // frames in the clip
    int64_t elapsedMs = 0;         // PLAYING -> EOS, wall clock
    int64_t clipDurationMs = 2000;
};

// SEL-01: AV1 is allowed only if the clip decoded completely in real time
// (no error, all frames, elapsed <= clip duration).
bool codecSelfTestPassed(const CodecSelfTestResult &result);

// ---- Decode-mode policy (PLY-20, PLY-21, PLT-08) -----------------------------

enum class ProbeState { NotRun, Running, Passed, Failed };

// What to persist per device/build (QSettings in the app layer). buildKey is
// the caller's identity of the hardware + software stack, e.g.
// "<device model>|<OS version>|<gst-droid version>|<app version>"; a record
// with another key is ignored, so an OS or app update re-probes.
struct HardwareProbeRecord {
    std::string buildKey;
    ProbeState state = ProbeState::NotRun;  // only Passed / Failed are stored
};

enum class DecodeEvent {
    None,
    ProbePassed,         // hardware verified: next video may use it
    ProbeFailed,         // show a notice: hardware decode unsupported here
    FallbackToSoftware   // rebuild this video in software and show a notice
};

// Time-injected state machine; all times are milliseconds from any monotonic
// origin. Usage:
//   restore(saved); setHardwareElementsPresent(...); setHardwareRequested(...)
//   if (needsProbe()) run buildProbePlan(HardwareDecode), calling
//     onProbeStarted / onProbeFrame (first frame) / onProbeError, and tick()
//     from a timer; persist record() on ProbePassed / ProbeFailed.
//   per video: mode = modeForVideo(id); build; at PLAYING onVideoStarted();
//     first rendered frame -> onVideoFrame(); pause/teardown ->
//     onVideoStopped(); tick() from a timer; FallbackToSoftware -> rebuild
//     with modeForVideo(id) (now Software) at the same position + notice.
class DecodeModePolicy {
public:
    static const int64_t kProbeFrameDeadlineMs = 2000;  // PLY-20 probe
    static const int64_t kWatchdogMs = 3000;            // PLY-20 per video

    explicit DecodeModePolicy(const std::string &buildKey = std::string());

    // Adopt a persisted record if its buildKey matches ours.
    void restore(const HardwareProbeRecord &record);
    HardwareProbeRecord record() const;

    // PLY-21: false by default. Takes effect from the next video.
    void setHardwareRequested(bool requested);
    bool hardwareRequested() const { return requested_; }
    // Per-build fallback: droidvdec and droideglsink both in the registry.
    void setHardwareElementsPresent(bool present);
    bool hardwareElementsPresent() const { return present_; }

    ProbeState probeState() const { return probe_; }
    // PLY-20: probe on first enable (requested, elements present, never run).
    bool needsProbe() const;
    void onProbeStarted(int64_t nowMs);
    DecodeEvent onProbeFrame(int64_t nowMs);
    DecodeEvent onProbeError();
    // Settings "retry": forget the stored result.
    void forgetProbe();

    // PLY-20/21: the decode mode for building `videoId`.
    DecodeMode modeForVideo(const std::string &videoId) const;
    // SEL-02: prefer VP9 when hardware decode is requested and verified.
    bool preferVp9() const;

    void onVideoStarted(const std::string &videoId, DecodeMode mode, int64_t nowMs);
    void onVideoFrame(int64_t nowMs);
    void onVideoStopped();
    bool watchdogArmed() const { return armed_; }

    // Deadlines: probe (2 s) and watchdog (3 s).
    DecodeEvent tick(int64_t nowMs);

private:
    bool hardwareUsable() const;

    std::string buildKey_;
    bool requested_ = false;
    bool present_ = false;
    ProbeState probe_ = ProbeState::NotRun;
    int64_t probeStartMs_ = 0;

    std::string currentVideo_;
    bool armed_ = false;
    int64_t armedAtMs_ = 0;
    std::set<std::string> softwareOnly_;  // videos that fell back this session
};

} } // namespace nordtile::player
