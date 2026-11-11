// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the player policies in Core/Player/Domain: seek planning,
// split-seek recovery, preroll barrier, speed, buffering, background freeze,
// position reporting and stream switches. Test names follow the requirement
// ids of SAILTUBE_REQUIREMENTS.md. The GStreamer-level halves of the
// acceptance criteria (NFR-TST-05) belong to the host player suite; here the
// engine is simulated so the decisions can be checked against the same
// thresholds. Build and run: tests/player_seek/run.sh
#include "Core/Player/Domain/BufferingPolicy.h"
#include "Core/Player/Domain/FreezePolicy.h"
#include "Core/Player/Domain/PlayerTypes.h"
#include "Core/Player/Domain/PositionPolicy.h"
#include "Core/Player/Domain/PrerollBarrier.h"
#include "Core/Player/Domain/SeekPolicy.h"
#include "Core/Player/Domain/SpeedPolicy.h"
#include "Core/Player/Domain/SplitSeekGuard.h"
#include "Core/Player/Domain/SwitchPlan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// Deterministic pseudo-random numbers for the "20 random seeks" criteria.
static uint32_t rngState = 12345u;
static int64_t randomMs(int64_t maxMs) {
    rngState = rngState * 1103515245u + 12345u;
    return static_cast<int64_t>((rngState >> 8) % static_cast<uint32_t>(maxMs));
}

// ---------------------------------------------------------------- types

static void types_containerFromExtension() {
    CHECK(containerFromExtension("webm") == Container::Matroska);
    CHECK(containerFromExtension("weba") == Container::Matroska);
    CHECK(containerFromExtension("mkv") == Container::Matroska);
    CHECK(containerFromExtension(".WebM") == Container::Matroska);
    CHECK(containerFromExtension("mp4") == Container::IsoBmff);
    CHECK(containerFromExtension("m4a") == Container::IsoBmff);
    CHECK(containerFromExtension("m4v") == Container::IsoBmff);
    CHECK(containerFromExtension("3gp") == Container::IsoBmff);
    CHECK(containerFromExtension("MOV") == Container::IsoBmff);
    CHECK(containerFromExtension("ts") == Container::Unknown);
    CHECK(containerFromExtension("") == Container::Unknown);
    CHECK(msToNs(1500) == 1500000000LL);
    CHECK(nsToMs(2500000000LL) == 2500);
}

// ---------------------------------------------------------------- PLY-02

static void ply02_syncModel() {
    VideoSinkSync s = videoSinkSync();
    CHECK(s.sync);
    CHECK(s.qos);
    CHECK(s.maxBuffers == 2);
    CHECK(s.drop);
    CHECK(avOffsetWithinBudget(10000, 10080));
    CHECK(avOffsetWithinBudget(10080, 10000));
    CHECK(!avOffsetWithinBudget(10000, 10081));
}

// ---------------------------------------------------------------- PLY-03

static SeekRequest req(PlayMode mode, Container v, Container a, int64_t target, double rate = 1.0) {
    SeekRequest r;
    r.mode = mode; r.video = v; r.audio = a; r.targetMs = target; r.rate = rate;
    return r;
}

static void ply03_webmDualSeeksEachSinkAccurate() {
    SeekPlan p = planSeek(req(PlayMode::Dual, Container::Matroska, Container::Matroska, 42000, 1.5));
    CHECK(p.now.size() == 2);
    CHECK(p.now[0].branch == Branch::Video);
    CHECK(p.now[1].branch == Branch::Audio);
    for (size_t i = 0; i < p.now.size(); ++i) {
        CHECK(p.now[i].flags == SeekFlags::flushAccurate());
        CHECK(p.now[i].positionNs == msToNs(42000));
        CHECK(near(p.now[i].rate, 1.5));
    }
    CHECK(!p.alignAudioAfterAsyncDone);
    CHECK(p.targetMs == 42000);
}

static void ply03_mp4VideoKeyUnitThenAlignAudio() {
    SeekPlan p = planSeek(req(PlayMode::Dual, Container::IsoBmff, Container::IsoBmff, 61000, 0.75));
    CHECK(p.now.size() == 1);
    CHECK(p.now[0].branch == Branch::Video);
    CHECK(p.now[0].flags == SeekFlags::flushKeyUnit());
    CHECK(near(p.now[0].rate, 0.75));
    CHECK(p.alignAudioAfterAsyncDone);
    CHECK(p.audioAlignFlags == SeekFlags::flushKeyUnit());

    SeekCommand a = planAudioAlign(p, true, 59800);
    CHECK(a.branch == Branch::Audio);
    CHECK(a.positionNs == msToNs(59800));    // where the video landed, not the target
    CHECK(near(a.rate, 0.75));
    SeekCommand fallback = planAudioAlign(p, false, 0);
    CHECK(fallback.positionNs == msToNs(61000));  // query failed: use the target

    // mp4 video + webm audio: the align is accurate on the Matroska audio.
    SeekPlan mixed = planSeek(req(PlayMode::Dual, Container::IsoBmff, Container::Matroska, 5000));
    CHECK(mixed.alignAudioAfterAsyncDone);
    CHECK(mixed.audioAlignFlags == SeekFlags::flushAccurate());
    // webm video + m4a audio: both now, each with its own container's flags.
    SeekPlan mixed2 = planSeek(req(PlayMode::Dual, Container::Matroska, Container::IsoBmff, 5000));
    CHECK(mixed2.now.size() == 2);
    CHECK(mixed2.now[0].flags == SeekFlags::flushAccurate());
    CHECK(mixed2.now[1].flags == SeekFlags::flushKeyUnit());
    CHECK(!mixed2.alignAudioAfterAsyncDone);
}

static void ply03_singleSourceModesOneKeyUnitSeek() {
    const PlayMode modes[] = {PlayMode::Muxed, PlayMode::Hls, PlayMode::Local};
    for (int i = 0; i < 3; ++i) {
        SeekPlan p = planSeek(req(modes[i], Container::IsoBmff, Container::IsoBmff, 1000, 2.0));
        CHECK(p.now.size() == 1);
        CHECK(p.now[0].branch == Branch::Muxed);
        CHECK(p.now[0].flags == SeekFlags::flushKeyUnit());
        CHECK(near(p.now[0].rate, 2.0));
        CHECK(!p.alignAudioAfterAsyncDone);
    }
    SeekPlan ao = planSeek(req(PlayMode::AudioOnly, Container::Unknown, Container::Matroska, 1000));
    CHECK(ao.now.size() == 1);
    CHECK(ao.now[0].branch == Branch::Audio);
    CHECK(ao.now[0].flags == SeekFlags::flushAccurate());
}

