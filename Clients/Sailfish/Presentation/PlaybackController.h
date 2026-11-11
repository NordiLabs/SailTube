// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The decision glue of the watch session, Qt-free and time-injected (all times
// are milliseconds from one monotonic clock owned by the caller). PlaybackSession
// (the QML-facing QObject) owns one controller, forwards every event to it and
// executes the effects through PlaybackPort. Host-tested with a fake port in
// tests/session_qt.
//
// What it wires together:
//   resolve request    resolveRequestFor: the shared StreamRequest fields (RES-10, RES-20, SEL-01/02/05/08)
//   stream choice      selectStreamsWithFallback / qualityMenu (SEL-02/03/08, PLY-21 interim rule)
//   attempt chain      candidate fallbacks before the first preroll, then one
//                      fresh re-resolve of the whole set (invalidate + resolve), then
//   switches           quality / dub / listen-only re-select from the same set; a
//                      re-resolve only when the set cannot serve the choice (SEL-07/08)
//   prefetch           the next queue / related item 4 s after the first preroll (RES-11)
//   RecoveryPolicy     3 recoveries, 1.5 s backoff, Retry (PLY-18)
//   SessionArbiter     one owner, re-attach, reclaim, tagged resolves (PLY-10)
//   AudioResourcePolicy  resource before PLAYING, pause on loss (PLY-23)
//   AutoplayPolicy     queue, 5 s related countdown, next/previous (PLY-25, MPR-01)
//   SponsorSkipper     deferred skips, owner only (SB-02)
//   speed::            remembered rate applied before the first frame (PLY-07)
//   ResumePolicy       saved points, end-of-stream position (PLY-24/26)
//   KeepOnPolicy       display keep-on (PLY-12, PLT-11)
//   MediaControlsPolicy  MPRIS capabilities, Stop, cover state (MPR-01/02)
//   upcoming hold      an upcoming video shows its scheduled start and is not resolved again
//                      until then: one deadline at the start, then one resolve (RES-19)
//   low-rung start     A/B (NFR-PERF-02, diagnostics/startLowRung): the first plan uses the best
//                      dual rung <= 480p of the normal pick's codec family, then one same-set
//                      switch up after the first frame and 2 s of playing
//   error setting      the settings key that would fix the current failure (errorSettingKey)
//   cover video        the live picture in the active cover: coverVideoActive (the cover item
//                      draws) and coverKeepsDecoding (FreezePolicy's cover input, PLY-09 lifted)
//   stream instead     a playing download switches to the stream of the same video: one resolve
//                      while the file plays on, then one SEL-07 switch (PLY-26)
//   overlay controls   dub picker and stable volume (same-set audio switches, SEL-04/05/07), writable
//                      loop, hold-to-speed, the sleep timer (one deadline), the skipper's segments
//   StallPolicy        stall watchdog after the first frame: a position that stops advancing with
//                      no error -> seek in place, reopen the candidate's streams, one fresh
//                      resolve, then a retryable failure; never a refusal report (PLY-18, RES-07)
//
// Port calls never call back into the controller synchronously: results
// (resolve answers, audio grants, player events) arrive later as events.

#include "Core/Catalog/Domain/Video.h"
#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Player/Domain/AudioResourcePolicy.h"
#include "Core/Player/Domain/AutoplayPolicy.h"
#include "Core/Player/Domain/KeepOnPolicy.h"
#include "Core/Player/Domain/MediaControlsPolicy.h"
#include "Core/Player/Domain/PipelinePlan.h"
#include "Core/Player/Domain/PlayerTypes.h"
#include "Core/Player/Domain/RecoveryPolicy.h"
#include "Core/Player/Domain/ResolveCache.h"
#include "Core/Player/Domain/SessionArbiter.h"
#include "Core/Player/Domain/SponsorSkip.h"
#include "Core/Player/Domain/StallPolicy.h"

#include <cstdint>
#include <map>
#include <set>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace sailfish {

// What the player loads; PlaybackSession copies it into player::PlayerSources.
struct PlaybackLoad {
    player::PlayMode mode = player::PlayMode::Dual;
    player::SourceSpec video, audio, muxed;
    std::string videoId;
    std::int64_t startPositionMs = 0;
    double rate = 1.0;
    player::DecodeMode decode = player::DecodeMode::Software;
    std::int64_t metadataDurationMs = 0;
    bool liveHls = false;
    // Stall recovery: open every branch through a new stream job; the previous load's jobs (the
    // ones that stopped delivering) are never reused.
    bool freshStreams = false;
};

// Stall watchdog: what the player says right now (PlaybackPort::progress), asked fresh from the
// pipeline and not taken from the position poll.
struct PlayerProgress {
    std::int64_t positionMs = 0;   // the furthest position a sink reached (never a seek target)
    std::int64_t bufferedMs = 0;   // how far the media is buffered, 0 unknown
    bool buffering = false;        // PLY-08: playback is held for data
    int bufferingPercent = 100;
    bool seeking = false;          // a seek is in flight
    bool ended = false;            // end of stream
    bool videoFrozen = false;      // PLY-09: the picture is frozen in the background
};

// PLY-01: one candidate as the player's sources (dual, muxed, live HLS,
// audio-only). Headers go to extraHeaders, a User-Agent header to userAgent.
PlaybackLoad loadForCandidate(const media::StreamCandidate &candidate, const std::string &videoId);

// ---- Watch overlay controls (YouTube-style player; the design component binds playback.*) -------------

// holdSpeed: the rate while the user holds the picture (scaletempo keeps the pitch); never remembered.
const double kHoldSpeedRate = 2.0;
// Sleep timer: setSleepTimer(kSleepAtEndOfVideo) pauses at the end of the video playing then.
const int kSleepAtEndOfVideo = -1;
const int kSleepTimerMaxMinutes = 24 * 60;

// SEL-04 / SEL-05 audio choice applied after selection: an explicit dub-picker entry of this video
// (PlaybackController::setAudioTrack) and the stable-volume switch.
struct AudioChoice {
    bool chosen = false;        // an entry was picked for this video
    std::string language;       // its normalized language ("" = untagged)
    bool descriptive = false;   // its descriptive (audio description) class
    bool stableVolume = false;  // prefer the DRC ("stable volume") track of the entry
};

// Replaces the audio branch of every dual / audio-only candidate of `chain` with
// media::bestEntryTrack of the chosen entry (without a choice: of the entry the candidate already
// plays), the DRC track first with stableVolume, the candidate's own codec next. Nothing changes
// without a choice and with stable volume off, or when the entry has no track in the set.
void applyAudioChoice(std::vector<media::StreamCandidate> *chain, const media::SelectionInput &input,
                      const media::DecoderCapabilities &caps, const AudioChoice &choice);

// playback.audioTracks ids: the entry's normalized language ("und" when untagged) with "#ad" appended
// for an audio-description entry ("en", "pt-br", "en#ad"). Unique within one set.
std::string audioTrackId(const media::AudioTrackOption &option);

