// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the playback-session decision policies: session
// ownership (PLY-10), recovery (PLY-18), tap gestures and controls (PLY-17),
// keep-on (PLY-12/PLT-11), audio resource (PLY-23/PLT-12), autoplay (PLY-25),
// resume (PLY-26/PLY-24/LIB-05), MPRIS and cover (MPR-01/MPR-02),
// SponsorBlock skips (SB-02, NFR-TST-03), the temp-dir rule (PLT-09) and the
// cover-video input of the background freeze (PLY-09 lifted while the cover shows the video).
// Build: tests/player_session/run.sh
#include "Core/Player/Domain/AudioResourcePolicy.h"
#include "Core/Player/Domain/AutoplayPolicy.h"
#include "Core/Player/Domain/FreezePolicy.h"
#include "Core/Player/Domain/KeepOnPolicy.h"
#include "Core/Player/Domain/MediaControlsPolicy.h"
#include "Core/Player/Domain/RecoveryPolicy.h"
#include "Core/Player/Domain/ResumePolicy.h"
#include "Core/Player/Domain/SessionArbiter.h"
#include "Core/Player/Domain/SponsorSkip.h"
#include "Core/Player/Domain/TapGestures.h"
#include "Core/Player/Domain/TempDirPolicy.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---------------------------------------------------------------- PLY-10

static void ply10_defaultKeepsSessionOnLeave() {
    SessionArbiter a;
    CHECK(a.keepSessionOnLeave());
    CHECK(!a.mayAct(kNoContext));
    CHECK(!a.hasSession());
}

static void ply10_leavePlayingOrPausedAboveZeroParks() {
    SessionArbiter a;
    SessionDecision d = a.open(1, "A");
    CHECK(d.resolve && d.videoId == "A" && d.generation == 1 && !d.stop);
    CHECK(a.mayAct(1));
    d = a.close(1, PlaybackSnapshot(true, 30000));
    CHECK(d.park && !d.stop && !d.savePosition);
    CHECK(a.parked() && a.owner() == kAppContext && a.mayAct(kAppContext) && !a.mayAct(1));

    SessionArbiter b;
    b.open(1, "A");
    d = b.close(1, PlaybackSnapshot(false, 1));
    CHECK(d.park); // paused above 0 is kept too

    SessionArbiter c;
    c.open(1, "A");
    d = c.close(1, PlaybackSnapshot(false, 0));
    CHECK(!d.park && d.savePosition && d.stop && d.stoppedVideoId == "A");
    CHECK(!c.hasSession() && c.owner() == kNoContext);

    SessionArbiter off;
    off.setKeepSessionOnLeave(false);
    off.open(1, "A");
    d = off.close(1, PlaybackSnapshot(true, 30000));
    CHECK(!d.park && d.savePosition && d.stop);
}

static void ply10_reopenSameVideoReattaches() {
    SessionArbiter a;
    a.open(1, "A");
    a.close(1, PlaybackSnapshot(true, 30000));
    SessionDecision d = a.open(2, "A"); // mini-player expands into a new page
    CHECK(d.reattach && !d.resolve && !d.stop && !d.savePosition);
    CHECK(d.generation == 1 && a.generation() == 1);
    CHECK(a.mayAct(2) && !a.parked());
}

static void ply10_differentVideoSavesThenStopsOld() {
    SessionArbiter a;
    a.open(1, "A");
    a.close(1, PlaybackSnapshot(true, 30000));
    SessionDecision d = a.open(2, "B");
    CHECK(d.savePosition && d.stop && d.stoppedVideoId == "A");
    CHECK(d.resolve && d.videoId == "B" && d.generation == 2);
    CHECK(a.sessionVideoId() == "B");
}

static void ply10_onlyOwnerActs() {
    SessionArbiter a;
    a.open(1, "A");
    a.deactivate(1, PlaybackSnapshot(true, 5000)); // B pushed over A
    a.open(2, "B");
    CHECK(a.mayAct(2));
    CHECK(!a.mayAct(1));         // the hidden page never skips, recovers, saves...
    CHECK(!a.mayAct(kAppContext));
    SessionDecision d = a.deactivate(1, PlaybackSnapshot(true, 5000));
    CHECK(!d.any());             // a non-owner leaving causes no side effect
    d = a.reresolve(1);
    CHECK(!d.resolve);           // a non-owner cannot re-resolve (late error on wrong page)
    d = a.reresolve(2);
    CHECK(d.resolve && d.videoId == "B");
}

static void ply10_lateResolveForOtherIdIgnored() {
    SessionArbiter a;
    SessionDecision da = a.open(1, "A");
    SessionDecision db = a.open(2, "B");
    CHECK(!a.acceptResolve("A", da.generation));
    CHECK(a.acceptResolve("B", db.generation));
    SessionDecision r = a.reresolve(2); // recovery re-resolve of B
    CHECK(!a.acceptResolve("B", db.generation)); // the failed attempt's late answer
    CHECK(a.acceptResolve("B", r.generation));
}

static void ply10_backToAReclaims() {
    // §6 P8: A -> B -> back to A; then reopen A from the mini-player.
    SessionArbiter a;
    a.open(1, "A");
    a.deactivate(1, PlaybackSnapshot(true, 40000));
    SessionDecision d = a.open(2, "B");
    CHECK(d.stop && d.stoppedVideoId == "A" && d.resolve && d.videoId == "B");
    unsigned genB = d.generation;
    d = a.close(2, PlaybackSnapshot(true, 9000)); // Back from B
    CHECK(d.park);
    d = a.activate(1);                             // A shown again: reclaim
    CHECK(d.savePosition && d.stop && d.stoppedVideoId == "B");
    CHECK(d.resolve && d.videoId == "A" && d.owner == 1);
    CHECK(a.mayAct(1) && !a.mayAct(2));
    CHECK(!a.acceptResolve("B", genB));            // no stray B answer reaches the player
    d = a.activate(1);                             // repeated activation: no-op
    CHECK(!d.any());
    d = a.close(1, PlaybackSnapshot(true, 41000));
    CHECK(d.park);
    d = a.open(3, "A");                            // mini-player tap
    CHECK(d.reattach && !d.resolve);
    CHECK(a.contextCount() == 1);
}

