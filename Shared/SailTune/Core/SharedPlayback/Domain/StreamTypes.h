// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Vocabulary of the shared playback component (Core/SharedPlayback/). Qt-free C++11.
//
// The component turns a video id into a ranked set of directly playable media addresses and
// feeds them to a player. It knows nothing about the application around it: no QML, no
// QtMultimedia, no application types. A host (SailTune today, SailTube later) supplies a
// StreamRequest and consumes a StreamSet.
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

// What the caller wants. Every field that can change the answer is part of the cache key.
struct StreamRequest {
    bool audioOnly = true;                    // false: video-only formats are ranked too
    int maxVideoHeight = 1080;                // quality ceiling for video; ignored when audioOnly
    int maxAudioKbps = 0;                     // 0: no ceiling (a ceiling never removes the last format)
    std::vector<std::string> codecPreference; // "opus", "aac", ...; empty: opus first, then aac
    std::string dubLanguage;                  // "" : the original / default audio track
    bool hardwareDecode = false;              // prefer video codecs the hardware decodes (H.264)

    std::string cacheKey(const std::string &videoId) const;
};

// One directly playable format (a plain address; ciphered ones are never kept).
struct StreamFormat {
    int itag = 0;
    std::string url;
    std::string mimeType;          // "audio/webm", "audio/mp4", "video/mp4", ...
    std::string codec;             // "opus", "mp4a.40.2", "avc1.64001F", "vp9"
    bool hasAudio = false;
    bool hasVideo = false;
    int bitrate = 0;               // bits per second as declared
    std::int64_t contentLength = 0;
    int width = 0, height = 0, fps = 0;
    std::string audioLanguage;     // "" unknown
    bool isDefaultAudio = true;    // the original / default track of the video
    bool isDrc = false;            // the "stable volume" variant
    bool isDescriptive = false;    // audio description track
    bool isHls = false;
    std::int64_t expiresAtUnixMs = 0;

    std::string container() const { return mimeType.find("webm") != std::string::npos ? "webm" : "mp4"; }
    // "opus" / "aac" / other: the family a ladder deduplicates on.
    std::string codecFamily() const;
};

enum class ResolvePath { None, Cache, Fast, YtDlp };

struct StreamTimings {
    int totalMs = -1;       // from the request to the answer
    int visitorMs = -1;     // visitor id mint (-1: not needed)
    int playerMs = -1;      // the /player round trip
    int parseMs = -1;
    int probeMs = -1;
    bool reusedConnection = false;
};

struct StreamMetadata {
    std::string title;
    std::string author;
    std::string thumbnailUrl;
    std::int64_t durationMs = 0;
    bool isLive = false;
};

// The answer. Ranked lists are best first.
struct StreamSet {
    std::string videoId;
    std::vector<StreamFormat> audio;    // audio-only, direct addresses
    std::vector<StreamFormat> video;    // video-only, direct addresses (empty for audioOnly requests)
    std::vector<StreamFormat> muxed;    // progressive audio+video, last resort (the only choice when `audio` is empty)
    // The format a host plays by default: the top audio-only rung, else the best progressive stream.
    const StreamFormat *top() const { return !audio.empty() ? &audio.front() : !muxed.empty() ? &muxed.front() : nullptr; }
    std::string hlsUrl;                 // live manifests are played as they are, never proxied
    std::int64_t expiresAtUnixMs = 0;   // earliest expiry among the kept addresses; 0: unknown
    bool hasLoudness = false;
    double loudnessDb = 0;              // positive: the track is louder than the target
    StreamMetadata metadata;
    std::string userAgent;              // the client's own user agent: the media fetch must send it
    std::string probedUrl;              // the audio address the probe accepted (a host that delivers another rung probes it itself)
    ResolvePath path = ResolvePath::None;
    StreamTimings timings;
};

// Why a resolve did not produce a StreamSet. A host reacts to `fallback`: the fast path gave no
// usable answer and the slower extractor may still succeed.
enum class FailureKind {
    None,
    BreakerOpen,      // fast path switched off after repeated refusals
    BotWall,          // the provider asked for a sign-in / bot check
    Unplayable,       // not playable anonymously (login, age, region ...); the extractor may differ
    Final,            // removed, terminated, premiere, region ... : skip, do not retry
    NoUsableFormat,   // no direct address survived
    ProbeRejected,    // the chosen address was refused with an HTTP status
    Network,          // connection or timeout
    BadResponse,      // not understood
    Cancelled
};

struct ResolveOutcome {
    FailureKind failure = FailureKind::None;
    std::string reason;     // short fixed class text for logs; never provider text
    bool fallback = false;  // a host may try its slower extractor
    StreamSet streams;
    bool ok() const { return failure == FailureKind::None; }
};

inline const char *failureLabel(FailureKind kind) {
    switch (kind) {
    case FailureKind::None: return "none";
    case FailureKind::BreakerOpen: return "breaker_open";
    case FailureKind::BotWall: return "bot_wall";
    case FailureKind::Unplayable: return "unplayable";
    case FailureKind::Final: return "final";
    case FailureKind::NoUsableFormat: return "no_usable_format";
    case FailureKind::ProbeRejected: return "probe_rejected";
    case FailureKind::Network: return "network";
    case FailureKind::BadResponse: return "bad_response";
    case FailureKind::Cancelled: return "cancelled";
    }
    return "unknown";
}

inline const char *pathLabel(ResolvePath path) {
    switch (path) {
    case ResolvePath::None: return "none";
    case ResolvePath::Cache: return "cache";
    case ResolvePath::Fast: return "fast";
    case ResolvePath::YtDlp: return "ytdlp";
    }
    return "unknown";
}

inline std::string StreamFormat::codecFamily() const {
    if (codec.compare(0, 4, "opus") == 0) return "opus";
    if (codec.compare(0, 4, "mp4a") == 0 || codec == "aac") return "aac";
    if (codec.compare(0, 4, "avc1") == 0 || codec == "h264") return "h264";
    if (codec.compare(0, 3, "vp9") == 0 || codec.compare(0, 3, "vp0") == 0) return "vp9";
    if (codec.compare(0, 4, "av01") == 0) return "av1";
    return codec;
}

inline std::string StreamRequest::cacheKey(const std::string &videoId) const {
    std::string key = videoId;
    key += audioOnly ? "|a" : "|v";
    key += "|h" + std::to_string(audioOnly ? 0 : maxVideoHeight);
    key += "|k" + std::to_string(maxAudioKbps);
    key += "|c";
    for (std::size_t i = 0; i < codecPreference.size(); ++i) { if (i) key += ','; key += codecPreference[i]; }
    key += "|d" + dubLanguage;
    key += hardwareDecode ? "|hw" : "|sw";
    return key;
}

}}}}
