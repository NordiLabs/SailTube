// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the stall watchdog policy (StallPolicy, the half of PLY-18 that starts without
// an error): detection thresholds (8 s frozen, 4 s starved), every guard (paused, not started,
// seeking, ended, live edge, the PLY-09 background freeze, buffering with progress), a late check,
// the recovery steps in order and at most once per episode, skipped steps, a failed reload, what
// counts as recovered, the budget (3 episodes per 10 min; user seek and new video clear it), and
// the one moving deadline (NFR-PERF-06: no ticker).
// Build: tests/player_stall/run.sh
#include "Core/Player/Domain/StallPolicy.h"

#include <cstdio>
#include <cstring>

using namespace nordtile::player;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// A session that plays: wants to play, prerolled, at `positionMs`.
static StallFacts playing(std::int64_t positionMs) {
    StallFacts f;
    f.wantsPlay = true;
    f.started = true;
    f.positionMs = positionMs;
    return f;
}

static bool is(const char *a, const char *b) { return std::strcmp(a, b) == 0; }

// Runs a fresh policy into its first step (a Seek at t = 9000, position 5000).
static void stallAtNine(StallPolicy &p) {
    p.observe(1000, playing(5000));
    const StallDecision d = p.observe(9000, playing(5000));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek);
}

// ---- Detection --------------------------------------------------------------------------------

// The position stands still for 8 s while the player says it plays: a stall. One deadline that
// moves with every position report and never fires while playback advances (NFR-PERF-06).
static void ply18_frozenPositionEightSeconds() {
    StallPolicy p;
    CHECK(p.nextDeadlineMs() == -1 && !p.armed() && !p.recovering());
    CHECK(p.observe(1000, playing(5000)).kind == StallDecision::None);
    CHECK(p.armed() && p.nextDeadlineMs() == 9000);
    CHECK(p.onPosition(1250, 5250).kind == StallDecision::None);
    CHECK(p.nextDeadlineMs() == 9250);
    CHECK(p.onPosition(1250, 5250).kind == StallDecision::None);  // the same position again is not progress
    CHECK(p.nextDeadlineMs() == 9250);
    p.onPosition(3000, 7000);
    CHECK(p.nextDeadlineMs() == 11000);
    CHECK(p.observe(10999, playing(7000)).kind == StallDecision::None);
    CHECK(p.stalls() == 0 && p.nextDeadlineMs() == 11000);
    const StallDecision d = p.observe(11000, playing(7000));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek && d.detected);
    CHECK(is(d.reason, "frozen") && d.ms == 8000 && is(d.failure, ""));
    CHECK(p.stalls() == 1 && p.recoveries() == 0 && p.lastStep() == 1);
    CHECK(p.recovering() && p.step() == StallStep::Seek && !p.armed());
    CHECK(p.nextDeadlineMs() == 11000 + p.config().stepGraceMs);  // the net below the caller's next look
}

// Buffering with no progress (the level does not rise, no bytes arrive): 4 s.
static void ply18_starvedFourSeconds() {
    StallPolicy p;
    StallFacts f = playing(5000);
    f.bufferedMs = 8000;
    p.observe(1000, f);
    f.buffering = true;
    f.bufferingPercent = 0;
    CHECK(p.observe(2000, f).kind == StallDecision::None);
    CHECK(p.nextDeadlineMs() == 5000);  // 4 s from the last progress, not from the first buffering report
    CHECK(p.observe(4999, f).kind == StallDecision::None);
    const StallDecision d = p.observe(5000, f);
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek && d.detected && is(d.reason, "starved"));
    CHECK(d.ms == 4000);
}