// Scrub preview from a storyboard (yt-dlp's storyboard formats sb0..sb3): each fragment is one image
// holding columns x rows cells of width x height pixels, left to right then top to bottom, and the
// fragments cover the video in order, durationMs each.
struct StoryboardFragment {
    std::string url;
    std::int64_t durationMs = 0;
};
struct Storyboard {
    int width = 0, height = 0;    // one cell, pixels
    int columns = 0, rows = 0;    // cells per fragment image
    std::vector<StoryboardFragment> fragments;
};
struct StoryboardCell {
    std::string url;              // the fragment image
    int x = 0, y = 0, width = 0, height = 0;  // the cell inside it, pixels
};
// The fragment image and cell showing `positionMs` (clamped into the storyboard). A cell lasts the
// first fragment's duration / (columns x rows), so a shorter last fragment keeps the same pace.
// False when the storyboard is empty or malformed.
bool storyboardCell(const Storyboard &storyboard, std::int64_t positionMs, StoryboardCell *cell);

// A resolve answer, already classified by the session (RES-09: Final = no re-resolve).
struct ResolveAnswer {
    enum Error { None, Retryable, Final };
    Error error = None;
    int errorCode = 0;              // opaque code the session turns into text (playback::StreamErrorClass)
    // Valid when error == None: formats, live state and manifest mapped from the shared
    // StreamSet; the summary (title, channel, duration, ...) from the open hints.
    catalog::VideoDetails details;
    std::int64_t expiresAtUnixMs = 0;  // earliest URL expiry of the set, 0 unknown (RES-10/12)
    // RES-19: the answer says the video is upcoming (an Upcoming error class, or a set whose live
    // state is Upcoming); its scheduled start in ms since the epoch, 0 unknown.
    bool upcoming = false;
    std::int64_t scheduledStartUnixMs = 0;
    // RES-09: a retryable answer that asks for a pause first (rate limiting): the one fresh
    // re-resolve waits PlaybackController::kRetryLaterMs (status RetryingLater).
    bool retryLater = false;
};

// What one resolve asks the shared resolver for (playback::StreamRequest without Qt types).
// Every output-affecting field the controller owns; the session adds the network class and
// signedIn = false (RES-20: the primary resolve never uses cookies).
struct ResolveRequest {
    std::string videoId;
    unsigned generation = 0;
    bool fresh = false;                    // drop cached answers first (whole-set failure, retry, recovery)
    // fresh only: what happened to the answer being dropped (RES-07). A stall, a lost connection
    // and a start that ran out of time are no verdict on the source, so never a strike; a byte-0
    // refusal is reported apart (reportRefused).
    player::SourceFault fault = player::SourceFault::Unspecified;
    bool speculative = false;              // RES-11 prefetch
    bool audioOnly = false;                // StreamPurpose::AudioOnly (SEL-08)
    int maxHeight = 0;                     // SEL-02 ceiling (or an explicit taller choice); 0 = none
    bool highFrameRate = false;            // SEL-03 explicit >30 fps choice
    std::vector<std::string> videoCodecs;  // SEL-01/02 preference order: "avc1", "vp09", "av01"
    std::vector<std::string> audioCodecs;  // empty = the resolver's default (Opus, then AAC; SEL-04)
    std::string dubLanguage;               // SEL-05 remembered dub (BCP-47), empty = original
    bool hardwareDecode = false;           // PLY-20: setting on and verified
};

// RES-10 / SEL-01/02/05/08: the request for `videoId` under these settings and choice.
// Video codecs: H.264 then VP9 with software decode, VP9 first with verified hardware decode
// (SEL-02), the codec preference first when set, AV1 last and only after its self-test (SEL-01).
ResolveRequest resolveRequestFor(const std::string &videoId, const media::DecoderCapabilities &,
                                 const media::PlaybackPreferences &, const media::QualityChoice &);

// ---- E12 speculative resolve and row prefetch (RES-11 MAY, RES-21 SHOULD) ------------------
// The waste bounds of the session's speculation, as pure decisions. PlaybackSession owns the
// speculation itself (one slot, latest press wins, reuse by the next open()); these say when it
// may start. Times are milliseconds on the session's monotonic clock.

const int kSpeculationQuietMs = 2000;      // never during the first 2 s of a start (E12)
const int kRowPrefetchInFlightMs = 4000;   // a row prefetch counts as in flight this long (no completion)
const int kRowPrefetchRepeatMs = 300000;   // the same top row is not prefetched again within 5 min
const int kKeepAliveIntervalMs = 30000;    // warmUp() at most this often (the resolver drops idle connections after 45 s)

struct SpeculationContext {
    std::int64_t nowMs = 0;
    bool appActive = true;          // never in the background
    int networkClass = -1;          // 0 unmetered, 1 mobile, -1 unknown (SEL-02)
    bool starting = false;          // a play is resolving or prerolling
    std::int64_t lastStartMs = -1;  // the last start (resolve or load) began, -1 none
};

// E12 / RES-11: a speculative resolve on touch-down may start (foreground, not within
// kSpeculationQuietMs of a start).
bool speculationAllowed(const SpeculationContext &context);

// What the session remembers of its row prefetches (RES-11) and keep-alive calls (RES-21).
struct RowPrefetchState {
    std::string lastVideoId;
    std::int64_t lastPrefetchMs = -1;
    std::int64_t lastWarmUpMs = -1;
};

struct RowsVisibleDecision {
    std::string prefetchVideoId;  // empty: no prefetch
    bool warmUp = false;          // call the resolver's warmUp()
    // Why no prefetch (marker key reason=): "background", "metered", "unknown-network",
    // "starting", "quiet", "speculating", "in-flight", "repeat", "no-row"; "" when prefetching.
    const char *reason = "";
};

// RES-11 / RES-21 for likely-visible rows (top first): on an unmetered network only, in the
// foreground, never while a play is starting or within kSpeculationQuietMs of one, never while a
// press speculation is in flight: a prefetch of the first row (valid id, not the session's video),
// one at a time and not the same row again within kRowPrefetchRepeatMs; warmUp() at most every
// kKeepAliveIntervalMs. Anything else (mobile data, unknown network, background) does nothing.
RowsVisibleDecision rowsVisibleDecision(const SpeculationContext &context, const std::vector<std::string> &videoIds,
                                        const std::string &sessionVideoId, bool speculationInFlight,
                                        const RowPrefetchState &state);

// playback.diagnostics counters (E12: used vs wasted).
struct SpeculationStats {
    int started = 0;        // speculative resolves started on touch-down
    int used = 0;           // reused by the following open()
    int wasted = 0;         // cancelled, superseded, dropped or failed before an open used them
    int rowPrefetches = 0;  // RES-11 prefetches of a visible row
    int warmUps = 0;        // RES-21 keep-alive calls
};

// ---- Shorts pager (DSC-17 MAY swipe viewer): likelyNext preload and loop items ----------------
// The pager names the Short it expects next (PlaybackSession::likelyNext): its answer is resolved
// into the cache and its planned branches are opened through the fetcher and held, so the
// following open() needs neither a resolve nor new stream opens. One preloaded id at a time.

