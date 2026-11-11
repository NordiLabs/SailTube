// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Small vocabulary shared by every player policy in Core/Player/Domain.
// Header-only, Qt-free, std library only. Keep it minimal and stable: other
// components (pipeline plan, engine) include it.
//
// Time units: policies use milliseconds (int64_t). Nanoseconds appear only in
// SeekCommand::positionNs (SeekPolicy.h), because that is what
// gst_event_new_seek() takes; use msToNs()/nsToMs() at the boundary.

#include <cstdint>
#include <string>

namespace nordtile { namespace player {

// PLY-01: the one player plays every mode.
enum class PlayMode {
    Dual,      // separate video + audio sources (DASH), one pipeline clock
    Muxed,     // one progressive source carrying video + audio
    Hls,       // one HLS source (live or VOD), demuxer carries video + audio
    Local,     // one local file (download)
    AudioOnly  // one audio source, no video branch (listen-only, SEL-08)
};

// Demuxer family of a branch. Decides seek flags (PLY-03) and whether the
// branch needs a byte-seekable, disk-backed upstream (PLY-04).
enum class Container {
    Matroska,  // webm / weba / mkv  -> matroskademux
    IsoBmff,   // mp4 / m4a / m4v / 3gp / mov -> qtdemux
    Unknown
};

// Maps a file extension or yt-dlp "ext" (case-insensitive, optional leading
// dot) to its container family. Anything else is Unknown.
inline Container containerFromExtension(const std::string &extension) {
    std::string e;
    e.reserve(extension.size());
    for (std::string::size_type i = 0; i < extension.size(); ++i) {
        char c = extension[i];
        if (i == 0 && c == '.') continue;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        e += c;
    }
    if (e == "webm" || e == "weba" || e == "mkv" || e == "mka") return Container::Matroska;
    if (e == "mp4" || e == "m4a" || e == "m4v" || e == "3gp" || e == "mov") return Container::IsoBmff;
    return Container::Unknown;
}

// Which sink a command targets. Muxed = the single source's pipeline/sinks
// (Muxed, Hls, Local modes); Video / Audio = that branch's sink in Dual mode,
// Audio also for AudioOnly.
enum class Branch { Video, Audio, Muxed };

// Seek flags as a tiny bitmask struct; maps 1:1 to GST_SEEK_FLAG_FLUSH,
// GST_SEEK_FLAG_ACCURATE and GST_SEEK_FLAG_KEY_UNIT.
struct SeekFlags {
    bool flush = false;
    bool accurate = false;
    bool keyUnit = false;

    static SeekFlags flushAccurate() { SeekFlags f; f.flush = true; f.accurate = true; return f; }
    static SeekFlags flushKeyUnit() { SeekFlags f; f.flush = true; f.keyUnit = true; return f; }

    bool operator==(const SeekFlags &o) const {
        return flush == o.flush && accurate == o.accurate && keyUnit == o.keyUnit;
    }
    bool operator!=(const SeekFlags &o) const { return !(*this == o); }
};

inline int64_t msToNs(int64_t ms) { return ms * 1000000; }
inline int64_t nsToMs(int64_t ns) { return ns / 1000000; }

} } // namespace nordtile::player
