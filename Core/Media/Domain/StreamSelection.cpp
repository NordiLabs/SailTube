// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "StreamSelection.h"

#include "LivePolicy.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace nordtile { namespace video { namespace media {

CodecSupport DecoderCapabilities::video(VideoCodec codec) const {
    switch (codec) {
    case VideoCodec::H264: return h264;
    case VideoCodec::Hevc: return hevc;
    case VideoCodec::Vp8: return vp8;
    case VideoCodec::Vp9: return vp9;
    case VideoCodec::Av1: return av1;
    case VideoCodec::None:
    case VideoCodec::Unknown: break;
    }
    return CodecSupport();
}

bool DecoderCapabilities::audio(AudioCodec codec) const {
    switch (codec) {
    case AudioCodec::Aac: return aac;
    case AudioCodec::Opus: return opus;
    case AudioCodec::Vorbis: return vorbis;
    case AudioCodec::None:
    case AudioCodec::Unknown: break;
    }
    return false;
}

namespace {

const int defaultSoftwareCeiling = 720;  // SEL-02
const int defaultHardwareCeiling = 1080; // SEL-02
const int defaultMobileCeiling = 480;    // SEL-02
const int standardHeightLimit = 1080;    // SEL-01

bool demuxable(const MediaFormat &f, const DecoderCapabilities &caps) {
    switch (containerOf(f)) {
    case Container::Hls: return caps.hls;
    case Container::Mp4: return caps.mp4Demux;
    case Container::WebM: return caps.webmDemux;
    case Container::None:
    case Container::Other: break;
    }
    return false;
}

bool directHttps(const MediaFormat &f) { return f.protocol == Protocol::Https && !f.url.empty(); }

// SEL-01: audio is Opus or AAC only.
bool audioAllowed(AudioCodec c, const DecoderCapabilities &caps) {
    return (c == AudioCodec::Opus || c == AudioCodec::Aac) && caps.audio(c);
}

int softwareHeightLimit(const PlaybackPreferences &prefs) {
    const int limit = prefs.softwareDecodeMaxHeight > 0 ? prefs.softwareDecodeMaxHeight : standardHeightLimit;
    return std::min(limit, standardHeightLimit);
}

int hardwareHeightLimit(const PlaybackPreferences &prefs) {
    return prefs.hardwareDecodeMaxHeight > 0 ? prefs.hardwareDecodeMaxHeight : standardHeightLimit;
}

bool hardwareFor(const MediaFormat &f, const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    return hardwareDecodeActive(caps, prefs) && caps.video(f.videoCodec).hardware
        && f.height <= hardwareHeightLimit(prefs);
}

// SEL-02 preferred codec: H.264 for software decode; with verified hardware
// decode, formats the hardware decodes first, VP9 before H.264. Lower is better.
int codecRank(const MediaFormat &f, const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    const VideoCodec c = f.videoCodec;
    if ((prefs.codec == CodecPreference::H264 && c == VideoCodec::H264)
        || (prefs.codec == CodecPreference::Vp9 && c == VideoCodec::Vp9)) return -1;
    int softwareOrder = c == VideoCodec::H264 ? 0 : c == VideoCodec::Vp9 ? 1 : 2;
    // PLY-06 deep start: a WebM rung (Cues index) before fragmented MP4 at equal height.
    if (prefs.preferSeekIndex) softwareOrder = containerOf(f) == Container::WebM ? 0 : 1 + softwareOrder;
    const int hardwareOrder = c == VideoCodec::Vp9 ? 0 : c == VideoCodec::H264 ? 1 : 2;
    if (hardwareDecodeActive(caps, prefs)) return hardwareFor(f, caps, prefs) ? hardwareOrder : 10 + softwareOrder;
    return softwareOrder;
}

// Tie-breakers at equal height (SEL-02): lower frame rate, preferred codec,
// non-Premium, higher bitrate, then id for a total order.
bool betterAtHeight(const MediaFormat &a, const MediaFormat &b,
                    const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    if (std::fabs(a.fps - b.fps) > 0.5) return a.fps < b.fps;
    const int ra = codecRank(a, caps, prefs), rb = codecRank(b, caps, prefs);
    if (ra != rb) return ra < rb;
    if (a.premium != b.premium) return !a.premium;
    if (a.totalKbps != b.totalKbps) return a.totalKbps > b.totalKbps;
    return a.id < b.id;
}

bool autoEligible(const MediaFormat &f) { return !f.highFrameRate() && !f.premium; }

// SEL-02 Auto: highest rung at or below the ceiling, else the lowest rung.
// High-frame-rate and Premium rungs only when nothing else is offered.
const MediaFormat *pickAuto(const std::vector<MediaFormat> &pool, int ceiling,
                            const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    bool anyEligible = false;
    for (const MediaFormat &f : pool) anyEligible = anyEligible || autoEligible(f);
    const MediaFormat *best = nullptr;
    for (const MediaFormat &f : pool) {
        if (anyEligible && !autoEligible(f)) continue;
        if (!best) { best = &f; continue; }
        const bool inF = f.height <= ceiling, inB = best->height <= ceiling;
        if (inF != inB) { if (inF) best = &f; continue; }
        if (f.height != best->height) {
            if (inF ? f.height > best->height : f.height < best->height) best = &f;
            continue;
        }
        if (betterAtHeight(f, *best, caps, prefs)) best = &f;
    }
    return best;
}

bool sameKey(const MediaFormat &f, int height, bool highFrameRate, bool premium) {
    return f.height == height && f.highFrameRate() == highFrameRate && f.premium == premium;
}

// SEL-03 explicit choice; nullptr when the choice is Auto or not offered.
const MediaFormat *pickChoice(const std::vector<MediaFormat> &pool, const QualityChoice &choice,
                              const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    if (choice.height <= 0 && choice.formatId.empty()) return nullptr;
    if (!choice.formatId.empty())
        for (const MediaFormat &f : pool) if (f.id == choice.formatId) return &f;
    const MediaFormat *best = nullptr;
    for (const MediaFormat &f : pool)
        if (sameKey(f, choice.height, choice.highFrameRate, choice.premium) && (!best || betterAtHeight(f, *best, caps, prefs)))
            best = &f;
    return best;
}

std::string heightLabel(const MediaFormat &f) {
    char buffer[48];
    if (f.highFrameRate()) std::snprintf(buffer, sizeof buffer, "%dp%d", f.height, static_cast<int>(std::lround(f.fps)));
    else std::snprintf(buffer, sizeof buffer, "%dp", f.height);
    std::string label(buffer);
    if (f.premium) label += " Premium";
    return label;
}

std::string audioLabel(const MediaFormat &f) {
    char buffer[64];
    const double kbps = f.audioKbps > 0 ? f.audioKbps : f.totalKbps;
    if (kbps > 0) std::snprintf(buffer, sizeof buffer, "%s %d kbps", audioCodecName(f.audioCodec), static_cast<int>(std::lround(kbps)));
    else std::snprintf(buffer, sizeof buffer, "%s", audioCodecName(f.audioCodec));
    return buffer;
}

struct Pools {
    std::vector<MediaFormat> videoOnly; // dual video branch candidates (SEL-01)
    std::vector<MediaFormat> muxed;     // progressive fallback
    std::vector<MediaFormat> audio;     // dual audio branch / listen-only (SEL-04)
    std::vector<MediaFormat> hlsMuxed;  // RES-19 live variants
    std::vector<MediaFormat> hlsAudio;  // live audio renditions (listen-only)
};

Pools classify(const SelectionInput &in, const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    Pools p;
    for (const MediaFormat &f : in.formats) {
        if (f.url.empty() || !demuxable(f, caps)) continue;
        if (f.protocol == Protocol::Hls) {
            if (muxedHls(f) && videoDecodable(f, caps, prefs) && audioAllowed(f.audioCodec, caps)) p.hlsMuxed.push_back(f);
            else if (!f.hasVideo() && audioAllowed(f.audioCodec, caps)) p.hlsAudio.push_back(f);
            continue;
        }
        if (!directHttps(f)) continue;
        if (f.muxed()) {
            if (videoDecodable(f, caps, prefs) && audioAllowed(f.audioCodec, caps)) p.muxed.push_back(f);
        } else if (f.hasVideo() && f.audioCodec == AudioCodec::None) {
            if (videoDecodable(f, caps, prefs)) p.videoOnly.push_back(f);
        } else if (!f.hasVideo() && audioAllowed(f.audioCodec, caps)) {
            p.audio.push_back(f);
        }
    }
    return p;
}

StreamCandidate dualCandidate(const MediaFormat &v, const MediaFormat &a) {
    StreamCandidate c;
    c.kind = StreamKind::Adaptive;
    c.video = v; c.audio = a; c.height = v.height;
    c.videoContainer = containerOf(v); c.audioContainer = containerOf(a);
    c.label = heightLabel(v) + " " + videoCodecName(v.videoCodec) + " + " + audioCodecName(a.audioCodec) + " (dual)";
    return c;
}

StreamCandidate muxedCandidate(const MediaFormat &m, StreamKind kind) {
    StreamCandidate c;
    c.kind = kind;
    c.muxed = m; c.height = m.height;
    c.muxedContainer = containerOf(m);
    if (kind == StreamKind::AudioOnly) c.label = "audio from " + heightLabel(m) + " stream";
    else c.label = heightLabel(m) + " " + videoCodecName(m.videoCodec) + (kind == StreamKind::Live ? " live" : " muxed");
    c.bypassProxyAndProbe = m.protocol == Protocol::Hls;
    return c;
}

StreamCandidate audioCandidate(const MediaFormat &a) {
    StreamCandidate c;
    c.kind = StreamKind::AudioOnly;
    c.audio = a;
    c.audioContainer = containerOf(a);
    c.label = "audio " + audioLabel(a);
    return c;
}

std::vector<MediaFormat> withoutCodec(const std::vector<MediaFormat> &pool, VideoCodec codec) {
    std::vector<MediaFormat> out;
    for (const MediaFormat &f : pool) if (f.videoCodec != codec) out.push_back(f);
    return out;
}

const MediaFormat *pickVideo(const std::vector<MediaFormat> &pool, const QualityChoice &choice, int ceiling,
                             const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    const MediaFormat *v = pickChoice(pool, choice, caps, prefs);
    return v ? v : pickAuto(pool, ceiling, caps, prefs);
}

// RES-19: classic muxed variants first, then other muxed HLS, then the master manifest.
std::vector<StreamCandidate> liveChain(const SelectionInput &in, const Pools &p, int ceiling,
                                       const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    std::vector<StreamCandidate> chain;
    std::vector<MediaFormat> classic, other;
    for (const MediaFormat &f : p.hlsMuxed) (classicMuxedHlsItag(f.id) ? classic : other).push_back(f);
    if (prefs.mode == PlaybackMode::AudioOnly) {
        if (const MediaFormat *a = bestAudioTrack(p.hlsAudio, prefs.audioLanguage)) {
            StreamCandidate c = muxedCandidate(*a, StreamKind::Live);
            c.label = "live audio " + audioLabel(*a);
            chain.push_back(c);
            return chain;
        }
        ceiling = 1; // SEL-08 without an audio rendition: the smallest muxed variant
    }
    if (const MediaFormat *v = pickAuto(classic, ceiling, caps, prefs)) chain.push_back(muxedCandidate(*v, StreamKind::Live));
    if (const MediaFormat *v = pickAuto(other, ceiling, caps, prefs)) chain.push_back(muxedCandidate(*v, StreamKind::Live));
    if (caps.hls && !in.manifestUrl.empty()) {
        StreamCandidate c;
        c.kind = StreamKind::Live;
        c.label = "live (master manifest)";
        c.muxed.url = in.manifestUrl; c.muxed.protocol = Protocol::Hls; c.muxed.container = "mp4";
        c.muxedContainer = Container::Hls;
        c.bypassProxyAndProbe = true;
        chain.push_back(c);
    }
    return chain;
}

std::vector<StreamCandidate> onDemandChain(const Pools &p, const QualityChoice &choice, int ceiling,
                                           const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    std::vector<StreamCandidate> chain;
    const MediaFormat *aud = bestAudioTrack(p.audio, prefs.audioLanguage);
    if (prefs.mode == PlaybackMode::AudioOnly) {
        // SEL-08: the SEL-04/05 best audio, then the other codec; a muxed
        // stream only when no audio-only format exists.
        if (aud) {
            chain.push_back(audioCandidate(*aud));
            const std::vector<MediaFormat> rest = [&]() {
                std::vector<MediaFormat> out;
                for (const MediaFormat &f : p.audio) if (f.audioCodec != aud->audioCodec) out.push_back(f);
                return out;
            }();
            if (const MediaFormat *b = bestAudioTrack(rest, prefs.audioLanguage)) chain.push_back(audioCandidate(*b));
        } else if (const MediaFormat *m = pickAuto(p.muxed, 1, caps, prefs)) {
            chain.push_back(muxedCandidate(*m, StreamKind::AudioOnly));
        }
        return chain;
    }
    if (prefs.allowAdaptive && aud) {
        if (const MediaFormat *v = pickVideo(p.videoOnly, choice, ceiling, caps, prefs)) {
            chain.push_back(dualCandidate(*v, *aud));
            // A second codec family as the next attempt (decoder refusal).
            // An Auto fallback stays Auto: no high-frame-rate or Premium rung
            // when the offer has ordinary ones.
            std::vector<MediaFormat> rest = withoutCodec(p.videoOnly, v->videoCodec);
            if (autoEligible(*v) && !pickChoice(p.videoOnly, choice, caps, prefs))
                rest.erase(std::remove_if(rest.begin(), rest.end(), [](const MediaFormat &f) { return !autoEligible(f); }),
                           rest.end());
            if (const MediaFormat *w = pickVideo(rest, choice, ceiling, caps, prefs)) chain.push_back(dualCandidate(*w, *aud));
        }
    }
    if (const MediaFormat *m = pickAuto(p.muxed, ceiling, caps, prefs)) chain.push_back(muxedCandidate(*m, StreamKind::Progressive));
    return chain;
}

std::vector<StreamCandidate> select(const SelectionInput &in, const DecoderCapabilities &caps,
                                    const PlaybackPreferences &prefs, const QualityChoice &choice, int ceiling) {
    std::vector<StreamCandidate> chain;
    if (in.liveStatus == LiveStatus::Upcoming) return chain; // RES-19: wait for scheduledStartUnix
    const Pools p = classify(in, caps, prefs);
    if (in.liveStatus == LiveStatus::IsLive) return liveChain(in, p, ceiling, caps, prefs);
    chain = onDemandChain(p, choice, ceiling, caps, prefs);
    // A just-ended stream may still be offered as HLS only.
    if (chain.empty() && in.liveStatus == LiveStatus::PostLive) chain = liveChain(in, p, ceiling, caps, prefs);
    return chain;
}

bool dualPossible(const SelectionInput &in, const Pools &p, const PlaybackPreferences &prefs) {
    return prefs.mode == PlaybackMode::Video && prefs.allowAdaptive && !p.audio.empty() && !p.videoOnly.empty()
        && in.liveStatus != LiveStatus::IsLive && in.liveStatus != LiveStatus::Upcoming;
}

std::vector<QualityOption> options(const Pools &p, const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    std::vector<QualityOption> out;
    std::vector<const MediaFormat *> representative;
    for (const MediaFormat &f : p.videoOnly) {
        std::size_t i = 0;
        while (i < out.size() && !sameKey(f, out[i].height, out[i].highFrameRate, out[i].premium)) ++i;
        if (i == out.size()) {
            QualityOption o;
            o.height = f.height; o.highFrameRate = f.highFrameRate(); o.premium = f.premium;
            o.kind = StreamKind::Adaptive;
            out.push_back(o);
            representative.push_back(nullptr);
        }
        out[i].formatIds.push_back(f.id);
        if (!representative[i] || betterAtHeight(f, *representative[i], caps, prefs)) representative[i] = &f;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const MediaFormat &f = *representative[i];
        out[i].formatId = f.id; out[i].fps = f.fps; out[i].codec = f.videoCodec; out[i].label = heightLabel(f);
    }
    std::sort(out.begin(), out.end(), [](const QualityOption &a, const QualityOption &b) {
        if (a.height != b.height) return a.height > b.height;
        if (a.premium != b.premium) return a.premium;
        if (a.highFrameRate != b.highFrameRate) return a.highFrameRate;
        return a.fps > b.fps;
    });
    return out;
}

int indexOfFormat(const std::vector<QualityOption> &entries, const std::string &id) {
    if (id.empty()) return -1;
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (std::find(entries[i].formatIds.begin(), entries[i].formatIds.end(), id) != entries[i].formatIds.end())
            return static_cast<int>(i);
    return -1;
}

} // namespace

