// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PipelinePlan.h"

#include <sstream>

namespace nordtile { namespace player {

namespace en = elementnames;

// ---- ElementPlan / PipelinePlan accessors ------------------------------------

int ElementPlan::propertyIndex(const std::string &key) const {
    for (size_t i = 0; i < properties.size(); ++i)
        if (properties[i].name == key) return static_cast<int>(i);
    return -1;
}

std::string ElementPlan::property(const std::string &key) const {
    const int i = propertyIndex(key);
    return i < 0 ? std::string() : properties[static_cast<size_t>(i)].value;
}

bool PipelinePlan::hasVideo() const { return route(RouteKind::Video) != nullptr; }

std::vector<std::string> PipelinePlan::factories() const {
    std::vector<std::string> out;
    for (size_t b = 0; b < branches.size(); ++b) {
        out.push_back(branches[b].source.factory);
        for (size_t r = 0; r < branches[b].routes.size(); ++r)
            for (size_t e = 0; e < branches[b].routes[r].chain.size(); ++e)
                out.push_back(branches[b].routes[r].chain[e].factory);
    }
    if (!unmatchedPadSink.factory.empty()) out.push_back(unmatchedPadSink.factory);
    return out;
}

const ElementPlan *PipelinePlan::find(const std::string &name) const {
    for (size_t b = 0; b < branches.size(); ++b) {
        if (branches[b].source.name == name) return &branches[b].source;
        for (size_t r = 0; r < branches[b].routes.size(); ++r)
            for (size_t e = 0; e < branches[b].routes[r].chain.size(); ++e)
                if (branches[b].routes[r].chain[e].name == name) return &branches[b].routes[r].chain[e];
    }
    return nullptr;
}

const PadRoute *PipelinePlan::route(RouteKind kind) const {
    for (size_t b = 0; b < branches.size(); ++b)
        for (size_t r = 0; r < branches[b].routes.size(); ++r)
            if (branches[b].routes[r].kind == kind) return &branches[b].routes[r];
    return nullptr;
}

// ---- Helpers ------------------------------------------------------------------

namespace {

PropertySetting prop(const std::string &name, const std::string &value, bool optional = false) {
    PropertySetting p;
    p.name = name;
    p.value = value;
    p.optional = optional;
    return p;
}

std::string toString(int64_t v) {
    std::ostringstream s;
    s << v;
    return s.str();
}

ElementPlan element(const std::string &factory, const std::string &name) {
    ElementPlan e;
    e.factory = factory;
    e.name = name;
    return e;
}

bool startsWith(const std::string &s, const char *prefix) { return s.compare(0, std::string(prefix).size(), prefix) == 0; }

bool isNetworkUri(const std::string &uri) { return startsWith(uri, "http://") || startsWith(uri, "https://"); }

std::string quoted(const std::string &value) {
    std::string out = "\"";
    for (size_t i = 0; i < value.size(); ++i) {
        const char c = value[i];
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

bool validFieldName(const std::string &name) {
    if (name.empty()) return false;
    const char f = name[0];
    if (!((f >= 'A' && f <= 'Z') || (f >= 'a' && f <= 'z'))) return false;
    for (size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) return false;
    }
    return true;
}

class Builder {
public:
    Builder(const PlanInput &in, PipelinePlan &plan) : in_(in), plan_(plan) {}

    bool has(const std::string &factory) const { return in_.available.count(factory) != 0; }

    void require(const std::string &factory, const std::string &why) {
        if (has(factory)) return;
        plan_.missingRequired.push_back(factory);
        plan_.errors.push_back("missing element " + factory + " (" + why + ")");
    }

    void error(const std::string &message) { plan_.errors.push_back(message); }

    // Optional chain stage: appended if present, recorded as skipped if not.
    bool optional(std::vector<ElementPlan> &chain, const ElementPlan &e) {
        if (has(e.factory)) { chain.push_back(e); return true; }
        plan_.skipped.push_back(e.name + " (" + e.factory + ")");
        return false;
    }

    BranchPlan branch(Branch which, const char *sourceName, const SourceSpec &spec, bool singleSource,
                      const std::string &stopCaps) {
        BranchPlan b;
        b.branch = which;
        b.container = spec.container;
        b.network = plan_.mode != PlayMode::Local && isNetworkUri(spec.uri);
        b.source = element("uridecodebin", sourceName);
        std::vector<PropertySetting> &p = b.source.properties;
        // PLY-21/PLT-08: never autoplug a hardware decoder (since 1.18;
        // optional for older cores). Hardware video goes through explicit
        // stop caps + droidvdec, so this holds on the hardware path too.
        p.push_back(prop("force-sw-decoders", "true", true));
        if (!stopCaps.empty()) p.push_back(prop("caps", stopCaps));
        // PLY-04 container hints, always before "uri".
        if (b.network && !singleSource && spec.container == Container::IsoBmff) {
            b.hideSizeUntilDemuxReady = true;  // pull mode: no end-of-file probe before the first pad
            p.push_back(prop("download", "true"));
            p.push_back(prop("buffer-duration", toString(kIsoBmffBufferDurationNs)));
            p.push_back(prop("buffer-size", toString(kIsoBmffBufferSizeBytes)));
        } else if (b.network && !singleSource) {
            p.push_back(prop("download", "false"));
            p.push_back(prop("buffer-duration", toString(kMatroskaBufferDurationNs)));
            p.push_back(prop("buffer-size", "-1"));
        } else {
            // Muxed / HLS / local: hints cleared, no disk buffer.
            p.push_back(prop("download", "false"));
            p.push_back(prop("buffer-duration", "-1"));
            p.push_back(prop("buffer-size", "-1"));
        }
        // HLS: adaptive streams buffer in decodebin's multiqueue; without
        // use-buffering no BUFFERING messages reach BufferingPolicy (PLY-08).
        if (plan_.mode == PlayMode::Hls) p.push_back(prop("use-buffering", "true"));
        p.push_back(prop("uri", spec.uri));

        if (b.network) {
            if (!spec.userAgent.empty()) b.sourceSetup.push_back(prop("user-agent", spec.userAgent, true));
            const std::string headers = extraHeadersStructure(spec.extraHeaders);
            if (!headers.empty()) b.sourceSetup.push_back(prop("extra-headers", headers, true));
            b.sourceSetup.push_back(prop("timeout", toString(kHttpTimeoutSeconds), true));
        }

        if (spec.container == Container::IsoBmff) require("qtdemux", "ISO-BMFF source");
        if (spec.container == Container::Matroska) require("matroskademux", "WebM source");
        return b;
    }

    PadRoute videoRoute() {
        PadRoute r;
        r.kind = RouteKind::Video;
        if (plan_.decode == DecodeMode::Hardware) {
            r.capsPrefixes.push_back("video/x-h264");
            r.capsPrefixes.push_back("video/x-vp9");
            r.chain.push_back(element("droidvdec", en::kVideoDecoder));
            ElementPlan sink = element("droideglsink", en::kVideoSink);
            sink.properties.push_back(prop("sync", "true"));
            sink.properties.push_back(prop("qos", "true"));
            r.chain.push_back(sink);
            plan_.videoSinkFactory = sink.factory;
            return r;
        }
        r.capsPrefixes.push_back("video/x-raw");
        require("videoconvert", "software video");
        r.chain.push_back(element("videoconvert", en::kVideoConvert));
        if (in_.videoSink.active()) {
            ElementPlan sink = element(in_.videoSink.factory, en::kVideoSink);
            sink.properties = in_.videoSink.properties;
            require(sink.factory, "video sink override");
            r.chain.push_back(sink);
            plan_.videoSinkFactory = sink.factory;
            return r;
        }
        // PLY-02: sync to the audio clock, QoS on, 2-buffer dropping queue.
        // max-lateness = GstVideoSink's 20 ms default (appsink inherits
        // basesink's -1, which would render late frames instead of dropping).
        ElementPlan sink = element("appsink", en::kVideoSink);
        sink.properties.push_back(prop("caps", kSoftwareVideoCaps));
        sink.properties.push_back(prop("sync", "true"));
        sink.properties.push_back(prop("qos", "true"));
        sink.properties.push_back(prop("max-buffers", "2"));
        sink.properties.push_back(prop("drop", "true"));
        sink.properties.push_back(prop("max-lateness", "20000000"));
        require("appsink", "software video sink");
        r.chain.push_back(sink);
        plan_.videoSinkFactory = sink.factory;
        return r;
    }

    // PLY-22 order: [audioconvert] scaletempo audioconvert(F32) equalizer
    // volume(loudness) volume(boost) limiter audioconvert audioresample sink.
    // Every stage but the sink is optional. The leading audioconvert turns
    // planar decoder output (e.g. avdec_aac F32 non-interleaved) into what
    // scaletempo accepts; it is passthrough otherwise. The F32 link filter
    // keeps boost in float so samples above full scale survive until the
    // limiter instead of clipping in volume.
    PadRoute audioRoute() {
        PadRoute r;
        r.kind = RouteKind::Audio;
        r.capsPrefixes.push_back("audio/x-raw");
        const std::vector<ElementProperties> live = audioChainProperties(in_.audioValues, std::string());

        optional(r.chain, element("audioconvert", en::kAudioConvertIn));
        // Kept on HLS too (no rate change at the live edge, but the element
        // stays so the chain shape is the same in every mode).
        optional(r.chain, element("scaletempo", en::kAudioTempo));
        ElementPlan toFloat = element("audioconvert", en::kAudioConvertFloat);
        toFloat.linkCapsToNext = "audio/x-raw,format=F32LE,layout=interleaved";
        optional(r.chain, toFloat);

        ElementPlan eq = element("equalizer-10bands", en::kAudioEq);
        ElementPlan loud = element("volume", en::kAudioLoudness);
        ElementPlan boost = element("volume", en::kAudioBoost);
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i].element == en::kAudioEq) eq.properties = live[i].properties;
            if (live[i].element == en::kAudioLoudness) loud.properties = live[i].properties;
            if (live[i].element == en::kAudioBoost) boost.properties = live[i].properties;
        }
        // equalizer-10bands and rglimiter take float only; without
        // audioconvert nothing could feed them (opusdec emits S16), so they
        // are skipped too and the chain stays negotiable.
        const bool canConvert = has("audioconvert");
        if (canConvert) optional(r.chain, eq);
        else plan_.skipped.push_back(eq.name + " (needs audioconvert)");
        optional(r.chain, loud);
        optional(r.chain, boost);

