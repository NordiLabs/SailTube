// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The one video the application is showing or playing, on the lane's own
// GStreamer player (VideoPlayer, PLY-01). QML-facing: details, stream choice,
// player state, MPRIS / cover read-outs. Every decision lives in
// PlaybackController (Qt-free, host-tested); this class forwards events to it
// and executes its effects (PlaybackPort).
//
// Start path (RES-01, RES-06, RES-16..19, SEL-01..08, NFR-PERF-02):
//   open(videoId, hints) -> one StreamResolver::resolve(StreamRequest) -> the StreamSet as
//   media::SelectionInput -> selectStreamsWithFallback -> every branch of the chosen
//   candidate opened through StreamFetcher::open in parallel -> VideoPlayer::load with the
//   local (loopback) URLs; HLS goes to the player directly. Title, channel, thumbnail and
//   duration come from the open hints; no yt-dlp details call is on this path (a metadata
//   request may run after the first preroll when the hints lacked a title).
//
// QML: `mediaObject` is the VideoPlayer; point a VideoSurfaceItem's `source`
// at it. A watch page claims a context (claimContext) when it is created,
// calls activateContext when it becomes the visible page again and
// closeContext when it is destroyed (PLY-10).
//
// Stall watchdog (PlaybackController / StallPolicy): a stream that stops delivering without an
// error (a stalled proxy job, an address that expired silently, a dropped network, a stuck sink)
// is found by its position standing still and recovered in steps (seek in place, the candidate's
// branches through new fetcher jobs, one fresh resolve), the picture kept; `buffering` and
// `recovering` are true meanwhile. Markers stall-detected reason=, stall-recover step=,
// stall-recovered ms=, stall-failed reason=; diagnostics stalls, stallRecoveries, lastStallStep.
// It never reports a refusal (RES-07): only a byte-0 HTTP refusal is a strike, as before.
//
// Watch overlay (YouTube-style controls; the design component binds exactly these names): bufferedPosition,
// audioTracks + setAudioTrack, stableVolume, captionTracks + captionTrackId + setCaptionTrack +
// captionText, sponsorSegments, a writable loop, the sleep timer (sleepTimerSecondsLeft,
// sleepTimerAtEnd, setSleepTimer), holdSpeed, and the scrub preview (scrubPreview /
// scrubPreviewUrl, empty until a storyboard is known: setStoryboard).

#include "Core/Catalog/Domain/Video.h"
#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Player/Domain/PipelinePlan.h"
#include "PlaybackController.h"
#include "Core/Player/Domain/AudioChain.h"
#include "Shared/Playback/StreamSource.h"
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

class QNetworkAccessManager;