static void ply03_everySeekCarriesSaneRateAndClampedTarget() {
    SeekRequest r = req(PlayMode::Dual, Container::Matroska, Container::Matroska, -500, 0.0);
    SeekPlan p = planSeek(r);
    CHECK(p.targetMs == 0);
    CHECK(near(p.now[0].rate, 1.0));
    r.targetMs = 999999; r.durationMs = 300000; r.rate = std::nan("");
    p = planSeek(r);
    CHECK(p.targetMs == 300000);
    CHECK(near(p.now[1].rate, 1.0));
    // Unknown video container gets the safe (key-unit) flags, never accurate.
    SeekPlan u = planSeek(req(PlayMode::Dual, Container::Unknown, Container::Unknown, 1000));
    CHECK(u.now[0].flags == SeekFlags::flushKeyUnit());
    CHECK(u.alignAudioAfterAsyncDone);
}

// Simulated dual pipeline executing plans the way the engine will.
struct FakeDual {
    int64_t gopMs;           // mp4 keyframe spacing
    int64_t videoMs = 0, audioMs = 0;
    int flushSeeks = 0;
    explicit FakeDual(int64_t gop) : gopMs(gop) {}

    int64_t land(const SeekCommand &c) const {
        const int64_t ms = nsToMs(c.positionNs);
        if (c.flags.keyUnit) return (c.branch == Branch::Audio) ? ms : (ms / gopMs) * gopMs;  // audio: every sample is a sync sample
        return ms;  // accurate
    }
    void send(const SeekCommand &c) {
        if (c.flags.flush) ++flushSeeks;
        const int64_t at = land(c);
        if (c.branch == Branch::Video) videoMs = at; else audioMs = at;
    }
    // Engine: execute the plan, then the async-done half (PLY-03 + PLY-24).
    int64_t execute(const SeekPlan &plan, PositionPolicy &pos) {
        pos.onSeekRequested(plan.targetMs);
        for (size_t i = 0; i < plan.now.size(); ++i) send(plan.now[i]);
        // async-done: query the video sink's landed position
        if (plan.alignAudioAfterAsyncDone) send(planAudioAlign(plan, true, videoMs));
        pos.onSeekLanded(true, videoMs);
        return videoMs;
    }
};

static void ply03_twentyRandomSeeksLandWithinBudget() {
    const int64_t duration = 600000;
    // WebM: within 100 ms of target, A/V offset <= 80 ms.
    {
        FakeDual dual(2000);
        PositionPolicy pos;
        for (int i = 0; i < 20; ++i) {
            const int64_t target = randomMs(duration);
            SeekRequest r = req(PlayMode::Dual, Container::Matroska, Container::Matroska, target, 1.25);
            r.durationMs = duration;
            const int64_t landed = dual.execute(planSeek(r), pos);
            CHECK(landed <= target + 100 && landed >= target - 100);
            CHECK(avOffsetWithinBudget(dual.videoMs, dual.audioMs));
            CHECK(pos.positionMs() == landed);     // scrubber shows the landed position
        }
    }
    // mp4: at most one GOP before target, A/V offset <= 80 ms.
    {
        const int64_t gop = 2000;
        FakeDual dual(gop);
        PositionPolicy pos;
        for (int i = 0; i < 20; ++i) {
            const int64_t target = randomMs(duration);
            SeekRequest r = req(PlayMode::Dual, Container::IsoBmff, Container::IsoBmff, target);
            r.durationMs = duration;
            const int64_t landed = dual.execute(planSeek(r), pos);
            CHECK(landed <= target && landed >= target - gop);
            CHECK(avOffsetWithinBudget(dual.videoMs, dual.audioMs));
            CHECK(pos.positionMs() == landed);
        }
    }
}

// NFR-PERF-03 "double-tap burst = 1 flush": the coalesced burst becomes one
// seek request; its plan issues exactly one flush per sink (one for muxed).
static void nfrPerf03_oneSeekIsOneFlushPerSink() {
    FakeDual dual(2000);
    PositionPolicy pos;
    dual.execute(planSeek(req(PlayMode::Dual, Container::Matroska, Container::Matroska, 30000)), pos);
    CHECK(dual.flushSeeks == 2);  // video sink + audio sink, one each
    SeekPlan muxed = planSeek(req(PlayMode::Muxed, Container::Unknown, Container::Unknown, 30000));
    int flushes = 0;
    for (size_t i = 0; i < muxed.now.size(); ++i) if (muxed.now[i].flags.flush) ++flushes;
    CHECK(flushes == 1);
}

// ---------------------------------------------------------------- PLY-04

static void ply04_diskBufferForMp4Branches() {
    SourceBufferParams mp4 = sourceBufferFor(PlayMode::Dual, Container::IsoBmff);
    CHECK(mp4.download);
    CHECK(mp4.bufferDurationMs == 1000);
    CHECK(mp4.bufferSizeBytes == 1536 * 1024);
    CHECK(needsDiskBackedBuffer(PlayMode::Dual, Container::IsoBmff));
    CHECK(needsDiskBackedBuffer(PlayMode::AudioOnly, Container::IsoBmff));

    SourceBufferParams webm = sourceBufferFor(PlayMode::Dual, Container::Matroska);
    CHECK(!webm.download);
    CHECK(webm.bufferDurationMs == 3000);
    CHECK(!needsDiskBackedBuffer(PlayMode::Dual, Container::Matroska));
    CHECK(!needsDiskBackedBuffer(PlayMode::Dual, Container::Unknown));
}

static void ply04_hintsClearedInMuxedMode() {
    CHECK(containerHintFor(PlayMode::Muxed, Container::IsoBmff) == Container::Unknown);
    CHECK(containerHintFor(PlayMode::Hls, Container::IsoBmff) == Container::Unknown);
    CHECK(containerHintFor(PlayMode::Local, Container::IsoBmff) == Container::Unknown);
    CHECK(containerHintFor(PlayMode::Dual, Container::IsoBmff) == Container::IsoBmff);
    CHECK(!needsDiskBackedBuffer(PlayMode::Muxed, Container::IsoBmff));
    CHECK(!sourceBufferFor(PlayMode::Muxed, Container::IsoBmff).download);
    SourceBufferParams hls = sourceBufferFor(PlayMode::Hls, Container::IsoBmff);
    CHECK(!hls.download && hls.bufferDurationMs == 0 && hls.bufferSizeBytes == 0);
}

