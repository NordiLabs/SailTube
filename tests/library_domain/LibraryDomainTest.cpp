// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the library policy (LIB-01..07, IMP-01 merge inputs).
// Build: tests/library_domain/run.sh
#include "Core/Library/Domain/LibraryPolicy.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace nordtile::video::library;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const std::int64_t S = 1000;
static const std::int64_t Day = 24LL * 3600 * 1000;

static PlaybackProgress progress(std::int64_t pos, std::int64_t dur, bool ended = false, bool live = false) {
    PlaybackProgress p;
    p.video.videoId = "dQw4w9WgXcQ";
    p.video.title = "Title";
    p.video.channel = "Channel";
    p.positionMs = pos;
    p.durationMs = dur;
    p.ended = ended;
    p.live = live;
    return p;
}

// LIB-04: watched at 80 % (exactly, in integers), not at 79 %.
static void watchedThreshold() {
    const std::int64_t d = 1000 * S;
    CHECK(reachesWatched(800 * S, d));
    CHECK(!reachesWatched(800 * S - 1, d));
    CHECK(!reachesWatched(790 * S, d));          // 79 %
    const std::int64_t l = 100 * S;              // 80 s = 80 %, 20 s from the end
    CHECK(reachesWatched(80 * S, l));
    CHECK(!reachesWatched(79 * S, l));
    CHECK(reachesWatched(l, l));
    CHECK(reachesWatched(l + 5 * S, l));         // past the end (rounding in reports)
}

// LIB-04: within 15 s of the end, isolated from the 80 % rule.
static void endMarginRule() {
    // 80 % of 60 s is 48 s; the end margin starts at 45 s, so 45..47 s are watched by the margin only.
    const std::int64_t d = 60 * S;
    CHECK(reachesWatched(45 * S, d));            // 15 s before the end
    CHECK(!reachesWatched(44 * S, d));           // 16 s before the end and 73 %
    CHECK(!reachesWatched(44 * S + 999, d));
    // Unknown or zero durations never count; nor does position 0.
    CHECK(!reachesWatched(10 * S, 0));
    CHECK(!reachesWatched(10 * S, -1));
    CHECK(!reachesWatched(0, d));
    // Clips no longer than the margin use the 80 % rule only.
    CHECK(!reachesWatched(1 * S, 12 * S));
    CHECK(reachesWatched(10 * S, 12 * S));       // 83 %
    CHECK(!reachesWatched(5 * S, 15 * S));
    CHECK(reachesWatched(1 * S, 16 * S));        // 16 s clip: 15 s before its end
}

static void mergeIsSticky() {
    HistoryRecord r = mergeProgress(nullptr, progress(90 * S, 100 * S), 1000, true);
    CHECK(r.watched);
    CHECK(r.playCount == 1);
    CHECK(r.watchedAtMs == 1000);
    CHECK(r.fraction > 0.899 && r.fraction < 0.901);
    CHECK(r.video.title == "Title");
    // A later partial view keeps watched and the furthest fraction, updates position and time.
    HistoryRecord later = mergeProgress(&r, progress(10 * S, 100 * S), 2000, true);
    CHECK(later.watched);
    CHECK(later.fraction > 0.899);
    CHECK(later.positionMs == 10 * S);
    CHECK(later.watchedAtMs == 2000);
    CHECK(later.playCount == 2);
    // Same session: no play count bump.
    HistoryRecord same = mergeProgress(&later, progress(20 * S, 100 * S), 3000, false);
    CHECK(same.playCount == 2);
    // Empty metadata in a report keeps the stored one.
    PlaybackProgress bare = progress(30 * S, 100 * S);
    bare.video.title.clear();
    bare.video.channel.clear();
    HistoryRecord kept = mergeProgress(&same, bare, 4000, false);
    CHECK(kept.video.title == "Title" && kept.video.channel == "Channel");
    // Fresh partial view: not watched.
    HistoryRecord partial = mergeProgress(nullptr, progress(30 * S, 100 * S), 1, true);
    CHECK(!partial.watched);
    CHECK(partial.fraction > 0.299 && partial.fraction < 0.301);
    // Ended marks watched with fraction 1.
    HistoryRecord ended = mergeProgress(&partial, progress(99 * S, 100 * S, true), 2, false);
    CHECK(ended.watched && ended.fraction == 1.0);
    // Live: no fraction, duration unknown; history still records the visit.
    HistoryRecord live = mergeProgress(nullptr, progress(500 * S, 0, false, true), 5, true);
    CHECK(!live.watched && live.fraction == 0 && live.playCount == 1);
}

