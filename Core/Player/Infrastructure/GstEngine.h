// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Low-level GStreamer engine: instantiates a PipelinePlan (Domain/PipelinePlan.h)
// and exposes the few operations the player needs. No policy lives here;
// VideoPlayer decides, the engine executes. Qt Core + GStreamer only, no
// SailTube types.
//
// Threading: create, use and destroy on one thread with a Qt event loop (the
// "owner" thread). GStreamer streaming threads never call into the owner:
// bus messages and frame/pad notifications are posted as events and
// delivered on the owner thread, tagged with the build generation, so
// anything from a torn-down pipeline is dropped.
//
// Teardown is asynchronous: teardown() cuts the old pipeline off on the owner
// thread (every streaming-thread callback checks the live generation under
// the engine mutex, the bus drops everything) and hands it to a private
// worker thread that sets it to NULL and frees it, so the owner (GUI) thread
// never waits for set_state(NULL). build() of the next pipeline does not wait
// for it either, except that a hardware build waits for a pending hardware
// teardown (droidvdec / droideglsink are single-instance device resources).
// The destructor waits for every pending teardown.

#include "Core/Player/Domain/FreezePolicy.h"
#include "Core/Player/Domain/PipelinePlan.h"
#include "Core/Player/Domain/SeekPolicy.h"

#include <QMutex>
#include <QWaitCondition>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

typedef struct _GstElement GstElement;
typedef struct _GstPad GstPad;
typedef struct _GstMessage GstMessage;
typedef struct _GstSample GstSample;
typedef struct _GThreadPool GThreadPool;

namespace nordtile { namespace player {

class VideoFrameMailbox;
class EglSinkBridge;

enum class PipelineState { Null, Ready, Paused, Playing };

// Diagnostics counters (PLY-02 "dropped frames reported in diagnostics";
// host-test observables). Reset by build() and resetStats() (which keeps the
// per-build facts: decoders, unrouted streams, hidden size queries, first sink buffer).
struct EngineStats {
    qint64 framesRendered = 0;     // video buffers that reached the video sink
    qint64 framesDropped = 0;      // QoS-reported drops of the video sink
    int videoSinkFlushes = 0;      // flushes (distinct seqnums) seen at the video sink
    int audioSinkFlushes = 0;      // flushes (distinct seqnums) seen at the audio sink
    double videoSegmentRate = 1.0; // rate * applied_rate of the last segment
    double audioSegmentRate = 1.0;
    qint64 lastAvOffsetMs = 0;     // video frame stream time - audio position, at render
    qint64 maxAbsAvOffsetMs = 0;
    int avSamples = 0;
    qint64 framesWhileFrozen = 0;  // decoded frames rendered while frozen (should stay ~0)
    qint64 freezeDropped = 0;      // buffers dropped before the decoder (freeze / keyframe wait)
    // PLY-06: stream time (ms) of the first buffer that reached the video sink, -1 none yet.
    // With a start seek before preroll it is at the start position (never a frame from 0).
    qint64 firstVideoSinkMs = -1;
    qint64 startDropped = 0;       // pre-seek buffers dropped at a held decoder (PLY-06 start seek)
    // PLY-04 / NFR-PERF-02: BYTES size queries of a pull-mode demuxer left unanswered before its
    // first pad (BranchPlan::hideSizeUntilDemuxReady; the fragmented-mp4 end-of-file probe).
    qint64 demuxerSizeQueriesHidden = 0;
    // Streams a source exposed that matched no route (sent to the unmatched fakesink): count and
    // caps names ("video/x-h264,..."), e.g. a video stream that found no decoder.
    int unroutedPads = 0;
    QString unroutedCaps;
    // PLT-08 diagnostics: factory of the video / audio decoder this build uses (autoplugged
    // inside the source bins, or the explicit droidvdec), "" = none. Kept by resetStats().
    QString videoDecoder, audioDecoder;
};

class GstEngine final : public QObject {
    Q_OBJECT
public:
    // gst_init_check once, demote elementsToDemote() to rank NONE (PLT-08)
    // and probe elementsToProbe(). Safe to call repeatedly and from any thread
    // (serialised by a mutex; a failed attempt is retried by the next call).
    static bool initialize(QString *error = nullptr);
    // Factories found by initialize() (PlanInput::available). Waits for a
    // running startInitialize() first (waitInitialized(kInitWaitMs)).
    static std::set<std::string> availableElements();

    // ---- Start-up off the GUI thread (NFR-PERF-01, NFR-PERF-02 "first tap within 5 s") ----
    //
    // One private worker thread (started on first use, joined when the
    // QCoreApplication is destroyed) runs these jobs in order. Results reach
    // the GUI thread through GstEngineInit::instance() (queued events). The
    // STORE build (GstEngineStore.cpp) has no worker: see there.