static void ply10_activateSameVideoReattaches() {
    SessionArbiter a;
    a.open(1, "A");
    a.deactivate(1, PlaybackSnapshot(true, 1000)); // channel page pushed over A
    CHECK(a.parked());
    SessionDecision d = a.activate(1);
    CHECK(d.reattach && !d.resolve && a.mayAct(1));
    CHECK(!a.activate(99).any());                  // unknown context
}

static void ply10_stopAndEndedEndTheSession() {
    SessionArbiter a;
    a.open(1, "A");
    a.close(1, PlaybackSnapshot(true, 1000));
    SessionDecision d = a.stopSession();
    CHECK(d.savePosition && d.stop && d.stoppedVideoId == "A");
    CHECK(!a.hasSession() && a.owner() == kNoContext);
    CHECK(!a.stopSession().any());
    a.open(2, "B");
    a.sessionEnded();
    CHECK(!a.hasSession() && a.mayAct(2)); // the page still shows the (ended) player
    d = a.open(2, "C");                    // replace page with the next item, same context
    CHECK(d.resolve && !d.stop && a.contextCount() == 1);
}

// ---------------------------------------------------------------- PLY-18

static void ply18_threeFailuresThenSuccess() {
    RecoveryPolicy r;
    std::int64_t t = 0;
    RecoveryAction a = r.onFault(t, 60000, false, "stall");
    CHECK(a.kind == RecoveryAction::Wait && a.deadlineMs == 1500);
    CHECK(r.nextDeadlineMs() == 1500);
    CHECK(r.onTimer(1499).kind == RecoveryAction::None);
    a = r.onTimer(1500);
    CHECK(a.kind == RecoveryAction::Recover && a.resumePositionMs == 60000 && !a.resumeAtLive);
    // Failure 2 while the recovery is still resolving: keeps the 60 s point.
    t = 3000;
    a = r.onFault(t, 0, false, "403");
    CHECK(a.kind == RecoveryAction::Wait && r.triesUsed() == 2);
    a = r.onTimer(t + 1500);
    CHECK(a.kind == RecoveryAction::Recover && a.resumePositionMs == 60000);
    // Failure 3.
    t = 6000;
    a = r.onFault(t, 0, false, "403");
    CHECK(a.kind == RecoveryAction::Wait && r.triesUsed() == 3 && r.triesLeft() == 0);
    a = r.onTimer(t + 1500);
    CHECK(a.kind == RecoveryAction::Recover);
    // Success: playback resumes and runs cleanly.
    r.onPlaybackProgress(60000);
    CHECK(r.state() == RecoveryState::Idle && !r.retryAvailable());
    r.onPlaybackProgress(64999);
    CHECK(r.triesUsed() == 3);
    r.onPlaybackProgress(65000); // 5 s past the recovery point
    CHECK(r.triesUsed() == 0);
}

static void ply18_exhaustedShowsReasonAndRetry() {
    RecoveryPolicy r;
    std::int64_t t = 0;
    for (int i = 0; i < 3; ++i) {
        CHECK(r.onFault(t, 1000, false, "decoder error").kind == RecoveryAction::Wait);
        CHECK(r.onTimer(t + 1500).kind == RecoveryAction::Recover);
        t += 2000;
    }
    RecoveryAction a = r.onFault(t, 0, false, "network down");
    CHECK(a.kind == RecoveryAction::Fail);
    CHECK(r.state() == RecoveryState::Failed && r.retryAvailable() && r.reason() == "network down");
    CHECK(r.onFault(t + 1, 0, false, "x").kind == RecoveryAction::None);
    a = r.retry(t + 5000);
    CHECK(a.kind == RecoveryAction::Recover && a.resumePositionMs == 1000);
    CHECK(r.triesUsed() == 0 && r.triesLeft() == 3);
    CHECK(r.onFault(t + 6000, 0, false, "again").kind == RecoveryAction::Wait); // fresh budget
}

static void ply18_noRestoreWithoutCleanPlayback() {
    RecoveryPolicy r;
    r.onFault(0, 10000, false, "stall");
    r.onTimer(1500);
    r.onPlaybackProgress(10000);
    r.onPlaybackProgress(12000);
    CHECK(r.onFault(5000, 12000, false, "stall").kind == RecoveryAction::Wait);
    CHECK(r.triesUsed() == 2); // 2 s of playback did not restore the budget
}

static void ply18_backgroundDefersWithoutSpendingBudget() {
    RecoveryPolicy r;
    r.setForeground(false, 0);
    RecoveryAction a = r.onFault(100, 30000, true, "stall");
    CHECK(a.kind == RecoveryAction::None && r.state() == RecoveryState::Deferred);
    CHECK(r.triesUsed() == 0 && r.nextDeadlineMs() == -1);
    r.onFault(200, 0, true, "stall"); // more background faults spend nothing
    CHECK(r.triesUsed() == 0);
    a = r.setForeground(true, 10000);
    CHECK(a.kind == RecoveryAction::Recover && a.resumeAtLive && a.resumePositionMs == 30000);
    CHECK(r.triesUsed() == 1 && r.triesLeft() == 2);
    // Going to the background during a backoff gives the try back.
    RecoveryPolicy w;
    w.onFault(0, 1000, false, "stall");
    CHECK(w.triesUsed() == 1);
    w.setForeground(false, 500);
    CHECK(w.state() == RecoveryState::Deferred && w.triesUsed() == 0);
    CHECK(w.onTimer(1500).kind == RecoveryAction::None);
}

// ---------------------------------------------------------------- PLY-17

