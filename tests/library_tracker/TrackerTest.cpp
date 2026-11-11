// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// LIB-05 cadence: save every 30 s while playing, on pause, leave, stop and end;
// nothing for a video that never started. Build: tests/library_tracker/run.sh
#include "Core/Library/Domain/ProgressTracker.h"

#include <cstdio>

using namespace nordtile::video::library;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static VideoEntry video(const char *id, long long seconds = 600) {
    VideoEntry v; v.videoId = id; v.title = "T"; v.durationSeconds = seconds; return v;
}

static void cadence() {
    ProgressTracker t;
    CHECK(t.open(video("aaaaaaaaaaa"), false, 0).empty());
    CHECK(t.playingChanged(true, 0).empty());
    CHECK(t.position(0, 600000, 500).empty());          // not started yet
    CHECK(t.position(1000, 600000, 1000).empty());      // starts the 30 s window
    CHECK(t.position(29000, 600000, 30999).empty());
    std::vector<ProgressReport> r = t.position(30000, 600000, 31000);
    CHECK(r.size() == 1);
    CHECK(r[0].newSession);
    CHECK(r[0].progress.positionMs == 30000);
    CHECK(r[0].progress.durationMs == 600000);
    CHECK(!r[0].progress.ended);
    CHECK(t.position(45000, 600000, 46000).empty());
    r = t.position(60000, 600000, 61000);
    CHECK(r.size() == 1 && !r[0].newSession && r[0].progress.positionMs == 60000);
    // pause saves at once, resume restarts the window
    t.position(62000, 600000, 63000);
    r = t.playingChanged(false, 63500);
    CHECK(r.size() == 1 && r[0].progress.positionMs == 62000);
    CHECK(t.playingChanged(false, 64000).empty());      // already paused
    CHECK(t.position(62000, 600000, 120000).empty());   // paused: no periodic save
    t.playingChanged(true, 120000);
    CHECK(t.position(63000, 600000, 149000).empty());
    CHECK(t.position(64000, 600000, 150000).size() == 1);
}

static void leaveStopEnd() {
    ProgressTracker t;
    t.open(video("aaaaaaaaaaa"), false, 0);
    t.playingChanged(true, 0);
    t.position(5000, 600000, 5000);
    // leave: opening another video reports the previous one
    std::vector<ProgressReport> r = t.open(video("bbbbbbbbbbb"), false, 6000);
    CHECK(r.size() == 1 && r[0].progress.video.videoId == "aaaaaaaaaaa" && r[0].progress.positionMs == 5000);
    CHECK(r[0].newSession);
    // the new one never started: stop reports nothing
    CHECK(t.stop(7000).empty());
    CHECK(!t.active());
    // end of stream reports the full duration as ended
    t.open(video("ccccccccccc", 100), false, 8000);
    t.playingChanged(true, 8000);
    t.position(90000, 100000, 98000);
    r = t.ended(99000);
    CHECK(r.size() == 1 && r[0].progress.ended && r[0].progress.positionMs == 100000);
    CHECK(t.stop(99500).empty());                        // nothing new after the end
    // stop after progress reports once
    t.open(video("ddddddddddd"), false, 100000);
    t.playingChanged(true, 100000);
    t.position(20000, 600000, 120000);
    r = t.stop(121000);
    CHECK(r.size() == 1 && r[0].progress.positionMs == 20000);
}

static void liveAndDetails() {
    ProgressTracker t;
    VideoEntry v = video("eeeeeeeeeee", -1);
    v.title.clear();
    t.open(v, false, 0);
    VideoEntry details = video("eeeeeeeeeee", 300);
    details.title = "Resolved title";
    t.updateVideo(details, true);
    t.playingChanged(true, 0);
    t.position(40000, 0, 40000);
    std::vector<ProgressReport> r = t.position(41000, 0, 40001 + 30000);
    CHECK(r.size() == 1);
    CHECK(r[0].progress.live && r[0].progress.durationMs == 0); // live: no resume point
    CHECK(r[0].progress.video.title == "Resolved title");
    // details for another id are ignored
    t.updateVideo(video("fffffffffff"), false);
    CHECK(t.video().videoId == "eeeeeeeeeee");
}

// Shorts loop item as LibraryPlaybackBridge drives it: the one end save is reported as ended
// (watched, no resume point); the bridge then stops feeding replays, so stop adds nothing.
static void loopItemEndsOnce() {
    ProgressTracker t;
    t.open(video("shortloop01"), false, 0);
    t.playingChanged(true, 0);
    CHECK(t.position(5000, 30000, 5000).empty());
    CHECK(t.position(29800, 30000, 29800).empty());     // under 30 s: no periodic save
    std::vector<ProgressReport> r = t.ended(30000);
    CHECK(r.size() == 1 && r[0].progress.ended && r[0].newSession);
    CHECK(r[0].progress.positionMs == 30000 && r[0].progress.durationMs == 30000);
    CHECK(t.stop(95000).empty());                        // replays were not fed: nothing overwrites it
    // The next Short opens as a new session.
    CHECK(t.open(video("shortloop02"), false, 96000).empty());
    t.playingChanged(true, 96000);
    t.position(1000, 20000, 97000);
    r = t.ended(116000);
    CHECK(r.size() == 1 && r[0].newSession && r[0].progress.video.videoId == "shortloop02");
}

int main() {
    cadence();
    leaveStopEnd();
    liveAndDetails();
    loopItemEndsOnce();
    std::printf("library tracker tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