    // Generous wait used by every path that needs GStreamer (build(),
    // availableElements(), VideoPlayer::load, GstHardwareProbeRunner).
    enum { kInitWaitMs = 20000 };

    // NFR-PERF-01: queues initialize() on the worker and returns at once
    // (no GStreamer call on the calling thread). `afterInit` (optional) then
    // runs on the worker, whether or not the init succeeded, e.g. the registry
    // capability probe (probeGstreamer), and only after it
    // GstEngineInit::ready(ok, error, initMs) is delivered, once per process.
    // waitInitialized() callers are released before `afterInit` runs.
    // `afterInit` must not wait for the GUI thread. Call it from the GUI
    // thread after the QCoreApplication exists (the notifier is created on
    // first use and lives on the application's thread). Only the first call
    // queues anything; later calls (and their `afterInit`) are ignored.
    static void startInitialize(const std::function<void()> &afterInit = std::function<void()>());
    // NFR-PERF-01: true once initialize() succeeded (never blocks).
    static bool initialized();
    // NFR-PERF-01: blocks until a queued/running startInitialize() attempt has
    // finished or timeoutMs passed; true when GStreamer is initialised. When no
    // asynchronous attempt is pending (never started, or it failed) it runs
    // initialize() synchronously on the calling thread instead (timeout not
    // applied), so a caller that never used startInitialize() keeps working.
    static bool waitInitialized(int timeoutMs);
    // NFR-PERF-02: on the worker, after the init, gst_plugin_feature_load() of
    // every preloadFeatures() entry present in the registry (missing ones are
    // skipped), so the first video does not pay for dlopen + plugin_init of
    // libav and friends. Never creates an element or a pipeline. Returns at
    // once; only the first call does anything; GstEngineInit::
    // decodersPreloaded(loaded, ms) is delivered once. No PulseAudio warm-up:
    // libpulse is already mapped by initialize() (pulsesink is probed), and
    // the server connection belongs to a pulsesink instance, so warming it
    // would need an element in READY.
    static void preloadDecoders();
    // NFR-PERF-02: registry feature names preloadDecoders() loads: decoders,
    // parsers, demuxers, the decodebin family, typefinders, the HTTP source
    // and the audio chain of the first video.
    static std::vector<std::string> preloadFeatures();

    explicit GstEngine(VideoFrameMailbox *mailbox, QObject *parent = nullptr);
    ~GstEngine() override;

    // Tear down any previous pipeline, then build `input`'s plan (state NULL).
    // False with *error when the plan is not ok or instantiation failed.
    // Waits for a running startInitialize() first (waitInitialized(kInitWaitMs)).
    bool build(const PlanInput &input, QString *error = nullptr);
    // Unlock a frozen branch, cut the pipeline off and hand it to the teardown
    // worker (NULL + unref there); returns at once. Idempotent. teardownDone()
    // reports when the worker finished.
    void teardown();
    bool built() const { return pipeline_ != nullptr; }
    const PipelinePlan &plan() const { return plan_; }
    quint64 generation() const { return generation_; }

    // gst_element_set_state on the pipeline; false on FAILURE.
    bool setState(PipelineState state);

    // PLY-03: one seek event to the command's sink (Video / Audio sink) or to
    // the pipeline (Branch::Muxed). Returns gst_element_send_event's result.
    // *seqnum (optional) receives the event's seqnum, see sinkSawSegment().
    // Sent under the pipeline's state lock, so a per-sink flush never interleaves with the
    // pipeline's own return to PLAYING after a preroll (that would leave the sink in PAUSED);
    // it may therefore wait for that state change, a few milliseconds.
    bool sendSeek(const SeekCommand &command, quint32 *seqnum = nullptr);
    // True once the branch's sink received the SEGMENT of the seek `seqnum`
    // (demuxers copy the seek seqnum onto their flush and segment events).
    bool sinkSawSegment(Branch which, quint32 seqnum) const;
    // Stream time (ms) where that segment starts: the landed position.
    bool sinkSegmentStart(Branch which, quint32 seqnum, qint64 *ms) const;

    // PLY-06 start seek before preroll. Every decoder of a build (the ones
    // decodebin autoplugs inside the source bins and the explicit droidvdec)
    // carries a probe on its sink pad from the start; it costs nothing until
    // holdForStartSeek() is called. From then on the first compressed buffer
    // of each branch is held at its decoder (the demuxer has parsed its
    // headers) and decoderHeld(branch) is emitted. A branch whose data already
    // passed a decoder, or that exposes a stream without any decoder, emits
    // branchNotHeld(branch) instead. Call it after build() and before data
    // flows (normally before the first setState(Paused)); a later call still
    // holds branches that have not decoded anything yet.
    void holdForStartSeek();
    // Sends `command` upstream from the held decoder of command.branch (the
    // branch, not a sink: Branch::Muxed = the single source) and releases that
    // branch: buffers older than the seek are dropped at the decoder until the
    // seek's segment arrives, so nothing from before the start is decoded.
    // False (branch released unchanged) when the branch is not held, upstream
    // reports it unseekable, or the demuxer refuses. *seqnum: the event's.
    bool sendStartSeek(const SeekCommand &command, quint32 *seqnum = nullptr);
    // Lets every held branch run unchanged (fallback, nothing to apply).
    void releaseStartHolds();