static void ply17_singleTapTogglesAfterWindow() {
    TapGestures g;
    TapOutput o = g.tap(1000, TapSide::Left, 0, 100000);
    CHECK(!o.toggleControls && g.nextDeadlineMs() == 1250);
    CHECK(!g.advance(1249).toggleControls);
    o = g.advance(1250);
    CHECK(o.toggleControls && !o.seek);
    CHECK(g.nextDeadlineMs() == -1 && !g.gestureActive());
    CHECK(tapSideFor(10, 100) == TapSide::Left && tapSideFor(50, 100) == TapSide::Right);
}

static void ply17_doubleTapSeeksDefaultTen() {
    TapGestures g;
    CHECK(g.stepSeconds() == 10);
    g.tap(0, TapSide::Right, 60000, 600000);
    g.tap(200, TapSide::Right, 60000, 600000);
    CHECK(g.gestureActive() && g.displayTotalSeconds() == 10 && g.targetMs() == 70000);
    CHECK(g.nextDeadlineMs() == 450);
    TapOutput o = g.advance(450);
    CHECK(o.seek && o.seekTargetMs == 70000 && !o.toggleControls && !o.gestureEnded);
    CHECK(g.nextDeadlineMs() == 900);
    o = g.advance(900);
    CHECK(o.gestureEnded && !o.seek && !g.gestureActive());
}

static void ply17_tapsStackIntoOneFlush() {
    // NFR-PERF-03: a double-tap burst is one flush.
    TapGestures g;
    int flushes = 0;
    std::int64_t target = -1;
    std::int64_t t = 0;
    g.tap(t, TapSide::Left, 100000, 600000);
    for (int i = 1; i < 6; ++i) {
        t += 150;
        TapOutput o = g.tap(t, TapSide::Left, 100000, 600000);
        if (o.seek) { ++flushes; target = o.seekTargetMs; }
    }
    CHECK(g.displayTotalSeconds() == -50 && g.targetMs() == 50000);
    for (std::int64_t now = t; now <= t + 1000; now += 10) {
        TapOutput o = g.advance(now);
        if (o.seek) { ++flushes; target = o.seekTargetMs; }
    }
    CHECK(flushes == 1 && target == 50000);
}

static void ply17_switchingSidesContinuesFromTarget() {
    TapGestures g;
    g.tap(0, TapSide::Right, 60000, 600000);
    g.tap(100, TapSide::Right, 60000, 600000);  // +10 -> 70
    g.tap(200, TapSide::Right, 60000, 600000);  // +20 -> 80
    CHECK(g.displayTotalSeconds() == 20);
    g.tap(300, TapSide::Left, 60000, 600000);   // switch: counter restarts, target continues
    CHECK(g.displaySide() == TapSide::Left && g.displayTotalSeconds() == -10 && g.targetMs() == 70000);
    TapOutput o = g.advance(550);
    CHECK(o.seek && o.seekTargetMs == 70000);
}

static void ply17_targetClampedAndStepConfigurable() {
    TapGestures g;
    g.setStepSeconds(15);
    CHECK(g.stepSeconds() == 15);
    g.tap(0, TapSide::Left, 4000, 600000);
    g.tap(100, TapSide::Left, 4000, 600000);
    CHECK(g.targetMs() == 0);
    g.cancel();
    g.setStepSeconds(5);
    g.tap(1000, TapSide::Right, 598000, 600000);
    g.tap(1100, TapSide::Right, 598000, 600000);
    CHECK(g.targetMs() == 600000);
    g.setStepSeconds(7);
    CHECK(g.stepSeconds() == 10);
}

static void ply17_tapsAfterFlushStackOnFlushedTarget() {
    TapGestures g;
    g.tap(0, TapSide::Right, 60000, 600000);
    g.tap(100, TapSide::Right, 60000, 600000);
    TapOutput o = g.advance(350);
    CHECK(o.seek && o.seekTargetMs == 70000);
    o = g.tap(600, TapSide::Right, 70500, 600000); // still inside the 700 ms gesture
    CHECK(g.gestureActive() && g.targetMs() == 80000 && g.displayTotalSeconds() == 20);
    o = g.advance(850);
    CHECK(o.seek && o.seekTargetMs == 80000);
    CHECK(g.advance(1300).gestureEnded);
}

static void ply17_otherHalfInsideWindowRestartsIt() {
    TapGestures g;
    g.tap(0, TapSide::Left, 0, 0);
    g.tap(100, TapSide::Right, 0, 0);
    CHECK(!g.gestureActive() && g.nextDeadlineMs() == 350);
    TapOutput o = g.advance(350);
    CHECK(o.toggleControls); // one toggle, not two
}

static void ply17_controlsAutoHide() {
    ControlsVisibility c;
    CHECK(c.visible() && c.nextDeadlineMs() == -1);
    c.setPlaying(0, true);
    CHECK(c.nextDeadlineMs() == 3500);
    CHECK(!c.advance(3499) && c.visible());
    c.interact(2000); // any interaction restarts the timer
    CHECK(c.nextDeadlineMs() == 5500);
    CHECK(c.advance(5500) && !c.visible());
    c.setPlaying(6000, false); // pause: show and stay
    CHECK(c.visible() && c.nextDeadlineMs() == -1);
    c.setPlaying(7000, true);
    c.setMenuOpen(7100, true); // gear open holds the controls
    CHECK(c.nextDeadlineMs() == -1);
    c.setMenuOpen(8000, false);
    CHECK(c.nextDeadlineMs() == 11500);
    c.toggle(9000);
    CHECK(!c.visible() && c.nextDeadlineMs() == -1);
    c.toggle(9100);
    CHECK(c.visible() && c.nextDeadlineMs() == 12600);
    c.setEnded(); // end: show and stay
    CHECK(c.visible() && c.nextDeadlineMs() == -1);
}

// ---------------------------------------------------------------- PLY-12 / PLT-11

