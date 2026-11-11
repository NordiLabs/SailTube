// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-06: Qt Core side of the player API that had no test yet: the
// audio resource gate in its host build (no policy daemon), the
// SharedStreamSource mapping helpers and SailTube's ladder over fixture
// formats of SailTune's component types, and the timing path label.
// Build: tests/player_api_qt/run.sh
#include "Core/Player/Infrastructure/AudioResourceGate.h"
#include "Core/Player/Infrastructure/SharedStreamSourceLadder.h"
#include "Core/Player/Infrastructure/SharedStreamSourceSailTune.h"
#include "Core/Player/Infrastructure/SharedStreamSourceThreaded.h"

#include <QCoreApplication>

#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile;
using namespace nordtile::player;
namespace sp = nordtile::music::sharedplayback::domain;
using playback::StreamFormat;
using playback::StreamRequest;
using playback::StreamSet;

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// ---- PLY-23 / PLT-12: audio resource gate (host build) ---------------------------------------

// Built with NORDTILE_NO_AUDIORESOURCE every request is granted at once; acquired()
// follows request()/release() and acquiredChanged fires only on a change.
static void ply23_audioResourceAcquired() {
    AudioResourceGate gate;
    int changed = 0, granted = 0, lost = 0;
    QObject::connect(&gate, &AudioResourceGate::acquiredChanged, [&]() { ++changed; });
    QObject::connect(&gate, &AudioResourceGate::granted, [&]() { ++granted; });
    QObject::connect(&gate, &AudioResourceGate::lost, [&]() { ++lost; });
    CHECK(!gate.acquired());
    CHECK(!gate.property("acquired").toBool());
    gate.request();
    CHECK(gate.acquired());
    CHECK(gate.property("acquired").toBool());
    CHECK(changed == 1 && granted == 1);
    gate.request();                          // already held: granted again, no change
    CHECK(gate.acquired() && changed == 1 && granted == 2);
    gate.release();
    CHECK(!gate.acquired() && changed == 2);
    gate.release();                          // nothing held: no change
    CHECK(!gate.acquired() && changed == 2);
    CHECK(lost == 0);                        // nothing takes it away on the host
}

// ---- RES-16..19: SharedStreamSource helpers ------------------------------------------------------

static StreamFormat format(const char *id, const char *container, const char *v, const char *a, int height = 0,
                           double kbps = 1000) {
    StreamFormat f;
    f.formatId = QString::fromLatin1(id);
    f.container = QString::fromLatin1(container);
    f.videoCodec = QString::fromLatin1(v);
    f.audioCodec = QString::fromLatin1(a);
    f.height = height;
    f.width = height * 16 / 9;
    f.fps = height ? 30 : 0;
    f.bitrateKbps = kbps;
    f.url = QUrl(QStringLiteral("https://rr1---sn-x.googlevideo.com/videoplayback?itag=%1").arg(QLatin1String(id)));
    return f;
}

static QStringList ids(const QList<StreamFormat> &l) {
    QStringList out;
    for (const StreamFormat &f : l) out << f.formatId;
    return out;
}

// RES-17: one MIME type per (kind, container family) on both resolve paths.
static void res17_mimeTypeOf() {
    CHECK(sharedsource::mimeTypeOf(format("248", "webm", "vp09.00.40.08", "none", 1080)) == QLatin1String("video/webm"));
    CHECK(sharedsource::mimeTypeOf(format("137", "mp4", "avc1.640028", "none", 1080)) == QLatin1String("video/mp4"));
    CHECK(sharedsource::mimeTypeOf(format("251", "webm", "none", "opus")) == QLatin1String("audio/webm"));
    CHECK(sharedsource::mimeTypeOf(format("140", "m4a", "none", "mp4a.40.2")) == QLatin1String("audio/mp4"));
    CHECK(sharedsource::mimeTypeOf(format("251", "WEBM", "none", "opus")) == QLatin1String("audio/webm"));  // case-insensitive
    CHECK(sharedsource::mimeTypeOf(format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360)) == QLatin1String("video/mp4"));
    CHECK(sharedsource::mimeTypeOf(format("x", "", "none", "opus")) == QLatin1String("audio/mp4"));
}

