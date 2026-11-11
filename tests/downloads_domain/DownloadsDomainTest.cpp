// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the download domain: DL-05 names, DL-01/DL-03 picks, the
// state machine, the scheduler, resume decisions, DL-06 fallback and DL-08.
// Build: tests/downloads_domain/run.sh
#include "Core/Downloads/Domain/DownloadPolicy.h"
#include "Core/Downloads/Domain/FileNaming.h"
#include "Core/Downloads/Domain/Mp4Layout.h"
#include <algorithm>
#include <cstring>

#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::video;
using namespace nordtile::video::downloads;
using media::MediaFormat;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool validUtf8(const std::string &s) {
    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::size_t len = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 0;
        if (len == 0 || i + len > s.size()) return false;
        for (std::size_t k = 1; k < len; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

static bool endsWith(const std::string &s, const std::string &tail) {
    return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

// The DL-05 shape: "<title> [<id>].<ext>", one safe component.
static bool wellFormed(const std::string &name, const std::string &id, const std::string &ext) {
    return safeFileName(name) && validUtf8(name) && name.size() <= maxNameBytes
        && endsWith(name, " [" + id + "]." + ext) && name.find('/') == std::string::npos;
}

static void naming() {
    const std::string id = "dQw4w9WgXcQ";
    CHECK(fileName(baseName("Hello world", id), "mp4") == "Hello world [dQw4w9WgXcQ].mp4");

    // 300-byte CJK title: cut on a character boundary to at most 150 bytes.
    std::string cjk;
    for (int i = 0; i < 100; ++i) cjk += "\xE6\x97\xA5"; // U+65E5, 3 bytes
    CHECK(cjk.size() == 300);
    const std::string t = sanitizeTitle(cjk);
    CHECK(t.size() == 150 && validUtf8(t));
    const std::string name = fileName(baseName(cjk, id), "webm");
    CHECK(wellFormed(name, id, "webm"));
    CHECK(partFileName(trackFileName(baseName(cjk, id), Track::Video, "webm")).size() <= maxNameBytes);

    // 4-byte emoji never split; 149-byte budget leaves no half character.
    std::string emoji;
    for (int i = 0; i < 80; ++i) emoji += "\xF0\x9F\x98\x80"; // U+1F600
    const std::string e = sanitizeTitle(emoji);
    CHECK(e.size() <= 150 && e.size() % 4 == 0 && validUtf8(e));
    // ZWJ sequence kept intact when short, dangling ZWJ trimmed after a cut.
    CHECK(sanitizeTitle("a\xE2\x80\x8D") == "a");
    const std::string family = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9";
    CHECK(sanitizeTitle(family) == family);

    // Combining marks, RTL text kept; bidi overrides dropped.
    const std::string combining = "Cafe\xCC\x81 de\xCC\x81ja\xCC\x80";
    CHECK(sanitizeTitle(combining) == combining);
    const std::string hebrew = "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D";
    CHECK(sanitizeTitle(hebrew) == hebrew);
    CHECK(sanitizeTitle("evil\xE2\x80\xAEgpj.exe") == "evilgpj.exe");
    CHECK(sanitizeTitle("\xE2\x80\x8B\xEF\xBB\xBFx") == "x");

    // Traversal, separators, NUL, controls, leading dot/dash.
    CHECK(sanitizeTitle("../../etc/passwd") == "etc passwd");
    CHECK(wellFormed(fileName(baseName("../../etc/passwd", id), "m4a"), id, "m4a"));
    CHECK(sanitizeTitle("a\\b/c") == "a b c");
    CHECK(sanitizeTitle(std::string("a\0b", 3)) == "a b");
    CHECK(sanitizeTitle("line\nbreak\ttab\x7f" "del") == "line break tab del");
    CHECK(sanitizeTitle(".hidden") == "hidden");
    CHECK(sanitizeTitle("-rf now") == "rf now");
    CHECK(sanitizeTitle("  spaced   out  ") == "spaced out");
    CHECK(sanitizeTitle("trailing dots...") == "trailing dots");
    CHECK(sanitizeTitle("\xC2\x85" "c1\xC2\x9F") == "c1");
    // FAT/exFAT reserved characters.
    CHECK(sanitizeTitle("a<b>c:d\"e|f?g*h") == "a b c d e f g h");
    // Template-looking text stays literal.
    CHECK(sanitizeTitle("%(title)s ${HOME} {id}") == "%(title)s ${HOME} {id}");
    CHECK(fileName(baseName("%(title)s", id), "mp4") == "%(title)s [dQw4w9WgXcQ].mp4");
    // Degenerate titles.
    CHECK(sanitizeTitle("") == "video");
    CHECK(sanitizeTitle(".") == "video");
    CHECK(sanitizeTitle("..") == "video");
    CHECK(sanitizeTitle("   ") == "video");
    CHECK(sanitizeTitle("\xFF\xFE\xC0\xAF") == "video"); // invalid UTF-8 and an overlong '/'
    CHECK(sanitizeTitle(std::string("\0", 1)) == "video");
    // Reserved device names are harmless: the id suffix makes the name distinct.
    CHECK(fileName(baseName("CON", id), "mp4") == "CON [dQw4w9WgXcQ].mp4");
    CHECK(fileName(baseName("NUL", id), "mp4") == "NUL [dQw4w9WgXcQ].mp4");

    // Id reduction.
    CHECK(sanitizeVideoId("ab/../c d%(x)") == "abcdx");
    CHECK(sanitizeVideoId(std::string(100, 'a')).size() == 64);
    CHECK(sanitizeVideoId("\xE6\x97\xA5") == "unknown");
    CHECK(wellFormed(fileName(baseName("x", "a/b\\c..d"), "mp4"), "abcd", "mp4"));

    // Extensions: allowlist only.
    CHECK(allowedExtension("m4a") && allowedExtension("webm") && allowedExtension("mp4") && allowedExtension("mkv"));
    CHECK(allowedExtension("opus") && allowedExtension("jpg"));
    CHECK(!allowedExtension("exe") && !allowedExtension("") && !allowedExtension("mp4/..") && !allowedExtension("MP4"));
    CHECK(fileName("a [b]", "sh").empty());
    CHECK(trackFileName("a [b]", Track::Audio, "m4a") == "a [b].audio.m4a");
    CHECK(thumbnailFileName("a [b]") == "a [b].jpg");
    CHECK(!safeFileName(".") && !safeFileName("..") && !safeFileName("a/b") && !safeFileName(std::string("a\0b", 3)));
    CHECK(!safeFileName(std::string(256, 'a')) && safeFileName(std::string(255, 'a')));

    // Whole name stays <= 255 bytes for every combination of long title and id.
    std::string mixed;
    for (int i = 0; i < 120; ++i) mixed += (i % 2) ? "\xF0\x9F\x98\x80" : "\xE6\x97\xA5";
    const std::string longBase = baseName(mixed, std::string(64, 'Z'));
    CHECK(partFileName(trackFileName(longBase, Track::Video, "webm")).size() <= maxNameBytes);
    CHECK(wellFormed(fileName(longBase, "webm"), std::string(64, 'Z'), "webm"));
    CHECK(utf8Prefix("\xE6\x97\xA5\xE6\x97\xA5", 4) == 3);
}

static MediaFormat fmt(const char *id, const char *ext, const char *v, const char *a, int h, double fps = 30,
                       double kbps = 1000) {
    MediaFormat f;
    f.id = id; f.container = ext; f.protocol = media::Protocol::Https;
    f.url = std::string("https://rr1.googlevideo.com/videoplayback?itag=") + id + "&clen=1000";
    f.videoCodecTag = v; f.audioCodecTag = a;
    f.videoCodec = media::parseVideoCodec(v); f.audioCodec = media::parseAudioCodec(a);
    f.height = h; f.width = h * 16 / 9; f.fps = fps; f.totalKbps = kbps;
    if (f.videoCodec == media::VideoCodec::None) f.audioKbps = kbps;
    return f;
}

static std::vector<MediaFormat> offer() {
    return {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("136", "mp4", "avc1.4D401F", "none", 720), fmt("137", "mp4", "avc1.640028", "none", 1080),
        fmt("247", "webm", "vp09.00.31.08", "none", 720), fmt("248", "webm", "vp09.00.40.08", "none", 1080),
        fmt("313", "webm", "vp09.00.51.08", "none", 2160),
        fmt("399", "mp4", "av01.0.08M.08", "none", 1080), fmt("401", "mp4", "av01.0.12M.08", "none", 2160),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129), fmt("139", "m4a", "none", "mp4a.40.5", 0, 0, 48),
        fmt("251", "webm", "none", "opus", 0, 0, 135), fmt("250", "webm", "none", "opus", 0, 0, 70),
    };
}

static media::DecoderCapabilities device(bool h264 = true, bool h264hw = true, bool vp9 = true, bool vp9hw = true) {
    media::DecoderCapabilities c;
    c.h264.available = h264; c.h264.hardware = h264hw;
    c.vp9.available = vp9; c.vp9.hardware = vp9hw;
    c.av1.available = true; // even a device claiming AV1 never gets it
    return c;
}

static MuxCapabilities muxers(bool mp4, bool webm) { MuxCapabilities m; m.mp4 = mp4; m.webm = webm; return m; }

static void audioPicks() {
    std::vector<MediaFormat> f = offer();
    AudioPick p = pickAudio(f);
    CHECK(p.ok && p.format.id == "140" && p.extension == "m4a");

    // 140 preferred over a higher-bitrate AAC.
    f.push_back(fmt("141", "m4a", "none", "mp4a.40.2", 0, 0, 256));
    CHECK(pickAudio(f).format.id == "140");
    // DRC variant never preferred.
    MediaFormat drc = fmt("140-drc", "m4a", "none", "mp4a.40.2", 0, 0, 129);
    drc.dynamicRangeCompressed = true;
    std::vector<MediaFormat> withDrc = { drc, fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129) };
    CHECK(pickAudio(withDrc).format.id == "140");

    // Without 140: the best AAC m4a.
    std::vector<MediaFormat> no140;
    for (const MediaFormat &x : offer()) if (x.id != "140") no140.push_back(x);
    CHECK(pickAudio(no140).format.id == "139" && pickAudio(no140).extension == "m4a");

    // Without any AAC: the best Opus, kept as WebM with a .webm extension.
    std::vector<MediaFormat> opusOnly = { fmt("250", "webm", "none", "opus", 0, 0, 70), fmt("251", "webm", "none", "opus", 0, 0, 135),
                                          fmt("137", "mp4", "avc1.640028", "none", 1080) };
    p = pickAudio(opusOnly);
    CHECK(p.ok && p.format.id == "251" && p.extension == "webm");
    CHECK(fileName(baseName("Song", "dQw4w9WgXcQ"), p.extension) == "Song [dQw4w9WgXcQ].webm");

    // The original-language track wins over a dub.
    MediaFormat dub = fmt("140-0", "m4a", "none", "mp4a.40.2", 0, 0, 129); dub.languagePreference = -1;
    MediaFormat orig = fmt("140-1", "m4a", "none", "mp4a.40.2", 0, 0, 129); orig.languagePreference = 10;
    std::vector<MediaFormat> dubs = { dub, orig };
    CHECK(pickAudio(dubs).format.id == "140-1");

    // Non-HTTPS or URL-less formats are ignored; nothing left -> no pick.
    MediaFormat hls = fmt("140", "m4a", "none", "mp4a.40.2", 0); hls.protocol = media::Protocol::Hls;
    MediaFormat nourl = fmt("140", "m4a", "none", "mp4a.40.2", 0); nourl.url.clear();
    std::vector<MediaFormat> bad = { hls, nourl };
    CHECK(!pickAudio(bad).ok);
}

static void videoPicks() {
    const std::vector<MediaFormat> f = offer();
    // Default ceiling 1080 with both muxers: equal height -> H.264 + AAC -> mp4.
    VideoPick p = pickVideo(f, effectiveQualityHeight(0, 1080), device(), muxers(true, true));
    CHECK(p.ok && !p.progressive && p.video.id == "137" && p.audio.id == "140");
    CHECK(p.mux == MuxTarget::Mp4 && p.layout == OutputLayout::MuxedFile && p.outputExtension == "mp4");
    CHECK(p.videoExtension == "mp4" && p.audioExtension == "m4a");

    // Height ceiling respected: 720.
    p = pickVideo(f, 720, device(), muxers(true, true));
    CHECK(p.ok && p.video.height == 720 && p.video.id == "136");
    CHECK(effectiveQualityHeight(480, 1080) == 480 && effectiveQualityHeight(0, 720) == 720);

    // No cap: VP9 2160 (never AV1 2160) paired with Opus -> WebM.
    p = pickVideo(f, 0, device(), muxers(true, true));
    CHECK(p.ok && p.video.id == "313" && p.audio.id == "251" && p.mux == MuxTarget::WebM && p.outputExtension == "webm");
    CHECK(p.audioExtension == "webm");

    // Only a WebM muxer: at 1080 the VP9 pair (muxable) beats H.264 (pair only).
    p = pickVideo(f, 1080, device(), muxers(false, true));
    CHECK(p.video.id == "248" && p.layout == OutputLayout::MuxedFile);
    // No muxer at all: a track pair, H.264 preferred at equal height.
    p = pickVideo(f, 1080, device(), muxers(false, false));
    CHECK(p.video.id == "137" && p.layout == OutputLayout::TrackPair && p.mux == MuxTarget::Mp4);
    // Hardware decode preferred at equal height: H.264 software, VP9 hardware.
    p = pickVideo(f, 1080, device(true, false, true, true), muxers(true, true));
    CHECK(p.video.id == "248");
    // Device without VP9: never VP9.
    p = pickVideo(f, 0, device(true, true, false, false), muxers(true, true));
    CHECK(p.video.id == "137");

    // AV1-only offer: never AV1; falls back to the progressive stream.
    std::vector<MediaFormat> av1 = { fmt("399", "mp4", "av01.0.08M.08", "none", 1080), fmt("140", "m4a", "none", "mp4a.40.2", 0),
                                     fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500) };
    p = pickVideo(av1, 1080, device(), muxers(true, true));
    CHECK(p.ok && p.progressive && p.video.id == "18" && p.layout == OutputLayout::SingleFile && p.outputExtension == "mp4");
    // H.264 video without AAC audio cannot pair.
    std::vector<MediaFormat> noAac = { fmt("137", "mp4", "avc1.640028", "none", 1080), fmt("251", "webm", "none", "opus", 0) };
    CHECK(!pickVideo(noAac, 1080, device(), muxers(true, true)).ok);
    // HDR is skipped.
    MediaFormat hdr = fmt("337", "webm", "vp09.02.51.10", "none", 2160); hdr.highDynamicRange = true;
    std::vector<MediaFormat> withHdr = { hdr, fmt("248", "webm", "vp09.00.40.08", "none", 1080), fmt("251", "webm", "none", "opus", 0) };
    CHECK(pickVideo(withHdr, 0, device(), muxers(true, true)).video.id == "248");

    const std::vector<int> heights = videoQualityHeights(f, device());
    CHECK(heights.size() == 4 && heights[0] == 2160 && heights[1] == 1080 && heights[3] == 360);
    CHECK(muxTargetFor(media::VideoCodec::Vp9, media::AudioCodec::Aac) == MuxTarget::None);
}