static KeepOnInputs playingForeground() {
    KeepOnInputs in;
    in.playing = true; in.foreground = true; in.hasVideo = true; in.keepAliveAvailable = true;
    return in;
}

static void ply12_keepOnStates() {
    KeepOnInputs d;
    CHECK(d.settingEnabled && kKeepDisplayOnDefault); // defaults to on
    KeepOnInputs in = playingForeground();
    CHECK(preventBlanking(in));
    in = playingForeground(); in.playing = false;
    CHECK(!preventBlanking(in));                      // paused
    in = playingForeground(); in.listenOnly = true;
    CHECK(!preventBlanking(in));                      // listen-only
    in = playingForeground(); in.foreground = false;
    CHECK(!preventBlanking(in));                      // background
    in = playingForeground(); in.hasVideo = false;
    CHECK(!preventBlanking(in));                      // audio file
    in = playingForeground(); in.settingEnabled = false;
    CHECK(!preventBlanking(in));
}

static void plt11_storeWithoutKeepAlive() {
    KeepOnInputs in = playingForeground();
    in.keepAliveAvailable = false;
    CHECK(!preventBlanking(in));
    CHECK(keepOnLimitationNote(false) && !keepOnLimitationNote(true));
    CHECK(!holdKeepAliveBackgroundJob(false, true)); // audio-only playback holds no job
    CHECK(holdKeepAliveBackgroundJob(true, true));
    CHECK(!holdKeepAliveBackgroundJob(true, false));
}

// ---------------------------------------------------------------- PLY-23 / PLT-12

static void plt12_mediaRoleConstant() {
    CHECK(std::strcmp(kMediaRole, "x-maemo") == 0);
}

static void ply23_acquireBeforePlaying() {
    AudioResourcePolicy p;
    AudioResourceAction a = p.requestPlay();
    CHECK(a.acquire && !a.play); // never PLAYING before the grant
    CHECK(p.state() == AudioResourcePolicy::State::Acquiring);
    CHECK(!p.requestPlay().any()); // no second request while acquiring
    a = p.onGranted();
    CHECK(a.play && !a.acquire);
    a = p.requestPause();
    CHECK(a.pause && !a.release);
    a = p.requestPlay();
    CHECK(a.play && !a.acquire); // still granted
    a = p.stop();
    CHECK(a.pause && a.release && p.state() == AudioResourcePolicy::State::Released);
    CHECK(!p.onGranted().any()); // late grant after stop
}

static void ply23_lossPausesAndResumesPerSetting() {
    AudioResourcePolicy p;
    CHECK(p.resumeAfterInterruption());
    p.requestPlay(); p.onGranted();
    AudioResourceAction a = p.onLost(true); // incoming call
    CHECK(a.pause && !a.play);              // pause at once (well within 1 s)
    CHECK(p.autoResumePending());
    a = p.onGranted();                      // call ended
    CHECK(a.play);

    AudioResourcePolicy off;
    off.setResumeAfterInterruption(false);
    off.requestPlay(); off.onGranted();
    CHECK(off.onLost(true).pause);
    CHECK(!off.onGranted().play);           // setting forbids resume

    AudioResourcePolicy paused;
    paused.requestPlay(); paused.onGranted(); paused.requestPause();
    paused.onLost(false);
    CHECK(!paused.onGranted().play);        // was not playing when lost

    AudioResourcePolicy userPause;
    userPause.requestPlay(); userPause.onGranted();
    userPause.onLost(true);
    userPause.requestPause();               // user pause during the loss
    CHECK(!userPause.autoResumePending());
    CHECK(!userPause.onGranted().play);
}

// ---------------------------------------------------------------- PLY-25

static void ply25_playlistAdvancesAndStopsAtEnd() {
    AutoplayPolicy p;
    std::vector<std::string> ids;
    ids.push_back("a"); ids.push_back("b"); ids.push_back("c");
    p.setQueue(ids, 1);
    p.setRelatedCandidate("r");
    AutoplayDecision d = p.onEnded(0, true);
    CHECK(d.kind == AutoplayDecision::PlayItem && d.videoId == "c" && d.queueIndex == 2 && d.replacePage);
    d = p.onEnded(1000, true);
    CHECK(d.kind == AutoplayDecision::StopAtEnd); // no related after the list
    CHECK(!p.countdownActive());
}

static void ply25_onlyOwnerAutoplays() {
    AutoplayPolicy p;
    std::vector<std::string> ids;
    ids.push_back("a"); ids.push_back("b");
    p.setQueue(ids, 0);
    CHECK(p.onEnded(0, false).kind == AutoplayDecision::None);
    CHECK(p.currentIndex() == 0);
}

static void ply25_relatedCountdownWithSetting() {
    AutoplayPolicy p;
    CHECK(p.relatedAutoplay()); // SET-01: on, with a countdown
    p.setRelatedCandidate("r");
    AutoplayDecision d = p.onEnded(10000, true);
    CHECK(d.kind == AutoplayDecision::StartCountdown && d.videoId == "r" && d.countdownDeadlineMs == 15000);
    CHECK(p.countdownRemainingMs(12000) == 3000 && p.nextDeadlineMs() == 15000);
    CHECK(p.advance(14999).kind == AutoplayDecision::None);
    d = p.advance(15000);
    CHECK(d.kind == AutoplayDecision::PlayItem && d.videoId == "r" && d.fromRelated && d.replacePage);
    CHECK(!p.countdownActive());

    p.onEnded(20000, true);
    p.cancelCountdown(); // cancel stops it
    CHECK(p.advance(30000).kind == AutoplayDecision::None);

    AutoplayPolicy off;
    off.setRelatedAutoplay(false);
    off.setRelatedCandidate("r");
    CHECK(off.onEnded(0, true).kind == AutoplayDecision::StopAtEnd);
    CHECK(off.advance(10000).kind == AutoplayDecision::None);
}