// RES-16: header lookup by case-insensitive name; absent = empty.
static void res16_headerValue() {
    StreamFormat f = format("251", "webm", "none", "opus");
    CHECK(sharedsource::headerValue(f, "User-Agent").isEmpty());
    f.headers << qMakePair(QByteArray("User-Agent"), QByteArray("UA/1.0"))
              << qMakePair(QByteArray("referer"), QByteArray("https://www.youtube.com/"));
    CHECK(sharedsource::headerValue(f, "User-Agent") == "UA/1.0");
    CHECK(sharedsource::headerValue(f, "user-agent") == "UA/1.0");
    CHECK(sharedsource::headerValue(f, "Referer") == "https://www.youtube.com/");
    CHECK(sharedsource::headerValue(f, "Origin").isEmpty());
}

// SEL-04 / RES-08: the component gets the request's audio families in order, or
// nothing (its own default: Opus, then AAC).
static void sel04_componentAudioCodecs() {
    StreamRequest r;
    CHECK(sharedsource::componentAudioCodecs(r).empty());
    r.audioCodecs = QStringList{QStringLiteral("mp4a.40.2"), QStringLiteral("opus"), QStringLiteral("mp4a"),
                                QStringLiteral("flac")};
    const std::vector<std::string> c = sharedsource::componentAudioCodecs(r);
    CHECK(c.size() == 2);
    CHECK(c.size() == 2 && c[0] == "aac" && c[1] == "opus");  // families, deduplicated, unknown dropped
    r.audioCodecs = QStringList{QStringLiteral("opus")};
    const std::vector<std::string> o = sharedsource::componentAudioCodecs(r);
    CHECK(o.size() == 1 && o[0] == "opus");
}

// RES-19 / RES-17: anything not proxied is played from its direct https URL; other schemes are an
// invalid response, never a plain-http fetch.
static void res17_directRemoteUrl() {
    playback::StreamError e = playback::streamError(playback::StreamErrorClass::Network, false);
    StreamFormat f = format("251", "webm", "none", "opus");
    const QUrl u = sharedsource::directRemoteUrl(f, &e);
    CHECK(u == f.url && e.ok());
    f.url = QUrl(QStringLiteral("http://rr1---sn-x.googlevideo.com/videoplayback"));
    CHECK(sharedsource::directRemoteUrl(f, &e).isEmpty());
    CHECK(e.kind == playback::StreamErrorClass::InvalidResponse && e.label == QLatin1String("invalid_response"));
    f.url = QUrl();
    CHECK(sharedsource::directRemoteUrl(f, &e).isEmpty() && !e.ok());
    f.url = QUrl(QStringLiteral("file:///etc/passwd"));
    CHECK(sharedsource::directRemoteUrl(f, nullptr).isEmpty());  // no error out-parameter: still refused
}

// ---- SEL-01/02/04: SailTube's ladder ------------------------------------------------------------

// SEL-02: at/below the ceiling tallest first, above it lowest first; non-Premium, <= 30 fps,
// codec order, SDR, bitrate; only allowed video-only non-HLS formats survive.
static void sel02_rankVideo() {
    StreamRequest r;
    r.maxHeight = 720;
    QList<StreamFormat> l;
    l << format("137", "mp4", "avc1.640028", "none", 1080, 3000) << format("248", "webm", "vp09.00.40.08", "none", 1080, 2600)
      << format("136", "mp4", "avc1.4D401F", "none", 720, 1500) << format("247", "webm", "vp09.00.31.08", "none", 720, 1300)
      << format("134", "mp4", "avc1.4D401E", "none", 360, 600) << format("399", "mp4", "av01.0.08M.08", "none", 1080, 2000)
      << format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 500) << format("140", "m4a", "none", "mp4a.40.2", 0, 129);
    StreamFormat hls = format("95", "mp4", "avc1.4D401F", "none", 720);
    hls.isHls = true;
    l << hls;
    StreamFormat premium = format("356", "webm", "vp09.00.31.08", "none", 720, 4000);
    premium.premium = true;
    l << premium;
    StreamFormat hfr = format("298", "mp4", "avc1.4D4020", "none", 720, 2500);
    hfr.fps = 60;
    l << hfr;
    QList<StreamFormat> ranked = l;
    rankVideo(&ranked, r);
    CHECK(ids(ranked) == (QStringList{"136", "247", "298", "356", "134", "137", "248"}));
    // Explicitly wanted high frame rate puts the 60 fps rung first at its height.
    r.highFrameRate = true;
    ranked = l;
    rankVideo(&ranked, r);
    CHECK(!ranked.isEmpty() && ranked.first().formatId == QLatin1String("298"));
    // Verified hardware decode: VP9 before H.264.
    r.highFrameRate = false;
    r.hardwareDecode = true;
    ranked = l;
    rankVideo(&ranked, r);
    CHECK(ranked.size() >= 2 && ranked[0].formatId == QLatin1String("247") && ranked[1].formatId == QLatin1String("136"));
    // AV1 only when listed.
    r.hardwareDecode = false;
    r.videoCodecs = QStringList{QStringLiteral("av01"), QStringLiteral("avc1")};
    r.maxHeight = 1080;
    ranked = l;
    rankVideo(&ranked, r);
    CHECK(!ranked.isEmpty() && ranked.first().formatId == QLatin1String("399"));
    CHECK(!ids(ranked).contains(QStringLiteral("248")));  // VP9 not listed
    // An audio-only request keeps no video.
    StreamRequest audio;
    audio.purpose = playback::StreamPurpose::AudioOnly;
    ranked = l;
    rankVideo(&ranked, audio);
    CHECK(ranked.isEmpty());
}