    // Position of a branch's sink (Branch::Muxed = pipeline), ms.
    bool queryPosition(Branch which, qint64 *ms) const;
    bool queryDuration(qint64 *ms) const;
    bool querySeekable(bool *seekable) const;
    // Seek bar: how far branch `which` (Branch::Muxed = the single source) is buffered in the
    // playing timeline, ms, the furthest of: positionMs plus its shortest non-empty demuxed queue
    // (decodebin's multiqueue "stats", running time x `rate`); the bytes its HTTP source fetched so
    // far (BYTES position / duration) scaled to durationMs; the end of the downloaded range holding
    // positionMs (a disk-backed branch answers the BUFFERING query in PERCENT ranges). Bytes and
    // time are taken as proportional. Never below positionMs nor above durationMs (when known).
    // False when none is known.
    bool queryBuffered(Branch which, qint64 positionMs, qint64 durationMs, double rate, qint64 *bufferedMs) const;

    // PLY-09: freeze / thaw the video per FreezePolicy::target().
    // Both forms drop buffers on the video decoder's sink pad (found via
    // deep-element-added, or the explicit droidvdec), paced to the pipeline
    // clock, so decoding stops while the demuxer and the download keep
    // playback pace. The video sink runs with async=false while frozen, so a
    // pause, buffering pause or seek never waits for a video preroll that
    // cannot come. Thaw: keep dropping delta units until a keyframe, then
    // remove the probe (that keyframe passes, DISCONT). No flush, ever.
    //   LockAndPauseVideoSource (Dual) additionally sets the video source bin
    //     to locked + PAUSED (thaw: unlock + sync_state_with_parent), as the
    //     FreezePolicy contract says. Measured on GStreamer 1.26 with
    //     souphttpsrc: lock+PAUSED ALONE does not stop decoding (a non-live
    //     source keeps pushing in PAUSED; 60 of 60 frames still reached the
    //     sink in 2 s), hence the probe in Dual too.
    //   DropBeforeVideoDecoder (Muxed/Hls/Local): the probe only.
    // Why paced: dropping as fast as possible lets decodebin's multiqueue
    // grow the audio queue without bound, the demuxer then reads (and
    // downloads) the whole stream ahead of the clock and the video is at EOS
    // when it thaws.
    void freeze(FreezeTarget target);
    void thaw(FreezeTarget target);
    bool frozen() const { return frozenTarget_ != FreezeTarget::None; }

    // PLY-06: while disabled, video frames are not posted to the mailbox and
    // do not count as the first frame (the preroll at 0 before the barrier
    // seek must not flash). The latest preroll sample is held and posted when
    // re-enabled. Enabled by default; build() re-enables.
    void setVideoOutputEnabled(bool enabled);

    // PLY-20: the zero-copy video sink (droideglsink) of a hardware build,
    // null in software mode. Owned by the pipeline; valid until teardown().
    // Its frames reach the surface through the mailbox's HardwareFrameSource.
    GstElement *hardwareVideoSink() const;

    // PLY-22: push live values into the chain (missing elements skipped).
    void applyAudioValues(const AudioChainValues &values);