        const char *limiter = (canConvert && has("rglimiter")) ? "rglimiter" : has("audiodynamic") ? "audiodynamic" : "";
        if (*limiter) {
            ElementPlan lim = element(limiter, en::kAudioLimiter);
            const std::vector<ElementProperties> lp = audioChainProperties(in_.audioValues, limiter);
            for (size_t i = 0; i < lp.size(); ++i)
                if (lp[i].element == en::kAudioLimiter) lim.properties = lp[i].properties;
            r.chain.push_back(lim);
            plan_.limiterFactory = limiter;
        } else {
            plan_.skipped.push_back(std::string(en::kAudioLimiter) + " (rglimiter|audiodynamic)");
        }

        optional(r.chain, element("audioconvert", en::kAudioConvertOut));
        optional(r.chain, element("audioresample", en::kAudioResample));

        ElementPlan sink;
        if (in_.audioSink.active()) {
            sink = element(in_.audioSink.factory, en::kAudioSink);
            sink.properties = in_.audioSink.properties;
            require(sink.factory, "audio sink override");
        } else if (has("pulsesink")) {
            sink = element("pulsesink", en::kAudioSink);
            // PLT-12: explicit media.role on every stream.
            if (!in_.mediaRole.empty()) {
                sink.properties.push_back(prop("stream-properties", streamPropertiesStructure(in_.mediaRole)));
                plan_.mediaRole = in_.mediaRole;
            }
        } else if (has("autoaudiosink")) {
            // Host/dev fallback only; it cannot carry media.role.
            sink = element("autoaudiosink", en::kAudioSink);
            plan_.skipped.push_back(std::string(en::kAudioSink) + " (pulsesink)");
        } else {
            require("pulsesink", "audio sink");
            sink = element("pulsesink", en::kAudioSink);
        }
        r.chain.push_back(sink);
        plan_.audioSinkFactory = sink.factory;
        plan_.clockProvider = sink.name;  // PLY-02: the audio sink is the clock
        return r;
    }