const int kLikelyNextHoldMs = 60000;  // held stream jobs are closed after this long unused
// While the playing item is a Short (it loops, so the pager may stay on it for minutes) the prepared
// next Short's stream jobs are held this long: the swipe finds them whenever it comes.
const int kShortsAheadHoldMs = 10 * 60000;

struct LikelyNextContext {
    bool appActive = true;          // never in the background
    bool resolverAvailable = true;
    bool validId = false;           // catalog::validVideoId
    bool sessionVideo = false;      // the id is the session's video and it plays (or starts)
    bool local = false;             // the id has a playable download (its open is already instant)
    bool blocked = false;           // RES-19: an upcoming id is not resolved ahead of its start
    bool alreadyPreloaded = false;  // the same id is the current preload (in flight, held or deferred)
    bool pressSpeculation = false;  // a touch-down speculation of the same id exists (one in flight per id)
    bool starting = false;          // a play is resolving or prerolling: wait until it has prerolled
};

struct LikelyNextDecision {
    enum Action { Skip, Keep, Defer, Start };
    Action action = Skip;
    // Marker key reason=: "background", "no-resolver", "invalid-id", "playing", "local", "upcoming",
    // "preloaded" (Keep), "pressed", "starting" (Defer); "" for Start.
    const char *reason = "";
};

// Shorts likelyNext: what a likelyNext(videoId) call does. Defer and Start replace the previous
// preload (it is cancelled / closed); Skip and Keep leave it as it is.
LikelyNextDecision likelyNextDecision(const LikelyNextContext &context);

// Shorts likelyNext: the load a fresh open() of `videoId` would start with for this answer: Auto
// quality, from 0, listen-only when preferences.mode says so, and the NFR-PERF-02 low-rung first
// plan when `startLowRung` is on (as onResolved applies it), the stable-volume audio choice when
// `stableVolume` is on. False when nothing in the set plays.
bool firstLoadFor(const std::string &videoId, const media::SelectionInput &input, const media::DecoderCapabilities &caps,
                  const media::PlaybackPreferences &preferences, bool startLowRung, PlaybackLoad *load,
                  bool stableVolume = false);

// NFR-PERF-02 low-rung A/B: the chain of the tallest non-premium, <= 30 fps dual rung <=
// kLowRungMaxHeight of `normal`'s video codec family; empty when `normal` keeps (muxed, live,
// listen-only, already low, no such rung).
std::vector<media::StreamCandidate> lowRungChain(const media::SelectionInput &input, const media::DecoderCapabilities &caps,
                                                 const media::PlaybackPreferences &preferences,
                                                 const media::StreamCandidate &normal);

// A downloaded item (DownloadsModel::localMedia), PLY-26.
struct LocalMedia {
    std::string videoId;
    std::string layout;      // "pair" = separate video + audio tracks
    std::string filePath;    // the single file (or the video track of a pair)
    std::string videoPath;   // empty for an audio download
    std::string audioPath;   // pair audio track, or the audio download
    std::string title;
    std::int64_t durationMs = 0;
    // LNK-02: where this play starts (a t= link of the open that found the download); -1 = from
    // the beginning (the library bridge's resume may still apply, LIB-05).
    std::int64_t startPositionMs = -1;
    // Cover video / videoAspect: the video track's size when the record knows it (0 unknown).
    int width = 0;
    int height = 0;
};

// NFR-PERF-02 low-rung A/B: the tallest rung the first plan may use when the setting is on.
const int kLowRungMaxHeight = 480;
// ... and how long the low rung plays (position advance after its first frame) before the switch up.
const int kLowRungPlayMs = 2000;

struct ErrorSettingFacts;

// Cover video diagnostics: how long decoding ran for the cover only (app in the background,
// the PLY-09 freeze lifted because the cover shows the picture). Time-injected, Qt-free.
struct CoverDecodeMeter {
    bool on = false;
    std::int64_t sinceMs = 0;
    std::int64_t totalMs = 0;
    // Returns true when `lifted` changed (the caller prints cover-video on / off).
    bool update(bool lifted, std::int64_t nowMs);
    // Accumulated time, including a run still going at `nowMs`.
    std::int64_t elapsedMs(std::int64_t nowMs) const { return totalMs + (on && nowMs > sinceMs ? nowMs - sinceMs : 0); }
};

// Effects. Implemented by PlaybackSession (Qt) and by the fake in tests.
class PlaybackPort {
public:
    virtual ~PlaybackPort() {}
    // The shared resolver (RES-01): answer later with PlaybackController::onResolved.
    // request.fresh: invalidate the video's cached answers first (one re-resolve, RES-10).
    virtual void resolve(const ResolveRequest &request) = 0;
    virtual void cancelResolve() = 0;
    // RES-11: a speculative resolve of the next item; no answer is delivered.
    virtual void prefetch(const ResolveRequest &request) = 0;
    virtual void load(const PlaybackLoad &load) = 0;  // paused until play()
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stopPlayer() = 0;
    virtual void seek(std::int64_t positionMs) = 0;
    // SB-02 / PLT-05: run the seek on the next event-loop turn, never inside
    // the position handler that decided it.
    virtual void postSeek(std::int64_t positionMs) = 0;
    virtual void setRate(double rate) = 0;
    virtual void acquireAudio() = 0;   // PLY-23: answer with onAudioGranted / onAudioLost
    virtual void releaseAudio() = 0;
    virtual void savePosition(const std::string &videoId, std::int64_t positionMs) = 0;  // kNoResume clears
    virtual void rememberRate(double rate) = 0;  // PLY-07 persistence
    // Stall watchdog: the player's progress right now; false when it has none to give (nothing
    // loaded, or a port without a player: the watchdog then stays off). Read-only.
    virtual bool progress(PlayerProgress *progress) { (void)progress; return false; }
};

class PlaybackController {
public:
    enum State { Idle, Loading, Preparing, Ready, Failed };  // = PlaybackSession::State
    enum Status { NoStatus, Resolving, ResolvingFull, Starting, Reconnecting, WaitingForForeground, RetryingLater };
    enum Failure { NoFailure, ResolveFailed, NoPlayableStream, UpcomingStream, NetworkFailed, PlaybackFailed };
    // Change groups (takeChanges()); PlaybackSession maps them to notify signals.
    enum Change {
        DetailsChange = 1, StreamChange = 2, StateChange = 4, PlayerChange = 8, ControlsChange = 16,
        UpNextChange = 32, SponsorChange = 64, ReplacePageChange = 128, EndedChange = 256,
        SwitchResumedChange = 512,  // NFR-PERF-04 "switch-resumed" marker
        LowRungSwitchChange = 1024, // NFR-PERF-02 A/B: "switch-start kind=lowrung" marker
        LocalOpenChange = 2048,     // PLY-26: a queue/autoplay item has a download; the session plays it locally
        StreamSwitchChange = 4096,  // PLY-26 / SEL-07 stream instead: "switch-start kind=stream" marker
        SleepTimerChange = 8192,    // the sleep timer was set, cleared or fired
        SegmentsChange = 16384,     // sponsorSegments() changed (SB-02 seek bar)
        AudioSwitchChange = 32768,  // SEL-05/07 dub or stable-volume switch: "switch-start kind=dub" marker
        StallChange = 65536         // stall watchdog: read takeStallEvents() (stall-* markers)
    };