namespace nordtile {
namespace player { class AudioResourceGate; class HardwareDecodeGate; class VideoPlayer; }
namespace video { namespace sailfish {

class AppSettings;
class PlaybackCaptionSource;

// ---- Shared stream source <-> selection mapping (Qt Core, host-tested) ----------------------

// SEL-01..05: one shared StreamFormat as the selection policy's MediaFormat (codec families from
// the RFC 6381 tags, https/HLS protocol class, contentLength as fileSize, descriptive tracks
// ranked last through languagePreference -10). The URL is kept as the lookup key back to the
// StreamFormat the fetcher opens.
media::MediaFormat mediaFormatFor(const playback::StreamFormat &);
// SEL-01..08, RES-19: the whole set (video, audio, muxed, live manifest and state) as input.
media::SelectionInput selectionInputFor(const playback::StreamSet &);
// RES-10 / RES-20: the shared request for the controller's request; networkClass 0 unmetered,
// 1 mobile, -1 unknown (SEL-02); signedIn is always false on the primary resolve. RES-14:
// poProvider is true only while the PO-token helper is ready (setPoProviderReady).
playback::StreamRequest streamRequestFor(const ResolveRequest &, int networkClass, bool poProvider = false);
// SB-02 / DL-07: SponsorBlock segments as the skipper's, from [{category, actionType, start, end}]
// in `unitMs` milliseconds per unit (1 for SponsorSegmentsModel's ms, 1000 for a download
// record's seconds). Unknown categories, actionType other than skip / poi (mute, full) and
// end < start are dropped; a poi_highlight is a point (end = start).
std::vector<player::SponsorSegment> sponsorSegmentsFor(const QVariantList &segments, double unitMs);
// Scrub preview: a storyboard as {width, height, columns, rows, fragments: [{url, durationMs}]}
// (yt-dlp's storyboard format with the fragment durations in ms); malformed input gives an empty
// Storyboard (no preview).
Storyboard storyboardFor(const QVariantMap &storyboard);
// The same from a resolve answer's storyboard (playback::StreamSet::storyboard): https sheets with
// a duration only; anything else gives an empty Storyboard.
Storyboard storyboardFor(const playback::StreamStoryboard &storyboard);
// playback.audioTracks entry label: the language's own name ("Deutsch"; the tag when Qt does not know
// it; "Default" when untagged), plus the translated "original" / "audio description" marks.
QString audioTrackLabel(const media::AudioTrackOption &option);
// RES-09: Final (no re-resolve) for final classes and for classes a re-resolve cannot change.
ResolveAnswer::Error resolveErrorKind(const playback::StreamError &);
// RES-09: the short, translated reason shown for a resolve failure class (never provider text).
QString streamErrorText(playback::StreamErrorClass);
// RES-07: a branch error is a refusal (HTTP answer >= 400 at byte 0), not a network death.
// httpStatus: the HTTP source's status (VideoPlayer::failed, 0 unknown) decides when known; the
// reason text ("(403)", status phrase) is the fallback.
bool refusalAtByteZero(const QString &reason, bool network, bool sawFirstByte, int httpStatus = 0);

class PlaybackSession final : public QObject {
    Q_OBJECT
    // VideoSurfaceItem reads the player (VideoFrameProvider) through this property.
    Q_PROPERTY(QObject *mediaObject READ mediaObject NOTIFY mediaObjectChanged)
    Q_PROPERTY(QString videoId READ videoId NOTIFY detailsChanged)
    Q_PROPERTY(QString title READ title NOTIFY detailsChanged)
    Q_PROPERTY(QString channel READ channel NOTIFY detailsChanged)
    Q_PROPERTY(QString channelId READ channelId NOTIFY detailsChanged)
    Q_PROPERTY(QString description READ description NOTIFY detailsChanged)
    Q_PROPERTY(QString thumbnail READ thumbnail NOTIFY detailsChanged)
    Q_PROPERTY(double viewCount READ viewCount NOTIFY detailsChanged)
    Q_PROPERTY(double likeCount READ likeCount NOTIFY detailsChanged)
    Q_PROPERTY(QString uploadDate READ uploadDate NOTIFY detailsChanged)
    Q_PROPERTY(bool live READ live NOTIFY detailsChanged)
    Q_PROPERTY(bool detailsLoaded READ detailsLoaded NOTIFY detailsChanged)
    Q_PROPERTY(QVariantList chapters READ chapters NOTIFY detailsChanged)
    // SEL-03: {height, highFrameRate, premium, label, codec, formatId, playing}; empty unless dual.
    Q_PROPERTY(QVariantList qualities READ qualities NOTIFY streamChanged)
    Q_PROPERTY(int requestedHeight READ requestedHeight NOTIFY streamChanged)
    Q_PROPERTY(bool requestedHighFrameRate READ requestedHighFrameRate NOTIFY streamChanged)
    Q_PROPERTY(QString streamLabel READ streamLabel NOTIFY streamChanged)
    Q_PROPERTY(bool audioOnly READ audioOnly NOTIFY streamChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY streamChanged)
    // PLY-20/21 diagnostics: decodeMode, hardwareDecodeFallback, decodeFallbackReason, plan, ...
    Q_PROPERTY(QVariantMap diagnostics READ diagnostics NOTIFY streamChanged)
    Q_PROPERTY(int state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    // PLY-18: the reason and a Retry button after the automatic recoveries.
    Q_PROPERTY(bool retryAvailable READ retryAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool recovering READ recovering NOTIFY stateChanged)
    // RES-19: the video has not started yet; its scheduled start in ms since the epoch (0 unknown).
    // errorText names the start; Retry stays hidden until then when the start is known.
    Q_PROPERTY(bool upcoming READ upcoming NOTIFY stateChanged)
    // PLY-18 / ux: the AppSettings key whose change would fix the current error ("" none), e.g.
    // "playback/hardwareDecode", "playback/maxHeight", "playback/maxHeightMobile",
    // "playback/codecPreference" (errorSettingKey() in PlaybackController.h). One value is not a
    // settings row: "playback/audioOnly" means listen-only (per session, SEL-08) blocks playback;
    // the page offers "Play video" (setAudioOnlyMode(false) + retry) instead of "Open setting".
    Q_PROPERTY(QString errorSettingKey READ errorSettingKey NOTIFY stateChanged)
    Q_PROPERTY(double scheduledStart READ scheduledStart NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playerChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY playerChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY playerChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY playerChanged)
    // PLY-07: applied rate; rateChangeable is false at the live edge.
    Q_PROPERTY(double rate READ rate NOTIFY playerChanged)
    Q_PROPERTY(bool rateChangeable READ rateChangeable NOTIFY playerChanged)
    // PLY-12 / PLT-11: bind DisplayKeepOn.wanted to this.
    Q_PROPERTY(bool keepDisplayOn READ keepDisplayOn NOTIFY controlsChanged)
    // MPR-01 capabilities (queue-aware) and a local artUrl (file://, never remote).
    Q_PROPERTY(bool canPlay READ canPlay NOTIFY controlsChanged)
    Q_PROPERTY(bool canPause READ canPause NOTIFY controlsChanged)
    Q_PROPERTY(bool canSeek READ canSeek NOTIFY controlsChanged)
    Q_PROPERTY(bool canGoNext READ canGoNext NOTIFY controlsChanged)
    Q_PROPERTY(bool canGoPrevious READ canGoPrevious NOTIFY controlsChanged)
    Q_PROPERTY(QString artUrl READ artUrl NOTIFY artChanged)
    // PLY-25 / MPR-01: items in the queue and the playing one's index; both -1 without a queue.
    Q_PROPERTY(int queueLength READ queueLength NOTIFY queueChanged)
    Q_PROPERTY(int queueIndex READ queueIndex NOTIFY queueChanged)
    // DL-07: the saved captions of the downloaded item playing ([{languageCode, name,
    // autoGenerated, path}], path = saved json3 body for CaptionService::parseJson3); empty for
    // streams. The caption model reads these instead of fetching.
    Q_PROPERTY(QVariantList localCaptions READ localCaptions NOTIFY detailsChanged)
    // PLY-26 stream instead: a downloaded item with a YouTube id plays (not Idle), no stream-instead
    // resolve is in flight and a resolver exists. Says nothing about the network (offline is only
    // known by trying: the failure keeps the file playing and shows errorText for 5 s).
    Q_PROPERTY(bool canStreamInstead READ canStreamInstead NOTIFY controlsChanged)
    // MPR-02 cover: mode 0 identity, 1 continue watching, 2 session;
    // actions 0 none, 1 play/pause, 2 next, 3 stop, 4 continue watching.
    Q_PROPERTY(int coverMode READ coverMode NOTIFY controlsChanged)
    Q_PROPERTY(int coverLeftAction READ coverLeftAction NOTIFY controlsChanged)
    Q_PROPERTY(int coverRightAction READ coverRightAction NOTIFY controlsChanged)
    // PLY-10: the session plays with no watch page on screen.
    Q_PROPERTY(bool parked READ parked NOTIFY controlsChanged)
    // Shorts pager: the playing item was opened with openShort / the `short` hint (bind the Short's
    // own controls to it; a Short is never parked, so it has no mini-player).
    Q_PROPERTY(bool shortActive READ shortActive NOTIFY controlsChanged)
    // At least one hold (holdShort, or the app in the background) keeps the Short paused.
    Q_PROPERTY(bool shortHeld READ shortHeld NOTIFY controlsChanged)
    // Cover video: bind CoverVideoItem.active to `coverVideoActive && status === Cover.Active`.
    // True with the setting on, a video plan (not listen-only / audio-only / a local file without
    // video), the load's first frame seen, no error and the app in the background.
    Q_PROPERTY(bool coverVideoActive READ coverVideoActive NOTIFY coverVideoActiveChanged)
    // Picture aspect (width / height): from the chosen plan's format (or the download record's
    // width / height) as soon as the plan is chosen, before the first frame; then the decoded
    // picture's (pixel aspect included). 0 when unknown (audio-only, listen-only, nothing chosen).
    Q_PROPERTY(double videoAspect READ videoAspect NOTIFY videoAspectChanged)
    // PLY-25 "up next" countdown ("" when none).
    Q_PROPERTY(QString upNextVideoId READ upNextVideoId NOTIFY upNextChanged)
    Q_PROPERTY(int upNextSecondsLeft READ upNextSecondsLeft NOTIFY upNextChanged)
    // SB-02: notice (1.6 s) and manual button, as category API names ("" when hidden).
    Q_PROPERTY(QString sponsorNotice READ sponsorNotice NOTIFY sponsorChanged)
    Q_PROPERTY(QString sponsorButton READ sponsorButton NOTIFY sponsorChanged)
    Q_PROPERTY(double highlightPosition READ highlightPosition NOTIFY sponsorChanged)
    // Shorts: the item restarts at 0 at its end and plays on (open hint `loop: true`; see open()).
    // Writable (the overlay's loop toggle): applies to the playing item with the hint's semantics
    // (PlaybackController::setLoop); the next video plays once unless its hints say otherwise.
    Q_PROPERTY(bool loop READ loop WRITE setLoop NOTIFY loopChanged)
    // ---- Watch overlay ------------------------------------------------------------------------
    // Seek bar: how far the media is buffered in the playing timeline (ms): the playing position
    // plus the demuxed queue, or the downloaded range of a disk-backed branch, whichever is further;
    // dual: the shorter branch; a local file: its duration; 0 when unknown. Polled with the position
    // (PositionPolicy cadence, no timer of its own).
    Q_PROPERTY(double bufferedPosition READ bufferedPosition NOTIFY bufferedPositionChanged)
    // SEL-05 dub picker: [{id, label, language, original, descriptive, selected}] of the current set,
    // original first; empty unless the plan has a separate audio branch (dual or listen-only; never
    // muxed, live or a download). Show the picker for two or more entries.
    Q_PROPERTY(QVariantList audioTracks READ audioTracks NOTIFY streamChanged)
    // Stable volume: prefer the DRC track of the playing language (same-set switch). Per session:
    // stored as AppSettings playback/stableVolume (the Sound switch).
    Q_PROPERTY(bool stableVolume READ stableVolume WRITE setStableVolume NOTIFY streamChanged)
    // True only when the switch can change what plays: a separate audio stream whose playing
    // language has both a levelled (DRC) and a plain track. Show the row only then.
    Q_PROPERTY(bool stableVolumeAvailable READ stableVolumeAvailable NOTIFY streamChanged)
    // SEL-06 captions of the playing video, through the catalog component's caption models
    // (setCaptionSource): [{id, label, language, autoGenerated, translated}], real tracks first.
    Q_PROPERTY(QVariantList captionTracks READ captionTracks NOTIFY captionsChanged)
    Q_PROPERTY(QString captionTrackId READ captionTrackId NOTIFY captionsChanged)  // "" = off
    // The cue at the playing position ("" between cues); follows the position at the PositionPolicy
    // cadence (the 250 ms one while a track is on and the app is in the foreground).
    Q_PROPERTY(QString captionText READ captionText NOTIFY captionTextChanged)
    // SB-02 seek bar: the segments the skipper uses, [{startMs, endMs, category}] (category API name;
    // a poi_highlight has endMs == startMs).
    Q_PROPERTY(QVariantList sponsorSegments READ sponsorSegments NOTIFY sponsorSegmentsChanged)
    // Sleep timer: seconds until it pauses (-1 off; at the end of the video: the media time left).
    // Notified when set, cleared or fired, and at most once a second while it runs, only while the
    // app is in the foreground and something is bound to it (a single deadline otherwise).
    Q_PROPERTY(int sleepTimerSecondsLeft READ sleepTimerSecondsLeft NOTIFY sleepTimerChanged)
    Q_PROPERTY(bool sleepTimerAtEnd READ sleepTimerAtEnd NOTIFY sleepTimerChanged)
public:
    enum State { Idle, Loading, Preparing, Ready, Failed };  // = PlaybackController::State
    Q_ENUM(State)

    // Display metadata off the start path: resolve `videoId`'s details and call
    // done(errorCode, details) later (errorCode a resolver::ErrorCode, 0 = none).
    // Returns a cancel function. main.cpp wraps core's YtDlpVideoService::details
    // on its own runner (RES-02). Only summary fields, description, likes, date
    // and chapters are used; its formats never are.
    typedef std::function<void(int errorCode, const catalog::VideoDetails &details)> ResolveDone;
    typedef std::function<std::function<void()>(const QString &videoId, const ResolveDone &done)> MetadataSource;
    // SEL-02 network class of the next selection: 0 unmetered, 1 mobile, -1 unknown.
    typedef std::function<int()> NetworkProbe;

    // The shared stream source (Shared/Playback/StreamSource.h; in the app the
    // ThreadedStreamSource and its fetcher) must outlive the session.
    PlaybackSession(playback::StreamResolver *resolver, playback::StreamFetcher *fetcher, AppSettings *settings,
                    const media::DecoderCapabilities &capabilities, bool keepAliveAvailable, QObject *parent = nullptr);
    ~PlaybackSession() override;

    // Metadata for videos opened without a title hint (links, queue and autoplay
    // items), requested once after the first preroll, never before.
    void setMetadataSource(const MetadataSource &source) { metadata_ = source; }

    void setNetworkProbe(const NetworkProbe &probe) { networkProbe_ = probe; }
    // PLY-26 / DL-07 / SB-02: the download record of a video (DownloadManager::localMedia keys,
    // docs/ARCHITECTURE.md §8). open() plays an `available` record from the device instead of
    // resolving, unless the hints say preferStream: true.
    typedef std::function<QVariantMap(const QString &videoId)> LocalMediaLookup;
    // Also used for queue / autoplay / next / previous items (PlaybackController::setLocalAvailable).
    void setLocalMediaLookup(const LocalMediaLookup &lookup);
    // RES-14 [FULL]: whether the PO-token helper is ready now; requests carry poProvider only
    // then (default: never). main.cpp passes PoTokenProvider::ready.
    typedef std::function<bool()> PoProviderReady;
    void setPoProviderReady(const PoProviderReady &ready) { poProviderReady_ = ready; }
    // RES-21 keep-alive while rows are visible (rowsVisible): main.cpp passes
    // ThreadedStreamSource::warmUp.
    typedef std::function<void()> WarmUp;
    void setWarmUp(const WarmUp &warmUp) { warmUp_ = warmUp; }
    // E12 A/B switch for the speculative resolve on touch-down (default on; the environment
    // variable SAILTUBE_SPECULATIVE_RESOLVE=0 turns it off at start). Row prefetch is not affected.
    void setSpeculativeResolve(bool on);
    // MPR-01: thumbnails are cached as files in `directory` for mpris:artUrl.
    void setArtworkCache(QNetworkAccessManager *network, const QString &directory);
    // PLY-20: the per-device hardware-decode probe and its stored verdict. The session passes the
    // hardware-decode setting to it (the first enable probes) and feeds its verdict into the
    // selection capabilities (DecoderCapabilities::hardwareDecodeVerified); playback.diagnostics
    // shows its result line ("hardwareDecodeProbe"). Without a gate the constructor's
    // capabilities are used as they are. The gate must outlive the session or be reset first.
    void setHardwareDecodeGate(player::HardwareDecodeGate *gate);
    // NFR-PERF-01: the registry probe runs on the GStreamer start-up worker; its capabilities
    // arrive after construction (the hardware-decode verdict of the gate is kept).
    void setCapabilities(const media::DecoderCapabilities &capabilities);
    // Host tests: replace pulsesink / appsink.
    void setSinkOverrides(const player::SinkOverride &audio, const player::SinkOverride &video);
    // Host tests: shorter stall-watchdog thresholds (StallPolicy; 8 s / 4 s by default).
    void setStallConfig(const player::StallConfig &config) { controller_.setStallConfig(config); }
    player::VideoPlayer *player() const { return player_; }
    const PlaybackController &controller() const { return controller_; }

    QObject *mediaObject() const;
    QString videoId() const;
    QString title() const;
    QString channel() const;
    QString channelId() const;
    QString description() const;
    QString thumbnail() const;
    double viewCount() const;
    double likeCount() const;
    QString uploadDate() const;
    bool live() const;
    bool detailsLoaded() const { return controller_.detailsLoaded(); }
    QVariantList chapters() const;
    QVariantList qualities() const;
    int requestedHeight() const { return controller_.qualityChoice().height; }
    bool requestedHighFrameRate() const { return controller_.qualityChoice().highFrameRate; }
    QString streamLabel() const;
    bool audioOnly() const { return controller_.audioOnly(); }
    bool hasVideo() const { return controller_.hasVideo(); }
    QVariantMap diagnostics() const;
    int state() const { return controller_.state(); }
    QString statusText() const;
    QString errorText() const;
    bool retryAvailable() const { return controller_.retryAvailable(); }
    bool recovering() const;
    bool upcoming() const { return controller_.upcoming(); }
    double scheduledStart() const { return static_cast<double>(controller_.scheduledStartUnixMs()); }
    QString errorSettingKey() const;
    bool playing() const;
    bool buffering() const;
    double position() const;
    double duration() const;
    bool seekable() const;
    double rate() const { return controller_.rate(); }
    bool rateChangeable() const { return controller_.rateChangeable(); }
    bool keepDisplayOn() const { return controller_.keepDisplayOn(); }
    bool canPlay() const { return controller_.mprisCapabilities().canPlay; }
    bool canPause() const { return controller_.mprisCapabilities().canPause; }
    bool canSeek() const { return controller_.mprisCapabilities().canSeek; }
    bool canGoNext() const { return controller_.mprisCapabilities().canGoNext; }
    bool canGoPrevious() const { return controller_.mprisCapabilities().canGoPrevious; }
    QString artUrl() const;
    int queueLength() const { return controller_.queueLength(); }
    int queueIndex() const { return controller_.queueIndex(); }
    QVariantList localCaptions() const;
    bool canStreamInstead() const { return resolver_ && controller_.canStreamInstead(); }
    int coverMode() const { return static_cast<int>(controller_.coverState().mode); }
    int coverLeftAction() const { return static_cast<int>(controller_.coverState().left); }
    int coverRightAction() const { return static_cast<int>(controller_.coverState().right); }
    bool parked() const { return controller_.arbiter().parked(); }
    bool shortActive() const { return controller_.shortItem() && controller_.state() != PlaybackController::Idle; }
    bool shortHeld() const { return controller_.shortHeld(); }
    bool coverVideoActive() const { return coverVideoActive_; }
    double videoAspect() const { return videoAspect_; }
    QString upNextVideoId() const;
    int upNextSecondsLeft() const;
    QString sponsorNotice() const;
    QString sponsorButton() const;
    double highlightPosition() const;
    bool loop() const { return controller_.loop(); }
    double bufferedPosition() const;
    QVariantList audioTracks() const;
    bool stableVolume() const { return controller_.stableVolume(); }
    bool stableVolumeAvailable() const { return controller_.stableVolumeAvailable(); }
    QVariantList captionTracks() const;
    QString captionTrackId() const;
    QString captionText() const { return captionText_; }
    QVariantList sponsorSegments() const;
    int sleepTimerSecondsLeft() const { return controller_.sleepTimerSecondsLeft(now()); }
    bool sleepTimerAtEnd() const { return controller_.sleepTimerAtEnd(); }

    // SEL-06: the caption models (main.cpp: a CatalogCaptionSource over the catalog's captionTracks
    // and captionCues). Not owned; it must outlive the session or be reset first. Without one the
    // caption properties stay empty.
    void setCaptionSource(PlaybackCaptionSource *source);

    // hints: title/channel/thumbnail/durationSeconds already known from a list
    // row; startSeconds (a t= link, LNK-02) wins over a stored resume point.
    // PLY-26 / DL-07: a downloaded copy (setLocalMediaLookup, `available` record) plays from the
    // device with no resolve and no network request, starting at startSeconds; preferStream: true
    // (bool) streams anyway. A record that is not available (file_missing) streams as usual.
    // NFR-PERF-02: the resolve is dispatched before the previous video's player stops.
    // Shorts: loop: true (bool) makes a loop item (PlaybackController::loop(): at the end of stream
    // it restarts at 0 and plays on; no `ended`, no up-next, no related autoplay, queue untouched;
    // the end is saved once through positionSaveRequested; setStartPosition, the library bridge's
    // resume, is ignored, a startSeconds link start still applies). Re-opening the playing video
    // (another page re-attaching) keeps playing without a reload and keeps loop unless the new
    // hints carry `loop` as a bool. A preload made by likelyNext() for this id is used (no resolve,
    // the held stream jobs reused); an open of another id drops the preload.
    Q_INVOKABLE void open(const QString &videoId, const QVariantMap &hints = QVariantMap());
    // PLY-26: a DownloadsModel::localMedia() record (videoId optional). DL-07 / SB-02: the record's
    // segments feed the skipper (setSponsorSegments is ignored while they are in use), its
    // captions are localCaptions.
    Q_INVOKABLE void openLocal(const QVariantMap &media);
    // PLY-26 / SEL-07: the downloaded item playing switches to streaming the same video at the
    // current position, without stop() / open(): the session, its context owner, title and
    // metadata, the cover state and the mini-player stay (no Idle state, no `ended`). One resolve
    // like open()'s (resolve-start / resolve-done markers, speculation untouched) while the file
    // plays on, then one quality-switch style SEL-07 switch at the position captured then, play /
    // pause, rate and listen-only kept (switch-start kind=stream, switch-resumed). Afterwards a
    // stream item: quality menu, local captions and download segments flag cleared,
    // diagnostics.local false. A failed resolve keeps the file playing; errorText says
    // "Streaming is not available right now." for 5 s (state unchanged, errorSettingKey "").
    // No-op unless canStreamInstead.
    Q_INVOKABLE void streamInstead();
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void togglePlayback();
    // MPR-01 "Stop really stops": saves the position, ends the session,
    // releases the stream jobs and the audio resource.
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(double milliseconds);
    // height 0 = automatic; highFrameRate picks the >30 fps rung of that height.
    Q_INVOKABLE void setQuality(int height, bool highFrameRate = false);
    Q_INVOKABLE void setAudioOnlyMode(bool audioOnly);
    Q_INVOKABLE void retry();
    // Library component (LIB-05): where the next start of the opened video begins.
    // Ignored once playback has started, or when a link start is pending.
    Q_INVOKABLE void setStartPosition(double milliseconds);
    Q_INVOKABLE void setRate(double rate);  // PLY-07, remembered
    // PLY-10 watch contexts.
    Q_INVOKABLE int claimContext();
    Q_INVOKABLE void activateContext(int contextId);
    Q_INVOKABLE void closeContext(int contextId);
    // PLY-25 / MPR-01 queue.
    Q_INVOKABLE void setQueue(const QStringList &videoIds, int currentIndex);
    Q_INVOKABLE void clearQueue();
    Q_INVOKABLE void setRelatedCandidate(const QString &videoId);
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void cancelUpNext();
    // SB-02: SponsorSegmentsModel.segments ({start, end, category}, ms).
    Q_INVOKABLE void setSponsorSegments(const QVariantList &segments);
    Q_INVOKABLE void skipSponsor();
    // MPR-02 idle cover action.
    Q_INVOKABLE void continueWatching();
    // PLY-24: 250 ms while the controls show, 1 s otherwise.
    Q_INVOKABLE void setControlsVisible(bool visible);
    // PLY-09 freeze, PLY-18 foreground, PLY-12; main.cpp calls it on activation changes.
    Q_INVOKABLE void setAppActive(bool active);
    // Cover video: the cover reports `status === Cover.Active`. While visible (and the setting on,
    // a video plan, no error) the PLY-09 background freeze is lifted so the cover gets frames;
    // hidden again, the freeze applies as before (800 ms grace). Timing: cover-video-on / -off.
    Q_INVOKABLE void setCoverVisible(bool visible);
    // Cover video: the playback/coverVideo setting (main.cpp forwards it; default on in FULL,
    // always off in STORE, where a true is ignored).
    Q_INVOKABLE void setCoverVideoEnabled(bool enabled);
    bool coverVideoEnabled() const { return controller_.coverVideoSetting(); }
    // NFR-PERF-02 "first-frame": the watch page's VideoSurfaceItem.hasFrame
    // became true; the next window frameSwapped() prints the marker.
    Q_INVOKABLE void surfaceFrameReady();
    // QQuickWindow::frameSwapped (render thread; thread-safe, direct connection).
    void frameSwapped();

    // ---- E12 speculative resolve (RES-11 MAY) and row prefetch (RES-11, RES-21) ------------
    // Connected (direct, same thread) to the catalog component's tap intent signals. Bounds: one
    // speculative resolve at a time, never in the background, never within 2 s of a start
    // (speculationAllowed / rowsVisibleDecision in PlaybackController.h).
    // touchDown: a speculative resolve of `videoId` with the request open(videoId, hints) would
    // make (speculative = true); the latest press wins (an older one is cancelled). Marker
    // speculative-start run=<pressId>.
    Q_INVOKABLE void pressStarted(const QString &videoId, const QVariantMap &hints, int pressId);
    // touchCancelled: cancels a speculation still running (speculative-cancel); a completed one is
    // only forgotten here, its answer stays in the resolver's cache for its TTL. Never cancels
    // anything an open() already uses.
    Q_INVOKABLE void pressCancelled(const QString &videoId, int pressId);
    // opened, right before open(): the next open() of `videoId` reuses that press's in-flight or
    // completed answer instead of a second resolve when its request is the same (markers
    // speculative-used run=<pressId>, then resolve-done path=prefetch).
    Q_INVOKABLE void openingFromPress(const QString &videoId, int pressId);
    // likelyVisible (top first): on an unmetered network, a low-priority prefetch of the first row
    // and a keep-alive warmUp(); nothing on mobile data, in the background or while a play starts.
    Q_INVOKABLE void rowsVisible(const QStringList &videoIds);

    // ---- Shorts pager (DSC-17 MAY swipe viewer) ------------------------------------------
    // The Short the pager shows next: a speculative resolve with the request open(videoId, hints)
    // would make (speculative = true); when it answers, the branches of the plan open() would
    // start with (firstLoadFor) are opened through the fetcher and HELD, for at most
    // kLikelyNextHoldMs, until another likelyNext, an open of another id, Stop or the background.
    // The following open(videoId) reuses the answer (speculative-used run=<run> kind=likely, then
    // resolve-done path=prefetch) and the held stream jobs (no new fetcher open for them); no
    // second pipeline or decoder. One preloaded id at a time (a new id replaces the old: its
    // resolve is cancelled, its jobs closed). Nothing in the background, for the session's playing
    // id, for a downloaded id or an upcoming one; while a play starts it waits until that play has
    // prerolled. rowsVisible and pressStarted skip an id preloaded here. Call it after open() of
    // the current Short. Marker likely-next video=<id> decision=<start|defer|keep|skip> reason=.
    Q_INVOKABLE void likelyNext(const QString &videoId, const QVariantMap &hints = QVariantMap());

    // The pager's open of a Short: open(videoId, hints) with `loop: true` and `short: true`. A Short
    // loops, stores no resume point (history is kept), is never parked and is held (paused) while the
    // app is in the background (screen lock, cover) instead of playing on. The hint `short: true`
    // given to open() does the same for a caller that opens through open().
    Q_INVOKABLE void openShort(const QString &videoId, const QVariantMap &hints = QVariantMap());
    // The Shorts after the current one, next first (two at most; a third is ignored): the first as
    // likelyNext (speculative resolve + held stream jobs), the second only resolved into the cache
    // (no jobs, no bytes), so that a swipe to it, or one back to the Short just left, finds its
    // answer. Call it with the next two ids on every settled page; nothing in the background.
    // Marker shorts-ahead n=<ids> second=<id|->. While the playing item is a Short the first one's
    // stream jobs are held for 10 minutes (kShortsAheadHoldMs; 60 s for any other item), so the pager
    // may stay on a looping Short and the swipe still finds them; the answer itself stays in the
    // resolver's cache (about 20 minutes) after the jobs are closed.
    Q_INVOKABLE void shortsAhead(const QStringList &videoIds);
    // The pager's own reasons to pause the current Short ("tab": the Shorts tab was left,
    // "comments": the comments are open, any text). The first reason pauses; the last release
    // resumes it when it was playing before and the user has not chosen since. A new Short, Stop
    // and closing the page end every hold. Screen lock and the app leaving the foreground hold a
    // Short by themselves.
    Q_INVOKABLE void holdShort(const QString &reason);
    Q_INVOKABLE void releaseShort(const QString &reason);

    // ---- Watch overlay ------------------------------------------------------------------------
    // The overlay's loop toggle (property `loop`).
    Q_INVOKABLE void setLoop(bool on);
    // SEL-05 / SEL-07: plays the audioTracks entry `id` from the same set (marker switch-start
    // kind=dub; the picture and the video stream stay; no resolve unless the URLs are about to expire).
    // The language is remembered for later videos and sessions (the original = no preference) in the
    // AppSettings key playback/dubLanguage (remembered, no settings row).
    Q_INVOKABLE void setAudioTrack(const QString &id);
    Q_INVOKABLE void setStableVolume(bool on);
    // SEL-06: an explicit caption pick ("" = off), persisted by the caption models.
    Q_INVOKABLE void setCaptionTrack(const QString &id);
    // Sleep timer: minutes > 0, -1 = at the end of the current video, 0 = off. It pauses (never
    // stops): the session, the page and the position stay.
    Q_INVOKABLE void setSleepTimer(int minutes);
    // Hold-to-speed: true while the finger holds, rate 2.0 (pitch kept); false restores the rate
    // from before. Not remembered; no effect at the live edge.
    Q_INVOKABLE void holdSpeed(bool held);
    // Scrub preview at `positionMs`: {url, x, y, width, height} (the storyboard image and the cell
    // inside it, pixels), an empty map while no storyboard is known. A resolve answer that carries
    // one fills it (today the yt-dlp path; the fast path waits for SailTune, S-27), as does setStoryboard.
    Q_INVOKABLE QVariantMap scrubPreview(double positionMs) const;
    // The image URL of scrubPreview() ("" none); a sprite, so the cell rect is needed to show it.
    Q_INVOKABLE QString scrubPreviewUrl(double positionMs) const;
    // The playing video's storyboard ({width, height, columns, rows, fragments: [{url, durationMs}]},
    // storyboardFor), for a caller that has one from elsewhere; dropped with the video.
    Q_INVOKABLE void setStoryboard(const QVariantMap &storyboard);

signals:
    void mediaObjectChanged();
    void detailsChanged();
    void streamChanged();
    void stateChanged();
    void playerChanged();
    void positionChanged();
    void controlsChanged();
    void artChanged();
    void upNextChanged();
    void sponsorChanged();
    void queueChanged();
    void coverVideoActiveChanged();
    void videoAspectChanged();
    void loopChanged();
    void bufferedPositionChanged();
    void captionsChanged();
    void captionTextChanged();
    void sponsorSegmentsChanged();
    void sleepTimerChanged();
    void ended();  // natural end of stream (library history, LIB-04)
    // PLY-25: the next item replaces the watch page; the page calls
    // pageStack.replace() and the new page claims the context.
    void replacePageRequested(const QString &videoId);
    // PLT-07 / PLY-09: re-upload the held frame (VideoSurfaceItem::repaint).
    void repaintRequested();
    // PLY-10 / MPR-01: a stopped video's resume point (-1 = none).
    void positionSaveRequested(const QString &videoId, double positionMs);

private:
    // The effects of PlaybackController, executed on this session.
    struct Port final : PlaybackPort {
        explicit Port(PlaybackSession *session) : s(session) {}
        void resolve(const ResolveRequest &request) override;
        void cancelResolve() override;
        void prefetch(const ResolveRequest &request) override;
        void load(const PlaybackLoad &load) override;
        void play() override;
        void pause() override;
        void stopPlayer() override;
        void seek(std::int64_t positionMs) override;
        void postSeek(std::int64_t positionMs) override;
        void setRate(double rate) override;
        void acquireAudio() override;
        void releaseAudio() override;
        void savePosition(const std::string &videoId, std::int64_t positionMs) override;
        void rememberRate(double rate) override;
        bool progress(PlayerProgress *progress) override;
        PlaybackSession *s;
    };
    friend struct Port;

    // One branch of a load: the remote format and its fetcher handle (RES-16..19).
    struct Stream {
        quint64 handle = 0;            // 0 = not through the fetcher (HLS)
        std::string remote;            // the set URL (lookup key, never logged)
        playback::StreamFormat format;
        QUrl local;                    // what the player opens
        bool ready = false;
    };
    // A load waiting for its branches to open; player calls made meanwhile are kept.
    struct PendingLoad {
        bool active = false;
        PlaybackLoad load;
        int branches[3] = {-1, -1, -1};   // video, audio, muxed -> index in streams_ (-1 none)
        int waiting = 0;
        bool play = false;
    };

    // Shorts likelyNext: the one preloaded next item (token 0 = empty slot).
    struct Likely {
        quint64 token = 0;
        std::string videoId;
        QByteArray key;            // playback::streamCacheKey of its request (RES-10 key)
        media::PlaybackPreferences preferences;  // with the mode open() would use
        quint64 op = 0;            // resolver operation in flight, 0 none
        bool done = false;         // answered; `set` holds the answer
        playback::StreamSet set;
        bool attached = false;     // an open's resolve waits for it (resolveOp_ == op)
        quint64 resolveToken = 0;
        unsigned generation = 0;
        std::vector<Stream> held;  // the planned branches, opened through the fetcher
    };

    // E12: the one speculative resolve of a press (token 0 = empty slot).
    struct Speculation {
        quint64 token = 0;
        int pressId = 0;
        std::string videoId;
        QByteArray key;            // playback::streamCacheKey of its request (RES-10 key)
        quint64 op = 0;            // resolver operation in flight, 0 none
        bool done = false;         // answered; `set` holds the answer
        playback::StreamSet set;
        bool claimed = false;      // openingFromPress named it: the next open() may use it
        bool attached = false;     // an open's resolve waits for it (resolveOp_ == op)
        quint64 resolveToken = 0;  // that resolve's token
        unsigned generation = 0;
    };

    qint64 now() const { return clock_.elapsed(); }
    void onStreams(const std::string &videoId, unsigned generation, const playback::StreamSet &set,
                   const playback::StreamError &error, bool fromSpeculation = false);
    void cancelResolveOp();
    bool takeSpeculation(const playback::StreamRequest &request, const std::string &videoId, unsigned generation,
                         quint64 token);
    void speculationDone(quint64 token, const playback::StreamSet &set, const playback::StreamError &error);
    void dropSpeculation();
    media::PlaybackPreferences selectionPreferences(int *networkClass) const;
    playback::StreamRequest speculativeRequest(const std::string &videoId, bool audioOnly,
                                               const media::PlaybackPreferences &preferences, int networkClass) const;
    SpeculationContext speculationContext(int networkClass) const;
    catalog::VideoDetails detailsFor(const playback::StreamSet &set) const;
    void openBranches(const PlaybackLoad &load);
    void branchOpened(quint64 generation, int index, const QUrl &local, const playback::StreamError &error);
    void startPlayer(const PlaybackLoad &load);
    void parkStreams();
    void closeParked();
    void closeStreams(std::vector<Stream> *streams);
    void branchFailed(int branch, bool network, const QString &reason, int httpStatus);
    void requestMetadata();
    QVariant hint(const char *key, const QVariant &fallback = QVariant()) const;
    void applySettings();
    void applyHardwareVerdict();
    void prepareSelection();
    void sync();
    void rearm();
    void updateCover();
    void updateAspect();
    double decodedAspect() const;
    void playerStateChanged();
    void fetchArtwork();
    void deferred(const std::function<void()> &f);
    QString run() const { return QStringLiteral("run=%1").arg(run_); }
    void openLocalRecord(const QVariantMap &media, qint64 startMs, int loopHint, int shortHint = 0);
    void prepareShortCache(const std::string &videoId);
    bool localCopy(const QString &videoId, const QVariantMap &hints, QVariantMap *record) const;
    bool poProviderReady() const { return poProviderReady_ && poProviderReady_(); }
    // Shorts likelyNext.
    void startLikely(const std::string &videoId, const QVariantMap &hints);
    void likelyAnswered(quint64 token, const playback::StreamSet &set, const playback::StreamError &error);
    void likelyStreamOpened(quint64 handle, const QUrl &local, const playback::StreamError &error);
    bool takeLikely(const playback::StreamRequest &request, const std::string &videoId, unsigned generation, quint64 token);
    void dropLikely(const char *reason);
    void dropAdopted();
    void runPendingLikely();
    QString net() const;
    // Watch overlay.
    void captionsUpdated();
    void updateCaptionText();
    void updatePositionCadence();
    bool sleepTimerShown() const;
    bool stallRecovering() const;

    playback::StreamResolver *resolver_ = nullptr;
    playback::StreamFetcher *fetcher_ = nullptr;
    MetadataSource metadata_;
    std::function<void()> cancelMetadata_;
    QString metadataVideoId_;
    quint64 metadataToken_ = 0;
    QPointer<AppSettings> settings_;
    QPointer<player::HardwareDecodeGate> hwGate_;
    media::DecoderCapabilities capabilities_;
    std::string lastLoadedVideoId_;   // the video of the player's current build (keepFrame)
    Port port_;
    PlaybackController controller_;
    player::VideoPlayer *player_ = nullptr;
    player::AudioResourceGate *gate_ = nullptr;
    NetworkProbe networkProbe_;
    LocalMediaLookup localLookup_;
    PoProviderReady poProviderReady_;
    QVariantList localCaptions_;      // DL-07: the playing download's saved captions
    bool localSegments_ = false;      // SB-02: the playing download's saved segments are in use
    bool localSidecarsPending_ = false;
    int queueLength_ = -1;            // last notified queueLength / queueIndex
    int queueIndex_ = -1;
    int networkClass_ = -1;
    quint64 resolveOp_ = 0;          // operation in flight, 0 none
    quint64 resolveToken_ = 0;
    std::string formatsVideoId_;
    std::map<std::string, playback::StreamFormat> formats_;  // set URL -> format (this video's answers)
    playback::ResolvePath path_ = playback::ResolvePath::Unknown;
    bool pathFromSpeculation_ = false;  // the playing set came from a press speculation (E12)
    Speculation spec_;
    quint64 specTokens_ = 0;
    SpeculationStats specStats_;
    RowPrefetchState rows_;
    qint64 lastStartMs_ = -1;           // the last start's resolve or load (E12 quiet window)
    bool speculativeResolve_ = true;
    WarmUp warmUp_;
    Likely likely_;
    quint64 likelyTokens_ = 0;
    QTimer likelyHold_;              // kLikelyNextHoldMs, single shot while jobs are held
    std::string likelyPendingId_;    // deferred while a play starts
    QVariantMap likelyPendingHints_;
    std::vector<Stream> adopted_;    // the used preload's held jobs, for the next load
    std::string lastResolveVideoId_; // an open of another id drops the preload
    int likelyStarted_ = 0, likelyUsed_ = 0, likelyWasted_ = 0;
    int shortsAheadCalls_ = 0, shortsAheadCached_ = 0;
    std::vector<std::string> shortCacheIds_;  // the last ids resolved into the cache for a Short two ahead
    bool loopNotified_ = false;
    std::vector<Stream> streams_;    // the current load's branches
    std::vector<Stream> parked_;     // the previous load's, reused by the next load (SEL-07)
    PendingLoad pending_;
    int firstBytes_ = 0;             // bit per player::Branch with a first byte in this load
    int refusedReports_ = 0;
    quint64 loadGeneration_ = 0;
    bool startedReported_ = false;
    QVariantMap hints_;
    QString hintsVideoId_;           // hints belong to this video only
    qint64 linkStartMs_ = -1;
    player::SinkOverride audioSink_, videoSink_;
    QPointer<QNetworkAccessManager> network_;
    QString artworkDir_;
    QString artPath_;
    QString artVideoId_;
    QElapsedTimer clock_;
    QTimer deadline_;
    int run_ = 0;
    int frameMarkedRun_ = 0;
    std::atomic<int> awaitingSwapRun_{0};
    int seekCount_ = 0;
    int switchCount_ = 0;
    bool syncing_ = false;
    // Cover video.
    bool coverVideoActive_ = false;
    bool coverKeepsDecoding_ = false;  // last value sent to VideoPlayer::setCoverVisible
    CoverDecodeMeter coverMeter_;
    // videoAspect: the last notified value and the decoded picture's, per load generation.
    double videoAspect_ = 0.0;
    double decodedAspect_ = 0.0;
    quint64 decodedGeneration_ = 0;
    // Watch overlay.
    PlaybackCaptionSource *captionSource_ = nullptr;
    QString captionText_;
    QString captionTrackNotified_;
    bool controlsVisible_ = true;      // the page's setControlsVisible (PLY-24)
    bool positionFast_ = true;         // last value sent to VideoPlayer::setControlsVisible
    int sleepSecondsNotified_ = -1;
    Storyboard storyboard_;
    std::string storyboardVideoId_;
    // PLY-22: the settings' boost and loudness normalisation, and the loudness of the last answer.
    player::AudioSettings audioSettings_;
    player::StreamLoudness loudness_;
    std::string loudnessVideoId_;
    player::StreamLoudness loudnessFor(const std::string &videoId) const;
};

}}}