    void unmatchedSink() {
        if (!has("fakesink")) return;
        plan_.unmatchedPadSink = element("fakesink", "unmatched-sink");
        plan_.unmatchedPadSink.properties.push_back(prop("sync", "false"));
        plan_.unmatchedPadSink.properties.push_back(prop("async", "false"));
    }

private:
    const PlanInput &in_;
    PipelinePlan &plan_;
};

// Stop caps for the audio-only layout: any video stream stops still
// compressed and goes to the unmatched fakesink, so it is never decoded.
const char *const kAudioOnlyStopCaps =
    "audio/x-raw; video/x-h264; video/x-h265; video/x-vp8; video/x-vp9; video/x-av1";

} // namespace

// ---- Public functions -----------------------------------------------------------

std::string extraHeadersStructure(const std::map<std::string, std::string> &headers) {
    std::string out;
    for (std::map<std::string, std::string>::const_iterator it = headers.begin(); it != headers.end(); ++it) {
        if (!validFieldName(it->first)) continue;
        out += ", " + it->first + "=(string)" + quoted(it->second);
    }
    return out.empty() ? out : "extra-headers" + out;
}

std::string streamPropertiesStructure(const std::string &mediaRole) {
    return "props, media.role=(string)" + quoted(mediaRole);
}

std::vector<std::string> elementsToDemote() {
    // PLT-08: no droid element is ever autoplugged, audio included (R2 run of
    // 2026-11-08: a Codec2 decoder appeared with hardware decode off). The
    // hardware video path creates droidvdec explicitly (PLY-20).
    static const char *const names[] = {"droidvdec", "droidadec"};
    return std::vector<std::string>(names, names + 2);
}