    static const int kStartWatchdogMs = 25000;     // a candidate with no preroll by then failed
    static const int kRetryLaterMs = 10000;        // RES-09: a rate-limited resolve's one re-resolve waits this long
    static const int kDeepStartMs = 30000;          // PLY-06: start positions from here prefer WebM
    static const int kMaximumAvailableWaitMs = 10000;  // provider pre-playback wait cap
    static const int kPrefetchDelayMs = 4000;      // RES-11: after the current item's first preroll
    static const int kExpiryMarginMs = 120000;     // RES-10: a set this close to expiry is resolved again
    static const int kStreamNoticeMs = 5000;       // stream instead: how long the "not available" text shows

    explicit PlaybackController(PlaybackPort *port);

    // ---- Configuration (applies to the next selection) --------------------------------
    void setCapabilities(const media::DecoderCapabilities &capabilities) { capabilities_ = capabilities; }
    // SEL-02 network class, codec, ceilings, hardware setting (PLY-21).
    void setPreferences(const media::PlaybackPreferences &preferences) { preferences_ = preferences; }
    void setKeepSessionOnLeave(bool on) { arbiter_.setKeepSessionOnLeave(on); }  // PLY-10
    void setRelatedAutoplay(bool on) { autoplay_.setRelatedAutoplay(on); }      // PLY-25
    void setResumeAfterInterruption(bool on) { audio_.setResumeAfterInterruption(on); }  // PLY-23
    void setRememberedRate(double rate);                                        // PLY-07
    void setKeepDisplayOnSetting(bool on) { keepOnSetting_ = on; changes_ |= ControlsChange; }  // PLY-12
    void setKeepAliveAvailable(bool available) { keepAliveAvailable_ = available; }  // PLT-11: FULL only
    void setSponsorSettings(const player::SponsorSettings &settings) { skipper_.setSettings(settings); }
    // NFR-PERF-02 A/B (diagnostics/startLowRung, default off): applies from the next video.
    void setStartLowRung(bool on) { startLowRung_ = on; }
    // Stall watchdog thresholds (host tests shorten them).
    void setStallConfig(const player::StallConfig &config) { stall_.setConfig(config); }
    // Cover video: the playback/coverVideo setting (the session passes false in STORE).
    void setCoverVideoSetting(bool on) { coverVideoSetting_ = on; }
    bool coverVideoSetting() const { return coverVideoSetting_; }
    bool startLowRung() const { return startLowRung_; }
    const media::DecoderCapabilities &capabilities() const { return capabilities_; }
    const media::PlaybackPreferences &preferences() const { return preferences_; }

    // ---- Watch contexts (PLY-10) -------------------------------------------------------
    // Shorts loop hint of an open: kLoopUnset (a new video plays once, the same video keeps its
    // mode), kLoopOff, kLoopOn.
    static const int kLoopUnset = -1;
    static const int kLoopOff = 0;
    static const int kLoopOn = 1;
    // A list row (or link, cover, autoplay) opens a video. Returns the context
    // id the watch page claims next (claimContext()). Shorts: `loopHint` sets loop() (see there).
    int open(std::int64_t nowMs, const std::string &videoId, int loopHint = kLoopUnset);
    // PLY-26 / DL-07: a downloaded item in the same player, no resolve and no network request
    // (no RES-11 prefetch either); media.startPositionMs >= 0 starts there (LNK-02).
    int openLocal(std::int64_t nowMs, const LocalMedia &media, int loopHint = kLoopUnset);
    // Shorts loop item: at the end of stream it restarts at 0 and keeps playing (one flush seek
    // through the port, no teardown); no up-next countdown, no related autoplay, no RES-11
    // prefetch, the queue untouched; the end position is saved once per item as for any video.
    // Set by open(): a new video loops only with kLoopOn; re-opening the session's own video
    // (re-attach) keeps the mode unless the hint says kLoopOn / kLoopOff (no reload either way).
    bool loop() const { return loop_; }
    // The watch page registers: the context of the last open(), or a new
    // context re-attached to the running session.
    int claimContext();
    // The page became the visible page again (A -> B -> back to A reclaims).
    void activateContext(std::int64_t nowMs, int contextId);
    // The page left the stack: park (playing / paused above 0) or stop.
    void closeContext(std::int64_t nowMs, int contextId);

    // ---- User actions ------------------------------------------------------------------
    void play(std::int64_t nowMs);
    void pause();
    // ---- Shorts (DSC-17): the item the pager opened (PlaybackSession::openShort) --------------------
    // A Short item loops (the loop hint), never stores a resume point (the history entry stays: the
    // position saved is "none"), is never parked, and holds instead of playing on in the
    // background: the screen locking or the app leaving the foreground is the hold "app".
    // holdShort(reason): the first reason pauses what plays; releaseShort(reason): the last one
    // resumes it, but only when it was playing before the first hold and the user did not pause or
    // play in between (a user's choice wins). No effect on an item that is not a Short. A new
    // video, Stop and closeContext end every hold.
    // Call right after open() of a new video (open() saves the previous item's position by its own rule).
    void setShortItem(bool on);
    bool shortItem() const { return shortItem_; }
    void holdShort(const std::string &reason);
    void releaseShort(std::int64_t nowMs, const std::string &reason);
    bool shortHeld() const { return heldByShort_; }
    int shortHolds() const { return static_cast<int>(shortHolds_.size()); }
    void togglePlayback(std::int64_t nowMs) { if (wantPlay_) pause(); else play(nowMs); }
    // MPR-01 "Stop really stops": save, end the session, release jobs and audio.
    void stop();
    void seek(std::int64_t positionMs);
    void setRate(double rate);  // PLY-07
    // SEL-03: height 0 = Auto; restarts the chain at the captured position.
    void setQuality(std::int64_t nowMs, const media::QualityChoice &choice);
    void setAudioOnly(std::int64_t nowMs, bool audioOnly);  // SEL-08, per session
    // PLY-18 Retry button (also used by play() after a failure).
    void retry(std::int64_t nowMs);
    // LIB-05 (library bridge): where the next start of the opened video begins.
    void setStartPosition(std::int64_t positionMs);
    // PLY-25 / MPR-01 queue.
    void setQueue(const std::vector<std::string> &videoIds, int currentIndex);
    void clearQueue() { autoplay_.clearQueue(); changes_ |= ControlsChange | UpNextChange; }
    // PLY-25 / MPR-01 read-out: items in the queue and the playing one's index; -1 without a queue.
    int queueLength() const;
    int queueIndex() const;
    void setRelatedCandidate(const std::string &videoId);
    // PLY-26 / DL-07: true when a video has a playable download. Queue, autoplay, next and
    // previous items then raise LocalOpenChange instead of resolving (read pendingLocalOpen()).
    void setLocalAvailable(std::function<bool(const std::string &videoId)> available) { localAvailable_ = available; }
    const std::string &pendingLocalOpen() const { return pendingLocalOpen_; }
    void next(std::int64_t nowMs);
    void previous(std::int64_t nowMs);
    void cancelUpNext();
    // SB-01 fills this; until then the segments stay empty.
    void setSponsorSegments(const std::vector<player::SponsorSegment> &segments);
    void skipSponsor();  // SB-02 manual button
    // MPR-02 idle cover: reopen the last video that has a resume point.
    bool continueWatching(std::int64_t nowMs);
    // PLY-26 / SEL-07 "stream instead": the downloaded item playing (or prerolling, or failed)
    // switches to streaming the SAME video without ending the session: one resolve like open()'s
    // (Auto quality, listen-only kept, RES-10 fields), state, details, context owner and the
    // local playback untouched meanwhile; with a playable answer one SEL-07 switch at the
    // position captured then (play / pause and rate kept, StreamSwitchChange, then
    // SwitchResumedChange at the stream's preroll). A failed answer (error, upcoming, nothing
    // playable) keeps the local file playing and raises streamNoticeActive() for kStreamNoticeMs.
    // Afterwards the item is a stream item (local() false). Returns false (nothing done) when
    // not canStreamInstead().
    bool streamInstead(std::int64_t nowMs);