bool hardwareDecodeActive(const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    return prefs.hardwareDecode && caps.hardwareDecodeVerified;
}

int qualityCeiling(const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    if (prefs.network == NetworkClass::Mobile) return prefs.mobileMaxHeight > 0 ? prefs.mobileMaxHeight : defaultMobileCeiling;
    if (prefs.maxHeight > 0) return prefs.maxHeight;
    return hardwareDecodeActive(caps, prefs) ? defaultHardwareCeiling : defaultSoftwareCeiling;
}

QualityChoice qualityChoice(const QualityOption &o) {
    QualityChoice c;
    c.height = o.height; c.highFrameRate = o.highFrameRate; c.premium = o.premium; c.formatId = o.formatId;
    return c;
}

bool videoDecodable(const MediaFormat &f, const DecoderCapabilities &caps, const PlaybackPreferences &prefs) {
    if (!f.hasVideo() || f.highDynamicRange) return false;
    const VideoCodec c = f.videoCodec;
    if (c != VideoCodec::H264 && c != VideoCodec::Vp9 && !(c == VideoCodec::Av1 && caps.av1SelfTestPassed)) return false;
    if (f.highFrameRate() && !prefs.allowHighFrameRate) return false;
    const CodecSupport support = caps.video(c);
    if (!support.available && !support.software) return false;
    if (hardwareFor(f, caps, prefs)) return true;
    return support.softwareAvailable() && f.height <= softwareHeightLimit(prefs);
}

