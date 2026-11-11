// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "VideoPlayer.h"

#include "Core/Player/Domain/SpeedPolicy.h"
#include "Core/Player/Domain/SwitchPlan.h"

#include <QVariantList>

#include <cstdio>
#include <vector>

namespace nordtile { namespace player {

namespace {
bool debugEnabled() {
    static const bool on = qEnvironmentVariableIsSet("SAILTUBE_PLAYER_DEBUG");
    return on;
}
const int kSeekLandTimeoutMs = 3000;
int slotOf(Branch b) { return b == Branch::Video ? 0 : b == Branch::Audio ? 1 : 2; }
const char *branchName(Branch b) { return b == Branch::Video ? "video" : b == Branch::Audio ? "audio" : "muxed"; }
}

VideoPlayer::VideoPlayer(QObject *parent) : QObject(parent), engine_(&mailbox_) {
    clock_.start();
    loadClock_.start();
    freezeTimer_.setSingleShot(true);
    resendTimer_.setSingleShot(true);
    watchdogTimer_.setSingleShot(true);
    watchdogTimer_.setTimerType(Qt::PreciseTimer);  // a coarse timer may fire up to 5 % early
    connect(&freezeTimer_, &QTimer::timeout, this, [this]() { applyFreeze(freeze_.onTimer(nowMs())); });
    connect(&pollTimer_, &QTimer::timeout, this, [this]() { pollPosition(); });
    connect(&resendTimer_, &QTimer::timeout, this, [this]() {
        if (!engine_.built()) return;
        ++diag_.resends;
        executeSeek(lastSeekMs_, rate_);
    });
    seekTimeoutTimer_.setSingleShot(true);
    connect(&seekTimeoutTimer_, &QTimer::timeout, this, [this]() {
        // A sink that accepted the seek never got its segment: it did not
        // move (some upstreams return TRUE without seeking). For the split
        // guard that is a refusal. Report what landed rather than wait.
        if (!seekInFlight_) return;
        const bool v = !videoSeqnum_ || engine_.sinkSawSegment(Branch::Video, videoSeqnum_);
        const bool a = !audioSeqnum_ || engine_.sinkSawSegment(Branch::Audio, audioSeqnum_);
        if (debugEnabled()) std::fprintf(stderr, "player: seek landing timeout (video %d audio %d)\n", v, a);
        const bool report = guardReportPending_;
        guardReportPending_ = false;
        finishSeek();
        if (report) handleGuard(alignSent_ ? guard_.onAudioAlignResult(a) : guard_.onSeekResult(v, a));
    });
    connect(&watchdogTimer_, &QTimer::timeout, this, [this]() {
        if (decodePolicy_.tick(nowMs()) == DecodeEvent::FallbackToSoftware)
            fallBackToSoftware(QStringLiteral("no video frame within 3 s from the hardware decoder"));
        else if (decodePolicy_.watchdogArmed())
            watchdogTimer_.start(5);  // woke a little early: the policy decides at 3 s
    });
    connect(&engine_, &GstEngine::asyncDone, this, [this]() { onAsyncDone(); });
    connect(&engine_, &GstEngine::endOfStream, this, [this]() { onEos(); });
    connect(&engine_, &GstEngine::errorOccurred, this,
            [this](const QString &m, const QString &d, int b, bool n, int status) { onError(m, d, b, n, status); });
    connect(&engine_, &GstEngine::bufferingPercent, this, [this](int b, int p) { onBuffering(b, p); });
    connect(&engine_, &GstEngine::pipelineStateChanged, this, [this](int s) { onPipelineState(s); });
    connect(&engine_, &GstEngine::firstVideoFrame, this, [this]() { onFirstVideoFrame(); });
    connect(&engine_, &GstEngine::firstByte, this, [this](int branch) {
        const Branch b = static_cast<Branch>(branch);
        // PLY-20: a hardware decoder that stalls never prerolls, so PLAYING never comes; the 3 s
        // count also starts once the picture's data reached the pipeline.
        if (b == Branch::Video || b == Branch::Muxed) armHardwareWatchdog();
        emit firstByte(b == Branch::Video ? QStringLiteral("video") : b == Branch::Audio ? QStringLiteral("audio")
                                                                                          : QStringLiteral("muxed"),
                       loadClock_.elapsed());
    });
    // PLY-20: frames the renderer cannot show are never a silent black picture.
    connect(&engine_, &GstEngine::hardwareRenderFailed, this, [this](const QString &reason) {
        if (effectiveDecode_ == DecodeMode::Hardware)
            fallBackToSoftware(QStringLiteral("hardware frames cannot be shown: %1").arg(reason));
    });
    connect(&engine_, &GstEngine::videoSizeChanged, this, [this](int w, int h) {
        videoWidth_ = w;
        videoHeight_ = h;
        emit videoSizeChanged();
    });
    // PLY-06 start seek before preroll.
    connect(&engine_, &GstEngine::decoderHeld, this, [this](int branch) { onDecoderHeld(branch); });
    connect(&engine_, &GstEngine::branchNotHeld, this, [this](int) { failStartSeek("a branch could not be held"); });
    connect(&engine_, &GstEngine::startSegment, this,
            [this](int branch, quint32 seqnum, qint64 ms) { onStartSegment(branch, seqnum, ms); });
    // NFR-PERF start-up split.
    connect(&engine_, &GstEngine::demuxReady, this, [this](int branch) {
        mark(QStringLiteral("demux-ready branch=%1").arg(QLatin1String(branchName(static_cast<Branch>(branch)))));
    });
    connect(&engine_, &GstEngine::firstDecodedVideo, this, [this]() { mark("first-decoded branch=video"); });
    connect(&engine_, &GstEngine::teardownDone, this,
            [this](qint64 ms) { emit timing(QStringLiteral("teardown-done"), ms); });
    connect(&engine_, &GstEngine::clockLost, this, [this]() {
        if (requestedState_ == int(PipelineState::Playing)) {
            engine_.setState(PipelineState::Paused);
            engine_.setState(PipelineState::Playing);
        }
    });
    // NFR-PERF-01: no GStreamer call here, so the player can be constructed while
    // GstEngine::startInitialize() still runs; load() waits for it.
}

VideoPlayer::~VideoPlayer() { teardown(true); }

// ---- Loading ---------------------------------------------------------------------

void VideoPlayer::load(const PlayerSources &sources) {
    // NFR-PERF-01: GStreamer may still be initialising on the start-up worker (a tap right
    // after launch); everything below needs it. A failure surfaces through build().
    GstEngine::waitInitialized(GstEngine::kInitWaitMs);
    teardown(!sources.keepFrame);  // keepFrame: the old picture stays until the new first frame
    sources_ = sources;
    loaded_ = true;
    loadClock_.restart();
    mark("load");
    errorText_.clear();
    emit errorTextChanged();
    audioValues_ = computeAudioChainValues(sources.audioSettings, sources.loudness);
    const bool liveEdge = sources.mode == PlayMode::Hls && sources.liveHls;
    rate_ = speed::rateToApply(sources.rate, liveEdge);
    emit rateChanged();
    std::string key = sources.video.uri + "|" + sources.audio.uri + "|" + sources.muxed.uri;
    guard_.onSourcesSet(key);
    resendTimer_.stop();
    position_.reset();
    position_.setMetadataDurationMs(sources.metadataDurationMs);
    setBuffered(0);
    buffering_.onUserIntent(sources.autoplay);
    startPipeline(sources.startPositionMs, rate_);
}

void VideoPlayer::startPipeline(int64_t positionMs, double rate) {
    teardown(false);
    PlanInput in;
    in.mode = sources_.mode;
    in.video = sources_.video;
    in.audio = sources_.audio;
    in.muxed = sources_.muxed;
    in.decode = softwareOnly_.count(sources_.videoId) ? DecodeMode::Software : sources_.decode;
    in.available = GstEngine::availableElements();
    for (std::set<std::string>::const_iterator it = sources_.unavailableElements.begin();
         it != sources_.unavailableElements.end(); ++it)
        in.available.erase(*it);
    in.audioValues = audioValues_;
    in.mediaRole = sources_.mediaRole;
    in.audioSink = sources_.audioSink;
    in.videoSink = sources_.videoSink;
    QString error;
    if (!engine_.build(in, &error)) {
        fail(error, false, -1);
        return;
    }
    const PipelinePlan &plan = engine_.plan();
    // PLY-20 per-build fallback (an element missing): software, with a notice.
    if (!plan.hardwareFallbackReason.empty())
        emit decodeFallback(QString::fromStdString(plan.hardwareFallbackReason));
    if (effectiveDecode_ != plan.decode) {
        effectiveDecode_ = plan.decode;
        emit decodeModeChanged();
    }
    if (hasVideo_ != plan.hasVideo()) {
        hasVideo_ = plan.hasVideo();
        emit hasVideoChanged();
    }
    barrier_.arm(positionMs, rate);
    // PLY-06: nothing is shown before the barrier seek has landed.
    engine_.setVideoOutputEnabled(!(positionMs > 0 || rate != 1.0));
    if (positionMs > 0) {  // show the start position until the barrier seek lands
        position_.onSeekRequested(positionMs);
        position_.onSeekLanded(false, 0);
        emit positionChanged();
    }
    bool network = false;
    for (size_t i = 0; i < plan.branches.size(); ++i) network = network || plan.branches[i].network;
    buffering_.reset(network, sources_.mode == PlayMode::Hls && sources_.liveHls);
    buffering_.beginStartPhase();  // PLY-08: the first preroll does not wait for a buffer level
    bufferingPercent_ = 100;
    applyFreeze(freeze_.configure(sources_.mode, plan.hasVideo(), nowMs()));
    started_ = false;
    sawFirstFrame_ = false;
    watchdogArmedThisBuild_ = false;
    seekInFlight_ = false;
    pendingSeek_ = false;
    alignSent_ = false;
    requestedState_ = -1;
    startHoldArmed_ = startFailed_ = startAnySent_ = false;
    for (int i = 0; i < 3; ++i) {
        startHeld_[i] = startSent_[i] = startLandedKnown_[i] = false;
        startSeq_[i] = 0;
        startLandedMs_[i] = 0;
    }
    // PLY-06: before any data flows. A local file decodes within microseconds of PAUSED, before
    // load() returns, so a start position set right after load() (setStartPosition, the library
    // bridge) would come too late: local builds always hold, and release at once when nothing
    // is to be applied (one owner-thread round trip).
    armStartSeek(sources_.mode == PlayMode::Local);
    // PLY-20: local media has no network wait, so its 3 s count starts with the build (a filesrc
    // is pulled, and a pulled source reports no first byte).
    if (!network) armHardwareWatchdog();
    setState(Loading);
    engine_.setState(PipelineState::Paused);
    requestedState_ = int(PipelineState::Paused);
    updatePollTimer();
}

void VideoPlayer::teardown(bool clearFrame) {
    resendTimer_.stop();
    seekTimeoutTimer_.stop();
    watchdogTimer_.stop();
    decodePolicy_.onVideoStopped();
    if (engine_.built()) {
        engine_.teardown();  // unlocks a frozen branch first (PLY-09)
        guard_.onTeardown();
    }
    const bool wasFrozen = freeze_.frozen();
    freeze_.onTeardown();
    armFreezeTimer();
    if (wasFrozen) emit frozenChanged();
    pipelinePlaying_ = false;
    seekInFlight_ = false;
    if (clearFrame) mailbox_.clear();
}

void VideoPlayer::stop() { stop(false); }

void VideoPlayer::stop(bool keepFrame) {
    teardown(!keepFrame);
    loaded_ = false;
    position_.reset();
    setBuffered(0);
    pollTimer_.stop();
    setState(Idle);
    emit positionChanged();
}

// ---- Play / pause (all through BufferingPolicy, PLY-08) ---------------------------------

void VideoPlayer::play() {
    if (!engine_.built()) {
        if (loaded_ && state_ != Failed) startPipeline(int64_t(position()), rate_);
        if (!engine_.built()) return;
    }
    buffering_.onUserIntent(true);
    if (position_.ended()) {  // replay from the start
        guard_.onUserSeek();
        executeSeek(0, rate_);
        emit endedChanged();
    }
    applyPipelineState();
}

void VideoPlayer::pause() {
    buffering_.onUserIntent(false);
    applyPipelineState();
}

void VideoPlayer::applyPipelineState(bool force) {
    if (!engine_.built()) return;
    const bool wantPlay = started_ && buffering_.wantsPlaying() && !buffering_.buffering() && !position_.ended();
    const int target = int(wantPlay ? PipelineState::Playing : PipelineState::Paused);
    if (target != requestedState_ || force) {
        requestedState_ = target;
        engine_.setState(wantPlay ? PipelineState::Playing : PipelineState::Paused);
    }
    refreshState();
}

void VideoPlayer::refreshState() {
    if (state_ == Failed || state_ == Idle) return;
    if (position_.ended()) setState(Ended);
    else if (!started_) setState(Loading);
    else setState(pipelinePlaying_ && requestedState_ == int(PipelineState::Playing) ? Playing : Paused);
    updatePollTimer();
}

// ---- Seeks (PLY-03/05/06) ------------------------------------------------------------

Container VideoPlayer::videoContainer() const {
    return sources_.mode == PlayMode::Dual ? sources_.video.container : sources_.muxed.container;
}

Container VideoPlayer::audioContainer() const {
    if (sources_.mode == PlayMode::Dual) return sources_.audio.container;
    if (sources_.mode == PlayMode::AudioOnly) return sources_.audio.present() ? sources_.audio.container : sources_.muxed.container;
    return sources_.muxed.container;
}

Branch VideoPlayer::positionBranch() const {
    // The audio sink is the clock master; its position is the playback position.
    if (sources_.mode == PlayMode::Dual || sources_.mode == PlayMode::AudioOnly) return Branch::Audio;
    return Branch::Muxed;
}

void VideoPlayer::seek(double ms) {
    if (!engine_.built()) return;
    const int64_t target = ms < 0 ? 0 : int64_t(ms);
    guard_.onUserSeek();
    resendTimer_.stop();
    if (barrier_.deferSeek(target)) {  // not prerolled yet: start seek or one seek at preroll
        position_.onSeekRequested(target);
        emit positionChanged();
        armStartSeek();
        return;
    }
    if (seekInFlight_) {  // coalesce: latest target wins
        pendingSeek_ = true;
        pendingSeekMs_ = target;
        position_.onSeekRequested(target);
        emit positionChanged();
        return;
    }
    const bool wasEnded = position_.ended();
    executeSeek(target, rate_);
    if (wasEnded) emit endedChanged();
}

void VideoPlayer::executeSeek(int64_t targetMs, double rate) {
    SeekRequest r;
    r.mode = sources_.mode;
    r.video = videoContainer();
    r.audio = audioContainer();
    r.targetMs = targetMs;
    r.durationMs = position_.durationKnown() ? position_.durationMs() : 0;
    r.rate = rate;
    seekPlan_ = planSeek(r);
    alignSent_ = false;
    lastSeekMs_ = seekPlan_.targetMs;
    position_.onSeekRequested(seekPlan_.targetMs);
    emit positionChanged();
    mark("seek-start");
    bool video = false, audio = false, any = false;
    videoSeqnum_ = audioSeqnum_ = pipelineSeqnum_ = 0;
    guardReportPending_ = false;
    for (size_t i = 0; i < seekPlan_.now.size(); ++i) {
        quint32 seqnum = 0;
        const bool accepted = engine_.sendSeek(seekPlan_.now[i], &seqnum);
        ++diag_.seeksSent;
        if (seekPlan_.now[i].branch == Branch::Video) { video = accepted; videoSeqnum_ = accepted ? seqnum : 0; }
        if (seekPlan_.now[i].branch == Branch::Audio) { audio = accepted; audioSeqnum_ = accepted ? seqnum : 0; }
        if (seekPlan_.now[i].branch == Branch::Muxed) pipelineSeqnum_ = accepted ? seqnum : 0;
        any = any || accepted;
    }
    if (debugEnabled())
        std::fprintf(stderr, "player: seek %lld ms rate %.2f -> video %d audio %d (commands %d)\n", (long long)seekPlan_.targetMs,
                     rate, video, audio, int(seekPlan_.now.size()));
    seekInFlight_ = any;
    if (any) seekTimeoutTimer_.start(kSeekLandTimeoutMs);
    if (!any) {
        position_.onSeekRefused();
        emit positionChanged();
        markStarted();
    }
    if (sources_.mode == PlayMode::Dual) {
        // PLY-05. A refusal is known now; "both accepted" is confirmed only
        // when both sinks got the seek's segment (onAsyncDone / timeout).
        if (seekPlan_.alignAudioAfterAsyncDone) {
            if (!video) handleGuard(guard_.onSeekResult(false, false));
        } else if (video && audio) {
            guardReportPending_ = true;
        } else {
            handleGuard(guard_.onSeekResult(video, audio));
        }
    }
}

void VideoPlayer::handleGuard(SplitSeekAction action) {
    if (debugEnabled()) std::fprintf(stderr, "player: split-seek guard action %d\n", int(action));
    switch (action) {
    case SplitSeekAction::None:
    case SplitSeekAction::GiveUpKeepPlaying:
        break;
    case SplitSeekAction::ResendAfter250ms:
        resendTimer_.start(int(SplitSeekGuard::kResendDelayMs));
        break;
    case SplitSeekAction::RebuildOnceWithDeferredSeek: {
        ++diag_.rebuilds;
        mark("rebuild");
        const int64_t target = lastSeekMs_;
        const quint64 generation = engine_.generation();
        // Deferred: we may be inside an engine signal. Skipped if another
        // build happened meanwhile (new video, stop).
        QTimer::singleShot(0, this, [this, target, generation]() {
            if (loaded_ && engine_.generation() == generation) startPipeline(target, rate_);
        });
        break;
    }
    }
}

void VideoPlayer::onAsyncDone() {
    DeferredSeek deferred;
    const bool startApplied = !barrier_.prerolled() && barrier_.startSeekApplied();
    if (!barrier_.prerolled()) engine_.releaseStartHolds();  // nothing stays held past the first preroll
    if (barrier_.onAsyncDone(&deferred)) {  // PLY-06: the one rate-carrying flush seek
        mark("preroll");
        rate_ = deferred.rate;
        executeSeek(deferred.positionMs, deferred.rate);
        return;
    }
    if (seekInFlight_) {
        if (!seekReachedSinks()) return;  // the other branch's async-done is still to come
        if (guardReportPending_) {
            guardReportPending_ = false;
            handleGuard(alignSent_ ? guard_.onAudioAlignResult(true) : guard_.onSeekResult(true, true));
        }
        if (seekPlan_.alignAudioAfterAsyncDone && !alignSent_) {
            // PLY-03 phase two: audio to where the video landed (its segment start).
            qint64 landed = 0;
            landedKnown_ = engine_.sinkSegmentStart(Branch::Video, videoSeqnum_, &landed) ||
                           engine_.queryPosition(Branch::Video, &landed);
            landedMs_ = landedKnown_ ? landed : seekPlan_.targetMs;
            const SeekCommand align = planAudioAlign(seekPlan_, landedKnown_, landedMs_);
            alignSent_ = true;
            quint32 seqnum = 0;
            const bool accepted = engine_.sendSeek(align, &seqnum);
            audioSeqnum_ = accepted ? seqnum : 0;
            videoSeqnum_ = 0;
            ++diag_.seeksSent;
            if (accepted) {
                guardReportPending_ = true;
                seekTimeoutTimer_.start(kSeekLandTimeoutMs);
                return;  // wait for the audio's segment and async-done
            }
            handleGuard(guard_.onAudioAlignResult(false));
        }
        finishSeek();
        return;
    }
    if (!started_) {
        mark("preroll");
        if (startApplied) {
            // PLY-06: prerolled at the start position; report where it landed (the position
            // branch's start segment: the keyframe for key-unit, the target for accurate).
            const int s = slotOf(positionBranch() == Branch::Audio ? Branch::Audio : Branch::Muxed);
            position_.onSeekRequested(startSentMs_);
            position_.onSeekLanded(startLandedKnown_[s], startLandedMs_[s]);
            emit positionChanged();
        }
        markStarted();
    }
}

// ---- PLY-06 start seek before preroll ------------------------------------------------------
// The decoders hold each branch's first compressed buffer (headers parsed); the start seek goes
// upstream from there, so the first preroll is already at the start position: no frame from 0
// is decoded, no flush reaches a sink, no second preroll. Anything unexpected (a branch that
// cannot be held, a refusal) falls back to the barrier seek at the first async-done.

void VideoPlayer::armStartSeek(bool provisional) {
    if (startHoldArmed_ || startFailed_ || !engine_.built() || !(provisional || barrier_.wantsStartSeek()) ||
        !startSeekBeforePreroll(sources_.mode))
        return;
    startHoldArmed_ = true;
    engine_.holdForStartSeek();
}

void VideoPlayer::failStartSeek(const char *why) {
    if (!startHoldArmed_ || startFailed_) return;
    startFailed_ = true;
    engine_.releaseStartHolds();
    if (debugEnabled()) std::fprintf(stderr, "player: start seek before preroll not possible (%s): seek at preroll\n", why);
}

void VideoPlayer::onDecoderHeld(int branch) {
    if (!startHoldArmed_ || startFailed_ || barrier_.prerolled()) {
        engine_.releaseStartHolds();
        return;
    }
    startHeld_[slotOf(static_cast<Branch>(branch))] = true;
    if (!startAnySent_ && !barrier_.wantsStartSeek()) {  // nothing to apply (provisional hold, or back to 0 at 1x)
        startFailed_ = true;
        engine_.releaseStartHolds();
        return;
    }
    sendStartSeeks();
}

void VideoPlayer::onStartSegment(int branch, quint32 seqnum, qint64 startMs) {
    const int s = slotOf(static_cast<Branch>(branch));
    if (!startSent_[s] || seqnum != startSeq_[s]) return;
    startLandedKnown_[s] = startMs >= 0;
    startLandedMs_[s] = startMs < 0 ? 0 : startMs;
    if (!startFailed_ && !barrier_.prerolled()) sendStartSeeks();  // Dual mp4: the audio follows the video
}

void VideoPlayer::sendStartSeeks() {
    if (startFailed_ || barrier_.startSeekApplied()) return;
    if (!startAnySent_) {
        startSentMs_ = barrier_.pendingPositionMs();
        startSentRate_ = barrier_.pendingRate();
    }
    SeekRequest r;
    r.mode = sources_.mode;
    r.video = videoContainer();
    r.audio = audioContainer();
    r.targetMs = startSentMs_;
    r.rate = startSentRate_;
    const SeekPlan plan = planSeek(r);
    std::vector<SeekCommand> commands = plan.now;
    bool complete = true;
    if (plan.alignAudioAfterAsyncDone) {
        // PLY-03 phase two before preroll: the audio goes to where the video's key-unit seek landed.
        if (startLandedKnown_[slotOf(Branch::Video)])
            commands.push_back(planAudioAlign(plan, true, startLandedMs_[slotOf(Branch::Video)]));
        else if (!startSent_[slotOf(Branch::Audio)])
            complete = false;
    }
    for (size_t i = 0; i < commands.size(); ++i) {
        const int s = slotOf(commands[i].branch);
        if (startSent_[s]) continue;
        if (!startHeld_[s]) {
            complete = false;
            continue;
        }
        quint32 seqnum = 0;
        const bool ok = engine_.sendStartSeek(commands[i], &seqnum);
        ++diag_.seeksSent;
        mark(QStringLiteral("start-seek branch=%1").arg(QLatin1String(branchName(commands[i].branch))));
        if (debugEnabled())
            std::fprintf(stderr, "player: start seek %s %lld ms rate %.2f -> %d\n", branchName(commands[i].branch),
                         (long long)(commands[i].positionNs / 1000000), commands[i].rate, ok);
        if (!ok) {
            failStartSeek("refused");
            return;
        }
        startSent_[s] = true;
        startSeq_[s] = seqnum;
        startAnySent_ = true;
    }
    // PLY-06 (R2 run 5): the video sink only ever sees its own branch's data. Once that branch's
    // start seek is out, everything it shows is at the start position, so its first frame reaches
    // the mailbox as soon as it is decoded, without waiting for the audio branch (held, late or
    // aligned to the video's landing); the audio joins when its segment lands, and the pipeline
    // goes to PLAYING only after the first async-done (both branches prerolled), so A/V sync is
    // unchanged. Not when a newer start / rate arrived meanwhile (the barrier seeks at the first
    // async-done then).
    const bool current = barrier_.pendingPositionMs() == startSentMs_ && barrier_.pendingRate() == startSentRate_;
    const int videoSlot = slotOf(sources_.mode == PlayMode::Dual ? Branch::Video : Branch::Muxed);
    if (startSent_[videoSlot] && current) engine_.setVideoOutputEnabled(true);
    if (!complete) return;
    barrier_.onStartSeekApplied(startSentMs_, startSentRate_);
    if (current) engine_.setVideoOutputEnabled(true);  // every branch is at the start position
}

void VideoPlayer::setStartPosition(qint64 ms) {
    if (!engine_.built() || barrier_.prerolled() || started_ || sawFirstFrame_) return;
    if (!barrier_.deferSeek(ms)) return;
    const int64_t target = barrier_.pendingPositionMs();
    position_.onSeekRequested(target);
    position_.onSeekLanded(false, 0);
    emit positionChanged();
    const bool sameAsApplied = barrier_.startSeekApplied() && target == startSentMs_ && barrier_.pendingRate() == startSentRate_;
    if (!sameAsApplied) engine_.setVideoOutputEnabled(!(target > 0 || barrier_.pendingRate() != 1.0));
    armStartSeek();
}

bool VideoPlayer::seekReachedSinks() const {
    // A pipeline-level seek (Muxed/Hls/Local) completes with one async-done.
    // Per-sink seeks can complete one at a time: wait until every accepted
    // sink has the seek's segment.
    if (videoSeqnum_ && !engine_.sinkSawSegment(Branch::Video, videoSeqnum_)) return false;
    if (audioSeqnum_ && !engine_.sinkSawSegment(Branch::Audio, audioSeqnum_)) return false;
    return true;
}

void VideoPlayer::finishSeek() {
    seekTimeoutTimer_.stop();
    // The landed position is the start of the seek's segment at the sink
    // (the keyframe for key-unit seeks); a position query is the fallback.
    bool known;
    qint64 landed = 0;
    if (seekPlan_.alignAudioAfterAsyncDone) {
        known = landedKnown_;
        landed = landedMs_;
    } else {
        const quint32 seq = pipelineSeqnum_ ? pipelineSeqnum_ : (audioSeqnum_ ? audioSeqnum_ : videoSeqnum_);
        const Branch at = audioSeqnum_ || (pipelineSeqnum_ && engine_.plan().route(RouteKind::Audio)) ? Branch::Audio
                                                                                                   : Branch::Video;
        known = seq && engine_.sinkSegmentStart(at, seq, &landed);
        if (!known) known = engine_.queryPosition(positionBranch(), &landed);
    }
    position_.onSeekLanded(known, landed);
    seekInFlight_ = false;
    mark("seek-done");
    emit positionChanged();
    if (pendingSeek_) {
        pendingSeek_ = false;
        executeSeek(pendingSeekMs_, rate_);
        return;
    }
    markStarted();
}

void VideoPlayer::markStarted() {
    if (!started_) {
        started_ = true;
        engine_.setVideoOutputEnabled(true);
        refreshDuration();
        bool s = false;
        engine_.querySeekable(&s);
        if (sources_.mode == PlayMode::Hls && sources_.liveHls) s = false;
        if (s != seekable_) {
            seekable_ = s;
            emit seekableChanged();
        }
    }
    applyPipelineState();
}

// ---- Speed (PLY-07) ----------------------------------------------------------------------

void VideoPlayer::setRate(double rate) {
    const bool liveEdge = sources_.mode == PlayMode::Hls && sources_.liveHls;
    const double applied = speed::rateToApply(speed::snap(rate), liveEdge);
    if (applied == rate_) return;
    rate_ = applied;
    emit rateChanged();
    if (!engine_.built()) return;
    if (barrier_.deferRate(applied)) {
        armStartSeek();
        return;
    }
    guard_.onUserSeek();
    resendTimer_.stop();
    qint64 now = position_.positionMs();
    if (!position_.seeking()) engine_.queryPosition(positionBranch(), &now);
    if (seekInFlight_) {
        pendingSeek_ = true;
        pendingSeekMs_ = now;
        return;
    }
    executeSeek(now, rate_);  // rate-carrying flush seek at the current position
}

// ---- Bus-driven state ---------------------------------------------------------------------

void VideoPlayer::onPipelineState(int state) {
    const bool playingNow = state == int(PipelineState::Playing);
    pipelinePlaying_ = playingNow;
    if (playingNow) armHardwareWatchdog();
    refreshState();
}

// PLY-20: at PLAYING or at the picture's first byte, whichever comes first: no hardware frame
// within 3 s -> software for this video, never a silent black picture.
void VideoPlayer::armHardwareWatchdog() {
    if (effectiveDecode_ != DecodeMode::Hardware || !hasVideo_ || sawFirstFrame_ || watchdogArmedThisBuild_) return;
    watchdogArmedThisBuild_ = true;
    decodePolicy_.onVideoStarted(sources_.videoId, DecodeMode::Hardware, nowMs());
    watchdogTimer_.start(int(DecodeModePolicy::kWatchdogMs));
}

void VideoPlayer::onFirstVideoFrame() {
    sawFirstFrame_ = true;
    decodePolicy_.onVideoFrame(nowMs());
    watchdogTimer_.stop();
    mark("first-frame");
    emit firstFrame();
}

void VideoPlayer::onEos() {
    refreshDuration();
    position_.onEos();
    mark("ended");
    refreshState();
    emit positionChanged();
    emit endedChanged();
    emit endOfStream();
    updatePollTimer();
}

void VideoPlayer::onError(const QString &message, const QString &debug, int branch, bool network, int httpStatus) {
    Q_UNUSED(debug);
    // PLY-20: a hardware video failure is never a silent black picture.
    if (effectiveDecode_ == DecodeMode::Hardware && (branch == int(Branch::Video) || branch == int(Branch::Muxed)) &&
        !network) {
        fallBackToSoftware(QStringLiteral("hardware decoder error: %1").arg(message));
        return;
    }
    fail(message, network, branch, httpStatus);
}

void VideoPlayer::onBuffering(int branch, int percent) {
    buffering_.onBufferingPercent(Branch(branch), percent);
    bufferingPercent_ = percent;
    emit bufferingChanged();
    applyPipelineState();
}

void VideoPlayer::fallBackToSoftware(const QString &reason) {
    softwareOnly_.insert(sources_.videoId);
    ++diag_.decodeFallbacks;
    mark("fallback");
    const int64_t at = captureSwitchPosition(barrier_.prerolled(), barrier_.pendingPositionMs(), position_.positionMs());
    emit decodeFallback(reason);
    const quint64 generation = engine_.generation();
    QTimer::singleShot(0, this, [this, at, generation]() {
        if (loaded_ && engine_.generation() == generation) startPipeline(at, rate_);
    });
}

void VideoPlayer::fail(const QString &reason, bool network, int branch, int httpStatus) {
    errorText_ = reason;
    emit errorTextChanged();
    setState(Failed);
    emit failed(reason, network, branch, httpStatus);
    const quint64 generation = engine_.generation();
    QTimer::singleShot(0, this, [this, generation]() {
        if (engine_.generation() == generation) teardown(false);
    });
}

// ---- Freeze (PLY-09) ------------------------------------------------------------------------

void VideoPlayer::setAppActive(bool active) { applyFreeze(freeze_.onAppActive(active, nowMs())); }

void VideoPlayer::setCoverVisible(bool visible) { applyFreeze(freeze_.onCoverVisible(visible, nowMs())); }

void VideoPlayer::applyFreeze(const FreezeAction &a) {
    const bool before = engine_.frozen();
    if (a.thawVideo) engine_.thaw(freeze_.target());  // no flush (PLY-09)
    if (a.freezeVideo) engine_.freeze(freeze_.target());
    if (a.repaint) emit repaintRequested();
    if (before != engine_.frozen() || a.thawVideo || a.freezeVideo) emit frozenChanged();
    armFreezeTimer();
}

void VideoPlayer::armFreezeTimer() {
    const int64_t deadline = freeze_.nextDeadlineMs();
    if (deadline < 0) {
        freezeTimer_.stop();
        return;
    }
    const int64_t wait = deadline - nowMs();
    freezeTimer_.start(int(wait < 0 ? 0 : wait));
}

// ---- Position (PLY-24) ----------------------------------------------------------------------

void VideoPlayer::setControlsVisible(bool visible) {
    controlsVisible_ = visible;
    updatePollTimer();
}

void VideoPlayer::setPositionConsumer(bool consumer) {
    positionConsumer_ = consumer;
    updatePollTimer();
}

void VideoPlayer::updatePollTimer() {
    const int64_t interval = engine_.built() ? position_.pollIntervalMs(positionConsumer_, controlsVisible_, state_ == Playing) : 0;
    if (interval <= 0) {
        pollTimer_.stop();
        return;
    }
    if (!pollTimer_.isActive() || pollTimer_.interval() != int(interval)) pollTimer_.start(int(interval));
}

void VideoPlayer::pollPosition() {
    if (!engine_.built()) return;
    const int64_t before = position_.positionMs();
    qint64 ms = 0;
    bool ok = engine_.queryPosition(positionBranch(), &ms);
    if (!ok) ok = engine_.queryPosition(Branch::Muxed, &ms);  // e.g. a source without audio
    position_.onPositionQuery(ok, ms);
    if (position_.needsDurationPoll()) refreshDuration();
    refreshBuffered();
    if (position_.positionMs() != before) emit positionChanged();
    updatePollTimer();
}

// Seek bar: queried at the position poll only (no timer of its own, NFR-PERF-06).
void VideoPlayer::refreshBuffered() { setBuffered(computeBuffered()); }

int64_t VideoPlayer::computeBuffered() const {
    const int64_t position = position_.positionMs(), duration = position_.durationMs();
    const SourceSpec *specs[] = {&sources_.video, &sources_.audio, &sources_.muxed};
    bool local = true, any = false;
    for (const SourceSpec *s : specs)
        if (s->present()) { any = true; local = local && s->uri.compare(0, 7, "file://") == 0; }
    if (any && local) return duration > 0 ? duration : 0;  // all of it is on the device
    int64_t buffered = -1;
    const std::vector<BranchPlan> &branches = engine_.plan().branches;
    for (std::size_t i = 0; i < branches.size(); ++i) {
        qint64 ms = 0;
        if (!engine_.queryBuffered(branches[i].branch, position, duration, rate_, &ms)) continue;
        if (buffered < 0 || ms < buffered) buffered = ms;  // dual: the shorter branch
    }
    return buffered < 0 ? 0 : buffered;
}

// For a stall watchdog above the player: asked when its deadline is due, never on a timer here.
bool VideoPlayer::queryProgress(qint64 *positionMs, qint64 *bufferedMs) const {
    if (!engine_.built()) return false;
    // The furthest any sink got: the audio sink (the reported position), the video sink (it plays
    // on after a shorter audio track ended) or the pipeline's own answer.
    const Branch sinks[] = {Branch::Audio, Branch::Video, Branch::Muxed};
    qint64 furthest = -1;
    for (std::size_t i = 0; i < sizeof(sinks) / sizeof(sinks[0]); ++i) {
        qint64 ms = 0;
        if (engine_.queryPosition(sinks[i], &ms) && ms > furthest) furthest = ms;
    }
    if (positionMs) *positionMs = furthest >= 0 ? furthest : position_.positionMs();
    if (bufferedMs) *bufferedMs = computeBuffered();
    return true;
}

void VideoPlayer::setBuffered(int64_t ms) {
    if (ms == bufferedMs_) return;
    bufferedMs_ = ms;
    emit bufferedPositionChanged();
}

void VideoPlayer::refreshDuration() {
    const int64_t before = position_.durationMs();
    qint64 d = 0;
    const bool ok = engine_.queryDuration(&d);
    position_.onDurationQuery(ok, d);
    if (position_.durationMs() != before) emit durationChanged();
}

// ---- Audio (PLY-22) -------------------------------------------------------------------------

void VideoPlayer::setAudioSettings(const AudioSettings &settings, const StreamLoudness &loudness) {
    sources_.audioSettings = settings;
    sources_.loudness = loudness;
    audioValues_ = computeAudioChainValues(settings, loudness);
    engine_.applyAudioValues(audioValues_);
}

void VideoPlayer::setAudioSettings(const QVariantMap &m) {
    AudioSettings s = sources_.audioSettings;
    StreamLoudness l = sources_.loudness;
    if (m.contains(QStringLiteral("eq"))) {
        const QVariantList list = m.value(QStringLiteral("eq")).toList();
        for (int i = 0; i < kEqBandCount && i < list.size(); ++i) s.eqGainsDb[size_t(i)] = list.at(i).toDouble();
    } else if (m.contains(QStringLiteral("eqPreset"))) {
        eqPresetGains(m.value(QStringLiteral("eqPreset")).toString().toStdString(), s.eqGainsDb);
    }
    if (m.contains(QStringLiteral("normalization"))) s.normalizationEnabled = m.value(QStringLiteral("normalization")).toBool();
    if (m.contains(QStringLiteral("normalizationMaxBoostPercent")))
        s.normalizationMaxBoostPercent = m.value(QStringLiteral("normalizationMaxBoostPercent")).toInt();
    if (m.contains(QStringLiteral("boostPercent"))) s.boostPercent = m.value(QStringLiteral("boostPercent")).toInt();
    if (m.contains(QStringLiteral("loudnessDb"))) {
        l.known = true;
        l.loudnessDb = m.value(QStringLiteral("loudnessDb")).toDouble();
    }
    setAudioSettings(s, l);
}

// ---- Misc ---------------------------------------------------------------------------------------

void VideoPlayer::setState(State s) {
    if (s == state_) return;
    const bool wasPlaying = playing();
    state_ = s;
    emit stateChanged();
    if (wasPlaying != playing()) emit playingChanged();
}

void VideoPlayer::mark(const char *marker) { emit timing(QString::fromLatin1(marker), loadClock_.elapsed()); }
void VideoPlayer::mark(const QString &marker) { emit timing(marker, loadClock_.elapsed()); }

PlayerDiagnostics VideoPlayer::diagnostics() const {
    PlayerDiagnostics d = diag_;
    d.engine = engine_.stats();
    return d;
}

} } // namespace nordtile::player