static void transitions() {
    CHECK(transition(State::Queued, Event::Start).next == State::Resolving);
    CHECK(transition(State::Resolving, Event::Resolved).next == State::Downloading);
    CHECK(transition(State::Downloading, Event::TracksDone).next == State::Remuxing);
    CHECK(transition(State::Remuxing, Event::Finished).next == State::Done);
    CHECK(transition(State::Downloading, Event::Finished).next == State::Done);
    CHECK(transition(State::Downloading, Event::Pause).next == State::Paused);
    CHECK(transition(State::WaitingForNetwork, Event::Pause).next == State::Paused);
    CHECK(!transition(State::Remuxing, Event::Pause).allowed);
    CHECK(!transition(State::Done, Event::Pause).allowed && transition(State::Done, Event::Pause).next == State::Done);
    CHECK(transition(State::Paused, Event::Resume).next == State::Queued);
    CHECK(!transition(State::Downloading, Event::Resume).allowed);
    CHECK(transition(State::Downloading, Event::Cancel).next == State::Cancelled);
    CHECK(transition(State::Paused, Event::Cancel).allowed);
    CHECK(!transition(State::Done, Event::Cancel).allowed);
    CHECK(transition(State::Downloading, Event::Fail).next == State::Failed);
    CHECK(!transition(State::Paused, Event::Fail).allowed);
    CHECK(transition(State::Failed, Event::Retry).next == State::Queued);
    CHECK(!transition(State::Done, Event::Retry).allowed);
    CHECK(transition(State::Downloading, Event::NetworkGated).next == State::WaitingForNetwork);
    CHECK(transition(State::WaitingForNetwork, Event::NetworkOpen).next == State::Queued);
    // Restart recovery.
    CHECK(transition(State::Downloading, Event::Recover).next == State::Queued);
    CHECK(transition(State::Resolving, Event::Recover).next == State::Queued);
    CHECK(transition(State::Remuxing, Event::Recover).next == State::Queued);
    CHECK(!transition(State::Paused, Event::Recover).allowed && transition(State::Paused, Event::Recover).next == State::Paused);
    CHECK(transition(State::Done, Event::Recover).next == State::Done);
    CHECK(transition(State::WaitingForNetwork, Event::Recover).next == State::WaitingForNetwork);
    CHECK(wantsKeepAlive(State::Remuxing) && wantsKeepAlive(State::Queued) && !wantsKeepAlive(State::Paused));
    CHECK(!wantsKeepAlive(State::WaitingForNetwork) && !wantsKeepAlive(State::Done));
}