static void ply04_neverAccurateOnMp4Branch() {
    // 20 of 20 seeks: no accurate flag on any mp4 branch, video or audio align.
    for (int i = 0; i < 20; ++i) {
        const int64_t target = randomMs(600000);
        const Container audio = (i % 2) ? Container::IsoBmff : Container::Matroska;
        SeekPlan p = planSeek(req(PlayMode::Dual, Container::IsoBmff, audio, target));
        for (size_t k = 0; k < p.now.size(); ++k) CHECK(!p.now[k].flags.accurate && p.now[k].flags.flush);
        if (audio == Container::IsoBmff) CHECK(!p.audioAlignFlags.accurate);
        SeekPlan q = planSeek(req(PlayMode::Dual, Container::Matroska, Container::IsoBmff, target));
        CHECK(!q.now[1].flags.accurate);
    }
}

static void ply04_hintsSetBeforeUrls() {
    SwitchRequest r;
    r.mode = PlayMode::Dual; r.videoContainer = Container::IsoBmff; r.audioContainer = Container::IsoBmff;
    SwitchPlan p = planSwitch(r);
    int hints = -1, urls = -1;
    for (size_t i = 0; i < p.steps.size(); ++i) {
        if (p.steps[i] == SwitchStep::SetContainerHints) hints = static_cast<int>(i);
        if (p.steps[i] == SwitchStep::SetUrls) urls = static_cast<int>(i);
    }
    CHECK(hints >= 0 && urls > hints);
    CHECK(p.videoHint == Container::IsoBmff);
    r.mode = PlayMode::Muxed;
    p = planSwitch(r);
    CHECK(p.videoHint == Container::Unknown && p.audioHint == Container::Unknown);
}

// ---------------------------------------------------------------- PLY-05

static void ply05_resendThenRebuildThenGiveUp() {
    SplitSeekGuard g;
    g.onSourcesSet("v1|a1");
    g.onUserSeek();
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::ResendAfter250ms);
    CHECK(SplitSeekGuard::kResendDelayMs == 250);
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::RebuildOnceWithDeferredSeek);
    g.onTeardown();                 // inside the rebuild: no reset
    g.onSourcesSet("v1|a1");        // same URLs: no reset
    CHECK(g.onSeekResult(false, true) == SplitSeekAction::GiveUpKeepPlaying);
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::GiveUpKeepPlaying);
    CHECK(g.rebuildUsed() && g.gaveUp());
}

static void ply05_rebuildsAtMostOnce() {
    // An unseekable audio branch refuses every seek; the engine obeys the
    // guard. The episode must terminate with at most one rebuild.
    SplitSeekGuard g;
    g.onSourcesSet("v|a");
    g.onUserSeek();
    int rebuilds = 0, resends = 0, rounds = 0;
    SplitSeekAction a = g.onSeekResult(true, false);
    while (a != SplitSeekAction::GiveUpKeepPlaying && a != SplitSeekAction::None && rounds < 100) {
        ++rounds;
        if (a == SplitSeekAction::RebuildOnceWithDeferredSeek) {
            ++rebuilds;
            g.onTeardown();
            g.onSourcesSet("v|a");
        }
        if (a == SplitSeekAction::ResendAfter250ms) ++resends;
        a = g.onSeekResult(true, false);  // still unseekable
    }
    CHECK(rebuilds == 1);
    CHECK(resends == 1);
    CHECK(rounds < 100);
    CHECK(a == SplitSeekAction::GiveUpKeepPlaying);
}

static void ply05_allowanceResets() {
    SplitSeekGuard g;
    g.onSourcesSet("v|a");
    g.onSeekResult(true, false);
    g.onSeekResult(true, false);    // rebuild used
    g.onUserSeek();                 // new user seek -> fresh allowance
    CHECK(!g.rebuildUsed());
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::ResendAfter250ms);
    CHECK(g.onSeekResult(true, true) == SplitSeekAction::None);   // both accept -> reset
    CHECK(g.splitsThisEpisode() == 0);
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::ResendAfter250ms);
    CHECK(g.onSeekResult(true, false) == SplitSeekAction::RebuildOnceWithDeferredSeek);
    g.onSourcesSet("v2|a");         // new URL -> reset
    CHECK(!g.rebuildUsed());
    CHECK(g.onSeekResult(false, true) == SplitSeekAction::ResendAfter250ms);
}

static void ply05_bothRefusingIsNotADesync() {
    SplitSeekGuard g;
    g.onSourcesSet("v|a");
    CHECK(g.onSeekResult(false, false) == SplitSeekAction::None);
    CHECK(g.splitsThisEpisode() == 0);
    // Refused mp4 video phase (audio never sent) is reported as (false, false).
    CHECK(g.onSeekResult(false, false) == SplitSeekAction::None);
}

static void ply05_refusedAudioAlignCountsAsSplit() {
    SplitSeekGuard g;
    g.onSourcesSet("v|a");
    g.onUserSeek();
    CHECK(g.onAudioAlignResult(false) == SplitSeekAction::ResendAfter250ms);
    CHECK(g.onAudioAlignResult(false) == SplitSeekAction::RebuildOnceWithDeferredSeek);
    CHECK(g.onAudioAlignResult(false) == SplitSeekAction::GiveUpKeepPlaying);
    g.onUserSeek();
    CHECK(g.onAudioAlignResult(true) == SplitSeekAction::None);
}

// ---------------------------------------------------------------- PLY-06