// The thresholds come from the config (host tests shorten them).
static void ply18_thresholdsFromConfig() {
    StallPolicy p;
    StallConfig c;
    c.noProgressMs = 300;
    c.starvedMs = 100;
    c.stepGraceMs = 50;
    p.setConfig(c);
    CHECK(p.config().noProgressMs == 300 && p.config().starvedMs == 100 && p.config().maxEpisodes == 3);
    p.observe(1000, playing(5000));
    CHECK(p.nextDeadlineMs() == 1300);
    CHECK(p.observe(1299, playing(5000)).kind == StallDecision::None);
    CHECK(p.observe(1300, playing(5000)).kind == StallDecision::Recover);
    CHECK(p.nextDeadlineMs() == 1350);
    StallPolicy q;
    q.setConfig(c);
    StallFacts f = playing(5000);
    f.buffering = true;
    f.bufferingPercent = 0;
    q.observe(1000, f);
    CHECK(q.nextDeadlineMs() == 1100);
    CHECK(q.observe(1100, f).kind == StallDecision::Recover);
}

// ---- Guards -----------------------------------------------------------------------------------

// Paused (the user, or an audio loss): nothing is watched, no deadline; playing again starts a
// fresh window, never an instant stall.
static void ply18_pausedIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts paused = playing(5000);
    paused.wantsPlay = false;
    CHECK(p.observe(2000, paused).kind == StallDecision::None);
    CHECK(!p.armed() && p.nextDeadlineMs() == -1);
    CHECK(p.observe(60000, paused).kind == StallDecision::None);
    CHECK(p.observe(61000, playing(5000)).kind == StallDecision::None);
    CHECK(p.armed() && p.nextDeadlineMs() == 69000);
    CHECK(p.observe(68999, playing(5000)).kind == StallDecision::None);
    CHECK(p.observe(69000, playing(5000)).kind == StallDecision::Recover);
    // suspend(): the same without a time at hand (pause(), media released).
    StallPolicy q;
    q.observe(1000, playing(5000));
    q.suspend();
    CHECK(!q.armed() && q.nextDeadlineMs() == -1 && q.stalls() == 0);
    CHECK(q.onPosition(50000, 5000).kind == StallDecision::None && q.nextDeadlineMs() == -1);
}

// A seek in flight: the reported position is its target, so nothing is judged; the policy looks
// again later, and a seek that landed on a dead stream is still found. A seek that never lands
// (the player bounds a landing itself) stops counting as one after noProgressMs.
static void ply18_seekingIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts s = playing(90000);
    s.seeking = true;
    CHECK(p.observe(1500, s).kind == StallDecision::None);
    CHECK(!p.armed() && p.nextDeadlineMs() == 9500);
    CHECK(p.observe(9499, s).kind == StallDecision::None && !p.armed() && p.nextDeadlineMs() == 17499);
    CHECK(p.observe(10000, playing(90000)).kind == StallDecision::None);  // landed: a fresh window
    CHECK(p.armed() && p.nextDeadlineMs() == 18000 && p.stalls() == 0);
    CHECK(p.observe(18000, playing(90000)).kind == StallDecision::Recover);
    // Scrubbing: every new seek of the session or the user has its own wait.
    StallPolicy q;
    q.observe(1000, playing(5000));
    for (std::int64_t t = 2000; t < 60000; t += 5000) {
        if (t % 10000 == 2000) q.onUserSeek(); else q.onInternalSeek();
        CHECK(q.observe(t, s).kind == StallDecision::None && !q.armed());
    }
    CHECK(q.stalls() == 0);
    // Never landing: after 8 s it is judged like any standing position.
    StallPolicy stuck;
    stuck.observe(1000, playing(5000));
    CHECK(stuck.observe(1500, s).kind == StallDecision::None && !stuck.armed());
    CHECK(stuck.observe(9500, s).kind == StallDecision::None && stuck.armed());  // the wait is over: a window starts
    CHECK(stuck.nextDeadlineMs() == 17500);
    const StallDecision d = stuck.observe(17500, s);
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek && d.detected);
}

// Not started: the first start, a quality / dub / stream switch, a resolve. A reload flag
// outside an episode changes nothing.
static void ply18_switchingIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts sw = playing(5000);
    sw.started = false;
    CHECK(p.observe(2000, sw).kind == StallDecision::None && p.nextDeadlineMs() == -1 && !p.armed());
    sw.reloading = true;
    CHECK(p.observe(40000, sw).kind == StallDecision::None && p.nextDeadlineMs() == -1);
    CHECK(p.stalls() == 0);
}