static ScheduleItem item(std::int64_t id, State s, std::int64_t key) { ScheduleItem i; i.id = id; i.state = s; i.sortKey = key; return i; }

static void scheduler() {
    // 5 queued: exactly 2 start, in sort order.
    std::vector<ScheduleItem> q = { item(1, State::Queued, 5), item(2, State::Queued, 1), item(3, State::Queued, 3),
                                    item(4, State::Queued, 4), item(5, State::Queued, 2) };
    ScheduleDecision d = schedule(q, true);
    CHECK(d.start.size() == 2 && d.start[0] == 2 && d.start[1] == 5);
    // One transfer running: one more starts; Remuxing does not count.
    q[1].state = State::Downloading; q[4].state = State::Remuxing;
    d = schedule(q, true);
    CHECK(d.start.size() == 1 && d.start[0] == 3);
    q[2].state = State::Resolving;
    CHECK(schedule(q, true).start.empty());
    // Paused / Done / Failed never start.
    std::vector<ScheduleItem> idle = { item(1, State::Paused, 0), item(2, State::Done, 0), item(3, State::Failed, 0) };
    CHECK(schedule(idle, true).start.empty());
    // Unmetered gate closed: nothing starts; queued and running transfers wait.
    d = schedule(q, false);
    CHECK(d.start.empty() && d.gate.size() == 4);
    // Gate open again: waiting items return to the queue and fill the slots.
    std::vector<ScheduleItem> w = { item(1, State::WaitingForNetwork, 1), item(2, State::WaitingForNetwork, 2),
                                    item(3, State::WaitingForNetwork, 3) };
    d = schedule(w, true);
    CHECK(d.ungate.size() == 3 && d.start.size() == 2 && d.start[0] == 1 && d.start[1] == 2);
}