static void manualMarks() {
    HistoryRecord r = mergeProgress(nullptr, progress(20 * S, 100 * S), 1, true);
    HistoryRecord m = markWatched(r, 77);
    CHECK(m.watched && m.fraction == 1.0 && m.positionMs == 100 * S && m.watchedAtMs == 77);
    HistoryRecord u = unmarkWatched(m);
    CHECK(!u.watched && u.fraction == 0 && u.positionMs == 0);
    CHECK(u.watchedAtMs == 77 && u.playCount == m.playCount && u.video.title == "Title");
    // A record created from a row (duration from the summary in seconds).
    HistoryRecord fromRow;
    fromRow.video.videoId = "dQw4w9WgXcQ";
    fromRow.video.durationSeconds = 212;
    HistoryRecord fm = markWatched(fromRow, 5);
    CHECK(fm.durationMs == 212 * S && fm.positionMs == 212 * S && fm.watched);
}

static void historyCap() {
    CHECK(clampHistoryCap(0) == 500);
    CHECK(clampHistoryCap(-5) == 500);
    CHECK(clampHistoryCap(99) == 100);
    CHECK(clampHistoryCap(100) == 100);
    CHECK(clampHistoryCap(5000) == 5000);
    CHECK(clampHistoryCap(5001) == 5000);
    CHECK(clampHistoryCap(1234) == 1234);
    CHECK(!validHistoryCap(99));
    CHECK(validHistoryCap(100));
    CHECK(validHistoryCap(500));
    CHECK(validHistoryCap(5000));
    CHECK(!validHistoryCap(5001));
    CHECK(!validHistoryCap(0));
    CHECK(limits::historyCapDefault == 500);
}

// LIB-05: 9 s and 14 s before the end store "none"; 10 s and 16 s store.
static void resumeStore() {
    const std::int64_t d = 600 * S;
    CHECK(resumePositionToStore(progress(9 * S, d)) == -1);
    CHECK(resumePositionToStore(progress(10 * S - 1, d)) == -1);
    CHECK(resumePositionToStore(progress(10 * S, d)) == 10 * S);
    CHECK(resumePositionToStore(progress(d - 14 * S, d)) == -1);
    CHECK(resumePositionToStore(progress(d - 15 * S, d)) == -1);
    CHECK(resumePositionToStore(progress(d - 16 * S, d)) == d - 16 * S);
    CHECK(resumePositionToStore(progress(300 * S, d, true)) == -1);       // ended
    CHECK(resumePositionToStore(progress(300 * S, d, false, true)) == -1); // live
    CHECK(resumePositionToStore(progress(300 * S, 0)) == -1);              // unknown duration
    CHECK(resumePositionToStore(progress(300 * S, -1)) == -1);
    // IMP-01 import: strictly more than 10 s, and more than 15 s from the end.
    CHECK(!resumeEligibleForImport(10 * S, d));
    CHECK(resumeEligibleForImport(10 * S + 1, d));
    CHECK(!resumeEligibleForImport(d - 15 * S, d));
    CHECK(resumeEligibleForImport(d - 15 * S - 1, d));
    CHECK(!resumeEligibleForImport(60 * S, 0));
    // Expiry: 30 days kept, 31 days gone.
    ResumeRecord r;
    r.touchedMs = 0;
    CHECK(!resumeExpired(r, 30 * Day));
    CHECK(resumeExpired(r, 30 * Day + 1));
    CHECK(resumeExpired(r, 31 * Day));
    CHECK(limits::resumeCapacity == 1000);
    CHECK(limits::resumeSaveIntervalMs == 30 * S);
}

