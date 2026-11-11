// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Pure download decisions (SAILTUBE_REQUIREMENTS §3.9): format picks (DL-01,
// DL-03), the state machine, the 2-transfer scheduler with the unmetered gate
// (DL-02), Range resume decisions, the location fallback (DL-06) and
// "download all" (DL-08). No I/O; unit-tested on the host.

#include "DownloadTypes.h"
#include "../../Media/Domain/MediaFormat.h"
#include "../../Media/Domain/StreamSelection.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace downloads {

// Error codes stored in DownloadRecord::error (presentation maps them to text).
// Resolver failures are stored as "resolve_<errorLabel>".
namespace errorCodes {
const char *const noStream = "no_stream";          // nothing downloadable for this kind
const char *const sizeMismatch = "size_mismatch";  // re-resolved size differs / final size wrong
const char *const forbidden = "forbidden";         // 403/410 after every re-resolve
const char *const http = "http";                   // other HTTP failure
const char *const network = "network";             // transport failures exhausted their retries
const char *const write = "write";                 // the part file could not be written
const char *const fileMissing = "file_missing";    // a Done item lost its file(s)
const char *const location = "location";           // no writable directory at all
const char *const hostRefused = "host_refused";    // URL outside the allowlist (NFR-SEC-05)
const char *const notMedia = "not_media";          // the server answered with a page, not media (NFR-SEC-05)
const char *const noSpace = "no_space";            // not enough free space for the rest of the download
}

// Text keys of notices (UI translates them).
namespace noticeKeys {
const char *const folderUnavailable = "downloads_notice_folder_unavailable"; // DL-06 fallback
const char *const remuxFailed = "downloads_notice_remux_failed";             // kept as a track pair
}

// ---- DL-01 / DL-03 picks ----------------------------------------------------

// What the device can remux into (from Remuxer::availability(), Qt-free copy).
struct MuxCapabilities {
    bool mp4 = false;
    bool webm = false;
};

enum class MuxTarget { None, Mp4, WebM };

struct AudioPick {
    bool ok = false;
    media::MediaFormat format;
    std::string extension; // "m4a" for AAC; "webm" for WebM/Opus (kept as downloaded)
};

struct VideoPick {
    bool ok = false;
    bool progressive = false;  // a muxed stream: one file, no audio track
    media::MediaFormat video;  // video-only track, or the progressive stream
    media::MediaFormat audio;  // empty when progressive
    OutputLayout layout = OutputLayout::SingleFile;
    MuxTarget mux = MuxTarget::None;
    std::string videoExtension; // track / progressive file extension
    std::string audioExtension;
    std::string outputExtension; // mp4 / webm: the muxed file (or progressive file)
};

// itag 140 (AAC m4a) first, then the best AAC m4a, then the best Opus in WebM.
// The original-language track and non-DRC variants win over dubs and DRC.
AudioPick pickAudio(const std::vector<media::MediaFormat> &formats);

// requested <= 0 means "the default height" (downloadDefaultFor, passed in by the caller).
int effectiveQualityHeight(int requested, int playbackCeiling);

// What a download starts as when the user picks nothing: the "Download quality" setting
// (downloads/quality: 1080, 720, 480, 360, or -1 for audio only).
struct DownloadDefault {
    Kind kind = Kind::Video;
    int qualityHeight = 0;   // for a video download; also what the dialog shows if the user switches to video
};
const int downloadQualityAudioOnly = -1;
const int downloadQualityFallbackHeight = 720;   // the setting's own default
// A height is used as it is (the setting is the user's explicit choice for downloads, so the playback
// ceiling does not cap it). Audio only keeps a video height for the dialog: the playback ceiling
// (SEL-02) when one is set, else 720. Any other value (0, unknown) is the old rule: the playback
// ceiling, 2160 without one.
DownloadDefault downloadDefaultFor(int qualitySetting, int playbackCeiling);