static void resume() {
    CHECK(resumeAtStart(0, 1000) == ResumeAction::ContinueFromOffset);
    CHECK(resumeAtStart(500, 1000) == ResumeAction::ContinueFromOffset);
    CHECK(resumeAtStart(1000, 1000) == ResumeAction::Complete);
    CHECK(resumeAtStart(1001, 1000) == ResumeAction::RestartFromZero);
    CHECK(resumeAtStart(10, 0) == ResumeAction::ContinueFromOffset);

    ResponseFacts f;
    f.nowMs = 5000; f.resolvedAtMs = 0;
    f.httpStatus = 403; f.requestOffset = 0;
    CHECK(resumeAfterResponse(f) == ResumeAction::RetryAfterYoungTokenDelay);
    f.youngRetries = 2; // budget spent: re-resolve
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    f.youngRetries = 0; f.nowMs = 20000; // old token
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    f.nowMs = 5000; f.requestOffset = 4096; // 403 later in the file
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    f.httpStatus = 410;
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    f.reResolves = 3;
    CHECK(resumeAfterResponse(f) == ResumeAction::Fail);
    f.reResolves = 0; f.httpStatus = 206; f.urlExpired = true;
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    f.urlExpired = false;
    CHECK(resumeAfterResponse(f) == ResumeAction::ContinueFromOffset);
    // A 200 to a ranged request is a restart only at offset 0.
    f.httpStatus = 200; f.requestOffset = 0;
    CHECK(resumeAfterResponse(f) == ResumeAction::ContinueFromOffset);
    f.requestOffset = 100;
    CHECK(resumeAfterResponse(f) == ResumeAction::ReResolveSameFormat);
    // Truncation and transport errors: reopen from the current offset, bounded.
    f.httpStatus = 206; f.truncated = true;
    CHECK(resumeAfterResponse(f) == ResumeAction::ContinueFromOffset);
    f.truncated = false; f.httpStatus = 0;
    CHECK(resumeAfterResponse(f) == ResumeAction::ContinueFromOffset);
    f.httpStatus = 503;
    CHECK(resumeAfterResponse(f) == ResumeAction::ContinueFromOffset);
    f.transientRetries = 5;
    CHECK(resumeAfterResponse(f) == ResumeAction::Fail);
    f.transientRetries = 0; f.httpStatus = 416;
    CHECK(resumeAfterResponse(f) == ResumeAction::RestartFromZero);
    f.httpStatus = 404;
    CHECK(resumeAfterResponse(f) == ResumeAction::Fail);

    // Re-resolve size check: equal -> continue, mismatch -> the item fails (DL-02).
    CHECK(resumeAfterReResolve(1000, 1000) == ResumeAction::ContinueFromOffset);
    CHECK(resumeAfterReResolve(1000, 999) == ResumeAction::Fail);
    CHECK(resumeAfterReResolve(1000, 1001) == ResumeAction::Fail);
    CHECK(resumeAfterReResolve(1000, 0) == ResumeAction::ContinueFromOffset);
    CHECK(resumeAfterReResolve(0, 999) == ResumeAction::ContinueFromOffset);

    // URL helpers.
    const std::string url = "https://rr3---sn-x.googlevideo.com/videoplayback?expire=1700000000&itag=140&clen=3456789&x=1";
    CHECK(queryParameter(url, "itag") == "140");
    CHECK(queryParameter(url, "clen") == "3456789");
    CHECK(queryParameter(url, "len").empty());
    CHECK(queryParameter(url, "x") == "1");
    CHECK(urlExpired(url, 1700000000));
    CHECK(urlExpired(url, 1699999990)); // within the safety margin
    CHECK(!urlExpired(url, 1600000000));
    CHECK(!urlExpired("https://h/x?itag=1", 1700000000));
    MediaFormat m; m.url = url;
    CHECK(contentLengthOf(m) == 3456789);
    m.fileSize = 42;
    CHECK(contentLengthOf(m) == 42);
    CHECK(rangeHeader(0, 1000, 256) == "bytes=0-255");
    CHECK(rangeHeader(768, 1000, 256) == "bytes=768-999");
    CHECK(rangeHeader(900, 1000, 256) == "bytes=900-999");
    CHECK(rangeHeader(10, 0, 100) == "bytes=10-109");
    std::int64_t a = -1, b = -1, t = -1;
    CHECK(parseContentRange("bytes 0-99/1000", &a, &b, &t) && a == 0 && b == 99 && t == 1000);
    CHECK(parseContentRange("bytes 100-199/*", &a, &b, &t) && a == 100 && t == 0);
    CHECK(!parseContentRange("bytes 5-1/10", &a, &b, &t));
    CHECK(!parseContentRange("bytes 0-99/50", &a, &b, &t));
    CHECK(!parseContentRange("items 0-1/2", &a, &b, &t));
    CHECK(!parseContentRange("bytes x-1/2", &a, &b, &t));

    CHECK(!progressPersistDue(1024, 100));
    CHECK(progressPersistDue(4 * 1024 * 1024, 100));
    CHECK(progressPersistDue(10, 2000));
}