static void ply06_resumeAndSpeedOneFlushSeekAfterFirstAsyncDone() {
    // Accept: resume 120 s, remembered 1.5x, preroll delayed by 2 s ->
    // exactly one flush-seek, after the first async-done, carrying rate 1.5,
    // and nothing that would render from position 0 first.
    PrerollBarrier b;
    b.arm(120000, 1.5);
    int flushSeeks = 0;
    double seekRate = 0;
    int64_t seekNs = -1;
    // 2 s of preroll: the barrier has no timers, so time passing changes
    // nothing; a user tap on speed during preroll is also absorbed.
    for (int64_t t = 0; t < 2000; t += 100) CHECK(b.deferRate(1.5));
    CHECK(!b.prerolled());
    DeferredSeek d;
    CHECK(b.onAsyncDone(&d));   // first async-done
    CHECK(b.prerolled());
    SeekPlan p = planSeek(req(PlayMode::Dual, Container::Matroska, Container::Matroska, d.positionMs, d.rate));
    // One logical seek: one flush per sink, identical target and rate.
    for (size_t i = 0; i < p.now.size(); ++i) {
        CHECK(p.now[i].flags.flush);
        seekRate = p.now[i].rate;
        seekNs = p.now[i].positionNs;
    }
    ++flushSeeks;
    CHECK(near(seekRate, 1.5));
    CHECK(seekNs == msToNs(120000));
    // That flush-seek produces its own async-done: nothing more is released.
    DeferredSeek again;
    CHECK(!b.onAsyncDone(&again));
    CHECK(!b.onAsyncDone(&again));
    CHECK(flushSeeks == 1);
    // After preroll, requests are no longer deferred: the caller seeks now.
    CHECK(!b.deferSeek(5000));
    CHECK(!b.deferRate(2.0));
}

static void ply06_nothingToApplyAtStartAndNormalSpeed() {
    PrerollBarrier b;
    DeferredSeek d;
    CHECK(!b.onAsyncDone(&d));       // not armed
    b.arm(0, 1.0);
    CHECK(!b.onAsyncDone(&d));       // position 0, rate 1 -> no seek
    CHECK(b.prerolled());
    b.arm(-30, 0.0);                 // sanitised to 0 / 1.0
    CHECK(!b.onAsyncDone(&d));
    b.arm(0, 2.0);                   // rate only: seek to 0 carrying 2.0
    CHECK(b.onAsyncDone(&d));
    CHECK(d.positionMs == 0 && near(d.rate, 2.0));
}

static void ply06_latestRequestWinsAndRearm() {
    PrerollBarrier b;
    b.arm(10000, 1.0);
    CHECK(b.deferSeek(20000));
    CHECK(b.deferSeek(25000));       // user scrubbed twice before preroll
    CHECK(b.pendingPositionMs() == 25000);
    DeferredSeek d;
    CHECK(b.onAsyncDone(&d) && d.positionMs == 25000);
    b.arm(40000, 1.25);              // rebuild / new URL re-arms
    CHECK(!b.prerolled());
    CHECK(b.onAsyncDone(&d) && d.positionMs == 40000 && near(d.rate, 1.25));
}

// PLY-06 start seek before preroll: every branch accepted -> the first
// async-done releases nothing (one flush per branch in total, none at 0).
static void ply06_startSeekBeforePrerollReleasesNothing() {
    PrerollBarrier b;
    b.arm(5000, 1.5);
    CHECK(b.wantsStartSeek());
    CHECK(startSeekBeforePreroll(PlayMode::Dual) && startSeekBeforePreroll(PlayMode::Muxed));
    CHECK(startSeekBeforePreroll(PlayMode::Local) && startSeekBeforePreroll(PlayMode::AudioOnly));
    b.onStartSeekApplied(5000, 1.5);
    CHECK(b.startSeekApplied() && !b.wantsStartSeek());
    DeferredSeek d;
    CHECK(!b.onAsyncDone(&d));      // nothing left: the preroll is at 5 s already
    CHECK(b.prerolled());
    CHECK(!b.onAsyncDone(&d));
    CHECK(!b.deferSeek(1000));      // after preroll: seek now
    // Rate only (position 0, remembered 1.5x) also goes through the start seek.
    b.arm(0, 1.5);
    CHECK(b.wantsStartSeek());
    b.onStartSeekApplied(0, 1.5);
    CHECK(!b.onAsyncDone(&d));
    // Nothing to apply: no start seek wanted at all.
    b.arm(0, 1.0);
    CHECK(!b.wantsStartSeek());
    // Re-arming forgets an earlier start seek.
    b.arm(7000, 1.0);
    CHECK(b.wantsStartSeek() && !b.startSeekApplied());
}

// PLY-06: a refused start seek (or a branch that could not be held) leaves
// the barrier as it was: the first async-done releases the classic seek.
static void ply06_startSeekRefusedFallsBackToBarrier() {
    PrerollBarrier b;
    b.arm(5000, 1.25);
    CHECK(b.wantsStartSeek());
    // no onStartSeekApplied()
    DeferredSeek d;
    CHECK(b.onAsyncDone(&d));
    CHECK(d.positionMs == 5000 && near(d.rate, 1.25));
    CHECK(!startSeekBeforePreroll(PlayMode::Hls));  // HLS always takes the classic path
}

// PLY-06: a user seek or speed change after the start seek but before the
// first async-done is not lost: the barrier releases the newer values.
static void ply06_userChangeAfterStartSeekIsReleased() {
    PrerollBarrier b;
    b.arm(5000, 1.0);
    b.onStartSeekApplied(5000, 1.0);
    CHECK(b.deferSeek(9000));
    DeferredSeek d;
    CHECK(b.onAsyncDone(&d) && d.positionMs == 9000 && near(d.rate, 1.0));
    b.arm(5000, 1.0);
    b.onStartSeekApplied(5000, 1.0);
    CHECK(b.deferRate(2.0));
    CHECK(b.onAsyncDone(&d) && d.positionMs == 5000 && near(d.rate, 2.0));
    b.arm(5000, 1.0);
    b.onStartSeekApplied(5000, 1.0);
    CHECK(b.deferSeek(0));          // back to the start at 1x: still a seek (the pipeline is at 5 s)
    CHECK(b.onAsyncDone(&d) && d.positionMs == 0);
    b.arm(5000, 1.0);
    b.onStartSeekApplied(5000, 1.0);
    CHECK(b.deferSeek(5000));       // same target again: nothing new
    CHECK(!b.onAsyncDone(&d));
    // A late start position arrives while the first branch's seek is out: the
    // applied values are what was sent, so the newer target is still released.
    b.arm(0, 1.0);
    CHECK(!b.wantsStartSeek());
    CHECK(b.deferSeek(5000));       // VideoPlayer::setStartPosition right after load()
    CHECK(b.wantsStartSeek());
    CHECK(b.deferSeek(6000));
    b.onStartSeekApplied(5000, 1.0);
    CHECK(b.onAsyncDone(&d) && d.positionMs == 6000);
}

