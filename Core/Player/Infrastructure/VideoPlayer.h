// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The QML-facing player (PLY-01): one GstEngine plus every Domain policy,
// wired together. Plays dual DASH, muxed progressive, HLS, local files and
// audio-only with the same object. Qt Core + GStreamer only, no SailTube
// types; register it as a QML type from the app (qmlRegisterType) and point a
// VideoSurfaceItem's `source` at it (VideoFrameProvider).
//
// Policies and where they act:
//   PrerollBarrier (PLY-06/07)  start position + rate applied before the first frame
//                               is decoded (start seek from the held decoders, no
//                               preroll at 0); fallback: held until the first
//                               async-done, applied as ONE rate-carrying flush seek
//   SeekPolicy (PLY-03/04)      per-sink seeks; ISO-BMFF audio aligned to the
//                               landed video position after async-done
//   SplitSeekGuard (PLY-05)     resend after 250 ms, one rebuild, then give up
//   speed:: (PLY-07)            rate snapping, live-edge lock
//   BufferingPolicy (PLY-08)    every play/pause decision goes through it; the first
//                               preroll never waits for a buffer level (start phase)
//   FreezePolicy (PLY-09)       one single-shot timer, no periodic timer
//   PositionPolicy (PLY-24)     poll interval, landed position, EOS duration
//   DecodeModePolicy (PLY-20)   3 s first-frame watchdog on the hardware path
//   AudioChain (PLY-22)         live EQ / normalisation / boost / limiter

#include "Core/Player/Domain/AudioChain.h"
#include "Core/Player/Domain/BufferingPolicy.h"
#include "Core/Player/Domain/FreezePolicy.h"
#include "Core/Player/Domain/PipelinePlan.h"
#include "Core/Player/Domain/PositionPolicy.h"
#include "Core/Player/Domain/PrerollBarrier.h"
#include "Core/Player/Domain/SeekPolicy.h"
#include "Core/Player/Domain/SplitSeekGuard.h"
#include "GstEngine.h"
#include "VideoFrameMailbox.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantMap>

#include <cstdint>
#include <set>
#include <string>