static void ply18_endedIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(600000));
    StallFacts e = playing(600000);
    e.ended = true;
    CHECK(p.observe(9000, e).kind == StallDecision::None && p.nextDeadlineMs() == -1);
    CHECK(p.observe(90000, e).kind == StallDecision::None && p.stalls() == 0);
}

// Live: waiting at the edge for the next segment is not a stall.
static void ply18_liveEdgeWaitIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts live = playing(5000);
    live.buffering = true;
    live.bufferingPercent = 0;
    live.liveEdgeWaiting = true;
    for (std::int64_t t = 2000; t < 60000; t += 8000) {
        CHECK(p.observe(t, live).kind == StallDecision::None);
        CHECK(p.nextDeadlineMs() == t + 8000);
    }
    CHECK(p.stalls() == 0);
}

// PLY-09: the picture is frozen in the background while the audio position advances: not a
// stall. Once the audio was seen advancing since the freeze, its stop is one.
static void ply09_backgroundFreezeWithAudioIsNotAStall() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts frozen = playing(5200);
    frozen.videoFrozen = true;
    CHECK(p.observe(1800, frozen).kind == StallDecision::None);  // the freeze: a new baseline
    for (std::int64_t t = 2800; t <= 30800; t += 1000)
        CHECK(p.onPosition(t, 5200 + (t - 1800)).kind == StallDecision::None);
    CHECK(p.stalls() == 0 && p.nextDeadlineMs() == 38800);
    frozen.positionMs = 5200 + 29000;
    CHECK(p.observe(38799, frozen).kind == StallDecision::None);
    const StallDecision d = p.observe(38800, frozen);
    CHECK(d.kind == StallDecision::Recover && d.detected && is(d.reason, "frozen"));
}

// PLY-09: a frozen plan whose position never advanced since the freeze (no audio clock to judge
// by) is left alone, however long; thawed, it is judged as usual again.
static void ply09_backgroundFreezeWithoutAudioClockIsLeftAlone() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    StallFacts frozen = playing(5000);
    frozen.videoFrozen = true;
    CHECK(p.observe(1800, frozen).kind == StallDecision::None);
    for (std::int64_t t = 9800; t < 120000; t += 8000) {
        CHECK(p.observe(t, frozen).kind == StallDecision::None);
        CHECK(p.nextDeadlineMs() == t + 8000);  // one slow re-check, no ticker
    }
    CHECK(p.stalls() == 0);
    CHECK(p.observe(122000, playing(5000)).kind == StallDecision::None);  // thawed: a fresh window
    CHECK(p.nextDeadlineMs() == 130000);
    CHECK(p.observe(130000, playing(5000)).kind == StallDecision::Recover);
}

// Buffering that makes progress (the level rises, or bytes arrive at the same level) is a slow
// link, not a stall; the 4 s count from the last progress.
static void ply18_bufferingWithProgressIsNotAStall() {
    StallPolicy p;
    StallFacts f = playing(5000);
    f.bufferedMs = 8000;
    p.observe(1000, f);
    f.buffering = true;
    f.bufferingPercent = 0;
    CHECK(p.observe(2000, f).kind == StallDecision::None);
    f.bufferingPercent = 3;
    CHECK(p.observe(5000, f).kind == StallDecision::None && p.nextDeadlineMs() == 9000);
    f.bufferedMs = 8200;  // the same level, more bytes
    CHECK(p.observe(9000, f).kind == StallDecision::None && p.nextDeadlineMs() == 13000);
    f.bufferingPercent = 2;  // a falling level is not progress
    CHECK(p.observe(12999, f).kind == StallDecision::None);
    const StallDecision d = p.observe(13000, f);
    CHECK(d.kind == StallDecision::Recover && is(d.reason, "starved"));
    // Bytes that arrive while nothing is buffering do not hide a frozen position (a stuck sink).
    StallPolicy q;
    StallFacts g = playing(5000);
    q.observe(1000, g);
    g.bufferedMs = 9000;
    CHECK(q.observe(5000, g).kind == StallDecision::None && q.nextDeadlineMs() == 9000);
    CHECK(q.observe(9000, g).kind == StallDecision::Recover);
}