// ---------------------------------------------------------------- PLY-07

static void ply07_eightSteps() {
    const double expected[] = {0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0};
    CHECK(speed::kStepCount == 8);
    for (int i = 0; i < 8; ++i) {
        CHECK(near(speed::step(i), expected[i]));
        CHECK(speed::indexOf(expected[i]) == i);
        CHECK(near(speed::snap(expected[i]), expected[i]));
    }
    CHECK(near(speed::snap(0.1), 0.25));
    CHECK(near(speed::snap(3.0), 2.0));
    CHECK(near(speed::snap(1.1), 1.0));
    CHECK(near(speed::snap(1.2), 1.25));
    CHECK(near(speed::snap(0.375), 0.5));      // halfway rounds up
    CHECK(near(speed::snap(std::nan("")), 1.0));
    CHECK(near(speed::next(1.0), 1.25));
    CHECK(near(speed::next(2.0), 2.0));
    CHECK(near(speed::previous(1.0), 0.75));
    CHECK(near(speed::previous(0.25), 0.25));
    CHECK(near(speed::next(1.1), 1.25));
}

static void ply07_speedCarriesOverToNextVideo() {
    double remembered = 1.0;
    remembered = speed::valueToRemember(remembered, 1.5, false);
    CHECK(near(remembered, 1.5));
    // Next video: the remembered rate is armed before the pipeline exists and
    // engaged by the first seek (before the first frame).
    PrerollBarrier b;
    b.arm(0, speed::rateToApply(remembered, false));
    DeferredSeek d;
    CHECK(b.onAsyncDone(&d));
    CHECK(near(d.rate, 1.5));
    SeekPlan p = planSeek(req(PlayMode::Muxed, Container::Unknown, Container::Unknown, d.positionMs, d.rate));
    CHECK(near(p.now[0].rate, 1.5));
}

static void ply07_notChangeableAtLiveEdge() {
    CHECK(!speed::speedChangeable(true));
    CHECK(speed::speedChangeable(false));
    CHECK(near(speed::rateToApply(1.75, true), 1.0));
    CHECK(near(speed::valueToRemember(1.75, 2.0, true), 1.75));  // live leaves memory alone
    CHECK(near(speed::rateToApply(1.75, false), 1.75));          // and VOD gets it back
}

// ---------------------------------------------------------------- PLY-08

static void ply08_underrunPausesAndResumesAtFull() {
    // Accept: video branch stalls for 5 s -> paused and buffering raised
    // within 1 s (here: on the first message), resume only at 100 %.
    BufferingPolicy b;
    b.reset(true, false);
    BufferingAction a = b.onUserIntent(true);
    CHECK(a.resumePipeline && !a.showIndicator);
    a = b.onBufferingPercent(Branch::Video, 40);
    CHECK(a.pausePipeline && a.showIndicator && b.buffering());
    for (int pct = 45; pct < 100; pct += 6) {
        a = b.onBufferingPercent(Branch::Video, pct);
        CHECK(!a.resumePipeline && !a.pausePipeline && a.showIndicator);
    }
    a = b.onBufferingPercent(Branch::Video, 99);
    CHECK(!a.resumePipeline);
    a = b.onBufferingPercent(Branch::Video, 100);
    CHECK(a.resumePipeline && !a.showIndicator && !b.buffering());
}

static void ply08_everyNetworkBranchMustRefill() {
    BufferingPolicy b;
    b.reset(true, false);
    b.onUserIntent(true);
    CHECK(b.onBufferingPercent(Branch::Video, 20).pausePipeline);
    CHECK(!b.onBufferingPercent(Branch::Audio, 50).pausePipeline);  // already paused
    CHECK(!b.onBufferingPercent(Branch::Video, 100).resumePipeline); // audio still low
    CHECK(b.onBufferingPercent(Branch::Audio, 100).resumePipeline);
}

static void ply08_userPauseDuringBufferingIsRespected() {
    BufferingPolicy b;
    b.reset(true, false);
    b.onUserIntent(true);
    b.onBufferingPercent(Branch::Muxed, 10);
    BufferingAction a = b.onUserIntent(false);
    CHECK(a.pausePipeline && !a.showIndicator);
    a = b.onBufferingPercent(Branch::Muxed, 100);
    CHECK(!a.resumePipeline);                       // stays paused
    // Play pressed during a later stall only resumes when the buffer is full.
    b.onBufferingPercent(Branch::Muxed, 30);
    a = b.onUserIntent(true);
    CHECK(!a.resumePipeline && a.showIndicator);
    CHECK(b.onBufferingPercent(Branch::Muxed, 100).resumePipeline);
}

static void ply08_liveHlsLowLatencyPolicy() {
    BufferingPolicy b;
    b.reset(true, true);
    b.onUserIntent(true);
    BufferingAction a = b.onBufferingPercent(Branch::Muxed, 40);   // VOD would pause here
    CHECK(!a.pausePipeline && !a.showIndicator && !b.buffering());
    a = b.onBufferingPercent(Branch::Muxed, 5);                    // nearly empty
    CHECK(a.pausePipeline && a.showIndicator);
    a = b.onBufferingPercent(Branch::Muxed, 59);
    CHECK(!a.resumePipeline);
    a = b.onBufferingPercent(Branch::Muxed, 60);                   // resumes early
    CHECK(a.resumePipeline && !b.buffering());
}

static void ply08_localFilesNeverBuffer() {
    BufferingPolicy b;
    b.reset(false, false);
    b.onUserIntent(true);
    CHECK(!b.onBufferingPercent(Branch::Muxed, 0).pausePipeline);
    CHECK(!b.buffering());
}