    // ---- Watch overlay controls ------------------------------------------------------------
    // The overlay's loop toggle: the current item loops from now on (or stops looping), with the
    // semantics of the open hint kLoopOn / kLoopOff on a re-open of the same video (no reload; the
    // end position is saved once). Turning it on cancels a running up-next countdown.
    void setLoop(bool on);
    // Hold-to-speed: while held the applied rate is kHoldSpeedRate (not at the live edge); released,
    // the rate from before (the remembered PLY-07 rate). Never remembered; a new video releases it.
    void holdSpeed(bool held);
    bool speedHeld() const { return speedHeld_; }
    // Sleep timer: minutes > 0 pauses that many minutes from now (at most kSleepTimerMaxMinutes),
    // kSleepAtEndOfVideo pauses at the end of the video playing then (no up-next, no autoplay; a loop
    // item returns to 0 paused), 0 (or any other value) turns it off. Firing pauses, never stops: the
    // session, its owner, the position and the page stay; the timer is then off. Stop clears it.
    // One deadline in nextDeadlineMs(), no ticker.
    void setSleepTimer(std::int64_t nowMs, int minutes);
    bool sleepTimerActive() const { return sleepDeadline_ >= 0 || sleepAtEnd_; }
    bool sleepTimerAtEnd() const { return sleepAtEnd_; }
    std::int64_t sleepDeadlineMs() const { return sleepDeadline_; }  // -1 none (end-of-video or off)
    // Seconds until it fires, rounded up; at the end of the video: the media time left at the
    // applied rate (0 when the duration is unknown); -1 when off.
    int sleepTimerSecondsLeft(std::int64_t nowMs) const;
    // SEL-05 dub picker: the entries of the current set (empty unless the playing plan has a separate
    // audio branch: dual or audio-only; never for muxed, live or local items) and the playing one.
    std::vector<media::AudioTrackOption> audioTrackOptions() const;
    int playingAudioTrackIndex() const;  // -1 none
    // SEL-05 / SEL-07: plays entry `id` (audioTrackId) from the same set: one same-set switch at the
    // captured position (AudioSwitchChange, then SwitchResumedChange; the session keeps the frame and
    // the video stream), a re-resolve only when the set cannot serve it (URLs about to expire). The
    // choice holds for this video; its language is remembered for the session's later videos
    // (storedLanguagePreference: the original stores "no preference"; an audio-description entry is
    // not remembered). False (nothing done) for an id not in the set.
    bool setAudioTrack(std::int64_t nowMs, const std::string &id);
    // SEL-05 remembered dub of this session (applied to every later selection and request).
    bool dubRemembered() const { return dubRemembered_; }
    const std::string &rememberedDub() const { return rememberedDub_; }
    // SEL-05: the audio language remembered across sessions (AppSettings playback/dubLanguage);
    // "" = no preference (the original track). Applies to the next selection.
    void setRememberedDub(const std::string &language) {
        rememberedDub_ = language;
        dubRemembered_ = !language.empty();
    }
    // Stable volume: prefer the DRC track of the playing entry (SEL-04 key 3 inverted); a same-set
    // switch when that changes the track. Per session.
    void setStableVolume(std::int64_t nowMs, bool on);
    bool stableVolume() const { return stableVolume_; }
    // True only when the switch can change what plays: the plan has a separate audio branch and
    // the playing dub-picker entry has both a levelled (DRC) and a plain track among the playable
    // ones. False for muxed and live plans, downloads, before the details and for an entry
    // without a DRC twin (or with nothing else).
    bool stableVolumeAvailable() const;
    // SB-02: the segments the skipper uses (setSponsorSegments), for the seek bar; cleared per video.
    const std::vector<player::SponsorSegment> &sponsorSegments() const { return segments_; }

    // ---- Stall watchdog (StallPolicy) ----------------------------------------------------------
    // While the session wants to play, its load prerolled and the audio resource is held, the
    // position must keep advancing (PlaybackPort::progress, asked at the policy's one deadline;
    // onPosition in between). A stall is recovered in steps, each at most once per episode:
    //   1 seek to the position on the same streams, and PLAYING asked again
    //   2 the same candidate through new stream jobs (PlaybackLoad::freshStreams), at the position
    //   3 one fresh resolve (invalidate + resolve, a PLY-18 recovery resolve: a failed answer is a
    //     fault), at the position
    // then Failed with stalledFailure() and Retry; in the background that failure is a deferred
    // PLY-18 fault instead (recovered when the app is shown again). Reloads keep the picture
    // (keepsPicture()). A reopened candidate that fails before its preroll goes straight to step
    // 3; every other fault during an episode is PLY-18's as before. Nothing here reports a refusal
    // (RES-07: a stall or a connection loss is not a strike). 3 episodes per 10 min; a user seek,
    // Retry and a new video clear the budget.
    const player::StallPolicy &stall() const { return stall_; }
    struct StallEvent {
        enum Kind { Detected, Step, Recovered, Failed };
        Kind kind = Detected;
        int step = 0;               // Step / Recovered: 1 seek, 2 reopen, 3 re-resolve
        std::string reason;         // Detected: "frozen" | "starved"; Failed: "budget" | "exhausted"
        std::int64_t ms = 0;        // Detected: time without progress; Recovered: detection -> recovered
        std::int64_t positionMs = 0;
    };
    // The events since the last call, in order (once per StallChange).
    std::vector<StallEvent> takeStallEvents();
    // The failure shown is the watchdog's ("Playback stopped responding.").
    bool stalledFailure() const { return state_ == Failed && stallFailed_; }
    // The player keeps its last picture through the stop and the load that follows: a same-set
    // switch (SEL-07) or a stall-recovery reload.
    bool keepsPicture() const { return switching_ || stallReload_ || stallNextLoad_; }

