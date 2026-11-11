// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free description of one media format offered for a video. Built by the
// resolver adapter from yt-dlp's documented format fields; consumed by the
// selection policy. URLs and headers are ephemeral: never persist or log them.

#include <cstdint>
#include <map>
#include <string>

namespace nordtile { namespace video { namespace media {

enum class Protocol { Https, Hls, Dash, Other };
enum class VideoCodec { None, H264, Hevc, Vp8, Vp9, Av1, Unknown };
enum class AudioCodec { None, Aac, Opus, Vorbis, Unknown };
// Container family of one stream, used for the player's container hints
// (PLY-04: set before the URL on a dual branch, cleared in muxed mode).
enum class Container { None, Mp4, WebM, Hls, Other };
// yt-dlp "live_status". Lives here so the Qt-free live policy can use it.
enum class LiveStatus { NotLive, IsLive, WasLive, PostLive, Upcoming, Unknown };

struct MediaFormat {
    std::string id;            // provider format id (itag), display/diagnostics only
    std::string container;     // yt-dlp "ext": mp4, webm, m4a, ...
    Protocol protocol = Protocol::Other;
    std::string url;
    std::map<std::string, std::string> headers;
    VideoCodec videoCodec = VideoCodec::Unknown;
    AudioCodec audioCodec = AudioCodec::Unknown;
    std::string videoCodecTag; // e.g. "avc1.640028" (RFC 6381 string when supplied)
    std::string audioCodecTag; // e.g. "mp4a.40.2"
    int width = 0;
    int height = 0;
    double fps = 0;
    double totalKbps = 0;
    double audioKbps = 0;
    int audioSampleRate = 0;
    std::int64_t fileSize = 0; // exact size when known, else 0
    std::string language;
    int languagePreference = -1; // higher is preferred; yt-dlp marks the original track
    bool dynamicRangeCompressed = false;
    bool highDynamicRange = false;
    // SEL-03: a "Premium" (enhanced bitrate) rung. The resolver adapter sets it
    // when yt-dlp's format_note contains "Premium" (see premiumFormatNote()).
    // Premium rungs are only ever explicit menu choices, never Auto.
    bool premium = false;
    std::int64_t availableAtUnixSeconds = 0; // provider pre-playback wait, 0 = none

    bool hasVideo() const { return videoCodec != VideoCodec::None; }
    bool hasAudio() const { return audioCodec != AudioCodec::None; }
    bool muxed() const { return hasVideo() && hasAudio(); }
    bool highFrameRate() const { return fps > 30.5; }
};

// SEL-03: yt-dlp marks enhanced-bitrate rungs with "Premium" in format_note.
inline bool premiumFormatNote(const std::string &formatNote) {
    return formatNote.find("Premium") != std::string::npos || formatNote.find("premium") != std::string::npos;
}

// Container family from yt-dlp's "ext" and protocol. HLS wins over the ext.
inline Container containerOf(const MediaFormat &f) {
    if (f.protocol == Protocol::Hls) return Container::Hls;
    const std::string &c = f.container;
    if (c == "mp4" || c == "m4a" || c == "m4v" || c == "3gp") return Container::Mp4;
    if (c == "webm" || c == "weba" || c == "mkv") return Container::WebM;
    return Container::Other;
}

inline const char *containerName(Container c) {
    switch (c) {
    case Container::None: return "none";
    case Container::Mp4: return "mp4";
    case Container::WebM: return "webm";
    case Container::Hls: return "hls";
    case Container::Other: break;
    }
    return "other";
}

// RFC 6381 / yt-dlp codec strings to codec families. "none" means absent;
// an empty or unrecognised string is Unknown (never treated as playable).
inline bool startsWith(const std::string &s, const char *prefix) {
    return s.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

inline VideoCodec parseVideoCodec(const std::string &tag) {
    if (tag == "none") return VideoCodec::None;
    if (startsWith(tag, "avc1") || startsWith(tag, "avc3") || tag == "h264") return VideoCodec::H264;
    if (startsWith(tag, "hev1") || startsWith(tag, "hvc1") || tag == "h265") return VideoCodec::Hevc;
    if (startsWith(tag, "vp09") || tag == "vp9" || startsWith(tag, "vp9.")) return VideoCodec::Vp9;
    if (startsWith(tag, "vp08") || tag == "vp8") return VideoCodec::Vp8;
    if (startsWith(tag, "av01") || tag == "av1") return VideoCodec::Av1;
    return VideoCodec::Unknown;
}

inline AudioCodec parseAudioCodec(const std::string &tag) {
    if (tag == "none") return AudioCodec::None;
    if (startsWith(tag, "mp4a") || tag == "aac") return AudioCodec::Aac;
    if (tag == "opus") return AudioCodec::Opus;
    if (tag == "vorbis") return AudioCodec::Vorbis;
    return AudioCodec::Unknown;
}

inline const char *videoCodecName(VideoCodec c) {
    switch (c) {
    case VideoCodec::None: return "none";
    case VideoCodec::H264: return "H.264";
    case VideoCodec::Hevc: return "HEVC";
    case VideoCodec::Vp8: return "VP8";
    case VideoCodec::Vp9: return "VP9";
    case VideoCodec::Av1: return "AV1";
    case VideoCodec::Unknown: break;
    }
    return "unknown";
}

inline const char *audioCodecName(AudioCodec c) {
    switch (c) {
    case AudioCodec::None: return "none";
    case AudioCodec::Aac: return "AAC";
    case AudioCodec::Opus: return "Opus";
    case AudioCodec::Vorbis: return "Vorbis";
    case AudioCodec::Unknown: break;
    }
    return "unknown";
}

}}}