// PLY-08 start phase: the first preroll never waits for a buffer level and a
// branch's first fill never pauses; a branch that filled once follows the
// normal rule (underrun pauses, resume at 100 %).
static void ply08_startPhaseDoesNotWaitForFullBuffer() {
    BufferingPolicy b;
    b.reset(true, false);
    b.beginStartPhase();
    CHECK(b.inStartPhase());
    b.onUserIntent(true);
    // Initial fill before and after the first frame, including queue2's 0 %: never buffering.
    for (int pct = 0; pct < 100; pct += 7) CHECK(!b.onBufferingPercent(Branch::Video, pct).pausePipeline);
    CHECK(!b.onBufferingPercent(Branch::Audio, 0).pausePipeline);
    CHECK(!b.onBufferingPercent(Branch::Audio, 12).pausePipeline);
    CHECK(!b.buffering() && b.wantsPlaying() && !b.current().showIndicator);
    // Video fills once; a later drop is a real underrun.
    CHECK(!b.onBufferingPercent(Branch::Video, 100).resumePipeline);
    BufferingAction a = b.onBufferingPercent(Branch::Video, 40);
    CHECK(a.pausePipeline && a.showIndicator && b.buffering());
    CHECK(!b.onBufferingPercent(Branch::Video, 99).resumePipeline);
    a = b.onBufferingPercent(Branch::Video, 100);
    CHECK(a.resumePipeline && !b.buffering());                       // audio (still filling, 12 %) does not hold it
    // The audio fills too: from then on its underrun counts.
    CHECK(!b.onBufferingPercent(Branch::Audio, 100).pausePipeline);
    CHECK(b.onBufferingPercent(Branch::Audio, 0).pausePipeline);
    CHECK(b.onBufferingPercent(Branch::Audio, 100).resumePipeline);
}

static void ply08_startPhaseEndsWithReset() {
    BufferingPolicy b;
    b.reset(true, false);
    b.beginStartPhase();
    b.onUserIntent(true);
    CHECK(!b.onBufferingPercent(Branch::Muxed, 20).pausePipeline);
    // reset() ends the start phase: the classic rule applies again (a new build that does not
    // opt in, e.g. other users of the policy).
    b.reset(true, false);
    CHECK(!b.inStartPhase());
    CHECK(b.onBufferingPercent(Branch::Muxed, 40).pausePipeline);
    // Live HLS start phase: the first fill does not hold it either; 60 % counts as filled.
    b.reset(true, true);
    b.beginStartPhase();
    CHECK(!b.onBufferingPercent(Branch::Muxed, 2).pausePipeline);
    CHECK(!b.onBufferingPercent(Branch::Muxed, 60).resumePipeline);
    CHECK(b.onBufferingPercent(Branch::Muxed, 5).pausePipeline);     // filled once: live rule (< 10 %)
    CHECK(b.onBufferingPercent(Branch::Muxed, 60).resumePipeline);
    // Local files: no buffering in the start phase either.
    b.reset(false, false);
    b.beginStartPhase();
    CHECK(!b.onBufferingPercent(Branch::Muxed, 0).pausePipeline);
}

// ---------------------------------------------------------------- PLY-09

static void ply09_freezesEveryModeWithVideo() {
    const PlayMode modes[] = {PlayMode::Dual, PlayMode::Muxed, PlayMode::Hls, PlayMode::Local};
    for (int i = 0; i < 4; ++i) {
        FreezePolicy f;
        f.configure(modes[i], true, 0);
        CHECK(f.nextDeadlineMs() == -1);
        f.onAppActive(false, 1000);
        CHECK(f.nextDeadlineMs() == 1800);
        CHECK(!f.onTimer(1799).freezeVideo);
        FreezeAction a = f.onTimer(1800);
        CHECK(a.freezeVideo && f.frozen());
        CHECK(f.target() != FreezeTarget::None);
        CHECK(f.nextDeadlineMs() == -1);   // NFR-PERF-06: nothing wakes the app while hidden
    }
    CHECK(FreezePolicy::freezeTarget(PlayMode::Dual) == FreezeTarget::LockAndPauseVideoSource);
    CHECK(FreezePolicy::freezeTarget(PlayMode::Muxed) == FreezeTarget::DropBeforeVideoDecoder);
    CHECK(FreezePolicy::freezeTarget(PlayMode::Hls) == FreezeTarget::DropBeforeVideoDecoder);
    CHECK(FreezePolicy::freezeTarget(PlayMode::Local) == FreezeTarget::DropBeforeVideoDecoder);
}

static void ply09_noFreezeWithoutVideo() {
    FreezePolicy f;
    f.configure(PlayMode::AudioOnly, false, 0);
    f.onAppActive(false, 0);
    CHECK(f.nextDeadlineMs() == -1);
    CHECK(!f.onTimer(5000).freezeVideo);
    FreezePolicy g;
    g.configure(PlayMode::Dual, false, 0);   // listen-only on a dual source
    g.onAppActive(false, 0);
    CHECK(!g.onTimer(5000).freezeVideo);
}

static void ply09_activeBeforeGraceCancels() {
    FreezePolicy f;
    f.configure(PlayMode::Dual, true, 0);
    f.onAppActive(false, 0);
    FreezeAction a = f.onAppActive(true, 500);
    CHECK(!a.thawVideo && !f.frozen());
    CHECK(f.nextDeadlineMs() == 750);        // the freeze deadline is gone; a repaint follows
    a = f.onTimer(800);
    CHECK(!a.freezeVideo && a.repaint);
    CHECK(f.nextDeadlineMs() == -1);
}

static void ply09_thawImmediatelyAndRepaintAfter250ms() {
    FreezePolicy f;
    f.configure(PlayMode::Muxed, true, 0);
    f.onAppActive(false, 0);
    f.onTimer(800);
    CHECK(f.frozen());
    FreezeAction a = f.onAppActive(true, 60000);
    CHECK(a.thawVideo);           // immediate; the engine does NOT flush
    CHECK(!a.repaint);
    CHECK(!f.frozen());
    CHECK(f.nextDeadlineMs() == 60250);
    CHECK(!f.onTimer(60249).repaint);
    CHECK(f.onTimer(60250).repaint);
    CHECK(f.nextDeadlineMs() == -1);
}

static void ply09_teardownUnlocksFirst() {
    FreezePolicy f;
    f.configure(PlayMode::Dual, true, 0);
    CHECK(!f.needsUnlockBeforeTeardown());
    f.onAppActive(false, 0);
    f.onTimer(800);
    CHECK(f.needsUnlockBeforeTeardown());
    f.onTeardown();
    CHECK(!f.needsUnlockBeforeTeardown());
    CHECK(f.nextDeadlineMs() == -1);
    // A new pipeline built while still hidden (autoplay) gets its own grace.
    f.configure(PlayMode::Hls, true, 10000);
    CHECK(f.nextDeadlineMs() == 10800);
    CHECK(f.onTimer(10800).freezeVideo);
}