std::vector<StreamCandidate> selectStreams(const SelectionInput &in, const DecoderCapabilities &caps,
                                           const PlaybackPreferences &prefs, int requestedHeight,
                                           bool requestHighFrameRate) {
    if (requestedHeight > 0 && requestHighFrameRate) {
        // Core's quality API: the >30 fps rung of that height is an explicit choice.
        QualityChoice choice;
        choice.height = requestedHeight;
        choice.highFrameRate = true;
        // Without such a rung (or with >30 fps disallowed) the height stays the cap.
        return select(in, caps, prefs, choice, requestedHeight);
    }
    const int ceiling = requestedHeight > 0 ? requestedHeight : qualityCeiling(caps, prefs);
    return select(in, caps, prefs, QualityChoice(), ceiling);
}

std::vector<StreamCandidate> selectStreams(const SelectionInput &in, const DecoderCapabilities &caps,
                                           const PlaybackPreferences &prefs, const QualityChoice &choice) {
    return select(in, caps, prefs, choice, qualityCeiling(caps, prefs));
}

std::vector<QualityOption> qualityOptions(const SelectionInput &in, const DecoderCapabilities &caps,
                                          const PlaybackPreferences &prefs) {
    const Pools p = classify(in, caps, prefs);
    if (!dualPossible(in, p, prefs)) return std::vector<QualityOption>();
    return options(p, caps, prefs);
}