std::vector<std::string> elementsToProbe() {
    static const char *const names[] = {
        "uridecodebin", "souphttpsrc", "filesrc", "qtdemux", "matroskademux", "hlsdemux", "hlsdemux2",
        "videoconvert", "appsink", "fakesink", "droidvdec", "droideglsink",
        "audioconvert", "audioresample", "scaletempo", "equalizer-10bands", "volume", "rglimiter",
        "audiodynamic", "pulsesink", "autoaudiosink", "dav1ddec", "av1dec", "avdec_av1"};
    return std::vector<std::string>(names, names + sizeof(names) / sizeof(names[0]));
}

bool containsDroidElement(const PipelinePlan &plan) {
    const std::vector<std::string> f = plan.factories();
    for (size_t i = 0; i < f.size(); ++i)
        if (startsWith(f[i], "droid")) return true;
    return false;
}

PipelinePlan buildPipelinePlan(const PlanInput &in) {
    PipelinePlan plan;
    plan.mode = in.mode;
    plan.requestedDecode = in.decode;
    plan.rateLockedAtLiveEdge = in.mode == PlayMode::Hls;
    Builder b(in, plan);
    b.require("uridecodebin", "every source");

    const bool video = in.mode != PlayMode::AudioOnly;

    // PLY-20/21 + PLT-08: hardware only when asked for AND both elements
    // exist; otherwise this build falls back to software.
    plan.decode = DecodeMode::Software;
    if (video && in.decode == DecodeMode::Hardware) {
        if (in.videoSink.active()) {
            plan.hardwareFallbackReason = "video sink override in use";
        } else if (!b.has("droidvdec") || !b.has("droideglsink")) {
            plan.hardwareFallbackReason = !b.has("droidvdec") ? "droidvdec missing" : "droideglsink missing";
        } else {
            plan.decode = DecodeMode::Hardware;
        }
    }
    const bool hw = plan.decode == DecodeMode::Hardware;

    switch (in.mode) {
    case PlayMode::Dual: {
        if (!in.video.present()) b.error("dual mode needs a video source");
        if (!in.audio.present()) b.error("dual mode needs an audio source");
        BranchPlan v = b.branch(Branch::Video, en::kVideoSource, in.video, false, hw ? kHardwareStopCaps : "");
        v.routes.push_back(b.videoRoute());
        BranchPlan a = b.branch(Branch::Audio, en::kAudioSource, in.audio, false, "");
        a.routes.push_back(b.audioRoute());
        plan.branches.push_back(v);
        plan.branches.push_back(a);
        break;
    }
    case PlayMode::Muxed:
    case PlayMode::Hls:
    case PlayMode::Local: {
        if (!in.muxed.present()) b.error("this mode needs the muxed source");
        if (in.mode == PlayMode::Local && !startsWith(in.muxed.uri, "file://"))
            b.error("local mode needs a file:// uri");
        if (in.mode == PlayMode::Hls && !b.has("hlsdemux") && !b.has("hlsdemux2")) {
            plan.missingRequired.push_back("hlsdemux");
            b.error("missing element hlsdemux (HLS source)");
        }
        const std::string stop = hw ? std::string(kHardwareStopCaps) + "; audio/x-raw" : std::string();
        BranchPlan m = b.branch(Branch::Muxed, en::kMuxedSource, in.muxed, true, stop);
        m.routes.push_back(b.videoRoute());
        m.routes.push_back(b.audioRoute());
        plan.branches.push_back(m);
        break;
    }
    case PlayMode::AudioOnly: {
        const SourceSpec &src = in.audio.present() ? in.audio : in.muxed;
        if (!src.present()) b.error("audio-only mode needs an audio source");
        // A DASH audio branch follows the per-container rules of PLY-04
        // (an m4a branch needs the byte-seekable buffer too).
        const bool dashBranch = in.audio.present();
        BranchPlan a = b.branch(Branch::Audio, en::kAudioSource, src, !dashBranch, kAudioOnlyStopCaps);
        a.routes.push_back(b.audioRoute());
        plan.branches.push_back(a);
        break;
    }
    }
    b.unmatchedSink();
    plan.ok = plan.errors.empty();
    return plan;
}