// NFR-PERF-06: decode stops within 1 s of hiding (800 ms grace), and
// no app timer above 1 Hz while hidden.
static void nfrPerf06_backgroundBudget() {
    FreezePolicy f;
    f.configure(PlayMode::Dual, true, 0);
    const int64_t hiddenAt = 5000;
    f.onAppActive(false, hiddenAt);
    const int64_t deadline = f.nextDeadlineMs();
    CHECK(deadline - hiddenAt <= 1000);
    CHECK(f.onTimer(deadline).freezeVideo);
    CHECK(f.nextDeadlineMs() == -1);
    PositionPolicy pos;
    pos.onDurationQuery(true, 300000);
    CHECK(pos.pollIntervalMs(true, false, true) >= 1000);   // hidden: at most 1 Hz
    CHECK(pos.pollIntervalMs(false, false, true) == 0);     // nobody listening
    CHECK(pos.pollIntervalMs(true, false, false) == 0);     // paused and hidden
}

// ---------------------------------------------------------------- PLY-24

static int updatesPerSecond(int64_t intervalMs) {
    if (intervalMs <= 0) return 0;
    int n = 0;
    for (int64_t t = intervalMs; t <= 1000; t += intervalMs) ++n;
    return n;
}

static void ply24_updateRates() {
    PositionPolicy p;
    p.onDurationQuery(true, 600000);
    CHECK(updatesPerSecond(p.pollIntervalMs(true, true, true)) >= 4);
    CHECK(updatesPerSecond(p.pollIntervalMs(true, false, true)) <= 1);
    CHECK(updatesPerSecond(p.pollIntervalMs(true, false, true)) >= 1);
    CHECK(updatesPerSecond(p.pollIntervalMs(false, true, true)) == 0);
    CHECK(p.pollIntervalMs(false, true, true) == 0);
}

static void ply24_seekReportsTargetThenLanded() {
    PositionPolicy p;
    p.onDurationQuery(true, 600000);
    p.onPositionQuery(true, 10000);
    p.onSeekRequested(90000);
    CHECK(p.positionMs() == 90000);          // first report after a seek = target
    p.onPositionQuery(true, 10040);          // stale query mid-seek: ignored
    CHECK(p.positionMs() == 90000);
    p.onSeekLanded(true, 88000);             // mp4 keyframe
    CHECK(p.positionMs() == 88000);
    p.onPositionQuery(true, 88250);
    CHECK(p.positionMs() == 88250);
    p.onSeekRequested(120000);
    p.onSeekRefused();
    p.onPositionQuery(true, 88500);
    CHECK(p.positionMs() == 88500);
}

static void ply24_eosRecordsDuration() {
    PositionPolicy p;
    p.onDurationQuery(true, 213000);
    p.onPositionQuery(true, 212400);
    p.onEos();
    CHECK(p.ended());
    CHECK(p.positionMs() == 213000);
    p.onPositionQuery(true, 0);              // unreliable query at EOS: ignored
    CHECK(p.positionMs() == 213000);
    p.onSeekRequested(0);                    // replay from 0
    CHECK(!p.ended() && p.positionMs() == 0);

    PositionPolicy m;                         // pipeline never gave a duration
    m.setMetadataDurationMs(180000);
    m.onPositionQuery(true, 179000);
    m.onEos();
    CHECK(m.positionMs() == 180000);
}

static void ply24_durationRepolledUntilKnown() {
    PositionPolicy p;
    CHECK(p.needsDurationPoll());
    CHECK(p.pollIntervalMs(true, true, false) > 0);   // paused but duration unknown
    p.onDurationQuery(false, 0);
    CHECK(p.needsDurationPoll());
    p.onDurationQuery(true, -1);
    CHECK(p.needsDurationPoll());
    p.onDurationQuery(true, 301000);
    CHECK(!p.needsDurationPoll() && p.durationMs() == 301000);
    CHECK(p.pollIntervalMs(true, true, false) == 0);
}

// ---------------------------------------------------------------- SEL-07

static void sel07_jobsKeptAndReleased() {
    SwitchRequest q;   // quality switch: audio 251 kept
    q.mode = PlayMode::Dual;
    q.oldJobFormatIds.push_back("248");
    q.oldJobFormatIds.push_back("251");
    q.oldJobFormatIds.push_back("251");
    q.newVideoFormatId = "247";
    q.newAudioFormatId = "251";
    SwitchPlan p = planSwitch(q);
    CHECK(p.keepJobs.size() == 1 && p.keepJobs[0] == "251");
    CHECK(p.releaseJobs.size() == 1 && p.releaseJobs[0] == "248");

    SwitchRequest d;   // dub switch: video kept, old audio released
    d.mode = PlayMode::Dual;
    d.oldJobFormatIds.push_back("248");
    d.oldJobFormatIds.push_back("251-orig");
    d.newVideoFormatId = "248";
    d.newAudioFormatId = "251-de";
    p = planSwitch(d);
    CHECK(p.keepJobs.size() == 1 && p.keepJobs[0] == "248");
    CHECK(p.releaseJobs.size() == 1 && p.releaseJobs[0] == "251-orig");
}

static void sel07_stepOrder() {
    SwitchRequest r;
    r.capturedPositionMs = 33000;
    r.rate = 1.75;
    SwitchPlan p = planSwitch(r);
    const SwitchStep expected[] = {SwitchStep::CapturePosition, SwitchStep::Stop, SwitchStep::SetContainerHints,
                                   SwitchStep::SetUrls, SwitchStep::ArmPrerollBarrier, SwitchStep::Play,
                                   SwitchStep::ReleaseOldJobs};
    CHECK(p.steps.size() == 7);
    for (size_t i = 0; i < p.steps.size() && i < 7; ++i) CHECK(p.steps[i] == expected[i]);
    CHECK(p.resumeMs == 33000 && near(p.rate, 1.75));
    CHECK(captureSwitchPosition(true, 5000, 42000) == 42000);
    CHECK(captureSwitchPosition(false, 5000, 0) == 5000);    // mid-preroll: the query says 0
    CHECK(captureSwitchPosition(true, 0, -7) == 0);
}