// Best video-only track at or under maxHeight (0 = no cap) in a pair the
// device plays: H.264 in MP4 with AAC, or VP9 in WebM with Opus. Never AV1 or
// HDR. Ranking: height, then a pair the device can remux, then hardware decode,
// then H.264, then frame rate and bitrate. Falls back to the best progressive
// stream under the cap when no pair exists.
VideoPick pickVideo(const std::vector<media::MediaFormat> &formats, int maxHeight,
                    const media::DecoderCapabilities &decoders, const MuxCapabilities &mux);

// Distinct downloadable heights, tallest first (the per-download picker).
std::vector<int> videoQualityHeights(const std::vector<media::MediaFormat> &formats,
                                     const media::DecoderCapabilities &decoders);

MuxTarget muxTargetFor(media::VideoCodec video, media::AudioCodec audio);
OutputLayout outputLayoutFor(MuxTarget target, const MuxCapabilities &mux);

// ---- state machine ------------------------------------------------------------

enum class Event {
    Start,        // the scheduler picked it: Queued -> Resolving
    Resolved,     // formats known: Resolving -> Downloading
    TracksDone,   // all bytes on disk, a pair to remux: Downloading -> Remuxing
    Finished,     // Downloading/Remuxing -> Done
    Pause,        // Queued/Resolving/Downloading/WaitingForNetwork -> Paused
    Resume,       // Paused -> Queued
    Cancel,       // any state but Done -> Cancelled
    Fail,         // Resolving/Downloading/Remuxing -> Failed
    Retry,        // Failed/Cancelled -> Queued
    NetworkGated, // unmetered gate closed: Queued/Resolving/Downloading -> WaitingForNetwork
    NetworkOpen,  // WaitingForNetwork -> Queued
    Recover       // app start: Resolving/Downloading/Remuxing -> Queued, others unchanged
};

struct Transition {
    bool allowed = false;
    State next = State::Queued;
};

Transition transition(State current, Event event);

bool isTransfer(State s);    // Resolving or Downloading (counts against the 2-transfer limit)
bool isUnfinished(State s);  // not Done/Failed/Cancelled
bool wantsKeepAlive(State s); // Queued, Resolving, Downloading, Remuxing (DL-09)

// ---- scheduler ----------------------------------------------------------------

struct ScheduleItem {
    std::int64_t id = 0;
    State state = State::Queued;
    std::int64_t sortKey = 0;
};

struct ScheduleDecision {
    std::vector<std::int64_t> start;       // Queued -> Resolving, in sort order
    std::vector<std::int64_t> gate;        // -> WaitingForNetwork (stop transfers, keep parts)
    std::vector<std::int64_t> ungate;      // WaitingForNetwork -> Queued
};

const int maxConcurrentTransfers = 2;

// networkAllowed = !unmeteredOnly || connectionIsUnmetered.
ScheduleDecision schedule(const std::vector<ScheduleItem> &items, bool networkAllowed,
                          int maxConcurrent = maxConcurrentTransfers);

// ---- resume -----------------------------------------------------------------------

enum class ResumeAction {
    ContinueFromOffset,        // request the next Range from the current part size
    RetryAfterYoungTokenDelay, // a fresh URL refused at byte 0: wait briefly, same URL
    ReResolveSameFormat,       // URL expired or refused later: new URL for the same format id
    RestartFromZero,           // drop the part and start this track again
    Complete,                  // the part already holds every byte
    Fail
};

struct ResumeLimits {
    std::int64_t youngTokenWindowMs = 10000;
    int youngTokenRetries = 2;
    int reResolves = 3;
    int transientRetries = 5;
};

// Before a request: what to do with an existing part of `partBytes`.
ResumeAction resumeAtStart(std::int64_t partBytes, std::int64_t expectedTotal);

struct ResponseFacts {
    int httpStatus = 0;              // 0: no HTTP response (transport error, timeout)
    std::int64_t requestOffset = 0;  // first byte asked for
    std::int64_t nowMs = 0;
    std::int64_t resolvedAtMs = 0;   // when the current URL was obtained
    bool urlExpired = false;         // the `expire` parameter is in the past
    bool truncated = false;          // fewer body bytes than announced, or closed early
    int youngRetries = 0;            // already spent for this URL
    int reResolves = 0;              // already spent for this track
    int transientRetries = 0;        // consecutive, reset whenever bytes arrive
};