// SEL-04: keeps the allowed audio families in their given order (the producer ranked them).
static void sel04_keepAllowedAudio() {
    QList<StreamFormat> l;
    l << format("140", "m4a", "none", "mp4a.40.2", 0, 129) << format("251", "webm", "none", "opus", 0, 135)
      << format("171", "webm", "none", "vorbis", 0, 128) << format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360)
      << format("137", "mp4", "avc1.640028", "none", 1080) << format("ac3", "mp4", "none", "ac-3", 0, 384);
    StreamFormat hls = format("233", "mp4", "none", "mp4a.40.5", 0, 48);
    hls.isHls = true;
    l << hls;
    QList<StreamFormat> kept = l;
    keepAllowedAudio(&kept, StreamRequest());
    CHECK(ids(kept) == (QStringList{"140", "251"}));  // order kept: not re-ranked
    StreamRequest aacOnly;
    aacOnly.audioCodecs = QStringList{QStringLiteral("mp4a")};
    kept = l;
    keepAllowedAudio(&kept, aacOnly);
    CHECK(ids(kept) == QStringList{"140"});
}

// INSTANT §4 muxed last resort: inside the ceiling first, HLS before progressive, tallest first.
static void sel02_rankMuxed() {
    StreamRequest r;
    r.maxHeight = 480;
    QList<StreamFormat> l;
    l << format("22", "mp4", "avc1.64001F", "mp4a.40.2", 720) << format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360)
      << format("17", "3gp", "mp4v.20.3", "mp4a.40.2", 144) << format("43", "webm", "vp8.0", "vorbis", 360)
      << format("137", "mp4", "avc1.640028", "none", 1080) << format("140", "m4a", "none", "mp4a.40.2");
    StreamFormat hls = format("93", "mp4", "avc1.4D401E", "mp4a.40.2", 360);
    hls.isHls = true;
    StreamFormat hlsLow = format("91", "mp4", "avc1.42C00B", "mp4a.40.5", 144);
    hlsLow.isHls = true;
    l << hls << hlsLow;
    rankMuxed(&l, r);
    CHECK(ids(l) == (QStringList{"93", "91", "18", "22"}));
    // No ceiling: HLS first, then progressive, each tallest first.
    QList<StreamFormat> m;
    m << format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360) << format("22", "mp4", "avc1.64001F", "mp4a.40.2", 720);
    StreamRequest open;
    open.maxHeight = 0;
    rankMuxed(&m, open);
    CHECK(ids(m) == (QStringList{"22", "18"}));
}