namespace nordtile { namespace player {

// Everything one playback needs; PlaybackSession fills it.
struct PlayerSources {
    PlayMode mode = PlayMode::Dual;
    SourceSpec video, audio, muxed;    // see PlanInput for which mode reads which
    std::string videoId;               // decode-fallback memory, diagnostics
    int64_t startPositionMs = 0;       // resume / captured switch position
    double rate = 1.0;                 // remembered speed
    DecodeMode decode = DecodeMode::Software;  // PLY-21 default
    AudioSettings audioSettings;
    StreamLoudness loudness;
    std::string mediaRole = kDefaultMediaRole;  // PLT-12
    int64_t metadataDurationMs = 0;    // fallback duration for EOS (PLY-24)
    bool liveHls = false;              // HLS at the live edge (speed locked, live buffering)
    bool autoplay = true;              // play once prerolled
    // Quality switch / stream instead of a local file: load() keeps the mailbox's last frame (no
    // clear, the surface keeps its picture and hasFrame) until the new build's first frame
    // replaces it. Nothing of the old pipeline is posted after the switch.
    bool keepFrame = false;
    SinkOverride audioSink, videoSink; // host tests (fakesink)
    // Factories to treat as absent (host degrade tests, diagnostics A/B).
    std::set<std::string> unavailableElements;
};

struct PlayerDiagnostics {
    int rebuilds = 0;          // PLY-05 split-seek rebuilds
    int seeksSent = 0;         // seek events sent (every sink counts)
    int resends = 0;           // PLY-05 250 ms resends
    int decodeFallbacks = 0;   // PLY-20 watchdog / error fallbacks
    int coverFramesDrawn = 0;  // cover video: frames a reduced-rate surface (CoverVideoItem) drew
    EngineStats engine;
};

class VideoPlayer final : public QObject, public VideoFrameProvider {
    Q_OBJECT
    Q_INTERFACES(nordtile::player::VideoFrameProvider)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool buffering READ buffering NOTIFY bufferingChanged)
    Q_PROPERTY(int bufferingPercent READ bufferingPercent NOTIFY bufferingChanged)
    Q_PROPERTY(double position READ position NOTIFY positionChanged)
    Q_PROPERTY(double duration READ duration NOTIFY durationChanged)
    // Seek bar: how far the media is buffered in the playing timeline (ms; dual: the shorter branch;
    // local files: the duration), 0 when unknown. Refreshed with the position poll (PositionPolicy).
    Q_PROPERTY(double bufferedPosition READ bufferedPosition NOTIFY bufferedPositionChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(double rate READ rate NOTIFY rateChanged)
    Q_PROPERTY(bool ended READ ended NOTIFY endedChanged)
    Q_PROPERTY(bool hasVideo READ hasVideo NOTIFY hasVideoChanged)
    Q_PROPERTY(bool frozen READ frozen NOTIFY frozenChanged)
    Q_PROPERTY(int videoWidth READ videoWidth NOTIFY videoSizeChanged)
    Q_PROPERTY(int videoHeight READ videoHeight NOTIFY videoSizeChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY errorTextChanged)
    Q_PROPERTY(int decodeMode READ decodeMode NOTIFY decodeModeChanged)
public:
    enum State { Idle, Loading, Paused, Playing, Ended, Failed };
    Q_ENUM(State)

    // Makes no GStreamer call (NFR-PERF-01: may be constructed while
    // GstEngine::startInitialize() is still running).
    explicit VideoPlayer(QObject *parent = nullptr);
    ~VideoPlayer() override;

    // PLY-01: (re)start playback of `sources`; tears down any previous one.
    // NFR-PERF-01: first waits for GStreamer (GstEngine::waitInitialized).
    void load(const PlayerSources &sources);

    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    // stop() that keeps the mailbox's last frame (keepFrame true) for a load() that follows.
    void stop(bool keepFrame);
    Q_INVOKABLE void seek(double ms);          // a user seek (PLY-03/05/06)
    // PLY-06: the start position of the current load, when it is only known after load() (the
    // library bridge sets it right after a local load starts). Before the first frame / preroll
    // it replaces the pending start (the start seek before preroll still applies, so nothing at
    // 0 is shown); afterwards it does nothing (use seek()).
    Q_INVOKABLE void setStartPosition(qint64 ms);
    Q_INVOKABLE void setRate(double rate);     // PLY-07
    Q_INVOKABLE void setAppActive(bool active);  // PLY-09
    // Cover video: the home-screen cover shows this player's picture (the caller decides: cover
    // visible, setting on, a video plan, no error). While true and the app is inactive the PLY-09
    // freeze is lifted (FreezePolicy::onCoverVisible); false = the freeze applies as before.
    Q_INVOKABLE void setCoverVisible(bool visible);
    // Cover video diagnostics: a reduced-rate surface (CoverVideoItem) drew one frame of this
    // player's mailbox (GUI thread). Counted in diagnostics().coverFramesDrawn.
    Q_INVOKABLE void noteCoverFrameDrawn() { ++diag_.coverFramesDrawn; }
    Q_INVOKABLE void setControlsVisible(bool visible);    // PLY-24
    Q_INVOKABLE void setPositionConsumer(bool consumer);  // PLY-24
    // PLY-22 live audio. Keys: eq (list of 10 dB), eqPreset (key, used when
    // eq is absent), normalization (bool), normalizationMaxBoostPercent,
    // boostPercent, loudnessDb (presence = known).
    Q_INVOKABLE void setAudioSettings(const QVariantMap &settings);
    void setAudioSettings(const AudioSettings &settings, const StreamLoudness &loudness);
    // What the chain plays with right now (PLY-22): EQ gains, loudness and boost gain, limiter.
    const AudioChainValues &audioValues() const { return audioValues_; }

    State state() const { return state_; }
    bool playing() const { return state_ == Playing; }
    bool buffering() const { return buffering_.current().showIndicator; }
    int bufferingPercent() const { return bufferingPercent_; }
    double position() const { return double(position_.positionMs()); }
    double duration() const { return double(position_.durationMs()); }
    double bufferedPosition() const { return double(bufferedMs_); }
    bool seekable() const { return seekable_; }
    double rate() const { return rate_; }
    bool ended() const { return position_.ended(); }
    bool hasVideo() const { return hasVideo_; }
    bool frozen() const { return freeze_.frozen(); }
    int videoWidth() const { return videoWidth_; }
    int videoHeight() const { return videoHeight_; }
    QString errorText() const { return errorText_; }
    int decodeMode() const { return int(effectiveDecode_); }

    VideoFrameMailbox *frameMailbox() override { return &mailbox_; }
    // PLY-20: the zero-copy sink (droideglsink) of a hardware build, else null
    // (diagnostics, the hardware probe). Its frames reach VideoSurfaceItem
    // through frameMailbox()->hardwareSource().
    GstElement *hardwareVideoSink() const { return engine_.hardwareVideoSink(); }
    PlayerDiagnostics diagnostics() const;
    const PipelinePlan &plan() const { return engine_.plan(); }
    // Position of one sink (diagnostics / A/V tests); Branch::Muxed = pipeline.
    bool sinkPosition(Branch branch, qint64 *ms) const { return engine_.queryPosition(branch, ms); }
    void resetEngineStats() { engine_.resetStats(); }
    // For a stall watchdog above the player: a look at the pipeline that needs no position poll,
    // which stops while the pipeline is paused for buffering, exactly when a watchdog must know
    // whether bytes still arrive. positionMs: the furthest position a sink reports (never a seek
    // target or the end-of-stream duration, which position() shows); bufferedMs: bufferedPosition
    // computed now (0 unknown). Emits nothing and leaves the reported position alone. False
    // without a pipeline.
    bool queryProgress(qint64 *positionMs, qint64 *bufferedMs) const;
    // A seek is on its way (sent, queued behind one, or waiting for its landing): position()
    // reports its target meanwhile, so a standing position says nothing.
    bool seeking() const { return seekInFlight_ || pendingSeek_ || position_.seeking(); }

signals:
    void stateChanged();
    void playingChanged();
    void bufferingChanged();
    void positionChanged();
    void durationChanged();
    void bufferedPositionChanged();
    void seekableChanged();
    void rateChanged();
    void endedChanged();
    void hasVideoChanged();
    void frozenChanged();
    void videoSizeChanged();
    void errorTextChanged();
    void decodeModeChanged();

    void firstFrame();
    void endOfStream();
    // httpStatus: the HTTP status of a refused source request (RES-07: 403 at byte 0), 0 unknown.
    void failed(const QString &reason, bool network, int branch, int httpStatus);
    void decodeFallback(const QString &reason);
    void repaintRequested();  // FreezePolicy: the surface calls repaint()
    // Markers: load, preroll, first-frame, seek-start, seek-done, rebuild,
    // fallback, ended; start-up split (NFR-PERF): "demux-ready branch=<b>",
    // "start-seek branch=<b>", "first-decoded branch=video"; a marker may carry
    // space-separated key=value after its name (print `<name> t=<ms> <keys>`).
    // "teardown-done": the old pipeline reached NULL and was freed; its ms are
    // since that teardown (stop() / the next load()), not since load().
    void timing(const QString &marker, qint64 msSinceLoad);
    // NFR-PERF timing: the first media byte of a branch reached the pipeline
    // ("video", "audio" or "muxed"), ms since load(). Print as `first-byte t=.. branch=..`.
    void firstByte(const QString &branch, qint64 msSinceLoad);

private:
    void startPipeline(int64_t positionMs, double rate);
    void teardown(bool clearFrame);
    void executeSeek(int64_t targetMs, double rate);
    void handleGuard(SplitSeekAction action);
    void finishSeek();
    bool seekReachedSinks() const;
    void markStarted();
    void applyPipelineState(bool force = false);
    void applyFreeze(const FreezeAction &action);
    void armFreezeTimer();
    void updatePollTimer();
    void pollPosition();
    void refreshDuration();
    void refreshBuffered();
    int64_t computeBuffered() const;
    void setBuffered(int64_t ms);
    void fallBackToSoftware(const QString &reason);
    void armHardwareWatchdog();
    void fail(const QString &reason, bool network, int branch, int httpStatus = 0);
    void setState(State s);
    void refreshState();
    void mark(const char *marker);
    void mark(const QString &marker);
    // PLY-06 start seek before preroll.
    void armStartSeek(bool provisional = false);
    void onDecoderHeld(int branch);
    void onStartSegment(int branch, quint32 seqnum, qint64 startMs);
    void sendStartSeeks();
    void failStartSeek(const char *why);
    Branch positionBranch() const;
    Container videoContainer() const;
    Container audioContainer() const;
    int64_t nowMs() const { return clock_.elapsed(); }

    void onAsyncDone();
    void onEos();
    void onError(const QString &message, const QString &debug, int branch, bool network, int httpStatus);
    void onBuffering(int branch, int percent);
    void onPipelineState(int state);
    void onFirstVideoFrame();

    VideoFrameMailbox mailbox_;
    GstEngine engine_;

    PrerollBarrier barrier_;
    SplitSeekGuard guard_;
    BufferingPolicy buffering_;
    FreezePolicy freeze_;
    PositionPolicy position_;
    DecodeModePolicy decodePolicy_;

    PlayerSources sources_;
    bool loaded_ = false;
    AudioChainValues audioValues_;
    DecodeMode effectiveDecode_ = DecodeMode::Software;
    std::set<std::string> softwareOnly_;  // PLY-20: videos that fell back

    State state_ = Idle;
    double rate_ = 1.0;
    bool started_ = false;       // first preroll (and the barrier seek) done
    bool pipelinePlaying_ = false;
    int requestedState_ = -1;    // last PipelineState asked for
    bool seekable_ = false;
    bool hasVideo_ = false;
    int bufferingPercent_ = 100;
    int64_t bufferedMs_ = 0;   // bufferedPosition
    int videoWidth_ = 0, videoHeight_ = 0;
    QString errorText_;
    bool controlsVisible_ = true;
    bool positionConsumer_ = true;
    bool watchdogArmedThisBuild_ = false;
    bool sawFirstFrame_ = false;

    // Seek in flight.
    bool seekInFlight_ = false;
    SeekPlan seekPlan_;
    bool alignSent_ = false;
    bool landedKnown_ = false;
    int64_t landedMs_ = 0;
    bool pendingSeek_ = false;
    int64_t pendingSeekMs_ = 0;
    int64_t lastSeekMs_ = 0;
    quint32 videoSeqnum_ = 0, audioSeqnum_ = 0;  // accepted per-sink seeks in flight
    quint32 pipelineSeqnum_ = 0;                 // accepted pipeline-level seek
    bool guardReportPending_ = false;            // "both accepted" awaits both segments

    // PLY-06 start seek before preroll (slots: 0 video, 1 audio, 2 muxed branch).
    bool startHoldArmed_ = false;   // engine asked to hold the decoders
    bool startFailed_ = false;      // fell back to the barrier seek at the first async-done
    bool startAnySent_ = false;
    int64_t startSentMs_ = 0;       // what the start seek carries
    double startSentRate_ = 1.0;
    bool startHeld_[3] = {false, false, false};
    bool startSent_[3] = {false, false, false};
    quint32 startSeq_[3] = {0, 0, 0};
    bool startLandedKnown_[3] = {false, false, false};
    int64_t startLandedMs_[3] = {0, 0, 0};

    PlayerDiagnostics diag_;

    QElapsedTimer clock_;      // monotonic time for the policies
    QElapsedTimer loadClock_;  // timing() markers
    QTimer freezeTimer_;       // FreezePolicy deadline (single shot)
    QTimer pollTimer_;         // PositionPolicy interval
    QTimer resendTimer_;       // SplitSeekGuard 250 ms
    QTimer watchdogTimer_;     // DecodeModePolicy 3 s
    QTimer seekTimeoutTimer_;  // safety: a seek that never reports landing
};

} } // namespace nordtile::player