QualityMenu qualityMenu(const SelectionInput &in, const DecoderCapabilities &caps, const PlaybackPreferences &prefs,
                        const StreamCandidate &playing) {
    QualityMenu menu;
    if (playing.kind != StreamKind::Adaptive) return menu;
    const Pools p = classify(in, caps, prefs);
    if (!dualPossible(in, p, prefs)) return menu;
    menu.entries = options(p, caps, prefs);
    menu.playingIndex = indexOfFormat(menu.entries, playing.video.id);
    if (const MediaFormat *a = pickAuto(p.videoOnly, qualityCeiling(caps, prefs), caps, prefs))
        menu.autoIndex = indexOfFormat(menu.entries, a->id);
    return menu;
}

std::vector<MediaFormat> playableAudioFormats(const SelectionInput &in, const DecoderCapabilities &caps) {
    return classify(in, caps, PlaybackPreferences()).audio;
}

std::vector<AudioTrackOption> audioTrackOptions(const SelectionInput &in, const DecoderCapabilities &caps) {
    return audioTrackOptions(playableAudioFormats(in, caps));
}

EmptySelection emptySelectionKind(const SelectionInput &in, const DecoderCapabilities &caps,
                                  const PlaybackPreferences &prefs) {
    if (in.liveStatus == LiveStatus::Upcoming) return EmptySelection::Upcoming;
    if (stillProcessing(in.liveStatus, in.formats)) return EmptySelection::StillProcessing;
    if (in.formats.empty()) return EmptySelection::NoFormats;
    bool anyVideo = false, anyDecodable = false;
    for (const MediaFormat &f : in.formats) {
        if (!f.hasVideo()) continue;
        anyVideo = true;
        if (videoDecodable(f, caps, prefs)) anyDecodable = true;
    }
    if (prefs.mode == PlaybackMode::Video && anyVideo && !anyDecodable) return EmptySelection::NoDecodableVideo;
    return EmptySelection::NoCompatibleStreams;
}