static void location() {
    LocationInput in;
    in.defaultDirectory = "/home/u/.local/share/org/app/downloads";
    LocationDecision d = chooseLocation(in);
    CHECK(d.directory == in.defaultDirectory && !d.fellBack && d.noticeKey.empty());
    in.preferred = "/run/media/u/SD/Videos"; in.preferredUsable = true;
    d = chooseLocation(in);
    CHECK(d.directory == in.preferred && !d.fellBack);
    // Missing at download time: private dir plus a visible notice.
    in.preferredUsable = false;
    d = chooseLocation(in);
    CHECK(d.directory == in.defaultDirectory && d.fellBack && d.noticeKey == noticeKeys::folderUnavailable);

    CHECK(validateChosenFolder("/home/u/Videos", true, true).empty());
    CHECK(validateChosenFolder("/home/u/Videos", true, false) == "downloads_error_folder_readonly");
    CHECK(validateChosenFolder("/home/u/Gone", false, false) == "downloads_error_folder_missing");
    CHECK(validateChosenFolder("/tmp/x", true, true) == "downloads_error_folder_temporary");
    CHECK(validateChosenFolder("/tmp", true, true) == "downloads_error_folder_temporary");
    CHECK(validateChosenFolder("/run/user/100000/x", true, true) == "downloads_error_folder_temporary");
    CHECK(validateChosenFolder("relative", true, true) == "downloads_error_folder_invalid");
    CHECK(validateChosenFolder("/home/u/../root", true, true) == "downloads_error_folder_invalid");
    CHECK(!isTemporaryPath("/tmpfoo") && !isTemporaryPath("/run/media/u/sd"));
}