    // ---- Events ------------------------------------------------------------------------
    void onResolved(std::int64_t nowMs, std::int64_t nowUnixMs, const std::string &videoId, unsigned generation,
                    const ResolveAnswer &answer);
    // Display metadata that arrived after the start (background details): fills summary fields,
    // description, likes, date and chapters that are still unknown; formats are untouched.
    void setMetadata(const std::string &videoId, const catalog::VideoDetails &metadata);
    void onPlayerStarted(std::int64_t nowMs);  // first preroll of this load
    void onFirstFrame(std::int64_t nowMs);
    void onPlayerPlaying(bool playing);
    void onPosition(std::int64_t nowMs, std::int64_t positionMs);
    void onDuration(std::int64_t durationMs);
    void onSeekable(bool seekable);
    void onPlayerFailed(std::int64_t nowMs, const std::string &reason, bool network);
    void onEnded(std::int64_t nowMs);
    void onDecodeFallback(const std::string &reason);
    void onAudioGranted();
    void onAudioLost();
    void setAppActive(std::int64_t nowMs, bool active);  // PLY-18 foreground, PLY-12
    // The "Background playback" setting. Off: leaving the foreground (cover, lock screen) pauses,
    // like a pause by the user; a play asked for afterwards (cover action, lock-screen controls)
    // plays. On (default): the sound keeps playing.
    void setBackgroundPlayback(bool on) { backgroundPlayback_ = on; }
    bool backgroundPlayback() const { return backgroundPlayback_; }
    // Cover video: the cover reports status === Cover.Active (visible on the home screen).
    void setCoverVisible(bool visible) { coverVisible_ = visible; }
    // RES-19: the wall clock (ms since the epoch) at monotonic `nowMs`; the session calls it before
    // tick() so a scheduled start stays right across device suspend (the monotonic clock pauses).
    void setWallClock(std::int64_t nowMs, std::int64_t nowUnixMs) { unixOffsetMs_ = nowUnixMs - nowMs; }
    // Call at nextDeadlineMs().
    void tick(std::int64_t nowMs);
    std::int64_t nextDeadlineMs() const;

    // ---- Read-out ----------------------------------------------------------------------
    unsigned takeChanges() { unsigned c = changes_; changes_ = 0; return c; }
    State state() const { return state_; }
    Status status() const { return status_; }
    Failure failure() const { return failure_; }
    int failureCode() const { return failureCode_; }
    // The answer code (playback::StreamErrorClass) that started the current fresh re-resolve or the
    // wait before it (RES-09 Retryable); 0 when the resolve is not such a retry.
    int retryCode() const { return retryCode_; }
    const std::string &failureReason() const { return failureReason_; }
    const std::string &videoId() const { return videoId_; }
    bool detailsLoaded() const { return detailsLoaded_; }
    const catalog::VideoDetails &details() const { return details_; }
    bool local() const { return local_; }
    // PLY-26 stream instead: a downloaded item with a YouTube id is in the session (not Idle) and
    // no stream-instead resolve is in flight.
    bool canStreamInstead() const;
    bool streamInsteadPending() const { return streamResolving_; }  // its resolve is in flight
    // The last stream-instead resolve failed; the local file kept playing (transient notice).
    bool streamNoticeActive() const { return streamNoticeDeadline_ >= 0; }
    // The playing set came from a fresh re-resolve (invalidate + resolve) after a whole-set
    // failure, a retry or a recovery (core's former full-profile pass; the name is kept).
    bool fullProfile() const { return fullProfile_; }
    bool setAudioOnly() const { return setAudioOnly_; }  // the set was resolved for listen-only
    // RES-11: the item the next prefetch is for ("" none).
    std::string prefetchCandidate() const;
    const std::vector<media::StreamCandidate> &chain() const { return chain_; }
    int attempt() const { return attempt_; }
    const media::StreamCandidate *candidate() const;
    const std::vector<media::QualityOption> &qualityOptions() const { return qualityOptions_; }
    int playingQualityIndex() const { return playingQualityIndex_; }
    const media::QualityChoice &qualityChoice() const { return choice_; }
    bool audioOnly() const { return audioOnly_; }
    bool hasVideo() const;
    bool live() const;
    bool started() const { return started_; }
    bool wantsPlay() const { return wantPlay_; }
    bool playerPlaying() const { return playerPlaying_; }
    std::int64_t positionMs() const { return positionMs_; }
    double rate() const { return rate_; }
    double rememberedRate() const { return rememberedRate_; }
    bool rateChangeable() const;
    bool hardwareDecodeFallback() const;  // PLY-21 interim rule for the playing item
    const std::string &decodeFallbackReason() const { return decodeFallbackReason_; }
    const player::RecoveryPolicy &recovery() const { return recovery_; }
    const player::SessionArbiter &arbiter() const { return arbiter_; }
    const player::AutoplayPolicy &autoplay() const { return autoplay_; }
    const player::AudioResourcePolicy &audioResource() const { return audio_; }
    // PLY-18; RES-19: not while an upcoming video waits for its known scheduled start.
    bool retryAvailable() const { return state_ == Failed && !upcomingWaiting_; }
    // RES-19: the current video is upcoming (failure UpcomingStream); its scheduled start in ms
    // since the epoch (0 unknown) and whether one resolve is armed at that start.
    bool upcoming() const { return state_ == Failed && failure_ == UpcomingStream; }
    std::int64_t scheduledStartUnixMs() const;
    bool waitingForScheduledStart() const { return upcomingWaiting_; }
    // RES-19: an automatic resolve of `videoId` (prefetch, speculation, row prefetch) must not run:
    // it answered upcoming, and its scheduled start has not passed or is unknown.
    bool resolveBlocked(std::int64_t nowMs, const std::string &videoId) const;
    bool switching() const { return switching_; }  // a quality / listen-only switch awaits playback
    bool appActive() const { return appActive_; }   // PLY-18 foreground (E12: no speculation in the background)
    // E12: a play is resolving or prerolling (Loading / Preparing).
    bool starting() const { return state_ == Loading || state_ == Preparing; }