std::string emptySelectionReason(const SelectionInput &in, const DecoderCapabilities &caps,
                                 const PlaybackPreferences &prefs) {
    switch (emptySelectionKind(in, caps, prefs)) {
    case EmptySelection::Upcoming: return "This live event has not started yet.";
    case EmptySelection::StillProcessing: return "This live recording is still being processed. Try again later.";
    case EmptySelection::NoFormats: return "The provider offered no playable formats.";
    case EmptySelection::NoDecodableVideo: return "None of the offered video codecs can be decoded on this device.";
    case EmptySelection::NoCompatibleStreams: break;
    }
    return "No compatible stream combination was offered.";
}

DecoderPlan decoderPlanFor(const SelectionInput &in, const DecoderCapabilities &caps,
                           const PlaybackPreferences &prefs) {
    DecoderPlan plan;
    plan.capabilities = caps;
    plan.preferences = prefs;
    if (prefs.mode != PlaybackMode::Video || hardwareDecodeActive(caps, prefs)) return plan;
    if (in.liveStatus == LiveStatus::Upcoming) return plan;
    if (!selectStreams(in, caps, prefs, QualityChoice()).empty()) return plan;
    const bool anyHardware = caps.h264.hardware || caps.vp9.hardware || caps.hevc.hardware
        || caps.vp8.hardware || caps.av1.hardware;
    if (!anyHardware) return plan;
    DecoderPlan forced = plan;
    forced.preferences.hardwareDecode = true;
    forced.capabilities.hardwareDecodeVerified = true;
    if (selectStreams(in, forced.capabilities, forced.preferences, QualityChoice()).empty()) return plan;
    forced.hardwareFallback = true;
    return forced;
}

std::vector<StreamCandidate> selectStreamsWithFallback(const SelectionInput &in, const DecoderCapabilities &caps,
                                                       const PlaybackPreferences &prefs, const QualityChoice &choice) {
    const DecoderPlan plan = decoderPlanFor(in, caps, prefs);
    std::vector<StreamCandidate> chain = selectStreams(in, plan.capabilities, plan.preferences, choice);
    for (StreamCandidate &c : chain) c.hardwareDecodeFallback = plan.hardwareFallback;
    return chain;
}

}}}