static void downloadAll() {
    // 10-item playlist, 3 already downloaded: exactly 7 queued.
    std::vector<std::string> ids;
    for (int i = 0; i < 10; ++i) ids.push_back(std::string("vid") + static_cast<char>('A' + i) + "1234567");
    std::vector<ExistingDownload> existing;
    for (int i = 0; i < 3; ++i) {
        ExistingDownload e; e.videoId = ids[i * 3]; e.kind = Kind::Video; e.state = State::Done;
        existing.push_back(e);
    }
    std::vector<std::string> first = idsToEnqueue(ids, existing, Kind::Video);
    CHECK(first.size() == 7);
    // The same ids as audio are a different kind: all 10.
    CHECK(idsToEnqueue(ids, existing, Kind::Audio).size() == 10);
    // Second run: everything is queued or done -> nothing.
    for (const std::string &id : first) {
        ExistingDownload e; e.videoId = id; e.kind = Kind::Video; e.state = State::Queued;
        existing.push_back(e);
    }
    CHECK(idsToEnqueue(ids, existing, Kind::Video).empty());
    // Failed items are retried; duplicates and invalid ids dropped.
    existing.back().state = State::Failed;
    std::vector<std::string> again = ids;
    again.push_back(ids[0]);
    again.push_back("../etc");
    const std::vector<std::string> retry = idsToEnqueue(again, existing, Kind::Video);
    CHECK(retry.size() == 1 && retry[0] == first.back());
    existing.back().state = State::Paused;
    CHECK(idsToEnqueue(again, existing, Kind::Video).empty());
}

