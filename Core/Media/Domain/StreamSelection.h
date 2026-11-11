// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Pure stream-selection policy for SailTube's own GStreamer player: which of
// the offered formats this device can play, and in which order to try them.
// No I/O; unit-tested on the host (tests/domain).
//
// Play plans (FINTUBE_SPEC 4.3 as adopted by SAILTUBE_REQUIREMENTS 3.2):
//   dual  (StreamKind::Adaptive)    video-only URL + audio-only URL, two GStreamer
//                                   branches on one clock, WebM or MP4 per branch;
//   muxed (StreamKind::Progressive) one muxed URL (itag 18 style), the fallback;
//   live  (StreamKind::Live)        muxed HLS, never probed or proxied (RES-19);
//   audio (StreamKind::AudioOnly)   listen-only mode (SEL-08).

#include "AudioTrackPolicy.h"
#include "MediaFormat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace media {

struct CodecSupport {
    bool available = false; // any decoder element (hardware or software)
    bool hardware = false;  // droidvdec claims the caps (not yet verified to work)
    bool software = false;  // a software decoder element is present (avdec_h264, vp9dec, ...)

    // A software decoder exists: set explicitly, or implied when the only
    // decoder is not hardware. With hardware set and software unset, the
    // policy assumes there is none (e.g. no software H.264 on the device).
    bool softwareAvailable() const { return software || (available && !hardware); }
};

// What the device's GStreamer installation can decode and demux, as probed at
// startup (GstCapabilityProbe) or assumed when probing is impossible.
struct DecoderCapabilities {
    CodecSupport h264, hevc, vp8, vp9, av1;
    bool aac = true;
    bool opus = true;
    bool vorbis = true;
    bool mp4Demux = true;
    bool webmDemux = true;
    // Legacy: a DASH demuxer reachable from the Qt Multimedia pipeline. Dual
    // playback needs no manifest, so selection no longer reads this field.
    bool adaptive = false;
    bool hls = true;
    // PLY-20: the per-device hardware probe decoded the bundled clip with
    // frames within 2 s. Without it, hardware flags above are never used.
    bool hardwareDecodeVerified = false;
    // SEL-01: a runtime self-test decoded the bundled 2 s AV1 clip in real
    // time. An available AV1 element alone is not enough.
    bool av1SelfTestPassed = false;

    CodecSupport video(VideoCodec codec) const;
    bool audio(AudioCodec codec) const;
};

enum class PlaybackMode { Video, AudioOnly };
enum class CodecPreference { Automatic, H264, Vp9 };
// SEL-02: the network class the next selection is made for.
enum class NetworkClass { Unmetered, Mobile };

struct PlaybackPreferences {
    PlaybackMode mode = PlaybackMode::Video; // AudioOnly = SEL-08 listen-only toggle
    // SEL-02 unmetered ceiling. 0 = the default for the decode mode: 720p with
    // software decode, 1080p with verified hardware decode. A very large value
    // (for example INT_MAX) means "best".
    int maxHeight = 0;
    int mobileMaxHeight = 480;               // SEL-02 ceiling on mobile data; 0 = 480
    NetworkClass network = NetworkClass::Unmetered;
    bool allowAdaptive = true;               // dual video + audio branches; false = muxed only
    bool allowHighFrameRate = true;          // false hides >30 fps rungs even as explicit choices
    CodecPreference codec = CodecPreference::Automatic;
    // SEL-01: tallest picture for a software decoder, never above 1080.
    int softwareDecodeMaxHeight = 1080;
    // PLY-20 / PLY-21: hardware decode setting (off by default). Effective only
    // with DecoderCapabilities::hardwareDecodeVerified.
    bool hardwareDecode = false;
    // SEL-01: tallest picture for verified hardware decode; 1440p+ is opt-in.
    int hardwareDecodeMaxHeight = 1080;
    // SEL-05: remembered dub language (BCP-47); empty = no preference (original).
    std::string audioLanguage;
    // PLY-06 deep start (resume far into a video): at equal height prefer a WebM rung (its Cues
    // index lets the start seek jump in one range request) over fragmented MP4 without an index
    // (qtdemux walks moof by moof). Software decode with the Automatic codec only.
    bool preferSeekIndex = false;
};

enum class StreamKind { None, Progressive, Adaptive, Live, AudioOnly };

struct StreamCandidate {
    StreamKind kind = StreamKind::None;
    MediaFormat video;   // Adaptive: video-only format
    MediaFormat audio;   // Adaptive / AudioOnly: audio-only format
    MediaFormat muxed;   // Progressive / Live / AudioOnly last resort: single source
    int height = 0;
    std::string label;   // "1080p VP9 + Opus (dual)", for UI and diagnostics
    // PLY-04: container of each branch, so the player sets its container
    // hints before the URLs. None for an absent branch.
    Container videoContainer = Container::None;
    Container audioContainer = Container::None;
    Container muxedContainer = Container::None;
    // RES-19: HLS is never probed or proxied; its manifest GET proves access.
    bool bypassProxyAndProbe = false;
    // Interim decoder rule (decision log 2026-11-08): no software-decodable
    // format, so this item is played with the hardware decoder (diagnostics).
    bool hardwareDecodeFallback = false;
};