// A check that comes long after its deadline (the process was stopped, the device slept) counts
// again from there; one that is on time decides.
static void ply18_lateCheckCountsAgain() {
    StallPolicy p;
    p.observe(1000, playing(5000));
    CHECK(p.observe(11001, playing(5000)).kind == StallDecision::None);  // 2001 ms late
    CHECK(p.stalls() == 0 && p.nextDeadlineMs() == 19001);
    CHECK(p.observe(19001, playing(5000)).kind == StallDecision::Recover);
    StallPolicy q;
    q.observe(1000, playing(5000));
    CHECK(q.observe(11000, playing(5000)).kind == StallDecision::Recover);  // inside the slack
}

// ---- Recovery steps ---------------------------------------------------------------------------

// Seek, Reopen, Reresolve, each once, then Fail; the grace after a step and the reload's own
// limit; nothing is watched while the fresh resolve runs.
static void ply18_stepsInOrderEachOnceThenFail() {
    StallPolicy p;
    stallAtNine(p);
    StallFacts s = playing(5000);
    s.seeking = true;
    CHECK(p.observe(9001, s).kind == StallDecision::None);          // the step's seek is in flight
    CHECK(p.observe(10000, playing(5000)).kind == StallDecision::None);  // landed, playing again
    CHECK(p.armed() && p.nextDeadlineMs() == 15000);
    CHECK(p.observe(14999, playing(5000)).kind == StallDecision::None);
    StallDecision d = p.observe(15000, playing(5000));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Reopen && !d.detected);
    CHECK(p.step() == StallStep::Reopen && p.lastStep() == 2 && p.stalls() == 1);
    StallFacts r = playing(5000);  // the reload was issued and has not prerolled
    r.started = false;
    r.reloading = true;
    CHECK(p.observe(15001, r).kind == StallDecision::None && p.nextDeadlineMs() == 27001);
    CHECK(p.observe(27000, r).kind == StallDecision::None);
    d = p.observe(27001, r);
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Reresolve && !d.detected);
    StallFacts resolving = playing(5000);
    resolving.started = false;
    CHECK(p.observe(27002, resolving).kind == StallDecision::None);
    CHECK(p.nextDeadlineMs() == -1 && p.recovering());  // the resolver has its own limits
    CHECK(p.observe(30000, r).kind == StallDecision::None && p.nextDeadlineMs() == 42000);
    CHECK(p.observe(31000, playing(5000)).kind == StallDecision::None && p.nextDeadlineMs() == 36000);
    d = p.observe(36000, playing(5000));
    CHECK(d.kind == StallDecision::Fail && is(d.failure, "exhausted") && d.step == StallStep::Reresolve && !d.detected);
    CHECK(!p.recovering() && p.step() == StallStep::None && p.nextDeadlineMs() == -1);
    CHECK(p.stalls() == 1 && p.recoveries() == 0 && p.lastStep() == 3);
}

// Recovered: the position advanced half a second past where the step left it. Less than that
// only extends the wait; a landing behind the old position (a key-unit seek) is not progress.
static void ply18_recoveredNeedsRealAdvance() {
    StallPolicy p;
    stallAtNine(p);
    p.observe(9500, playing(5000));
    CHECK(p.nextDeadlineMs() == 14500);
    CHECK(p.onPosition(9750, 5250).kind == StallDecision::None);
    CHECK(p.recovering() && p.nextDeadlineMs() == 14750);
    StallDecision d = p.onPosition(10000, 5500);
    CHECK(d.kind == StallDecision::Recovered && d.step == StallStep::Seek && d.ms == 1000);
    CHECK(!p.recovering() && p.step() == StallStep::None && p.recoveries() == 1 && p.stalls() == 1);
    CHECK(p.armed() && p.nextDeadlineMs() == 18000 && p.lastStep() == 1);
    // Through observe(), after a landing two seconds back.
    StallPolicy q;
    stallAtNine(q);
    q.observe(9500, playing(5000));
    CHECK(q.observe(9600, playing(3000)).kind == StallDecision::None && q.recovering());
    CHECK(q.observe(9900, playing(3400)).kind == StallDecision::None && q.recovering());
    d = q.observe(10100, playing(3500));
    CHECK(d.kind == StallDecision::Recovered && d.ms == 1100 && q.recoveries() == 1);
    // Buffer progress after a step extends the wait (the reload fills its queues).
    StallPolicy b;
    stallAtNine(b);
    StallFacts f = playing(5000);
    f.buffering = true;
    f.bufferingPercent = 10;
    b.observe(9500, f);
    f.bufferingPercent = 60;
    CHECK(b.observe(14500, f).kind == StallDecision::None && b.nextDeadlineMs() == 19500);
    CHECK(b.observe(19500, f).kind == StallDecision::Recover && b.step() == StallStep::Reopen);
}

