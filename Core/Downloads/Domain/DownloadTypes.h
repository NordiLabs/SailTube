// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free records of the download queue (SAILTUBE_REQUIREMENTS §3.9). One
// DownloadRecord mirrors one row of library.db `downloads` (LibrarySchema.h).
// The integer values of the enums are persisted: append, never renumber.

#include <cstdint>
#include <string>

namespace nordtile { namespace video { namespace downloads {

enum class Kind { Audio = 0, Video = 1 };

enum class State {
    Queued = 0,
    Resolving = 1,
    Downloading = 2,
    Paused = 3,            // by the user; parts are kept
    WaitingForNetwork = 4, // "unmetered only" is on and the connection is metered
    Remuxing = 5,
    Done = 6,
    Failed = 7,
    Cancelled = 8          // transient: a cancelled row is removed with its parts
};

enum class OutputLayout {
    SingleFile = 0, // audio track or progressive (muxed) stream as downloaded
    MuxedFile = 1,  // video + audio tracks remuxed into one file (DL-03)
    TrackPair = 2   // no muxer: the two tracks stay as one logical item
};

enum class Track { Video = 0, Audio = 1 };

struct DownloadRecord {
    std::int64_t id = 0;
    std::string videoId;
    Kind kind = Kind::Audio;
    int qualityHeight = 0;           // video: picture ceiling chosen for this download
    State state = State::Queued;
    std::string title;
    std::string channel;
    std::string channelId;
    std::int64_t durationSeconds = -1;
    std::string thumbnailUrl;
    std::string directory;           // absolute, decided when the transfer starts (DL-06)
    std::string fileName;            // the playable file once Done (TrackPair: the video track)
    std::string videoFormatId;       // Video kind (or progressive stream)
    std::string audioFormatId;       // Audio kind, or the audio track of a pair
    std::string videoContainer;
    std::string audioContainer;
    std::string videoCodec;          // RFC 6381 tags, diagnostics and the local player
    std::string audioCodec;
    std::int64_t videoBytesTotal = 0; // 0 = unknown
    std::int64_t videoBytesDone = 0;
    std::int64_t audioBytesTotal = 0;
    std::int64_t audioBytesDone = 0;
    OutputLayout layout = OutputLayout::SingleFile;
    std::string error;               // short code (see DownloadPolicy.h errorCodes), empty when fine
    std::string notice;              // text key of the last notice (location fallback, remux fallback)
    // Kept in the `extras` JSON column:
    std::string baseName;            // "<title> [<id>]", fixed once the first part file exists
    std::string thumbnailFile;       // "<base>.jpg" when saved (DL-07), else empty
    int restarts = 0;                // restarts from zero (416, part longer than the total)
    std::string muxExtension;        // planned output extension of a pair ("mp4", "webm")
    std::string extras = "{}";       // the raw column, unknown keys preserved by the store
    std::int64_t createdMs = 0;
    std::int64_t updatedMs = 0;
    std::int64_t sortKey = 0;        // lower runs first
};

// Bytes over both tracks of a video download.
struct Progress {
    std::int64_t done = 0;
    std::int64_t total = 0; // 0 unknown
    double fraction() const {
        if (total <= 0) return 0;
        const double f = static_cast<double>(done) / static_cast<double>(total);
        return f < 0 ? 0 : (f > 1 ? 1 : f);
    }
};

inline Progress progressOf(const DownloadRecord &r) {
    Progress p;
    p.done = r.videoBytesDone + r.audioBytesDone;
    p.total = r.videoBytesTotal + r.audioBytesTotal;
    if (r.state == State::Done && p.total <= 0) { p.total = 1; p.done = 1; }
    return p;
}

inline const char *stateName(State s) {
    switch (s) {
    case State::Queued: return "queued";
    case State::Resolving: return "resolving";
    case State::Downloading: return "downloading";
    case State::Paused: return "paused";
    case State::WaitingForNetwork: return "waiting_for_network";
    case State::Remuxing: return "remuxing";
    case State::Done: return "done";
    case State::Failed: return "failed";
    case State::Cancelled: return "cancelled";
    }
    return "unknown";
}

}}}