    bool keepDisplayOn() const;                       // PLY-12 / PLT-11
    player::MediaControlsState mediaControlsState() const;
    player::MprisCapabilities mprisCapabilities() const;  // MPR-01
    player::CoverState coverState() const;            // MPR-02
    // Cover video: the cover item draws live frames. Setting on, a video plan (not listen-only,
    // not audio-only, not a local file without a video track), this load's first frame seen, no
    // error, and the app in the background.
    bool coverVideoActive() const;
    // Cover video: FreezePolicy's cover input (VideoPlayer::setCoverVisible). Cover visible,
    // setting on, a video plan and no error. The first frame is NOT required: a load that goes to
    // the background before its first frame keeps decoding while the cover shows it, otherwise a
    // frozen video branch would never produce the frame coverVideoActive waits for. The policy
    // ignores it in the foreground (unchanged behaviour there).
    bool coverKeepsDecoding() const;
    // Cover video: decoding runs for the cover only (background and coverKeepsDecoding).
    bool coverDecoding() const { return !appActive_ && coverKeepsDecoding(); }
    // Why the cover is not decoding (marker key reason=): "foreground", "cover-hidden",
    // "setting-off", "idle", "no-video", "error"; "" while it decodes.
    const char *coverOffReason() const;
    // playback.videoAspect before the first frame: width / height of the chosen plan's video
    // (dual: the video format, progressive / live: the muxed format, local: the record's size);
    // 0 when unknown or without a video plan (listen-only, audio-only, nothing chosen).
    double plannedVideoAspect() const;

    // PLY-25 "up next" countdown.
    const std::string &upNextVideoId() const { return upNextVideo_; }
    std::int64_t upNextRemainingMs(std::int64_t nowMs) const { return autoplay_.countdownRemainingMs(nowMs); }
    // The next item replaces the watch page (PLY-25); read once per ReplacePageChange.
    const std::string &replacePageVideoId() const { return replaceVideo_; }

    // SB-02 notice and manual button.
    bool sponsorNoticeVisible() const { return noticeDeadline_ >= 0; }
    player::SponsorCategory sponsorNoticeCategory() const { return noticeCategory_; }
    bool sponsorButtonVisible() const { return buttonVisible_; }
    player::SponsorCategory sponsorButtonCategory() const { return buttonCategory_; }
    bool highlightVisible() const { return highlightVisible_; }
    std::int64_t highlightMs() const { return highlightMs_; }

    // MPR-02 "continue watching".
    bool hasResumable() const { return !resumableVideo_.empty(); }
    const std::string &resumableVideoId() const { return resumableVideo_; }

    // NFR-PERF-02 A/B: the playing plan is the low rung, waiting for its switch up.
    bool lowRungActive() const { return lowRungPending_; }

    // PLY-18 / PLY-20 / SEL-02 / SEL-08: what errorSettingKey() needs about the current failure.
    // noPlayableAnswer: the resolver answered "no playable stream" for the request (RES-09 class
    // NoPlayableStream; the session knows the class, the controller only its opaque code).
    ErrorSettingFacts errorSettingFacts(bool noPlayableAnswer) const;

private:
    void execute(const player::SessionDecision &decision, std::int64_t nowMs);
    void resetVideo(const std::string &videoId);
    void beginOpen(std::int64_t nowMs, const std::string &videoId);
    enum UpcomingBlock { NotBlocked, AutomaticOnly, AllResolves };
    UpcomingBlock upcomingBlock(std::int64_t nowMs, const std::string &videoId) const;
    bool holdUpcoming(std::int64_t nowMs);
    void noteUpcoming(std::int64_t nowMs, std::int64_t scheduledStartUnixMs);
    void showUpcoming();
    void beginLocal(std::int64_t nowMs);
    void onStreamInsteadResolved(std::int64_t nowMs, const ResolveAnswer &answer);
    void cancelStreamInstead();
    bool renewGeneration();
    void startResolve(bool fresh);
    bool setServes(std::int64_t nowMs) const;
    void maybePrefetch();
    media::SelectionInput selectionInput() const;
    media::PlaybackPreferences effectivePreferences() const;
    void rebuildChain();
    void startAttempt(std::int64_t nowMs, int index);
    void loadNow(std::int64_t nowMs);
    PlaybackLoad localLoad() const;
    void releaseMedia();
    bool applyLowRung();
    void clearLowRung();
    void switchUpFromLowRung(std::int64_t nowMs);
    void fault(std::int64_t nowMs, const std::string &reason, bool network);
    void applyRecovery(std::int64_t nowMs, const player::RecoveryAction &action);
    void watchStall(std::int64_t nowMs);
    void applyStall(std::int64_t nowMs, const player::StallDecision &decision);
    void noteStall(StallEvent::Kind kind, int step, const char *reason, std::int64_t ms);
    void seekTo(std::int64_t positionMs);
    void recoverNow(std::int64_t nowMs, const player::RecoveryAction &action);
    void applyAudio(const player::AudioResourceAction &action);
    void applyAutoplay(std::int64_t nowMs, const player::AutoplayDecision &decision);
    void syncQueue(const std::string &videoId);
    void requestPlayback();
    void fail(Failure failure, int code, const std::string &reason);
    void setState(State state, Status status = NoStatus);
    void noteResumable(const std::string &videoId, std::int64_t positionMs);
    std::int64_t capturePosition() const;
    bool liveEdge() const;
    bool owner() const { return arbiter_.owner() != player::kNoContext; }

    PlaybackPort *port_;
    media::DecoderCapabilities capabilities_;
    media::PlaybackPreferences preferences_;
    player::SessionArbiter arbiter_;
    player::RecoveryPolicy recovery_;
    player::AudioResourcePolicy audio_;
    player::AutoplayPolicy autoplay_;
    player::SponsorSkipper skipper_;

    State state_ = Idle;
    Status status_ = NoStatus;
    Failure failure_ = NoFailure;
    int failureCode_ = 0;
    int retryCode_ = 0;                  // retryCode()
    std::int64_t retryDeadline_ = -1;    // RES-09: the delayed re-resolve of a rate-limited answer
    std::string failureReason_;
    unsigned changes_ = 0;

    int nextContext_ = 1;
    int pendingContext_ = player::kNoContext;
    unsigned generation_ = 0;       // arbiter generation of the resolve in flight
    bool resolving_ = false;
    bool resolvingFull_ = false;
    bool resolvingAudioOnly_ = false;
    int resolvingMaxHeight_ = 0;
    bool recoveryResolve_ = false;  // the resolve in flight is a recovery (PLY-18)
    bool streamResolving_ = false;  // a stream-instead resolve is in flight (the local file plays on)
    std::int64_t streamNoticeDeadline_ = -1;  // stream instead failed: notice until then