static void resumeDecision() {
    ResumeDecision d = decideResume(90 * S, ResumeBehaviour::Always, -1);
    CHECK(d.action == ResumeAction::ResumeAt && d.positionMs == 90 * S);
    d = decideResume(90 * S, ResumeBehaviour::Ask, -1);
    CHECK(d.action == ResumeAction::AskThenResume && d.positionMs == 90 * S && d.autoResumeSeconds == 5);
    d = decideResume(90 * S, ResumeBehaviour::Never, -1);
    CHECK(d.action == ResumeAction::StartOver && d.positionMs == 0);
    d = decideResume(-1, ResumeBehaviour::Always, -1);
    CHECK(d.action == ResumeAction::StartOver);
    d = decideResume(-1, ResumeBehaviour::Ask, -1);
    CHECK(d.action == ResumeAction::StartOver && d.autoResumeSeconds == 0);
    // A link t= wins in every mode, also t=0 and with no stored point (§6 L1: t=90 -> 1:30).
    const ResumeBehaviour modes[] = {ResumeBehaviour::Always, ResumeBehaviour::Ask, ResumeBehaviour::Never};
    for (ResumeBehaviour m : modes) {
        d = decideResume(300 * S, m, 90 * S);
        CHECK(d.action == ResumeAction::StartAtLink && d.positionMs == 90 * S);
        d = decideResume(300 * S, m, 0);
        CHECK(d.action == ResumeAction::StartAtLink && d.positionMs == 0);
        d = decideResume(-1, m, 90 * S);
        CHECK(d.action == ResumeAction::StartAtLink && d.positionMs == 90 * S);
    }
}

// LIB-06: bar above 0.5 % (0.6 % shows, 0.5 % and 0.4 % do not).
static void overlay() {
    WatchState s;
    OverlayFlags f = overlayFor(s);
    CHECK(!f.showProgress && !f.watchedBadge && f.progress == 0);
    s.known = true;
    s.fraction = 0.006;
    CHECK(overlayFor(s).showProgress);
    s.fraction = 0.005;
    CHECK(!overlayFor(s).showProgress);
    s.fraction = 0.004;
    CHECK(!overlayFor(s).showProgress);
    s.fraction = 0.5;
    s.watched = true;
    f = overlayFor(s);
    CHECK(f.showProgress && f.watchedBadge && f.progress == 0.5);
    s.fraction = 1.7; // defensive clamp
    CHECK(overlayFor(s).progress == 1.0);
    WatchState unknown;
    unknown.fraction = 0.9;
    unknown.watched = true;
    CHECK(!overlayFor(unknown).watchedBadge && !overlayFor(unknown).showProgress);
}

// LIB-07: 3 watched among 10 rows -> 7 shown in feed and channel; search only with the option.
static void hideWatched() {
    std::vector<WatchState> rows(10);
    for (int i = 0; i < 10; ++i) {
        rows[i].known = i < 6;
        rows[i].fraction = 0.3;
        rows[i].watched = i == 1 || i == 3 || i == 5;
    }
    HideWatchedSettings on;
    on.enabled = true;
    HideWatchedSettings off;
    HideWatchedSettings withSearch;
    withSearch.enabled = true;
    withSearch.includeSearch = true;
    const ListContext contexts[] = {ListContext::Feed, ListContext::Channel};
    for (ListContext c : contexts) {
        int shown = 0, shownOff = 0;
        for (const WatchState &w : rows) {
            if (!hideFromList(w, c, on)) ++shown;
            if (!hideFromList(w, c, off)) ++shownOff;
        }
        CHECK(shown == 7);
        CHECK(shownOff == 10);
    }
    int search = 0, searchOpt = 0, other = 0;
    for (const WatchState &w : rows) {
        if (!hideFromList(w, ListContext::Search, on)) ++search;
        if (!hideFromList(w, ListContext::Search, withSearch)) ++searchOpt;
        if (!hideFromList(w, ListContext::Other, withSearch)) ++other;
    }
    CHECK(search == 10);
    CHECK(searchOpt == 7);
    CHECK(other == 10);
    HideWatchedSettings searchOnly; // includeSearch without the main switch does nothing
    searchOnly.includeSearch = true;
    CHECK(!hideFromList(rows[1], ListContext::Search, searchOnly));
}

static ChannelRecord ch(const char *id, const char *name, std::int64_t latest) {
    ChannelRecord c;
    c.channelId = id;
    c.name = name;
    c.latestUploadMs = latest;
    return c;
}

static std::string ucId(int i) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "UC%022d", i);
    return buf;
}