static void progressMath() {
    DownloadRecord r;
    r.kind = Kind::Video;
    r.videoBytesTotal = 300; r.videoBytesDone = 300; r.audioBytesTotal = 100; r.audioBytesDone = 50;
    Progress p = progressOf(r);
    CHECK(p.done == 350 && p.total == 400 && p.fraction() > 0.87 && p.fraction() < 0.88);
    DownloadRecord empty;
    CHECK(progressOf(empty).fraction() == 0);
    empty.state = State::Done;
    CHECK(progressOf(empty).fraction() == 1);
}


// DL-01/DL-03: top-level box scan. Flat = one moov + one mdat, no moof, no sidx.
static std::string box(const char *type, std::size_t payload, std::uint64_t largeSize = 0) {
    std::string b;
    const std::uint64_t size = largeSize ? largeSize : 8 + payload;
    const std::uint32_t s32 = largeSize ? 1u : static_cast<std::uint32_t>(size);
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<char>((s32 >> (8 * i)) & 0xff));
    b.append(type, 4);
    if (largeSize) for (int i = 7; i >= 0; --i) b.push_back(static_cast<char>((size >> (8 * i)) & 0xff));
    b.append(largeSize ? static_cast<std::size_t>(largeSize - 16) : payload, 'x');
    return b;
}
static std::string ftyp(const char *brand) {
    std::string b;
    const std::string payload = std::string(brand, 4) + std::string("\0\0\0\0isom", 8);
    const std::uint32_t n = static_cast<std::uint32_t>(8 + payload.size());
    for (int i = 3; i >= 0; --i) b.push_back(static_cast<char>((n >> (8 * i)) & 0xff));
    return b + "ftyp" + payload;
}
static Mp4Layout scanString(const std::string &data) {
    return scanMp4([&data](std::uint64_t off, char *buf, std::size_t len) -> std::size_t {
        if (off >= data.size()) return 0;
        const std::size_t n = std::min<std::size_t>(len, data.size() - static_cast<std::size_t>(off));
        std::memcpy(buf, data.data() + off, n);
        return n;
    }, data.size());
}