struct SelectionInput {
    std::vector<MediaFormat> formats;
    LiveStatus liveStatus = LiveStatus::NotLive;
    std::string manifestUrl;              // HLS master manifest for live streams, when supplied
    std::int64_t scheduledStartUnix = 0;  // RES-19: upcoming start (yt-dlp release_timestamp), 0 unknown
};

// SEL-03: one quality-menu entry per (height, high-fps, Premium).
struct QualityOption {
    int height = 0;
    double fps = 0;
    VideoCodec codec = VideoCodec::Unknown; // codec of formatId
    StreamKind kind = StreamKind::None;
    std::string label;                      // "720p", "1080p60", "1080p Premium"
    bool highFrameRate = false;
    bool premium = false;
    std::string formatId;                   // the format this entry requests
    std::vector<std::string> formatIds;     // every member format (all codecs), for highlighting
};

// SEL-03: the menu shown for the current play plan.
struct QualityMenu {
    std::vector<QualityOption> entries; // empty for muxed, live and listen-only plans
    int autoIndex = -1;                 // entry the "Auto" (SEL-02 ceiling) rule plays
    int playingIndex = -1;              // entry holding the playing video format id
};

// SEL-03: an explicit quality choice. With formatId set and playable, that
// format is used; otherwise the best member of (height, highFrameRate,
// premium). An empty choice (height 0) means Auto.
struct QualityChoice {
    int height = 0;
    bool highFrameRate = false;
    bool premium = false;
    std::string formatId;
};
QualityChoice qualityChoice(const QualityOption &);

// SEL-02: the Auto ceiling for the current network class and decode mode.
int qualityCeiling(const DecoderCapabilities &, const PlaybackPreferences &);
// PLY-20: hardware decode is on and verified on this device.
bool hardwareDecodeActive(const DecoderCapabilities &, const PlaybackPreferences &);

// Ordered attempts: the first entry is the best choice, later entries are
// fallbacks after a playback failure. Empty when nothing is playable (an
// upcoming video always); emptySelectionReason() explains why.
// SEL-01/02/04/05/08, RES-19. requestedHeight > 0 replaces the Auto ceiling;
// with requestHighFrameRate it picks the >30 fps rung of that height (the
// Auto rule never picks one). Kept for core's PlaybackSession quality API.
std::vector<StreamCandidate> selectStreams(const SelectionInput &, const DecoderCapabilities &,
                                           const PlaybackPreferences &, int requestedHeight = 0,
                                           bool requestHighFrameRate = false);
// SEL-03: the same chain for an explicit menu choice (Auto when choice.height is 0).
std::vector<StreamCandidate> selectStreams(const SelectionInput &, const DecoderCapabilities &,
                                           const PlaybackPreferences &, const QualityChoice &);

// SEL-03: menu entries ordered by height (tallest first), then Premium first,
// then higher fps first. Empty when no dual plan is possible (muxed only),
// for live and upcoming videos and in listen-only mode.
std::vector<QualityOption> qualityOptions(const SelectionInput &, const DecoderCapabilities &,
                                          const PlaybackPreferences &);
// SEL-03: entries plus the Auto and playing indexes; empty unless the
// playing candidate is a dual plan (a muxed switch would drop the audio).
QualityMenu qualityMenu(const SelectionInput &, const DecoderCapabilities &, const PlaybackPreferences &,
                        const StreamCandidate &playing);

// SEL-04: audio-only formats this device can play (Opus in WebM, AAC in MP4,
// direct HTTPS), unranked.
std::vector<MediaFormat> playableAudioFormats(const SelectionInput &, const DecoderCapabilities &);
// SEL-05: dub-picker entries for this video (see audioTrackOptions()).
std::vector<AudioTrackOption> audioTrackOptions(const SelectionInput &, const DecoderCapabilities &);

// RES-09 / RES-19: why a selection came back empty.
enum class EmptySelection { Upcoming, StillProcessing, NoFormats, NoDecodableVideo, NoCompatibleStreams };
EmptySelection emptySelectionKind(const SelectionInput &, const DecoderCapabilities &,
                                  const PlaybackPreferences &);
// Static English explanation for an empty selection (diagnostics / fallback text).
std::string emptySelectionReason(const SelectionInput &, const DecoderCapabilities &,
                                 const PlaybackPreferences &);

// Interim decoder rule (decided 2026-11-08), until E3 has a
// device answer: with hardware decoding off, software is preferred; when an
// item offers no format a present software decoder can play (H.264-only
// videos, the itag 18 fallback) but a hardware decoder exists for one, that
// item uses the hardware decoder instead of failing. Not a new default.
struct DecoderPlan {
    DecoderCapabilities capabilities;  // use these for selection and the quality menu
    PlaybackPreferences preferences;
    bool hardwareFallback = false;     // the item is played with hardware decode
};
DecoderPlan decoderPlanFor(const SelectionInput &, const DecoderCapabilities &, const PlaybackPreferences &);
// selectStreams() under decoderPlanFor(); candidates carry hardwareDecodeFallback.
std::vector<StreamCandidate> selectStreamsWithFallback(const SelectionInput &, const DecoderCapabilities &,
                                                       const PlaybackPreferences &, const QualityChoice &);

// SEL-01: H.264 or VP9 (AV1 only after its self-test), SDR, within the height
// limit of the decoder that will run, and not >30 fps when that is disallowed.
bool videoDecodable(const MediaFormat &, const DecoderCapabilities &, const PlaybackPreferences &);

}}}