std::string av1DecoderFor(const std::set<std::string> &available) {
    static const char *const order[] = {"dav1ddec", "av1dec", "avdec_av1"};
    for (size_t i = 0; i < 3; ++i)
        if (available.count(order[i])) return order[i];
    return std::string();
}

PipelinePlan buildProbePlan(ProbeKind kind, const std::string &clipUri, const std::set<std::string> &available) {
    PipelinePlan plan;
    plan.mode = PlayMode::Local;
    PlanInput in;
    in.mode = PlayMode::Local;
    in.available = available;
    Builder b(in, plan);
    b.require("uridecodebin", "probe source");
    if (!startsWith(clipUri, "file://")) b.error("probe clip must be a file:// uri");

    BranchPlan br;
    br.branch = Branch::Video;
    br.source = element("uridecodebin", en::kVideoSource);
    PadRoute r;
    r.kind = RouteKind::Video;
    if (kind == ProbeKind::HardwareDecode) {
        plan.requestedDecode = plan.decode = DecodeMode::Hardware;
        b.require("droidvdec", "hardware probe");
        b.require("droideglsink", "hardware probe");
        br.source.properties.push_back(prop("caps", kHardwareStopCaps));
        r.capsPrefixes.push_back("video/x-h264");
        r.capsPrefixes.push_back("video/x-vp9");
        r.chain.push_back(element("droidvdec", en::kVideoDecoder));
        ElementPlan sink = element("droideglsink", en::kVideoSink);
        sink.properties.push_back(prop("sync", "false"));
        r.chain.push_back(sink);
    } else {
        if (av1DecoderFor(available).empty()) {
            plan.missingRequired.push_back("dav1ddec");
            b.error("no AV1 decoder (dav1ddec, av1dec, avdec_av1)");
        }
        b.require("fakesink", "AV1 probe sink");
        br.source.properties.push_back(prop("force-sw-decoders", "true", true));
        r.capsPrefixes.push_back("video/x-raw");
        ElementPlan sink = element("fakesink", en::kVideoSink);
        sink.properties.push_back(prop("sync", "false"));
        sink.properties.push_back(prop("signal-handoffs", "true"));
        r.chain.push_back(sink);
    }
    br.source.properties.push_back(prop("uri", clipUri));
    br.routes.push_back(r);
    plan.videoSinkFactory = r.chain.back().factory;
    plan.branches.push_back(br);
    b.unmatchedSink();
    plan.ok = plan.errors.empty();
    return plan;
}

bool codecSelfTestPassed(const CodecSelfTestResult &r) {
    return !r.error && r.expectedFrames > 0 && r.framesDecoded >= r.expectedFrames && r.clipDurationMs > 0 &&
           r.elapsedMs >= 0 && r.elapsedMs <= r.clipDurationMs;
}