static void mp4Layout() {
    // flat, faststart: ftyp moov mdat
    Mp4Layout l = scanString(ftyp("mp42") + box("moov", 100) + box("mdat", 1000));
    CHECK(l.valid && l.majorBrand == "mp42" && isFlatMp4(l) && !isFragmentedMp4(l) && l.moovBeforeMdat);
    // flat with mdat first (no faststart) is still flat, but moov is not first
    l = scanString(ftyp("isom") + box("mdat", 1000) + box("moov", 100) + box("free", 10));
    CHECK(isFlatMp4(l) && !l.moovBeforeMdat);
    // DASH: ftyp dash, moov, sidx, moof/mdat x2 -> fragmented, never flat
    l = scanString(ftyp("dash") + box("moov", 100) + box("sidx", 40) + box("moof", 50) + box("mdat", 500) + box("moof", 50) + box("mdat", 500));
    CHECK(l.valid && l.majorBrand == "dash" && l.moof == 2 && l.sidx == 1 && l.mdat == 2);
    CHECK(isFragmentedMp4(l) && !isFlatMp4(l));
    // a sidx alone already makes it fail the flat check
    l = scanString(ftyp("isom") + box("moov", 100) + box("sidx", 40) + box("mdat", 500));
    CHECK(!isFlatMp4(l) && isFragmentedMp4(l));
    // two mdat without moof: not flat
    CHECK(!isFlatMp4(scanString(ftyp("isom") + box("moov", 10) + box("mdat", 10) + box("mdat", 10))));
    // 64-bit size and size 0 (to the end of the file)
    l = scanString(ftyp("mp42") + box("moov", 10) + box("mdat", 0, 40));
    CHECK(l.valid && isFlatMp4(l) && l.boxes.back().size == 40);
    std::string toEnd = ftyp("mp42") + box("moov", 10);
    toEnd += std::string("\0\0\0\0mdat", 8) + std::string(300, 'y');
    l = scanString(toEnd);
    CHECK(l.valid && isFlatMp4(l) && l.boxes.back().size == 308);
    // truncated or not MP4 at all: invalid, so neither flat nor fragmented
    std::string cut = ftyp("mp42") + box("moov", 100) + box("mdat", 1000);
    cut.resize(cut.size() - 10);
    CHECK(!scanString(cut).valid && !isFlatMp4(scanString(cut)) && !isFragmentedMp4(scanString(cut)));
    CHECK(!scanString(std::string("\x1A\x45\xDF\xA3 webm data", 15)).valid);
    CHECK(!scanString(std::string()).valid);
    CHECK(isMp4Family("m4a") && isMp4Family("mp4") && !isMp4Family("webm") && !isMp4Family("mkv"));
}

// The "Download quality" setting (downloads/quality) decides what a download starts as.
static void downloadDefaults() {
    struct Row { int setting; int ceiling; Kind kind; int height; };
    const Row rows[] = {
        { 720, 0, Kind::Video, 720 },        // the fresh-install default
        { 1080, 0, Kind::Video, 1080 },
        { 480, 0, Kind::Video, 480 },
        { 360, 0, Kind::Video, 360 },
        { 1080, 480, Kind::Video, 1080 },    // the setting is explicit: the playback ceiling does not cap it
        { 360, 1080, Kind::Video, 360 },
        { -1, 0, Kind::Audio, 720 },         // audio only; a switch to video in the dialog starts at 720
        { -1, 1080, Kind::Audio, 1080 },     // ... or at the playback ceiling when there is one
        { 0, 0, Kind::Video, 2160 },         // no usable setting: the old rule (the playback ceiling)
        { 0, 720, Kind::Video, 720 },
        { -7, 480, Kind::Video, 480 },
    };
    for (const Row &r : rows) {
        const DownloadDefault d = downloadDefaultFor(r.setting, r.ceiling);
        CHECK(d.kind == r.kind);
        CHECK(d.qualityHeight == r.height);
        CHECK(effectiveQualityHeight(0, d.qualityHeight) == r.height);   // what enqueue(…, 0) uses
    }
}

// NFR-SEC-05: error pages are refused whatever their size; the free-space rule.
static void responseAndSpaceChecks() {
    for (const char *ok : {"", "video/mp4", "audio/mp4", "audio/webm", "video/webm; codecs=vp9",
                           "application/octet-stream", "VIDEO/MP4"})
        CHECK(mediaContentTypeAllowed(ok));
    for (const char *bad : {"text/html", "text/html; charset=utf-8", "TEXT/PLAIN", " text/xml", "application/json",
                            "application/json; charset=UTF-8", "application/xml", "application/javascript",
                            "application/xhtml+xml", "application/problem+json", "application/vnd.error+json",
                            "image/svg+xml"})
        CHECK(!mediaContentTypeAllowed(bad));
    CHECK(spaceNeeded(1000, 0) == 1000 + freeSpaceReserveBytes);
    CHECK(spaceNeeded(1000, 2000) == 3000 + freeSpaceReserveBytes);
    CHECK(spaceNeeded(-5, -5) == freeSpaceReserveBytes);
    CHECK(enoughSpace(-1, spaceNeeded(1LL << 40, 0)));   // unknown: not blocked here
    CHECK(enoughSpace(spaceNeeded(1000, 0), spaceNeeded(1000, 0)));
    CHECK(!enoughSpace(spaceNeeded(1000, 0) - 1, spaceNeeded(1000, 0)));
    CHECK(!enoughSpace(0, spaceNeeded(0, 0)));
}

int main() {
    mp4Layout();
    naming();
    audioPicks();
    videoPicks();
    transitions();
    scheduler();
    resume();
    location();
    downloadAll();
    progressMath();
    responseAndSpaceChecks();
    downloadDefaults();
    std::printf("downloads domain tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