// RES-10 / RES-12: the answer expires with its earliest URL (or the HLS manifest's expire).
static void res12_earliestExpiryMs() {
    StreamSet s;
    CHECK(earliestExpiryMs(s) == 0);
    StreamFormat v = format("136", "mp4", "avc1.4D401F", "none", 720);
    v.expiresAtUnixMs = 1700000900000LL;
    StreamFormat a = format("251", "webm", "none", "opus");
    a.expiresAtUnixMs = 1700000500000LL;
    StreamFormat m = format("18", "mp4", "avc1.42001E", "mp4a.40.2", 360);  // unknown (0): ignored
    s.video << v;
    s.audio << a;
    s.muxed << m;
    CHECK(earliestExpiryMs(s) == 1700000500000LL);
    s.hlsManifest = QUrl(QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_variant/expire/1700000100/id/x"));
    CHECK(earliestExpiryMs(s) == 1700000100000LL);
    s.hlsManifest = QUrl(QStringLiteral("https://manifest.googlevideo.com/api/manifest/hls_variant/expire/1800000000/id/x"));
    CHECK(earliestExpiryMs(s) == 1700000500000LL);
}

static sp::StreamFormat componentFormat(int itag, const char *mime, const char *codec, bool video, bool audio, int height,
                                        int bitrate) {
    sp::StreamFormat f;
    f.itag = itag;
    f.mimeType = mime;
    f.codec = codec;
    f.hasVideo = video;
    f.hasAudio = audio;
    f.height = height;
    f.width = height * 16 / 9;
    f.fps = video ? 30 : 0;
    f.bitrate = bitrate;
    f.url = "https://rr1---sn-x.googlevideo.com/videoplayback?itag=" + std::to_string(itag);
    return f;
}

// RES-03 / RES-08: the component's answer as a StreamSet, every field mapped and re-ranked by
// SailTube's ladder for the request.
static void res08_mapSetFromComponentAnswer() {
    sp::StreamSet set;
    set.videoId = "AAAAAAAAAAA";
    set.userAgent = "Component-UA/2.0";
    set.video.push_back(componentFormat(248, "video/webm", "vp9", true, false, 1080, 2600000));
    set.video.push_back(componentFormat(136, "video/mp4", "avc1.4D401F", true, false, 720, 1500000));
    set.video.push_back(componentFormat(399, "video/mp4", "av01.0.08M.08", true, false, 1080, 2000000));
    set.video.push_back(componentFormat(247, "video/webm", "vp9", true, false, 720, 1300000));
    sp::StreamFormat opus = componentFormat(251, "audio/webm", "opus", false, true, 0, 135000);
    opus.audioLanguage = "en";
    opus.contentLength = 3456789;
    opus.expiresAtUnixMs = 1700000600000LL;
    sp::StreamFormat dub = componentFormat(140, "audio/mp4", "mp4a.40.2", false, true, 0, 129000);
    dub.audioLanguage = "de";
    dub.isDefaultAudio = false;
    dub.isDrc = true;
    sp::StreamFormat described = componentFormat(139, "audio/mp4", "mp4a.40.5", false, true, 0, 48000);
    described.audioLanguage = "en";
    described.isDescriptive = true;
    sp::StreamFormat ac3 = componentFormat(380, "audio/mp4", "ac-3", false, true, 0, 384000);
    set.audio.push_back(opus);
    set.audio.push_back(dub);
    set.audio.push_back(described);
    set.audio.push_back(ac3);
    set.muxed.push_back(componentFormat(18, "video/mp4", "avc1.42001E", true, true, 360, 500000));
    set.hasLoudness = true;
    set.loudnessDb = 3.5;
    set.path = sp::ResolvePath::Fast;
    set.timings.totalMs = 420;
    set.timings.visitorMs = 80;
    set.timings.playerMs = 300;
    set.timings.probeMs = 40;

    StreamRequest r;
    r.videoId = QStringLiteral("AAAAAAAAAAA");
    r.maxHeight = 720;
    const StreamSet s = SailTuneStreamSource::mapSet(set, r);
    CHECK(s.videoId == QLatin1String("AAAAAAAAAAA"));
    CHECK(ids(s.video) == (QStringList{"136", "247", "248"}));  // SEL-02 ladder; AV1 not allowed by default
    CHECK(ids(s.audio) == (QStringList{"251", "140", "139"}));  // component order kept; AC-3 dropped
    CHECK(ids(s.muxed) == QStringList{"18"});
    if (s.video.size() == 3 && s.audio.size() == 3 && s.muxed.size() == 1) {
        const StreamFormat &v = s.video[0];
        CHECK(v.container == QLatin1String("mp4") && v.videoCodec == QLatin1String("avc1.4D401F"));
        CHECK(v.audioCodec == QLatin1String("none") && v.hasVideo() && !v.hasAudio());
        CHECK(v.height == 720 && v.width == 1280 && v.fps == 30 && v.bitrateKbps == 1500);
        CHECK(v.contentLength == -1);                                     // unknown
        CHECK(sharedsource::headerValue(v, "User-Agent") == "Component-UA/2.0");
        CHECK(v.url.toString() == QLatin1String("https://rr1---sn-x.googlevideo.com/videoplayback?itag=136"));
        CHECK(s.video[1].container == QLatin1String("webm"));
        const StreamFormat &a = s.audio[0];
        CHECK(a.audioCodec == QLatin1String("opus") && a.videoCodec == QLatin1String("none"));
        CHECK(a.language == QLatin1String("en") && a.originalAudio && a.languagePreference == 10 && !a.descriptive);
        CHECK(a.contentLength == 3456789 && a.expiresAtUnixMs == 1700000600000LL);
        CHECK(sharedsource::mimeTypeOf(a) == QLatin1String("audio/webm"));
        const StreamFormat &d = s.audio[1];
        CHECK(!d.originalAudio && d.languagePreference == 0 && d.dynamicRangeCompressed);
        const StreamFormat &x = s.audio[2];
        CHECK(x.descriptive && !x.originalAudio && x.languagePreference == -1);
        const StreamFormat &m = s.muxed[0];
        CHECK(m.videoCodec == QLatin1String("avc1.42001E") && m.audioCodec == QLatin1String("mp4a.40.2"));  // mp4 progressive = AAC-LC
    }
    CHECK(!s.live && s.liveState == playback::LiveState::NotLive && s.hlsManifest.isEmpty());
    CHECK(s.expiresAtUnixMs == 1700000600000LL);                          // the earliest kept URL
    CHECK(s.hasLoudness && s.loudnessDb == 3.5);
    CHECK(s.path == playback::ResolvePath::FastPath);
    CHECK(s.resolveMs == 420 && s.phases.visitorMs == 80 && s.phases.requestMs == 300 && s.phases.probeMs == 40);

    // An audio-only request keeps no video; a live answer carries its manifest; a stated expiry wins.
    sp::StreamSet live = set;
    live.metadata.isLive = true;
    live.hlsUrl = "https://manifest.googlevideo.com/api/manifest/hls_variant/expire/1700000100/id/x";
    live.expiresAtUnixMs = 1700000200000LL;
    live.path = sp::ResolvePath::Cache;
    StreamRequest audioOnly = r;
    audioOnly.purpose = playback::StreamPurpose::AudioOnly;
    const StreamSet l = SailTuneStreamSource::mapSet(live, audioOnly);
    CHECK(l.video.isEmpty() && !l.audio.isEmpty());
    CHECK(l.live && l.liveState == playback::LiveState::Live);
    CHECK(l.hlsManifest.toString() == QString::fromStdString(live.hlsUrl));
    CHECK(l.expiresAtUnixMs == 1700000200000LL);
    CHECK(l.path == playback::ResolvePath::Cache);
    sp::StreamSet extractor = set;
    extractor.path = sp::ResolvePath::YtDlp;
    CHECK(SailTuneStreamSource::mapSet(extractor, r).path == playback::ResolvePath::Extractor);
    extractor.path = sp::ResolvePath::None;
    CHECK(SailTuneStreamSource::mapSet(extractor, r).path == playback::ResolvePath::Unknown);
}

// NFR-PERF timing markers: the resolve path label of perf_timing.md.
static void nfrPerf_timingPathLabel() {
    CHECK(std::string(timingPathLabel(playback::ResolvePath::FastPath)) == "fast");
    CHECK(std::string(timingPathLabel(playback::ResolvePath::Cache)) == "cache");
    CHECK(std::string(timingPathLabel(playback::ResolvePath::Extractor)) == "extractor");
    CHECK(std::string(timingPathLabel(playback::ResolvePath::ExtractorWithJavaScript)) == "extractor");
    CHECK(std::string(timingPathLabel(playback::ResolvePath::Unknown)) == "extractor");
    CHECK(resolveDoneMarker(1234, 2, playback::ResolvePath::FastPath).endsWith(QLatin1String("path=fast")));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    std::printf("ply23_audioResourceAcquired\n");
    ply23_audioResourceAcquired();
    std::printf("res17_mimeTypeOf\n");
    res17_mimeTypeOf();
    std::printf("res16_headerValue\n");
    res16_headerValue();
    std::printf("sel04_componentAudioCodecs\n");
    sel04_componentAudioCodecs();
    std::printf("res17_directRemoteUrl\n");
    res17_directRemoteUrl();
    std::printf("sel02_rankVideo\n");
    sel02_rankVideo();
    std::printf("sel04_keepAllowedAudio\n");
    sel04_keepAllowedAudio();
    std::printf("sel02_rankMuxed\n");
    sel02_rankMuxed();
    std::printf("res12_earliestExpiryMs\n");
    res12_earliestExpiryMs();
    std::printf("res08_mapSetFromComponentAnswer\n");
    res08_mapSetFromComponentAnswer();
    std::printf("nfrPerf_timingPathLabel\n");
    nfrPerf_timingPathLabel();
    std::printf("player api core tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