std::string describePlan(const PipelinePlan &plan) {
    std::ostringstream s;
    for (size_t b = 0; b < plan.branches.size(); ++b) {
        const BranchPlan &br = plan.branches[b];
        if (b) s << " | ";
        s << br.source.factory << " name=" << br.source.name;
        for (size_t r = 0; r < br.routes.size(); ++r) {
            const PadRoute &route = br.routes[r];
            s << (r ? " " + br.source.name + "." : std::string()) << " ! ";
            for (size_t e = 0; e < route.chain.size(); ++e) {
                if (e) s << " ! ";
                s << route.chain[e].factory << " name=" << route.chain[e].name;
            }
        }
    }
    return s.str();
}

// ---- DecodeModePolicy --------------------------------------------------------------

const int64_t DecodeModePolicy::kProbeFrameDeadlineMs;
const int64_t DecodeModePolicy::kWatchdogMs;

DecodeModePolicy::DecodeModePolicy(const std::string &buildKey) : buildKey_(buildKey) {}

void DecodeModePolicy::restore(const HardwareProbeRecord &record) {
    if (record.buildKey != buildKey_) return;
    if (record.state == ProbeState::Passed || record.state == ProbeState::Failed) probe_ = record.state;
}

HardwareProbeRecord DecodeModePolicy::record() const {
    HardwareProbeRecord r;
    r.buildKey = buildKey_;
    r.state = (probe_ == ProbeState::Passed || probe_ == ProbeState::Failed) ? probe_ : ProbeState::NotRun;
    return r;
}

void DecodeModePolicy::setHardwareRequested(bool requested) { requested_ = requested; }
void DecodeModePolicy::setHardwareElementsPresent(bool present) { present_ = present; }

bool DecodeModePolicy::needsProbe() const { return requested_ && present_ && probe_ == ProbeState::NotRun; }

void DecodeModePolicy::onProbeStarted(int64_t nowMs) {
    probe_ = ProbeState::Running;
    probeStartMs_ = nowMs;
}

DecodeEvent DecodeModePolicy::onProbeFrame(int64_t nowMs) {
    if (probe_ != ProbeState::Running) return DecodeEvent::None;
    if (nowMs - probeStartMs_ <= kProbeFrameDeadlineMs) {
        probe_ = ProbeState::Passed;
        return DecodeEvent::ProbePassed;
    }
    probe_ = ProbeState::Failed;
    return DecodeEvent::ProbeFailed;
}

DecodeEvent DecodeModePolicy::onProbeError() {
    if (probe_ != ProbeState::Running) return DecodeEvent::None;
    probe_ = ProbeState::Failed;
    return DecodeEvent::ProbeFailed;
}

void DecodeModePolicy::forgetProbe() {
    probe_ = ProbeState::NotRun;
    softwareOnly_.clear();
}

bool DecodeModePolicy::hardwareUsable() const { return requested_ && present_ && probe_ == ProbeState::Passed; }

DecodeMode DecodeModePolicy::modeForVideo(const std::string &videoId) const {
    if (!hardwareUsable() || softwareOnly_.count(videoId)) return DecodeMode::Software;
    return DecodeMode::Hardware;
}

bool DecodeModePolicy::preferVp9() const { return hardwareUsable(); }

void DecodeModePolicy::onVideoStarted(const std::string &videoId, DecodeMode mode, int64_t nowMs) {
    currentVideo_ = videoId;
    armed_ = mode == DecodeMode::Hardware;
    armedAtMs_ = nowMs;
}

void DecodeModePolicy::onVideoFrame(int64_t) { armed_ = false; }

void DecodeModePolicy::onVideoStopped() { armed_ = false; }

DecodeEvent DecodeModePolicy::tick(int64_t nowMs) {
    if (probe_ == ProbeState::Running && nowMs - probeStartMs_ > kProbeFrameDeadlineMs) {
        probe_ = ProbeState::Failed;
        return DecodeEvent::ProbeFailed;
    }
    if (armed_ && nowMs - armedAtMs_ >= kWatchdogMs) {
        armed_ = false;
        softwareOnly_.insert(currentVideo_);
        return DecodeEvent::FallbackToSoftware;
    }
    return DecodeEvent::None;
}

} } // namespace nordtile::player