static void ply25_queueEditChangesOrder() {
    AutoplayPolicy p;
    std::vector<std::string> ids;
    ids.push_back("a"); ids.push_back("b"); ids.push_back("c"); ids.push_back("d");
    p.setQueue(ids, 0);
    CHECK(p.moveItem(3, 1)); // a d b c
    CHECK(p.next().videoId == "d");
    CHECK(p.removeItem(2));  // a d c
    CHECK(!p.removeItem(1)); // the playing item stays
    CHECK(p.onEnded(0, true).videoId == "c");
    CHECK(p.queue().size() == 3);
    CHECK(p.moveItem(2, 0) && p.currentIndex() == 0); // the playing item moved with it
}

static void mpr01_queueNavigation() {
    AutoplayPolicy p;
    CHECK(!p.hasQueue() && !p.canGoNext() && !p.canGoPrevious());
    std::vector<std::string> ids;
    ids.push_back("a"); ids.push_back("b");
    p.setQueue(ids, 0);
    CHECK(p.canGoNext() && p.canGoPrevious());
    AutoplayDecision d = p.previous(1000);
    CHECK(d.kind == AutoplayDecision::RestartCurrent); // first item restarts
    d = p.next();
    CHECK(d.kind == AutoplayDecision::PlayItem && d.videoId == "b");
    CHECK(!p.canGoNext());
    d = p.previous(5000);
    CHECK(d.kind == AutoplayDecision::RestartCurrent && d.videoId == "b");
    d = p.previous(1000);
    CHECK(d.kind == AutoplayDecision::PlayItem && d.videoId == "a");
}

// ---------------------------------------------------------------- PLY-26 / PLY-24 / LIB-05

static void ply26_resumePointRules() {
    CHECK(resumePointToStore(9000, 600000) == kNoResume);           // 9 s -> none
    CHECK(resumePointToStore(10000, 600000) == 10000);
    CHECK(resumePointToStore(600000 - 14000, 600000) == kNoResume); // 14 s before the end
    CHECK(resumePointToStore(600000 - 16000, 600000) == 584000);
    CHECK(resumePointToStore(3600000, 0) == 3600000);               // unknown duration
    CHECK(resumeSaveDue(30000, 0) && !resumeSaveDue(29999, 0) && resumeSaveDue(0, -1));
}

static void ply26_resumeStartDecision() {
    ResumeStart r = decideResumeStart(120000, 600000, -1, kResumeBehaviourDefault);
    CHECK(kResumeBehaviourDefault == ResumeBehaviour::Always);
    CHECK(r.startMs == 120000 && !r.ask);
    r = decideResumeStart(120000, 600000, 90000, ResumeBehaviour::Always);
    CHECK(r.startMs == 90000);                                       // link t= wins
    r = decideResumeStart(120000, 600000, -1, ResumeBehaviour::Ask);
    CHECK(r.ask && r.offerMs == 120000 && r.startMs == 0 && kResumeAskAutoMs == 5000);
    r = decideResumeStart(120000, 600000, -1, ResumeBehaviour::Never);
    CHECK(r.startMs == 0 && !r.ask);
    r = decideResumeStart(595000, 600000, -1, ResumeBehaviour::Always);
    CHECK(r.startMs == 0);                                           // near the end: start over
    r = decideResumeStart(kNoResume, 600000, -1, ResumeBehaviour::Always);
    CHECK(r.startMs == 0);
}

static void ply26_watchedAndEndOfStream() {
    CHECK(isWatched(80000, 100000) && !isWatched(79000, 100000));
    CHECK(isWatched(600000 - 15000, 600000) && !isWatched(600000 - 16000, 600000 * 10));
    CHECK(watchedAfter(true, 1000, 600000));                         // sticky
    CHECK(positionToRecordAtEnd(600000, 0, 598000) == 600000);       // PLY-24
    CHECK(positionToRecordAtEnd(0, 599000, 598000) == 599000);
    CHECK(positionToRecordAtEnd(0, 0, 598000) == 598000);
}

// ---------------------------------------------------------------- MPR-01 / MPR-02

static void mpr01_capabilities() {
    MediaControlsState s;
    s.hasSession = true; s.playing = true; s.seekable = true;
    MprisCapabilities c = mprisCapabilities(s);
    CHECK(c.canControl && c.canPlay && c.canPause && c.canSeek);
    CHECK(!c.canGoNext && !c.canGoPrevious);  // no queue
    s.canGoNext = true; s.canGoPrevious = true;
    c = mprisCapabilities(s);
    CHECK(c.canGoNext && c.canGoPrevious);
    s.live = true;
    CHECK(!mprisCapabilities(s).canSeek);     // live: not seekable
    MediaControlsState idle;
    c = mprisCapabilities(idle);
    CHECK(c.canControl && !c.canPlay && !c.canPause && !c.canSeek);
}

static void mpr01_stopReallyStops() {
    StopActions a = mprisStopActions(true);
    CHECK(a.savePosition && a.endSession && a.releaseStreamJobs && a.closeMiniPlayer && a.releaseAudioResource);
    a = mprisStopActions(false);
    CHECK(!a.savePosition && !a.endSession);
}

static void mpr01_metadataUsesLocalArtOnly() {
    TrackInfo t;
    t.title = "Title"; t.channel = "Channel"; t.thumbnailPath = "/home/defaultuser/.cache/x/thumb 1.jpg";
    t.durationMs = 212000;
    MprisMetadata m = mprisMetadata(t);
    CHECK(m.title == "Title" && m.artist == "Channel");
    CHECK(m.artUrl == "file:///home/defaultuser/.cache/x/thumb%201.jpg");
    CHECK(m.hasLength && m.lengthUs == 212000000LL);
    CHECK(localArtUrl("https://i.ytimg.com/vi/x/hq.jpg").empty());   // never remote
    CHECK(localArtUrl("http://example/a.jpg").empty());
    CHECK(localArtUrl("relative/a.jpg").empty());
    CHECK(localArtUrl("file:///a/b.jpg") == "file:///a/b.jpg");
    t.live = true;
    CHECK(!mprisMetadata(t).hasLength);
    CHECK(std::strcmp(kMprisLengthKey, "mpris:length") == 0 && std::strcmp(kMprisArtUrlKey, "mpris:artUrl") == 0);
}