// Accept: 20 switches each land within 1 s of the prior position. The
// engine is simulated: playback advances, stop resets the position to 0,
// the barrier releases the seek at async-done, mp4 lands on a keyframe
// (500 ms GOP here). Every third switch happens before the previous one
// prerolled, the case that loses the position without the barrier.
static void sel07_twentySwitchesKeepPosition() {
    PrerollBarrier barrier;
    PositionPolicy pos;
    pos.onDurationQuery(true, 3600000);
    const int64_t gop = 500;
    int64_t pipelinePos = 0;
    double rate = 1.25;
    barrier.arm(0, rate);
    DeferredSeek d;
    if (barrier.onAsyncDone(&d)) { pos.onSeekRequested(d.positionMs); pos.onSeekLanded(true, d.positionMs); }
    pipelinePos = 60000;
    pos.onPositionQuery(true, pipelinePos);

    bool allWithin = true;
    int64_t truth = pipelinePos;   // what the user was watching before each switch
    std::vector<std::string> jobs;
    jobs.push_back("137"); jobs.push_back("140");
    for (int i = 0; i < 20; ++i) {
        const bool rushed = (i % 3) == 2;   // switch again before preroll finished
        const int64_t prior = truth;
        const int64_t captured = captureSwitchPosition(barrier.prerolled(), barrier.pendingPositionMs(), pos.positionMs());

        SwitchRequest r;
        const bool mp4 = (i % 2) == 0;
        r.mode = PlayMode::Dual;
        r.videoContainer = mp4 ? Container::IsoBmff : Container::Matroska;
        r.audioContainer = Container::IsoBmff;
        r.capturedPositionMs = captured;
        r.rate = rate;
        r.oldJobFormatIds = jobs;
        r.newVideoFormatId = mp4 ? "137" : "248";
        r.newAudioFormatId = "140";
        SwitchPlan plan = planSwitch(r);

        pipelinePos = 0;                       // Stop resets the position
        pos.reset(true);
        pos.onDurationQuery(true, 3600000);
        barrier.arm(plan.resumeMs, plan.rate);
        jobs = plan.keepJobs;
        if (std::find(jobs.begin(), jobs.end(), r.newVideoFormatId) == jobs.end()) jobs.push_back(r.newVideoFormatId);
        bool audioKept = false, oldVideoReleased = true;
        for (size_t k = 0; k < plan.keepJobs.size(); ++k) {
            if (plan.keepJobs[k] == "140") audioKept = true;
            if (plan.keepJobs[k] != "140" && plan.keepJobs[k] != r.newVideoFormatId) oldVideoReleased = false;
        }
        CHECK(audioKept && oldVideoReleased);
        CHECK(plan.keepJobs.size() + plan.releaseJobs.size() == r.oldJobFormatIds.size());

        if (rushed) {
            // No async-done yet; the next loop iteration switches again.
            allWithin = allWithin && (barrier.pendingPositionMs() - prior <= 1000 && prior - barrier.pendingPositionMs() <= 1000);
            continue;
        }
        CHECK(barrier.onAsyncDone(&d));
        CHECK(near(d.rate, rate));
        SeekPlan sp = planSeek(req(r.mode, r.videoContainer, r.audioContainer, d.positionMs, d.rate));
        pos.onSeekRequested(sp.targetMs);
        const int64_t landed = sp.alignAudioAfterAsyncDone ? (sp.targetMs / gop) * gop : sp.targetMs;
        pipelinePos = landed;
        pos.onSeekLanded(true, landed);
        const int64_t delta = pos.positionMs() - prior;
        allWithin = allWithin && delta <= 1000 && delta >= -1000;
        CHECK(delta <= 1000 && delta >= -1000);
        // Play on for a while before the next switch.
        pipelinePos += 7300;
        pos.onPositionQuery(true, pipelinePos);
        truth = pipelinePos;
    }
    CHECK(allWithin);
}

int main() {
    types_containerFromExtension();
    ply02_syncModel();
    ply03_webmDualSeeksEachSinkAccurate();
    ply03_mp4VideoKeyUnitThenAlignAudio();
    ply03_singleSourceModesOneKeyUnitSeek();
    ply03_everySeekCarriesSaneRateAndClampedTarget();
    ply03_twentyRandomSeeksLandWithinBudget();
    nfrPerf03_oneSeekIsOneFlushPerSink();
    ply04_diskBufferForMp4Branches();
    ply04_hintsClearedInMuxedMode();
    ply04_neverAccurateOnMp4Branch();
    ply04_hintsSetBeforeUrls();
    ply05_resendThenRebuildThenGiveUp();
    ply05_rebuildsAtMostOnce();
    ply05_allowanceResets();
    ply05_bothRefusingIsNotADesync();
    ply05_refusedAudioAlignCountsAsSplit();
    ply06_resumeAndSpeedOneFlushSeekAfterFirstAsyncDone();
    ply06_nothingToApplyAtStartAndNormalSpeed();
    ply06_latestRequestWinsAndRearm();
    ply06_startSeekBeforePrerollReleasesNothing();
    ply06_startSeekRefusedFallsBackToBarrier();
    ply06_userChangeAfterStartSeekIsReleased();
    ply07_eightSteps();
    ply07_speedCarriesOverToNextVideo();
    ply07_notChangeableAtLiveEdge();
    ply08_underrunPausesAndResumesAtFull();
    ply08_everyNetworkBranchMustRefill();
    ply08_userPauseDuringBufferingIsRespected();
    ply08_liveHlsLowLatencyPolicy();
    ply08_localFilesNeverBuffer();
    ply08_startPhaseDoesNotWaitForFullBuffer();
    ply08_startPhaseEndsWithReset();
    ply09_freezesEveryModeWithVideo();
    ply09_noFreezeWithoutVideo();
    ply09_activeBeforeGraceCancels();
    ply09_thawImmediatelyAndRepaintAfter250ms();
    ply09_teardownUnlocksFirst();
    nfrPerf06_backgroundBudget();
    ply24_updateRates();
    ply24_seekReportsTargetThenLanded();
    ply24_eosRecordsDuration();
    ply24_durationRepolledUntilKnown();
    sel07_jobsKeptAndReleased();
    sel07_stepOrder();
    sel07_twentySwitchesKeepPosition();
    std::printf("player_seek tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