// A jump the session made itself (SponsorBlock skip, loop restart) is neither progress nor a
// recovery: the next look takes a new baseline.
static void ply18_internalSeekIsNotProgress() {
    StallPolicy p;
    stallAtNine(p);
    p.observe(9500, playing(5000));
    p.onInternalSeek();
    CHECK(!p.armed() && p.recovering());
    CHECK(p.onPosition(9600, 65000).kind == StallDecision::None);  // the skip's target
    CHECK(p.observe(9700, playing(65000)).kind == StallDecision::None && p.recovering());
    CHECK(p.observe(14700, playing(65000)).kind == StallDecision::Recover && p.step() == StallStep::Reopen);
}

// A step that cannot run is skipped: no seek on an unseekable (live) stream, no re-resolve for a
// local file.
static void ply18_stepsThatCannotRunAreSkipped() {
    StallPolicy live;
    StallFacts f = playing(5000);
    f.seekable = false;
    live.observe(1000, f);
    StallDecision d = live.observe(9000, f);
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Reopen && d.detected);
    StallPolicy local;
    StallFacts g = playing(5000);
    g.resolvable = false;
    local.observe(1000, g);
    CHECK(local.observe(9000, g).step == StallStep::Seek);
    local.observe(9100, g);
    CHECK(local.observe(14100, g).step == StallStep::Reopen);
    StallFacts r = g;
    r.started = false;
    r.reloading = true;
    local.observe(14200, r);
    d = local.observe(26200, r);
    CHECK(d.kind == StallDecision::Fail && is(d.failure, "exhausted") && d.step == StallStep::Reopen);
}

// The reopened candidate failed before its preroll: the fresh resolve is next, at once; a failed
// last step fails; without an episode nothing happens.
static void ply18_failedReloadGoesToTheNextStep() {
    StallPolicy none;
    CHECK(none.onStepFailed(1000).kind == StallDecision::None);
    StallPolicy p;
    stallAtNine(p);
    p.observe(9100, playing(5000));
    CHECK(p.observe(14100, playing(5000)).step == StallStep::Reopen);
    StallDecision d = p.onStepFailed(14300);
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Reresolve && !d.detected);
    d = p.onStepFailed(15000);
    CHECK(d.kind == StallDecision::Fail && is(d.failure, "exhausted") && !p.recovering());
}

// suspend() keeps an open episode: paused in the middle of it, the next step still follows.
static void ply18_episodeSurvivesAPause() {
    StallPolicy p;
    stallAtNine(p);
    p.suspend();
    CHECK(p.recovering() && p.step() == StallStep::Seek && p.nextDeadlineMs() == -1);
    CHECK(p.observe(100000, playing(5000)).kind == StallDecision::None && p.nextDeadlineMs() == 105000);
    CHECK(p.observe(105000, playing(5000)).step == StallStep::Reopen);
}

// ---- Budget -----------------------------------------------------------------------------------

// One stall recovered by the seek, starting from playback at `t`, position `pos`.
static void stallAndRecover(StallPolicy &p, std::int64_t t, std::int64_t pos) {
    p.onPosition(t, pos);
    const StallDecision d = p.observe(t + 8000, playing(pos));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek && d.detected);
    p.observe(t + 8100, playing(pos));
    CHECK(p.onPosition(t + 8700, pos + 600).kind == StallDecision::Recovered);
}