    std::string videoId_;
    catalog::VideoDetails details_;
    bool detailsLoaded_ = false;
    bool fullProfile_ = false;
    bool setAudioOnly_ = false;          // purpose of the set in details_ (SEL-08)
    int setMaxHeight_ = 0;               // ceiling the set was resolved for (0 none)
    std::int64_t setExpiresAtUnixMs_ = 0;
    bool local_ = false;
    bool pendingLocal_ = false;
    LocalMedia localMedia_;
    std::string localVideoId_;
    std::vector<media::StreamCandidate> chain_;
    std::vector<media::QualityOption> qualityOptions_;
    int playingQualityIndex_ = -1;
    media::QualityChoice choice_;
    bool audioOnly_ = false;
    int attempt_ = -1;
    bool loaded_ = false;
    bool started_ = false;      // this load prerolled
    bool everStarted_ = false;  // any load of this video prerolled
    bool switching_ = false;
    std::int64_t resumeMs_ = 0;          // where the next load starts
    std::int64_t unixOffsetMs_ = 0;      // wall clock - monotonic clock, at the last resolve
    std::int64_t loadDeadline_ = -1;     // provider pre-playback wait
    std::int64_t watchdogDeadline_ = -1; // kStartWatchdogMs
    bool backgroundPlayback_ = true;
    bool shortItem_ = false;
    std::set<std::string> shortHolds_;    // the reasons that hold a Short
    bool heldByShort_ = false;            // a hold paused it (or it was already paused)
    bool resumeAfterHold_ = false;        // it was playing before the first hold and nobody chose since
    void pauseNow();
    void clearShortHolds();
    std::int64_t resumePointFor(std::int64_t positionMs, std::int64_t durationMs) const;
    bool lastFaultNetwork_ = false;
    bool startTimedOut_ = false;         // the fault being raised is the start watchdog's
    bool lastFaultStartTimeout_ = false; // ... and the last fault was
    player::SourceFault resolveFault_ = player::SourceFault::Unspecified;  // for the next fresh resolve
    player::SourceFault lastFaultKind() const;
    std::string decodeFallbackReason_;

    bool wantPlay_ = true;
    bool playerPlaying_ = false;
    bool seekable_ = false;
    bool appActive_ = true;
    std::int64_t positionMs_ = 0;
    std::int64_t durationMs_ = 0;
    double rate_ = 1.0;
    double rememberedRate_ = 1.0;
    bool keepOnSetting_ = player::kKeepDisplayOnDefault;
    bool keepAliveAvailable_ = false;

    std::string relatedCandidate_;
    std::int64_t prefetchDeadline_ = -1;  // RES-11
    bool prefetchPending_ = false;        // due, waiting for a candidate
    std::string prefetchedVideo_;

    std::string upNextVideo_;
    std::string replaceVideo_;
    std::function<bool(const std::string &)> localAvailable_;
    std::string pendingLocalOpen_;

    std::int64_t noticeDeadline_ = -1;
    player::SponsorCategory noticeCategory_ = player::SponsorCategory::Sponsor;
    bool buttonVisible_ = false;
    player::SponsorCategory buttonCategory_ = player::SponsorCategory::Sponsor;
    bool highlightVisible_ = false;
    std::int64_t highlightMs_ = 0;

    std::string resumableVideo_;

    // RES-19: videos that answered upcoming. scheduled = one resolve is due at startUnixMs (a known
    // start that had not passed); otherwise only automatic resolves are held (Retry allowed).
    struct Upcoming {
        std::int64_t startUnixMs = 0;  // 0 unknown
        bool scheduled = false;
    };
    std::map<std::string, Upcoming> upcoming_;
    bool upcomingWaiting_ = false;  // the current video waits for its scheduled start
    bool automaticOpen_ = false;    // open() comes from autoplay, not the user

    // NFR-PERF-02 low-rung A/B.
    bool startLowRung_ = false;
    bool lowRungTried_ = false;          // once per video (its first plan)
    bool lowRungPending_ = false;        // the low plan is loaded, the switch up is still due
    bool lowRungFrame_ = false;          // its first frame arrived
    std::int64_t lowRungFromMs_ = 0;     // position at that first frame
    std::int64_t lowRungDeadline_ = -1;  // the switch up runs at the next tick (never in onPosition)

    bool loadHardwareSetting_ = false;   // the last load decoded in hardware by the setting (PLY-20)

    // Cover video.
    bool coverVideoSetting_ = false;     // playback/coverVideo (false until the session applies it)
    bool coverVisible_ = false;          // status === Cover.Active
    bool firstFrameSeen_ = false;        // this load showed its first frame
    int failedMaxHeight_ = 0;            // ceiling of the resolve that answered the failure

    // Shorts loop item.
    bool loop_ = false;
    bool loopEndSaved_ = false;          // the end position of this item was saved (once)
    void applyLoopHint(bool sameVideo, int loopHint);

    // Watch overlay controls.
    AudioChoice chainAudioChoice() const;
    bool switchAudioIfNeeded(std::int64_t nowMs);
    void sleepNow();
    bool speedHeld_ = false;
    std::int64_t sleepDeadline_ = -1;    // minutes timer: fires then
    bool sleepAtEnd_ = false;            // pause at the end of the video
    AudioChoice audioChoice_;            // this video's explicit entry (chosen) only
    bool stableVolume_ = false;

    // Stall watchdog.
    player::StallPolicy stall_;
    bool stallPort_ = false;             // the port answered progress() once: it has a player to ask
    bool stallDirty_ = false;            // a guard may have changed: look again at the next tick
    bool stallNextLoad_ = false;         // the next load is a recovery step's reload
    bool stallReload_ = false;           // ... and this load is one, until its preroll
    bool stallFailed_ = false;           // the failure shown is the watchdog's
    std::int64_t stallAtMs_ = 0;         // the position the open episode recovers at
    std::vector<StallEvent> stallEvents_;
    bool dubRemembered_ = false;         // SEL-05: a pick this session (rememberedDub_ "" = original)
    std::string rememberedDub_;
    std::vector<player::SponsorSegment> segments_;
};

// ---- Error -> setting (PLY-18 retry dialog, ux "fix it" link) ----------------------------------

// Facts about the current failure, Qt-free (PlaybackController::errorSettingFacts fills them).
struct ErrorSettingFacts {
    PlaybackController::Failure failure = PlaybackController::NoFailure;
    bool noPlayableAnswer = false;   // resolver class NoPlayableStream (RES-09)
    int requestMaxHeight = 0;        // the failed request's ceiling, 0 none (SEL-02)
    bool mobileNetwork = false;      // that ceiling was the mobile-data one (SEL-02)
    bool audioOnly = false;          // listen-only was on (SEL-08)
    bool emptySelection = false;     // the answered set gave no candidate at all
    bool noDecodableVideo = false;   // ... because no video format is decodable (SEL-01)
    bool videoModePlayable = false;  // with listen-only off the set would give a candidate
    bool playerFault = false;        // candidates failed in the player (not a network fault)
    bool hardwareDecode = false;     // the failed load decoded in hardware by the setting (PLY-20)
    bool local = false;              // a downloaded item (PLY-26): no setting changes its file
    bool stalled = false;            // the stall watchdog gave up: no setting fixes a stream that stopped
};

// The AppSettings key (tests/platform/output_settings.json names) whose change would fix the
// failure, or "" when no setting does (network, provider, final classes, upcoming, local files):
//   player fault while the hardware-decode setting decoded   -> "playback/hardwareDecode"
//   listen-only on and the set (or the resolver) has nothing for it -> "playback/audioOnly"
//   the resolver found nothing under the request's ceiling   -> "playback/maxHeight"
//                                                               ("playback/maxHeightMobile" on mobile data)
//   no decodable video codec in the set                      -> "playback/codecPreference"
std::string errorSettingKey(const ErrorSettingFacts &facts);

}}}