static void mpr02_coverActionStates() {
    MediaControlsState s;
    s.hasSession = true; s.canGoNext = true;
    CoverState c = coverState(s);
    CHECK(c.mode == CoverMode::Session && c.left == CoverAction::PlayPause && c.right == CoverAction::Next);
    s.canGoNext = false;
    c = coverState(s);
    CHECK(c.left == CoverAction::PlayPause && c.right == CoverAction::Stop);
    MediaControlsState idle;
    c = coverState(idle);
    CHECK(c.mode == CoverMode::Identity && c.left == CoverAction::None && c.right == CoverAction::None);
    idle.hasResumable = true;
    c = coverState(idle);
    CHECK(c.mode == CoverMode::ContinueWatching && c.left == CoverAction::ContinueWatching);
}

// ---------------------------------------------------------------- SB-02 / NFR-TST-03

static void nfrTst03_everyFetchedCategoryHasUiRow() {
    const std::vector<SponsorCategoryInfo> &rows = sponsorCategories();
    std::vector<std::string> fetched = sponsorFetchCategories();
    std::set<std::string> rowNames;
    std::set<int> enums;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        rowNames.insert(rows[i].apiName);
        enums.insert(static_cast<int>(rows[i].category));
    }
    CHECK(rows.size() == static_cast<std::size_t>(kSponsorCategoryCount));
    CHECK(rowNames.size() == rows.size() && enums.size() == rows.size()); // one row per category
    for (std::size_t i = 0; i < fetched.size(); ++i) CHECK(rowNames.count(fetched[i]) == 1);
    // The defaults table agrees with the settings defaults and with SB-02.
    SponsorSettings s;
    CHECK(s.enabled && kSponsorBlockEnabledDefault);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        CHECK(s.mode(rows[i].category) == rows[i].defaultMode);
        CHECK(defaultSkipMode(rows[i].category) == rows[i].defaultMode);
        SponsorCategory back = SponsorCategory::Sponsor;
        CHECK(sponsorCategoryFromApiName(rows[i].apiName, &back) && back == rows[i].category);
    }
    const char *autoNames[] = { "sponsor", "selfpromo", "interaction" };
    const char *offNames[] = { "intro", "outro", "preview", "filler", "music_offtopic", "poi_highlight" };
    for (int i = 0; i < 3; ++i) {
        SponsorCategory c = SponsorCategory::Sponsor;
        CHECK(sponsorCategoryFromApiName(autoNames[i], &c) && defaultSkipMode(c) == SkipMode::AutoSkip);
    }
    for (int i = 0; i < 6; ++i) {
        SponsorCategory c = SponsorCategory::Sponsor;
        CHECK(sponsorCategoryFromApiName(offNames[i], &c) && defaultSkipMode(c) == SkipMode::Off);
    }
    CHECK(sponsorCategoryInfo(SponsorCategory::PoiHighlight).highlight);
    CHECK(!sponsorCategoryFromApiName("chapter", 0));
}

static std::vector<SponsorSegment> oneSponsor() {
    std::vector<SponsorSegment> v;
    v.push_back(SponsorSegment(10000, 20000, SponsorCategory::Sponsor));
    return v;
}

static void sb02_autoSkipOnlyWhilePlaying() {
    SponsorSkipper k;
    k.setSegments(oneSponsor());
    SkipDecision d = k.evaluate(12000, false, true);
    CHECK(!d.seek && !d.showButton); // paused: no skip, auto beats the button
    d = k.evaluate(12000, true, true);
    CHECK(d.seek && d.seekToMs == 20000);
    CHECK(d.notice && d.noticeCategory == SponsorCategory::Sponsor && d.noticeMs == 1600);
}

static void sb02_onlyOwnerSkips() {
    SponsorSkipper k;
    k.setSegments(oneSponsor());
    CHECK(!k.evaluate(12000, true, false).seek);
    CHECK(k.manualSkip(12000, false) == -1);
    CHECK(k.evaluate(12000, true, true).seek); // still armed for the owner
}

static void sb02_endsHalfSecondEarly() {
    SponsorSkipper k;
    k.setSegments(oneSponsor());
    CHECK(!k.evaluate(19500, true, true).seek); // the last 0.5 s is outside
    CHECK(k.evaluate(19499, true, true).seek);
    CHECK(!k.evaluate(9999, true, true).seek);
}

static void sb02_oncePerPassReArmsAfterSeekBack() {
    SponsorSkipper k;
    k.setSegments(oneSponsor());
    CHECK(k.evaluate(10000, true, true).seek);
    CHECK(!k.evaluate(10250, true, true).seek); // the deferred seek has not landed yet
    CHECK(!k.evaluate(15000, true, true).seek); // user sought into the middle: no loop
    CHECK(!k.evaluate(5000, true, true).seek);  // sought back before it: re-armed
    CHECK(k.evaluate(10100, true, true).seek);
}

static void sb02_manualButtonAndAutoPriority() {
    SponsorSettings s;
    s.setMode(SponsorCategory::Intro, SkipMode::ShowButton);
    SponsorSkipper k;
    k.setSettings(s);
    std::vector<SponsorSegment> v;
    v.push_back(SponsorSegment(0, 8000, SponsorCategory::Intro));
    v.push_back(SponsorSegment(5000, 12000, SponsorCategory::Sponsor));
    k.setSegments(v);
    SkipDecision d = k.evaluate(2000, true, true);
    CHECK(d.showButton && d.buttonCategory == SponsorCategory::Intro && d.buttonTargetMs == 8000 && !d.seek);
    d = k.evaluate(2000, false, true);
    CHECK(d.showButton);                    // the button also shows while paused
    d = k.evaluate(6000, true, true);       // overlap: auto beats manual
    CHECK(d.seek && d.seekToMs == 12000 && !d.showButton);
    k.setSegments(v);
    CHECK(k.manualSkip(1000, true) == 8000);
    CHECK(k.manualSkip(9000, true) == -1);  // no button segment there
}