// LIB-01: both sort orders on a 20-channel fixture.
static void subscriptionSort() {
    const char *names[20] = {"zeta", "Alpha", "beta", "Beta", "gamma", "Delta", "epsilon", "Ñandú", "alpha",
                             "omega", "Omicron", "pi", "Rho", "sigma", "Tau", "upsilon", "Phi", "chi", "Psi", "eta"};
    std::vector<ChannelRecord> list;
    for (int i = 0; i < 20; ++i) {
        ChannelRecord c = ch(ucId(i).c_str(), names[i], i % 4 == 0 ? 0 : 1000 + (i * 37) % 100);
        list.push_back(c);
    }
    std::vector<ChannelRecord> byName = list;
    sortSubscriptions(byName, SubscriptionSort::Name);
    CHECK(byName.size() == 20);
    // "Alpha"(UC..01) and "alpha"(UC..08) fold equal: tie by channel id.
    CHECK(byName[0].name == "Alpha" && byName[1].name == "alpha");
    CHECK(byName[2].name == "beta" && byName[3].name == "Beta"); // UC..02 before UC..03
    CHECK(byName[4].name == "chi");
    CHECK(byName[19].name == "Ñandú"); // non-ASCII bytes sort after ASCII
    CHECK(byName[18].name == "zeta");
    for (std::size_t i = 1; i < byName.size(); ++i) {
        std::string a = byName[i - 1].name, b = byName[i].name;
        for (char &c : a) if (c >= 'A' && c <= 'Z') c = char(c + 32);
        for (char &c : b) if (c >= 'A' && c <= 'Z') c = char(c + 32);
        CHECK(a < b || (a == b && byName[i - 1].channelId < byName[i].channelId));
    }
    std::vector<ChannelRecord> byLatest = list;
    sortSubscriptions(byLatest, SubscriptionSort::LatestUpload);
    for (std::size_t i = 1; i < byLatest.size(); ++i) {
        const ChannelRecord &a = byLatest[i - 1], &b = byLatest[i];
        if (a.latestUploadMs == 0) CHECK(b.latestUploadMs == 0); // unknown last
        if (a.latestUploadMs > 0 && b.latestUploadMs > 0) CHECK(a.latestUploadMs >= b.latestUploadMs);
    }
    // Ties on the upload time fall back to the name order.
    std::vector<ChannelRecord> tie;
    tie.push_back(ch(ucId(1).c_str(), "b", 5));
    tie.push_back(ch(ucId(2).c_str(), "A", 5));
    tie.push_back(ch(ucId(3).c_str(), "c", 0));
    tie.push_back(ch(ucId(4).c_str(), "a", 0));
    tie.push_back(ch(ucId(5).c_str(), "z", 9));
    sortSubscriptions(tie, SubscriptionSort::LatestUpload);
    CHECK(tie[0].name == "z" && tie[1].name == "A" && tie[2].name == "b" && tie[3].name == "a" && tie[4].name == "c");
}

