// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// MPR-01 MPRIS (Amber.Mpris) and MPR-02 cover decisions. Qt-free; the QML
// MprisPlayer and CoverBackground bind to these values.

#include <cstdint>
#include <string>

namespace nordtile { namespace player {

// MPRIS metadata keys (MPRIS 2 spec). xesam:artist is a string list on the
// bus; it carries exactly one entry, the channel.
const char *const kMprisTitleKey = "xesam:title";
const char *const kMprisArtistKey = "xesam:artist";
const char *const kMprisArtUrlKey = "mpris:artUrl";
const char *const kMprisLengthKey = "mpris:length"; // microseconds (int64)

struct MediaControlsState {
    bool hasSession = false;    // a video is loaded (playing, paused or parked)
    bool playing = false;
    bool seekable = false;      // the engine reports a seekable range
    bool live = false;          // live stream (not seekable from MPRIS)
    bool canGoNext = false;     // AutoplayPolicy::canGoNext()
    bool canGoPrevious = false; // AutoplayPolicy::canGoPrevious()
    bool hasResumable = false;  // a last video with a stored resume point
};

struct MprisCapabilities {
    bool canControl = false;
    bool canPlay = false;
    bool canPause = false;
    bool canSeek = false;       // SetPosition and Seek
    bool canGoNext = false;
    bool canGoPrevious = false;
};

// MPR-01: CanControl always; CanPlay/CanPause with a session (CanPlay also
// with a resumable last video); CanSeek when seekable and not live;
// CanGoNext/CanGoPrevious only with a queue.
MprisCapabilities mprisCapabilities(const MediaControlsState &state);

// MPR-01: "Stop really stops". Execute in field order.
struct StopActions {
    bool savePosition = false;      // store the resume point first
    bool endSession = false;        // SessionArbiter::stopSession(), pipeline to NULL
    bool releaseStreamJobs = false; // cancel resolve/prefetch/download-proxy jobs of the stream
    bool closeMiniPlayer = false;
    bool releaseAudioResource = false; // AudioResourcePolicy::stop()
};
StopActions mprisStopActions(bool hasSession);

struct TrackInfo {
    std::string title;
    std::string channel;
    std::string thumbnailPath; // local cached thumbnail; the caller checks it exists
    std::int64_t durationMs = 0; // <= 0 unknown
    bool live = false;
};

struct MprisMetadata {
    std::string title;
    std::string artist;            // = channel
    std::string artUrl;            // file:// URL or "" (never a remote URL)
    bool hasLength = false;        // false for live / unknown duration: omit the key
    std::int64_t lengthUs = 0;
};

// MPR-01 metadata map values.
MprisMetadata mprisMetadata(const TrackInfo &track);

// MPR-01: artUrl only for a LOCAL file. Accepts an absolute path or a
// file:// URL; anything else (http, https, data, relative paths) gives "".
// The path is percent-encoded outside [A-Za-z0-9-._~/].
std::string localArtUrl(const std::string &pathOrFileUrl);

enum class CoverAction { None, PlayPause, Next, Stop, ContinueWatching };
enum class CoverMode { Identity, ContinueWatching, Session };

struct CoverState {
    CoverMode mode = CoverMode::Identity;
    CoverAction left = CoverAction::None;
    CoverAction right = CoverAction::None;
};

// MPR-02: session with a queue: play/pause + next; session without one:
// play/pause + stop; idle: "continue watching" (one action) when a
// resumable last video exists, otherwise the app identity with no action.
CoverState coverState(const MediaControlsState &state);

} } // namespace nordtile::player