static void sb02_overlappingAutoSegmentsChainIntoOneSeek() {
    SponsorSkipper k;
    std::vector<SponsorSegment> v;
    v.push_back(SponsorSegment(10000, 20000, SponsorCategory::Sponsor));
    v.push_back(SponsorSegment(19800, 30000, SponsorCategory::SelfPromo));
    v.push_back(SponsorSegment(15000, 25000, SponsorCategory::Intro)); // off by default
    k.setSegments(v);
    SkipDecision d = k.evaluate(11000, true, true);
    CHECK(d.seek && d.seekToMs == 30000 && d.noticeCategory == SponsorCategory::Sponsor);
}

static void sb02_masterSwitchAndOffCategories() {
    SponsorSettings s;
    s.enabled = false;
    SponsorSkipper k;
    k.setSettings(s);
    k.setSegments(oneSponsor());
    CHECK(!k.evaluate(12000, true, true).seek);
    SponsorSkipper off;
    std::vector<SponsorSegment> v;
    v.push_back(SponsorSegment(10000, 20000, SponsorCategory::Outro));
    off.setSegments(v);
    SkipDecision d = off.evaluate(12000, true, true);
    CHECK(!d.seek && !d.showButton);
}

static void sb02_jumpToHighlight() {
    SponsorSkipper k;
    std::vector<SponsorSegment> v;
    v.push_back(SponsorSegment(90000, 90000, SponsorCategory::PoiHighlight));
    k.setSegments(v);
    CHECK(!k.evaluate(1000, true, true).showHighlight); // off by default
    SponsorSettings s;
    s.setMode(SponsorCategory::PoiHighlight, SkipMode::ShowButton);
    k.setSettings(s);
    SkipDecision d = k.evaluate(1000, true, true);
    CHECK(d.showHighlight && d.highlightMs == 90000 && !d.seek);
    s.setMode(SponsorCategory::PoiHighlight, SkipMode::AutoSkip);
    k.setSettings(s);
    d = k.evaluate(1000, true, true);
    CHECK(!d.seek && d.showHighlight);          // a highlight is never auto-jumped
    CHECK(!k.evaluate(95000, true, true).showHighlight);
}

// ---------------------------------------------------------------- PLT-09

static void plt09_flashTempDir() {
    CHECK(flashTempDir("/home/defaultuser/.cache/org/app") == "/home/defaultuser/.cache/org/app/tmp");
    CHECK(flashTempDir("/home/defaultuser/.cache/org/app/") == "/home/defaultuser/.cache/org/app/tmp");
    CHECK(flashTempDir("/tmp/cache").empty());
    CHECK(flashTempDir("relative").empty() && flashTempDir("").empty() && flashTempDir("/").empty());
    CHECK(kEmptyTempDirAtStartup);
}

static void plt09_rejectsRamBackedSpools() {
    CHECK(!isAllowedSpoolLocation("/tmp"));
    CHECK(!isAllowedSpoolLocation("/tmp/spool.webm"));
    CHECK(!isAllowedSpoolLocation("/run/user/100000/x"));
    CHECK(!isAllowedSpoolLocation("/dev/shm/x"));
    CHECK(!isAllowedSpoolLocation("/var/run/x"));
    CHECK(!isAllowedSpoolLocation("//tmp//x"));
    CHECK(!isAllowedSpoolLocation("/home/../tmp/x"));
    CHECK(!isAllowedSpoolLocation("relative/x") && !isAllowedSpoolLocation(""));
    CHECK(isAllowedSpoolLocation("/home/defaultuser/.cache/app/tmp/spool"));
    CHECK(isAllowedSpoolLocation("/tmpfoo/x"));
    CHECK(isAllowedSpoolLocation("/home/defaultuser/Downloads/a.mkv"));
}

// ---------------------------------------------------------------- cover video (PLY-09 lifted)

// Background + cover visible (+ setting on and a video plan, folded into `visible` by the
// controller) -> not frozen, in every mode with video; nothing wakes the app meanwhile.
static void ply09_coverVisibleInBackgroundNeverFreezes() {
    const PlayMode modes[] = {PlayMode::Dual, PlayMode::Muxed, PlayMode::Hls, PlayMode::Local};
    for (int i = 0; i < 4; ++i) {
        FreezePolicy f;
        f.configure(modes[i], true, 0);
        f.onAppActive(false, 1000);
        CHECK(f.nextDeadlineMs() == 1800);
        FreezeAction a = f.onCoverVisible(true, 1200);   // within the grace: cancelled
        CHECK(!a.freezeVideo && !a.thawVideo && !a.repaint);
        CHECK(f.nextDeadlineMs() == -1);
        CHECK(!f.onTimer(5000).freezeVideo && !f.frozen());
        CHECK(f.pictureWanted());
    }
    // Cover already visible when the app goes to the background: no grace starts at all.
    FreezePolicy g;
    g.configure(PlayMode::Dual, true, 0);
    g.onCoverVisible(true, 0);
    g.onAppActive(false, 100);
    CHECK(g.nextDeadlineMs() == -1 && !g.onTimer(10000).freezeVideo);
    // A pipeline built in the background while the cover shows it: no freeze either.
    FreezePolicy b;
    b.onAppActive(false, 0);
    b.onCoverVisible(true, 0);
    b.configure(PlayMode::Muxed, true, 50);
    CHECK(b.nextDeadlineMs() == -1 && !b.onTimer(5000).freezeVideo);
}

