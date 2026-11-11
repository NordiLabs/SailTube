// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-09 background freeze + NFR-PERF-06 CPU budget. Time-injected state
// machine (milliseconds from any monotonic clock; never read here).
//
//   app inactive for kFreezeGraceMs (800 ms) with a video branch
//       -> freezeVideo: stop video decoding, in EVERY mode with video
//          (Dual, Muxed, Hls, Local). Audio continues with no gap.
//   active again before the grace ends -> the pending freeze is cancelled.
//   active again while frozen -> thawVideo immediately, WITHOUT a flush-seek
//          (a flush re-prerolls the sink, drags the pipeline to PAUSED and
//          silences audio); the video catches up by itself.
//   kRepaintDelayMs (250 ms) after any inactive -> active transition with
//          video -> repaint (re-upload the frame onto a possibly rebuilt GL
//          context).
//   teardown: if needsUnlockBeforeTeardown(), unlock / remove the freeze
//          first (a state-locked bin would not follow the pipeline to NULL),
//          then call onTeardown().
//
// Timers: no periodic timer. After every call, (re)arm ONE single-shot timer
// for nextDeadlineMs() - now (or stop it when -1) and call onTimer(now) when
// it fires. Early or spurious onTimer() calls are harmless.
//
// What "freeze" means per mode (freezeTarget()):
//   Dual -> LockAndPauseVideoSource: gst_element_set_locked_state(videoSrcBin,
//          TRUE) then set it to PAUSED. The video uridecodebin is a separate
//          bin from the audio source, so audio is untouched. Thaw: locked
//          state FALSE + gst_element_sync_state_with_parent(), no flush.
//   Muxed / Hls / Local -> DropBeforeVideoDecoder: the demuxer is shared with
//          audio, so nothing on its streaming thread may be paused (a PAUSED
//          sink blocks the demuxer and stalls the audio). Install a buffer
//          pad probe on the video decoder's SINK pad (found via
//          deep-element-added, or the parsed-caps video pad when decodebin
//          stops at parsed caps as in hardware mode) that returns
//          GST_PAD_PROBE_DROP for buffers and passes events and queries.
//          Thaw: keep dropping DELTA_UNIT buffers until the first keyframe,
//          then remove the probe; no flush. Decode CPU stops; the demuxer
//          still parses (cheap).
//   AudioOnly -> None (no video branch; never freezes).
//
// Cover video (live picture in the Sailfish active cover; Sailfish has no
// PiP): onCoverVisible(true) while the app is inactive lifts the freeze, as
// if the app were active for decoding only (no repaint deadline: the cover
// item presents frames itself):
//   inactive + cover visible        -> no freeze; a pending freeze is
//          cancelled, a frozen video thaws at once (no flush).
//   inactive + cover hidden again   -> the grace starts again (kFreezeGraceMs),
//          then freezeVideo, exactly as after going inactive.
//   active                          -> the cover input changes nothing.
// The caller decides what "visible" means (cover status Active, cover-video
// setting on, a video plan, no error); this policy only sees the result.

#include "PlayerTypes.h"

#include <cstdint>

namespace nordtile { namespace player {

enum class FreezeTarget { None, LockAndPauseVideoSource, DropBeforeVideoDecoder };

struct FreezeAction {
    bool freezeVideo = false;
    bool thawVideo = false;
    bool repaint = false;
};

class FreezePolicy {
public:
    static const int64_t kFreezeGraceMs = 800;
    static const int64_t kRepaintDelayMs = 250;

    static FreezeTarget freezeTarget(PlayMode mode);

    // A pipeline was built (or its video branch appeared / went away, e.g.
    // listen-only). `hasVideo` false for AudioOnly or a source without video.
    // If the app is already inactive, the grace starts at `nowMs`.
    FreezeAction configure(PlayMode mode, bool hasVideo, int64_t nowMs);

    // Qt application state: active (true) or inactive/minimised/locked (false).
    FreezeAction onAppActive(bool active, int64_t nowMs);

    // Cover video: the cover shows the live picture (true) or not (false).
    // Lifts the background freeze while true and the app is inactive.
    FreezeAction onCoverVisible(bool visible, int64_t nowMs);

    // The single-shot timer fired.
    FreezeAction onTimer(int64_t nowMs);

    // Absolute time of the next deadline, or -1 when nothing is pending.
    int64_t nextDeadlineMs() const;

    bool frozen() const { return frozen_; }
    bool coverVisible() const { return coverVisible_; }
    // The decoded picture is wanted: app active, or the cover shows it.
    bool pictureWanted() const { return active_ || coverVisible_; }
    bool needsUnlockBeforeTeardown() const { return frozen_; }
    FreezeTarget target() const { return hasVideo_ ? freezeTarget(mode_) : FreezeTarget::None; }

    // The pipeline is gone (after the engine unlocked it). Clears frozen and
    // all deadlines; app activity is kept.
    void onTeardown();

private:
    PlayMode mode_ = PlayMode::Dual;
    bool hasVideo_ = false;
    bool active_ = true;
    bool coverVisible_ = false;
    bool frozen_ = false;
    int64_t freezeAt_ = -1;
    int64_t repaintAt_ = -1;
};

} } // namespace nordtile::player