// After a failed or unusable response at requestOffset.
ResumeAction resumeAfterResponse(const ResponseFacts &facts, const ResumeLimits &limits = ResumeLimits());

// After re-resolving the same format id: the new size (clen or filesize, 0
// unknown) must equal the stored total to keep the part. A mismatch marks the
// item failed (DL-02 v1.0); an unknown new size is checked against the next
// Content-Range instead.
ResumeAction resumeAfterReResolve(std::int64_t storedTotal, std::int64_t newTotal);

// URL helpers (query parameters of googlevideo URLs, public and documented by
// observation of the URL itself, no protocol knowledge).
std::string queryParameter(const std::string &url, const std::string &name);
bool urlExpired(const std::string &url, std::int64_t nowUnixSeconds, std::int64_t marginSeconds = 30);
std::int64_t contentLengthOf(const media::MediaFormat &format); // filesize, else clen, else 0

// "bytes=a-b" for the next chunk (b inclusive, clipped to total when known).
std::string rangeHeader(std::int64_t offset, std::int64_t total, std::int64_t chunkBytes);
std::int64_t rangeEnd(std::int64_t offset, std::int64_t total, std::int64_t chunkBytes); // inclusive
// "bytes a-b/total" (total "*" -> 0). false when malformed.
bool parseContentRange(const std::string &value, std::int64_t *first, std::int64_t *last, std::int64_t *total);

// Progress is persisted at most every 2 s or every 4 MiB per item.
bool progressPersistDue(std::int64_t bytesSinceLastWrite, std::int64_t msSinceLastWrite,
                        std::int64_t everyMs = 2000, std::int64_t everyBytes = 4 * 1024 * 1024);

// ---- DL-06 location -------------------------------------------------------------

struct LocationInput {
    std::string preferred;        // the folder chosen by the user, empty = default
    bool preferredUsable = false; // exists and passed the write probe
    std::string defaultDirectory; // private app data dir
};

struct LocationDecision {
    std::string directory;
    bool fellBack = false;
    std::string noticeKey;        // noticeKeys::folderUnavailable when fellBack
};

LocationDecision chooseLocation(const LocationInput &input);

// A folder offered by the user: refused when missing, not writable or on a RAM
// file system (PLT-09). Returns the error key, empty when accepted.
std::string validateChosenFolder(const std::string &path, bool exists, bool writable);
bool isTemporaryPath(const std::string &path);

// ---- DL-08 download all --------------------------------------------------------------

struct ExistingDownload {
    std::string videoId;
    Kind kind = Kind::Audio;
    State state = State::Queued;
};

// Playlist ids to enqueue for `kind`: valid ids only, each once, skipping
// ones already Done or unfinished (queued, active, paused, waiting). Failed
// items are included (enqueueing them retries).
std::vector<std::string> idsToEnqueue(const std::vector<std::string> &playlistVideoIds,
                                      const std::vector<ExistingDownload> &existing, Kind kind);

// ---- NFR-SEC-05 response and space checks ---------------------------------------------

// A media response's Content-Type (parameters and case ignored). Refused: text/*, JSON, XML,
// JavaScript and any "+json" / "+xml" type, which are error pages rather than media. An empty
// type is allowed (the size checks still apply).
bool mediaContentTypeAllowed(const std::string &contentType);

// Space kept free on the volume after a download, for the system and the remux temporaries.
const std::int64_t freeSpaceReserveBytes = 32LL * 1024 * 1024;

// Bytes a download still needs: what is left to transfer, plus one more copy of the output when
// a remux or flatten will write it next to the tracks.
std::int64_t spaceNeeded(std::int64_t remainingBytes, std::int64_t outputCopyBytes);

// available < 0 means unknown (the check is skipped, the write itself still fails safely).
bool enoughSpace(std::int64_t availableBytes, std::int64_t neededBytes);

}}}