    EngineStats stats() const;
    void resetStats();

signals:
    void asyncDone();
    void endOfStream();
    // httpStatus (RES-07): the HTTP source's status from the error's details structure
    // ("http-status-code", souphttpsrc), 0 when the error carries none.
    void errorOccurred(const QString &message, const QString &debug, int branch, bool network, int httpStatus);
    void warningOccurred(const QString &message);
    void bufferingPercent(int branch, int percent);  // branch = int(Branch)
    void pipelineStateChanged(int state);            // int(PipelineState)
    // Once per build. Software builds: after the frame was posted to the
    // mailbox (the mailbox holds it when this arrives). Hardware builds: the
    // first frame the zero-copy sink announced (frame-ready / show-frame), not
    // just a buffer at its pad.
    void firstVideoFrame();
    // NFR-PERF timing: first buffer out of a branch's source (int(Branch)), once per build.
    void firstByte(int branch);
    // NFR-PERF timing: the branch's demuxer added its first pad (headers parsed), once per build.
    void demuxReady(int branch);
    // NFR-PERF timing: the first decoded video buffer left the video decoder, once per build.
    void firstDecodedVideo();
    // PLY-06 start seek (see holdForStartSeek()).
    void decoderHeld(int branch);
    void branchNotHeld(int branch);
    // The start seek's segment reached the branch's decoder: startMs is where it landed
    // (the keyframe for a key-unit seek). Once per branch and start seek.
    void startSegment(int branch, quint32 seqnum, qint64 startMs);
    // The teardown worker finished a pipeline: NULL + unref took msSinceTeardown after teardown().
    // Not tied to a build generation (it may arrive after the next build()).
    void teardownDone(qint64 msSinceTeardown);
    // PLY-20: hardware frames cannot be shown (sink without a usable
    // interface, GL extension missing, no EGLImage). The player falls back.
    void hardwareRenderFailed(const QString &reason);
    void videoSizeChanged(int width, int height);
    void clockLost();

protected:
    bool event(QEvent *event) override;

public:
    // Implementation details shared with the C callbacks in GstEngine.cpp.
    struct RouteCtx;
    struct BranchCtx;
    struct Ctx;
    struct Callbacks;
    struct HoldPad;
    struct TeardownJob;

private:
    void handleMessage(GstMessage *message);
    void removeUnlinkedRoutes(int branch);
    int branchOf(GstElement *element) const;
    GstElement *sinkFor(Branch which) const;
    void setSinkAsync(bool async);
    void attachHardwareSink();
    void stopHardwareSink();    // before the sink goes to NULL
    void detachHardwareSink();  // after
    void noteHardwareFrame(quint64 generation);
    BranchCtx *branchFor(Branch which) const;
    static void runTeardown(void *job, void *engine);

    VideoFrameMailbox *mailbox_;
    PipelinePlan plan_;
    GstElement *pipeline_ = nullptr;
    GstElement *videoSink_ = nullptr;
    GstElement *audioSink_ = nullptr;
    GstElement *videoSource_ = nullptr;  // video (Dual) or muxed source bin
    std::vector<std::unique_ptr<BranchCtx>> branches_;
    quint64 generation_ = 0;
    Ctx *ctx_ = nullptr;  // user data of every C callback of the current build
    FreezeTarget frozenTarget_ = FreezeTarget::None;
    std::shared_ptr<EglSinkBridge> hwBridge_;  // PLY-20, hardware builds only
    QString hwFailure_;                        // why hwBridge_ could not be made

    // Shared with streaming threads.
    mutable QMutex mutex_;
    EngineStats stats_;
    GstPad *decoderSinkPad_ = nullptr;   // video decoder input (freeze probe)
    unsigned long freezeProbe_ = 0;
    int freezeMode_ = 0;                 // 0 off, 1 drop/block, 2 wait for keyframe
    quint32 videoSegmentSeqnum_ = 0, audioSegmentSeqnum_ = 0;
    qint64 videoSegmentStartMs_ = -1, audioSegmentStartMs_ = -1;
    quint32 videoFlushSeqnum_ = 0, audioFlushSeqnum_ = 0;
    void *freezeWait_ = nullptr;         // GstClockID of a pacing wait in the freeze probe
    bool firstFramePosted_ = false;
    bool outputEnabled_ = true;
    GstSample *heldPreroll_ = nullptr;
    int lastWidth_ = 0, lastHeight_ = 0;
    EglSinkBridge *hwSink_ = nullptr;    // = hwBridge_.get() for the streaming threads
    quint64 liveGeneration_ = 0;         // generation of the running pipeline, 0 after teardown
    QWaitCondition holdCond_;            // PLY-06: held decoders wait here (with mutex_)
    bool decodersLogged_ = false;        // PLT-08 diagnostics line printed for this build

    // Teardown worker (asynchronous teardown).
    GThreadPool *teardownPool_ = nullptr;
    QMutex teardownMutex_;
    QWaitCondition teardownCond_;
    int pendingHardwareTeardowns_ = 0;   // under teardownMutex_
};

// NFR-PERF-01/02: GUI-thread notifier of the start-up jobs (startInitialize,
// preloadDecoders). Lives on the QCoreApplication's thread; both signals are
// emitted there, each once per process.
class GstEngineInit final : public QObject {
    Q_OBJECT
public:
    // NFR-PERF-01: the process-wide notifier (created on first call, never destroyed).
    static GstEngineInit *instance();

signals:
    // NFR-PERF-01: the startInitialize() attempt finished (after its afterInit
    // job). ms: duration of the initialize() that set the state (0 in STORE).
    void ready(bool ok, const QString &error, qint64 ms);
    // NFR-PERF-02: preloadDecoders() finished; loaded = features loaded, ms = job duration.
    void decodersPreloaded(int loaded, qint64 ms);

protected:
    bool event(QEvent *event) override;

private:
    GstEngineInit();
};

} } // namespace nordtile::player