// Cover visible after the freeze happened: thaw at once (no flush), no repaint deadline (the
// cover item presents frames itself).
static void ply09_coverVisibleThawsAFrozenVideo() {
    FreezePolicy f;
    f.configure(PlayMode::Muxed, true, 0);
    f.onAppActive(false, 0);
    CHECK(f.onTimer(800).freezeVideo && f.frozen());
    FreezeAction a = f.onCoverVisible(true, 5000);
    CHECK(a.thawVideo && !a.freezeVideo && !a.repaint);
    CHECK(!f.frozen());
    CHECK(f.nextDeadlineMs() == -1);
}

// Any input false -> frozen per the existing rules: the cover hidden again starts the 800 ms grace.
static void ply09_coverHiddenFreezesAfterGrace() {
    FreezePolicy f;
    f.configure(PlayMode::Dual, true, 0);
    f.onAppActive(false, 0);
    f.onCoverVisible(true, 100);
    FreezeAction a = f.onCoverVisible(false, 10000);
    CHECK(!a.freezeVideo && !a.thawVideo);
    CHECK(f.nextDeadlineMs() == 10800);
    CHECK(!f.onTimer(10799).freezeVideo);
    a = f.onTimer(10800);
    CHECK(a.freezeVideo && f.frozen());
    CHECK(f.nextDeadlineMs() == -1);
    // Cover visible but no video branch (listen-only / audio-only): nothing to freeze or thaw.
    FreezePolicy n;
    n.configure(PlayMode::AudioOnly, false, 0);
    n.onAppActive(false, 0);
    CHECK(!n.onCoverVisible(true, 10).thawVideo);
    CHECK(!n.onCoverVisible(false, 20).freezeVideo && n.nextDeadlineMs() == -1);
    // Hidden before the grace ended while the app stays in the background: the grace restarts.
    FreezePolicy r;
    r.configure(PlayMode::Hls, true, 0);
    r.onCoverVisible(true, 0);
    r.onAppActive(false, 0);
    r.onCoverVisible(false, 300);
    CHECK(r.nextDeadlineMs() == 1100 && r.onTimer(1100).freezeVideo);
}

// Foreground: the cover input changes nothing (no freeze, no thaw, no deadline, repaint as before).
static void ply09_coverInputIgnoredInForeground() {
    FreezePolicy f;
    f.configure(PlayMode::Dual, true, 0);
    FreezeAction a = f.onCoverVisible(true, 10);
    CHECK(!a.freezeVideo && !a.thawVideo && !a.repaint && f.nextDeadlineMs() == -1);
    a = f.onCoverVisible(false, 20);
    CHECK(!a.freezeVideo && !a.thawVideo && !a.repaint && f.nextDeadlineMs() == -1);
    // Background -> foreground with the cover visible: the PLY-09 repaint still follows.
    f.onCoverVisible(true, 30);
    f.onAppActive(false, 40);
    a = f.onAppActive(true, 1000);
    CHECK(!a.thawVideo && f.nextDeadlineMs() == 1250 && f.onTimer(1250).repaint);
    // Teardown keeps the cover input (like app activity): a new build in the background with
    // the cover visible does not freeze.
    f.onAppActive(false, 2000);
    f.onTeardown();
    f.configure(PlayMode::Dual, true, 2100);
    CHECK(f.coverVisible() && f.nextDeadlineMs() == -1);
}

int main() {
    ply10_defaultKeepsSessionOnLeave();
    ply10_leavePlayingOrPausedAboveZeroParks();
    ply10_reopenSameVideoReattaches();
    ply10_differentVideoSavesThenStopsOld();
    ply10_onlyOwnerActs();
    ply10_lateResolveForOtherIdIgnored();
    ply10_backToAReclaims();
    ply10_activateSameVideoReattaches();
    ply10_stopAndEndedEndTheSession();
    ply18_threeFailuresThenSuccess();
    ply18_exhaustedShowsReasonAndRetry();
    ply18_noRestoreWithoutCleanPlayback();
    ply18_backgroundDefersWithoutSpendingBudget();
    ply17_singleTapTogglesAfterWindow();
    ply17_doubleTapSeeksDefaultTen();
    ply17_tapsStackIntoOneFlush();
    ply17_switchingSidesContinuesFromTarget();
    ply17_targetClampedAndStepConfigurable();
    ply17_tapsAfterFlushStackOnFlushedTarget();
    ply17_otherHalfInsideWindowRestartsIt();
    ply17_controlsAutoHide();
    ply12_keepOnStates();
    plt11_storeWithoutKeepAlive();
    plt12_mediaRoleConstant();
    ply23_acquireBeforePlaying();
    ply23_lossPausesAndResumesPerSetting();
    ply25_playlistAdvancesAndStopsAtEnd();
    ply25_onlyOwnerAutoplays();
    ply25_relatedCountdownWithSetting();
    ply25_queueEditChangesOrder();
    mpr01_queueNavigation();
    ply26_resumePointRules();
    ply26_resumeStartDecision();
    ply26_watchedAndEndOfStream();
    mpr01_capabilities();
    mpr01_stopReallyStops();
    mpr01_metadataUsesLocalArtOnly();
    mpr02_coverActionStates();
    ply09_coverVisibleInBackgroundNeverFreezes();
    ply09_coverVisibleThawsAFrozenVideo();
    ply09_coverHiddenFreezesAfterGrace();
    ply09_coverInputIgnoredInForeground();
    nfrTst03_everyFetchedCategoryHasUiRow();
    sb02_autoSkipOnlyWhilePlaying();
    sb02_onlyOwnerSkips();
    sb02_endsHalfSecondEarly();
    sb02_oncePerPassReArmsAfterSeekBack();
    sb02_manualButtonAndAutoPriority();
    sb02_overlappingAutoSegmentsChainIntoOneSeek();
    sb02_masterSwitchAndOffCategories();
    sb02_jumpToHighlight();
    plt09_flashTempDir();
    plt09_rejectsRamBackedSpools();
    std::printf("player session tests: %s (%d checks, %d failure%s)\n", failures ? "FAIL" : "PASS", checks,
                failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