// Three episodes in ten minutes are recovered; the fourth fails at once. Ten minutes after the
// first the budget has room again.
static void ply18_budgetThreeEpisodesPerTenMinutes() {
    StallPolicy p;
    p.observe(1000, playing(1000));
    stallAndRecover(p, 2000, 2000);
    stallAndRecover(p, 20000, 20000);
    stallAndRecover(p, 40000, 40000);
    CHECK(p.episodesInBudget() == 3 && p.stalls() == 3 && p.recoveries() == 3);
    p.onPosition(60000, 60000);
    StallDecision d = p.observe(68000, playing(60000));
    CHECK(d.kind == StallDecision::Fail && is(d.failure, "budget") && d.detected && is(d.reason, "frozen"));
    CHECK(d.step == StallStep::None && !p.recovering() && p.nextDeadlineMs() == -1);
    CHECK(p.stalls() == 4 && p.recoveries() == 3);
    // 10 min after the first episode (detected at 10000).
    p.observe(602001, playing(60000));
    d = p.observe(610001, playing(60000));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek);
    CHECK(p.episodesInBudget() == 3);  // the first one left the window
}

// A user seek (and Retry) and a new video clear the budget and drop an open episode; the totals
// stay for the diagnostics.
static void ply18_userSeekAndNewVideoClearTheBudget() {
    StallPolicy p;
    p.observe(1000, playing(1000));
    stallAndRecover(p, 2000, 2000);
    stallAndRecover(p, 20000, 20000);
    stallAndRecover(p, 40000, 40000);
    p.onUserSeek();
    CHECK(p.episodesInBudget() == 0 && !p.armed() && p.nextDeadlineMs() == -1);
    p.observe(60000, playing(300000));
    StallDecision d = p.observe(68000, playing(300000));
    CHECK(d.kind == StallDecision::Recover && d.step == StallStep::Seek);
    p.onUserSeek();  // in the middle of the episode: dropped, not recovered
    CHECK(!p.recovering() && p.step() == StallStep::None && p.recoveries() == 3 && p.episodesInBudget() == 0);
    StallPolicy q;
    q.observe(1000, playing(1000));
    stallAndRecover(q, 2000, 2000);
    stallAndRecover(q, 20000, 20000);
    q.onPosition(40000, 40000);
    CHECK(q.observe(48000, playing(40000)).kind == StallDecision::Recover);
    q.reset();
    CHECK(q.episodesInBudget() == 0 && !q.recovering() && !q.armed() && q.nextDeadlineMs() == -1);
    CHECK(q.stalls() == 3 && q.recoveries() == 2 && q.lastStep() == 1);
    q.observe(50000, playing(0));
    CHECK(q.observe(58000, playing(0)).kind == StallDecision::Recover);
    CHECK(q.episodesInBudget() == 1);
}

int main() {
    ply18_frozenPositionEightSeconds();
    ply18_starvedFourSeconds();
    ply18_thresholdsFromConfig();
    ply18_pausedIsNotAStall();
    ply18_seekingIsNotAStall();
    ply18_switchingIsNotAStall();
    ply18_endedIsNotAStall();
    ply18_liveEdgeWaitIsNotAStall();
    ply09_backgroundFreezeWithAudioIsNotAStall();
    ply09_backgroundFreezeWithoutAudioClockIsLeftAlone();
    ply18_bufferingWithProgressIsNotAStall();
    ply18_lateCheckCountsAgain();
    ply18_stepsInOrderEachOnceThenFail();
    ply18_recoveredNeedsRealAdvance();
    ply18_internalSeekIsNotProgress();
    ply18_stepsThatCannotRunAreSkipped();
    ply18_failedReloadGoesToTheNextStep();
    ply18_episodeSurvivesAPause();
    ply18_budgetThreeEpisodesPerTenMinutes();
    ply18_userSeekAndNewVideoClearTheBudget();
    std::printf("player stall tests: %s (%d checks, %d failure%s)\n", failures ? "FAIL" : "PASS", checks, failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