static void urlParsers() {
    const std::string uc = "UCuAXFkgsw1L7xaCfnd5JJOw";
    CHECK(channelIdFromUrl("https://www.youtube.com/channel/" + uc) == uc);
    CHECK(channelIdFromUrl("http://youtube.com/channel/" + uc + "/videos") == uc);
    CHECK(channelIdFromUrl("https://m.youtube.com/channel/" + uc + "?view=0") == uc);
    CHECK(channelIdFromUrl("https://music.youtube.com/channel/" + uc + "#x") == uc);
    CHECK(channelIdFromUrl("youtube.com/channel/" + uc) == uc);
    CHECK(channelIdFromUrl("  https://WWW.YouTube.com/channel/" + uc + "  ") == uc);
    CHECK(channelIdFromUrl("https://www.youtube.com/channel/UCshort").empty());
    CHECK(channelIdFromUrl("https://www.youtube.com/channel/UC" + std::string(22, '!')).empty());
    CHECK(channelIdFromUrl("https://evil.example/channel/" + uc).empty());
    CHECK(channelIdFromUrl("https://notyoutube.com/channel/" + uc).empty());
    CHECK(channelIdFromUrl("https://www.youtube.com/@handle").empty());
    CHECK(channelIdFromUrl(uc).empty());
    CHECK(channelIdFromUrl("ftp://www.youtube.com/channel/" + uc).empty());

    CHECK(isHandleUrl("https://www.youtube.com/@LinusTechTips"));
    CHECK(isHandleUrl("https://youtube.com/@x"));
    CHECK(isHandleUrl("https://www.youtube.com/@x/videos"));
    CHECK(isHandleUrl("https://www.youtube.com/c/SomeName"));
    CHECK(isHandleUrl("https://www.youtube.com/user/oldname?x=1"));
    CHECK(!isHandleUrl("https://www.youtube.com/@"));
    CHECK(!isHandleUrl("https://www.youtube.com/c/"));
    CHECK(!isHandleUrl("https://www.youtube.com/user"));
    CHECK(!isHandleUrl("https://www.youtube.com/channel/" + uc));
    CHECK(!isHandleUrl("https://example.com/@x"));
    CHECK(!isHandleUrl("https://www.youtube.com/watch?v=dQw4w9WgXcQ"));

    const std::string v = "dQw4w9WgXcQ";
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?v=" + v) == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?feature=share&v=" + v + "&t=42s") == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?v=" + v + "&list=PLx#t=1") == v);
    CHECK(videoIdFromUrl("https://m.youtube.com/watch?app=desktop&v=" + v) == v);
    CHECK(videoIdFromUrl("https://music.youtube.com/watch?v=" + v + "&si=abc") == v);
    CHECK(videoIdFromUrl("https://youtu.be/" + v) == v);
    CHECK(videoIdFromUrl("https://youtu.be/" + v + "?si=xyz&t=90") == v);
    CHECK(videoIdFromUrl("youtu.be/" + v) == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/shorts/" + v) == v);
    CHECK(videoIdFromUrl("https://youtube.com/shorts/" + v + "?feature=share") == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/embed/" + v + "?start=10") == v);
    CHECK(videoIdFromUrl("https://www.youtube-nocookie.com/embed/" + v) == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/live/" + v + "?si=a") == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/v/" + v + "&fs=1").empty()); // '&' is not a path separator
    CHECK(videoIdFromUrl("https://www.youtube.com/v/" + v) == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/v/" + v + "?fs=1") == v);
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?v=short").empty());
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?vv=" + v).empty());
    CHECK(videoIdFromUrl("https://www.youtube.com/watch?v=" + v + "x").empty());
    CHECK(videoIdFromUrl("https://www.youtube.com/watch").empty());
    CHECK(videoIdFromUrl("https://www.youtube.com/shorts/").empty());
    CHECK(videoIdFromUrl("https://example.com/watch?v=" + v).empty());
    CHECK(videoIdFromUrl("https://youtu.be.evil.com/" + v).empty());
    CHECK(videoIdFromUrl(v).empty());
    CHECK(videoIdFromUrl("").empty());

    const std::string pl = "PLrAXtmErZgOeiKm4sgNOknGvNjby9efdf";
    CHECK(playlistIdFromUrl("https://www.youtube.com/playlist?list=" + pl) == pl);
    CHECK(playlistIdFromUrl("https://www.youtube.com/watch?v=" + v + "&list=" + pl + "&index=3") == pl);
    CHECK(playlistIdFromUrl("https://youtube.com/playlist?list=" + pl + "#frag") == pl);
    CHECK(playlistIdFromUrl(pl) == pl);
    CHECK(playlistIdFromUrl("  " + pl + " ") == pl);
    CHECK(playlistIdFromUrl("OLAK5uy_kvvZ3x0j6yGhV8iL1k4KRQd9ldn5oSb3A") == "OLAK5uy_kvvZ3x0j6yGhV8iL1k4KRQd9ldn5oSb3A");
    CHECK(playlistIdFromUrl("list=" + pl) == pl);
    CHECK(playlistIdFromUrl("https://www.youtube.com/playlist?list=").empty());
    CHECK(playlistIdFromUrl("https://www.youtube.com/playlist?list=PL!bad").empty());
    CHECK(playlistIdFromUrl("https://www.youtube.com/watch?v=" + v).empty());
    CHECK(playlistIdFromUrl("P").empty());
    CHECK(playlistIdFromUrl(std::string(65, 'A')).empty());
    CHECK(playlistIdFromUrl("").empty());
}

static void playlistNames() {
    const std::string a = localPlaylistId(0x0123456789abcdefull);
    CHECK(a.size() == 12);
    for (char c : a) CHECK((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'));
    CHECK(localPlaylistId(0) == "000000000000");
    CHECK(localPlaylistId(1) != localPlaylistId(2));
    CHECK(localPlaylistId(0xffffffffffffull).size() == 12);
    CHECK(localPlaylistId(~0ull).size() == 12);
    std::set<std::string> ids;
    for (std::uint64_t i = 0; i < 1000; ++i) ids.insert(localPlaylistId(i * 0x9E3779B97F4A7C15ull));
    CHECK(ids.size() == 1000);

    CHECK(normalizePlaylistTitle("  My   list \t\n now ", "New playlist") == "My list now");
    CHECK(normalizePlaylistTitle("", "New playlist") == "New playlist");
    CHECK(normalizePlaylistTitle(" \t\n ", "New playlist") == "New playlist");
    CHECK(normalizePlaylistTitle("x", "fb") == "x");
    const std::string hundred(100, 'a');
    CHECK(normalizePlaylistTitle(hundred, "") == hundred);
    CHECK(normalizePlaylistTitle(hundred + "b", "") == hundred);
    // 99 characters, a space, then more: the space is not kept at the cut.
    CHECK(normalizePlaylistTitle(std::string(99, 'a') + " bcd", "") == std::string(99, 'a'));
    CHECK(normalizePlaylistTitle(std::string(98, 'a') + " bcd", "") == std::string(98, 'a') + " b");
    // UTF-8: 100 two-byte characters are kept whole, the 101st is dropped, nothing split.
    std::string nn;
    for (int i = 0; i < 101; ++i) nn += "\xC3\xB1"; // ñ
    const std::string capped = normalizePlaylistTitle(nn, "");
    CHECK(capped.size() == 200);
    CHECK(capped == nn.substr(0, 200));
    std::string mixed = std::string(99, 'x') + "\xF0\x9F\x8E\xB5" + "\xE2\x82\xAC"; // 99 + emoji + euro
    const std::string cm = normalizePlaylistTitle(mixed, "");
    CHECK(cm == std::string(99, 'x') + "\xF0\x9F\x8E\xB5");

    CHECK(sameListName("Favorites", "favorites"));
    CHECK(sameListName("FAVORITES", "  favorites "));
    CHECK(sameListName("Música  Rock", "música rock"));
    CHECK(sameListName("ÁRBOL", "árbol"));
    CHECK(sameListName("ÑANDÚ", "ñandú"));
    CHECK(sameListName("ΑΒΓ", "αβγ"));
    CHECK(sameListName("ПЛЕЙЛИСТ", "плейлист"));
    CHECK(sameListName("ŁÓDŹ", "łódź"));
    CHECK(!sameListName("Favorites", "Favorite"));
    CHECK(!sameListName("abc", "abd"));
    CHECK(!sameListName("árbol", "arbol"));
    CHECK(sameListName("", "  "));
}

// Shorts loop items: the player's one "-1" end save counts as watched only near the end; the same
// -1 from a stop in the first seconds does not.
static void loopEnd() {
    const std::int64_t d = 30000;                      // a 30 s Short: margin 3 s
    CHECK(loopEndReached(-1, 29800, d));
    CHECK(loopEndReached(-1, 27000, d));
    CHECK(!loopEndReached(-1, 26999, d));
    CHECK(!loopEndReached(-1, 2000, d));               // stopped 2 s in: below the resume minimum, not watched
    CHECK(!loopEndReached(-1, 0, d));
    CHECK(!loopEndReached(12000, 29800, d));           // a real resume point is never an end
    CHECK(!loopEndReached(-1, 29800, 0));              // unknown duration
    CHECK(loopEndReached(-1, 10500, 12000));           // a 12 s Short: margin 2 s
    CHECK(!loopEndReached(-1, 5000, 12000));           // stopped at 5 s
    CHECK(loopEndReached(-1, 540000, 600000));         // 10 min: margin 60 s
    CHECK(!loopEndReached(-1, 530000, 600000));
}

int main() {
    watchedThreshold();
    endMarginRule();
    mergeIsSticky();
    manualMarks();
    historyCap();
    resumeStore();
    resumeDecision();
    overlay();
    hideWatched();
    subscriptionSort();
    urlParsers();
    playlistNames();
    loopEnd();
    std::printf("library domain tests: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
