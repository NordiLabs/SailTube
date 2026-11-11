// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt Core checks of the shared playback interface (Shared/Playback/StreamSource.h: RES-10 cache
// key registry, RES-09 table), SailTube's ladder (SharedStreamSourceLadder: SEL-01/02/04/05) and
// the yt-dlp adapter's format mapping. No event loop. Build: tests/resolve_cache/run.sh
#include "Shared/Playback/StreamSource.h"
#include "Core/Player/Infrastructure/SharedStreamSourceLadder.h"
#include "Core/Player/Infrastructure/YtDlpStreamMapping.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

using namespace nordtile;
using playback::StreamErrorClass;
using playback::StreamFormat;
using playback::StreamRequest;
using playback::StreamSet;

static int checks = 0;
static int failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const char *const vid = "aBcDeFgHiJk";

// ---------------------------------------------------------------------------------------------
// RES-10 cache key

static StreamRequest nonDefaultRequest() {
    StreamRequest r;
    r.videoId = QString::fromLatin1(vid);
    r.purpose = playback::StreamPurpose::AudioOnly;
    r.maxHeight = 720;
    r.highFrameRate = true;
    r.network = playback::NetworkClass::Unmetered;
    r.videoCodecs = QStringList{QStringLiteral("avc1"), QStringLiteral("vp09")};
    r.audioCodecs = QStringList{QStringLiteral("opus"), QStringLiteral("mp4a")};
    r.dubLanguage = QStringLiteral("pl");
    r.hardwareDecode = true;
    r.signedIn = true;
    r.poProvider = true;
    r.clientOverride = QStringLiteral("tv");
    return r;
}

static void res10_cacheKeyCoversEveryOutputAffectingField() {
    const std::vector<playback::StreamRequestField> &fields = playback::outputAffectingFields();
    CHECK(static_cast<int>(fields.size()) == playback::streamRequestFieldCount);
    std::set<std::string> names;
    for (const playback::StreamRequestField &f : fields) names.insert(f.name);
    CHECK(names.size() == fields.size());

    // Changing any one registered field changes the key, from the default and from a
    // non-default starting point; the video id is in the key; `speculative` is not.
    StreamRequest plain;
    plain.videoId = QString::fromLatin1(vid);
    const StreamRequest bases[] = {plain, nonDefaultRequest()};
    for (const StreamRequest &base : bases) {
        const QByteArray key = playback::streamCacheKey(base);
        CHECK(key == playback::streamCacheKey(StreamRequest(base)));
        for (const playback::StreamRequestField &f : fields) {
            StreamRequest changed = base;
            f.mutate(changed);
            if (playback::streamCacheKey(changed) == key) std::printf("FAIL field %s is missing from streamCacheKey()\n", f.name);
            CHECK(playback::streamCacheKey(changed) != key);
        }
        StreamRequest other = base;
        other.videoId = QStringLiteral("zzzzzzzzzzz");
        CHECK(playback::streamCacheKey(other) != key);
        StreamRequest speculative = base;
        speculative.speculative = !speculative.speculative; // prefetch and play share the answer
        CHECK(playback::streamCacheKey(speculative) == key);
    }

    // SailTube's output-affecting settings (RES-10 list, SET-01 rows, SEL-08) each map to a field.
    const char *const settings[] = {
        "quality.ceiling", "quality.networkClass", "quality.highFrameRate", "playback.dubLanguage",
        "playback.hardwareDecode", "playback.codecPolicy", "resolver.clientOverride", "account.signedIn",
        "resolver.poProvider", "playback.listenOnly",
    };
    for (const char *s : settings) {
        bool found = false;
        for (const playback::StreamRequestField &f : fields) found = found || std::string(f.setting) == s;
        if (!found) std::printf("FAIL setting %s has no cache-key field\n", s);
        CHECK(found);
    }

    // Field boundaries cannot collide.
    StreamRequest a, b;
    a.dubLanguage = QStringLiteral("ab");
    b.dubLanguage = QStringLiteral("a"); b.clientOverride = QStringLiteral("b");
    CHECK(playback::streamCacheKey(a) != playback::streamCacheKey(b));
    StreamRequest c, d;
    c.videoCodecs = QStringList{QStringLiteral("a"), QStringLiteral("b")};
    d.videoCodecs = QStringList{QStringLiteral("ab")};
    CHECK(playback::streamCacheKey(c) != playback::streamCacheKey(d));
    StreamRequest e, f;
    e.videoCodecs = QStringList{QStringLiteral("avc1")};
    f.audioCodecs = QStringList{QStringLiteral("avc1")};
    CHECK(playback::streamCacheKey(e) != playback::streamCacheKey(f));
}

// ---------------------------------------------------------------------------------------------
// RES-09

static void res09_finalClassification() {
    using playback::isFinal;
    const StreamErrorClass finals[] = {StreamErrorClass::InvalidId, StreamErrorClass::Removed, StreamErrorClass::Terminated,
                                       StreamErrorClass::Upcoming, StreamErrorClass::GeoBlocked, StreamErrorClass::Unavailable};
    for (StreamErrorClass e : finals) { CHECK(isFinal(e, false)); CHECK(isFinal(e, true)); }
    CHECK(isFinal(StreamErrorClass::Private, false));
    CHECK(!isFinal(StreamErrorClass::Private, true));
    const StreamErrorClass transient[] = {StreamErrorClass::TryLater, StreamErrorClass::BotCheck, StreamErrorClass::AgeRestricted,
                                          StreamErrorClass::StillProcessing, StreamErrorClass::NoPlayableStream,
                                          StreamErrorClass::Network, StreamErrorClass::Timeout, StreamErrorClass::Busy,
                                          StreamErrorClass::InvalidResponse, StreamErrorClass::Failed,
                                          StreamErrorClass::SignInRequired, StreamErrorClass::Cancelled,
                                          StreamErrorClass::RuntimeUnavailable, StreamErrorClass::DeliveryChanged};
    for (StreamErrorClass e : transient) { CHECK(!isFinal(e, false)); CHECK(!isFinal(e, true)); }
    const playback::StreamError removed = playback::streamError(StreamErrorClass::Removed, false);
    CHECK(removed.final && removed.label == QLatin1String("removed") && !removed.ok());
    // Short, cleaned reasons and fixed labels for every class: no extractor prefix, no desktop advice.
    std::set<std::string> labels;
    for (int i = static_cast<int>(StreamErrorClass::Cancelled); i <= static_cast<int>(StreamErrorClass::DeliveryChanged); ++i) {
        const StreamErrorClass k = static_cast<StreamErrorClass>(i);
        const std::string r = playback::userReason(k);
        CHECK(!r.empty() && r.size() <= 300);
        CHECK(r.find("ERROR") == std::string::npos && r.find("cookies") == std::string::npos
              && r.find("[youtube]") == std::string::npos && r.find("http") == std::string::npos);
        labels.insert(playback::errorLabel(k));
    }
    CHECK(labels.size() == static_cast<std::size_t>(StreamErrorClass::DeliveryChanged)); // distinct
    CHECK(std::string(playback::userReason(StreamErrorClass::None)).empty());
    CHECK(playback::streamError(StreamErrorClass::None, false).ok());
}

// ---------------------------------------------------------------------------------------------
// Ladder and yt-dlp mapping

static video::media::MediaFormat fmt(const char *id, const char *ext, const char *v, const char *a, int h,
                                     double fps, double kbps, const std::string &url = std::string()) {
    using namespace video::media;
    MediaFormat f;
    f.id = id; f.container = ext; f.protocol = Protocol::Https;
    f.url = url.empty() ? std::string("https://rr1.example/videoplayback?itag=") + id : url;
    f.videoCodecTag = v; f.audioCodecTag = a;
    f.videoCodec = parseVideoCodec(v); f.audioCodec = parseAudioCodec(a);
    f.height = h; f.width = h * 16 / 9; f.fps = fps;
    if (f.videoCodec == VideoCodec::None) f.audioKbps = kbps; else f.totalKbps = kbps;
    return f;
}

static std::vector<std::string> ids(const QList<StreamFormat> &list) {
    std::vector<std::string> out;
    for (const StreamFormat &f : list) out.push_back(f.formatId.toStdString());
    return out;
}

static video::catalog::VideoDetails offer() {
    using namespace video::media;
    video::catalog::VideoDetails d;
    d.summary.videoId = vid;
    d.summary.liveStatus = LiveStatus::NotLive;
    d.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500),
        fmt("298", "mp4", "avc1.4D4020", "none", 720, 60, 2500),
        fmt("247", "webm", "vp09.00.31.08", "none", 720, 30, 1400),
        fmt("137", "mp4", "avc1.640028", "none", 1080, 30, 4000),
        fmt("248", "webm", "vp09.00.40.08", "none", 1080, 30, 3000),
        fmt("399", "mp4", "av01.0.08M.08", "none", 1080, 30, 2000),
        fmt("134", "mp4", "avc1.4D401E", "none", 360, 30, 600),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
        fmt("251", "webm", "none", "opus", 0, 0, 135),
        fmt("250", "webm", "none", "opus", 0, 0, 70),
        fmt("251-drc", "webm", "none", "opus", 0, 0, 135),
        fmt("251-pl", "webm", "none", "opus", 0, 0, 135),
        fmt("140-pl", "m4a", "none", "mp4a.40.2", 0, 0, 129),
        fmt("251-dup", "webm", "none", "opus", 0, 0, 134.8),
        fmt("bad", "mp4", "avc1.640028", "none", 480, 30, 900, "http://insecure.example/x"),
    };
    for (MediaFormat &f : d.formats) {
        if (f.id == "251-drc") f.dynamicRangeCompressed = true;
        if (f.id == "251-pl" || f.id == "140-pl") { f.language = "pl"; f.languagePreference = -1; }
        else if (f.audioCodec != AudioCodec::None && f.videoCodec == VideoCodec::None) { f.language = "en"; f.languagePreference = 10; }
    }
    return d;
}

static StreamRequest video720() {
    StreamRequest r;
    r.videoId = QString::fromLatin1(vid);
    r.maxHeight = 720;
    return r;
}

static void sel02_videoLadderRespectsCeilingAndCodecOrder() {
    using video::playeradapter::toStreamSet;
    StreamRequest r = video720();
    // At/below the ceiling tallest first (<= 30 fps, then codec order), then above it lowest
    // first; AV1 excluded by default (SEL-01); the http format never appears.
    CHECK((ids(toStreamSet(offer(), r).video) == std::vector<std::string>{"136", "247", "298", "134", "137", "248"}));
    r.hardwareDecode = true; // VP9 first with verified hardware decode (SEL-02)
    CHECK((ids(toStreamSet(offer(), r).video) == std::vector<std::string>{"247", "136", "298", "134", "248", "137"}));
    r.hardwareDecode = false;
    r.highFrameRate = true; // the 60 fps rung first at its height only when explicitly wanted
    CHECK((ids(toStreamSet(offer(), r).video) == std::vector<std::string>{"298", "136", "247", "134", "137", "248"}));
    StreamRequest av1;
    av1.maxHeight = 0;
    av1.videoCodecs = QStringList{QStringLiteral("av01"), QStringLiteral("avc1")};
    CHECK((ids(toStreamSet(offer(), av1).video) == std::vector<std::string>{"399", "137", "136", "298", "134"}));
    StreamRequest audioOnly; // listen-only answer (SEL-08)
    audioOnly.purpose = playback::StreamPurpose::AudioOnly;
    const StreamSet a = toStreamSet(offer(), audioOnly);
    CHECK(a.video.isEmpty() && !a.audio.isEmpty() && a.muxed.size() == 1);
    CHECK(a.path == playback::ResolvePath::Extractor && a.videoId == QLatin1String(vid));
}

// SEL-04 (four keys in order) and SEL-05 (dub: exact BCP-47, then base language).
static void sel04_audioLadderRankedAndDeduplicated() {
    using video::playeradapter::toStreamSet;
    StreamRequest r;
    // Language, Opus before AAC, non-DRC before DRC, bitrate; 251-dup (same family, language,
    // DRC and rounded kbps as 251) is removed.
    CHECK((ids(toStreamSet(offer(), r).audio) == std::vector<std::string>{"251", "250", "251-drc", "140", "251-pl", "140-pl"}));
    r.dubLanguage = QStringLiteral("pl");
    CHECK((ids(toStreamSet(offer(), r).audio) == std::vector<std::string>{"251-pl", "140-pl", "251", "250", "251-drc", "140"}));
    r.dubLanguage = QStringLiteral("pl-PL"); // base-language match
    CHECK(ids(toStreamSet(offer(), r).audio).front() == "251-pl");
    StreamRequest aac;
    aac.audioCodecs = QStringList{QStringLiteral("mp4a")};
    CHECK((ids(toStreamSet(offer(), aac).audio) == std::vector<std::string>{"140", "140-pl"}));
    // Descriptive tracks come last whatever their language.
    QList<StreamFormat> list;
    StreamFormat desc, plain;
    desc.formatId = QStringLiteral("d"); desc.audioCodec = QStringLiteral("opus"); desc.videoCodec = QStringLiteral("none");
    desc.descriptive = true; desc.languagePreference = 10; desc.bitrateKbps = 200;
    plain = desc; plain.formatId = QStringLiteral("p"); plain.descriptive = false; plain.languagePreference = -1; plain.bitrateKbps = 50;
    list << desc << plain;
    player::rankAudio(&list, StreamRequest());
    CHECK((ids(list) == std::vector<std::string>{"p", "d"}));
}

static void res22_nonHttpsFormatsDropped() {
    using video::playeradapter::toStreamSet;
    StreamRequest r;
    r.maxHeight = 0;
    const StreamSet s = toStreamSet(offer(), r);
    for (const QList<StreamFormat> *list : {&s.video, &s.audio, &s.muxed})
        for (const StreamFormat &f : *list) { CHECK(f.formatId != QLatin1String("bad")); CHECK(f.url.scheme() == QLatin1String("https")); }
}

static void res10_expireAndClenFromUrl() {
    using player::urlQueryInteger;
    using player::urlExpireUnixSeconds;
    const QByteArray u = "https://rr1.example/videoplayback?expire=1790000000&clen=123456&itag=251#x";
    CHECK(urlQueryInteger(u, "expire") == 1790000000);
    CHECK(urlQueryInteger(u, "clen") == 123456);
    CHECK(urlQueryInteger(u, "lmt") == 0);
    CHECK(urlQueryInteger("https://h/v?expire=1&expire=2", "expire") == 0); // not exactly one
    CHECK(urlQueryInteger("https://h/v?expire=12a", "expire") == 0);
    CHECK(urlQueryInteger("https://h/v?xexpire=5", "expire") == 0);
    CHECK(urlQueryInteger("https://h/v", "expire") == 0);
    CHECK(urlExpireUnixSeconds("https://manifest.example/api/manifest/hls_variant/expire/1790000123/ei/x/file/index.m3u8") == 1790000123);
    CHECK(urlExpireUnixSeconds(u) == 1790000000);

    video::catalog::VideoDetails d = offer();
    d.formats[1].url = "https://rr1.example/videoplayback?itag=136&expire=1790000500&clen=999";
    d.formats[9].url = "https://rr1.example/videoplayback?itag=251&expire=1790000400";
    StreamRequest r = video720();
    const StreamSet s = video::playeradapter::toStreamSet(d, r);
    CHECK(s.expiresAtUnixMs == 1790000400LL * 1000);
    const StreamFormat f136 = video::playeradapter::toStreamFormat(d.formats[1]);
    CHECK(f136.contentLength == 999 && f136.expiresAtUnixMs == 1790000500LL * 1000 && !f136.premium);
    CHECK(f136.hasVideo() && !f136.hasAudio() && f136.audioCodec == QLatin1String("none"));
    CHECK(video::playeradapter::toStreamFormat(d.formats[2]).contentLength == -1); // unknown
    d.formats[2].premium = true;
    CHECK(video::playeradapter::toStreamFormat(d.formats[2]).premium); // core's flag passes through
}

static void res19_liveAnswerUsesHlsAndIsNotCached() {
    using namespace video::media;
    video::catalog::VideoDetails d;
    d.summary.videoId = vid;
    d.summary.liveStatus = LiveStatus::IsLive;
    d.manifestUrl = "https://manifest.example/api/manifest/hls_variant/expire/1790009999/index.m3u8";
    MediaFormat f94 = fmt("94", "mp4", "avc1.4d401e", "mp4a.40.2", 480, 30, 1200, "https://manifest.example/94.m3u8");
    MediaFormat f95 = fmt("95", "mp4", "avc1.4d401f", "mp4a.40.2", 720, 30, 2500, "https://manifest.example/95.m3u8");
    f94.protocol = f95.protocol = Protocol::Hls;
    d.formats = {f94, f95};
    StreamRequest r = video720();
    const StreamSet s = video::playeradapter::toStreamSet(d, r);
    CHECK(s.live && s.liveState == playback::LiveState::Live && s.hlsManifest.toEncoded() == QByteArray(d.manifestUrl.c_str()));
    CHECK((ids(s.muxed) == std::vector<std::string>{"95", "94"}));
    CHECK(s.muxed[0].isHls && s.video.isEmpty() && s.audio.isEmpty());
    CHECK(player::playable(s, r));
    CHECK(s.expiresAtUnixMs == 1790009999LL * 1000);

    // Post-live with segment-only formats: nothing playable (the adapter reports StillProcessing).
    video::catalog::VideoDetails post;
    post.summary.videoId = vid;
    post.summary.liveStatus = LiveStatus::PostLive;
    MediaFormat seg = fmt("299", "mp4", "avc1.64002a", "none", 1080, 60, 5000);
    seg.protocol = Protocol::Dash;
    post.formats = {seg};
    const StreamSet p = video::playeradapter::toStreamSet(post, r);
    CHECK(p.liveState == playback::LiveState::PostLive && !p.live && !player::playable(p, r));
}

static void playableNeedsVideoOnlyWhenWanted() {
    StreamSet s;
    StreamFormat a;
    a.formatId = QStringLiteral("251"); a.audioCodec = QStringLiteral("opus"); a.videoCodec = QStringLiteral("none");
    s.audio << a;
    StreamRequest audioOnly, video;
    audioOnly.purpose = playback::StreamPurpose::AudioOnly;
    CHECK(player::playable(s, audioOnly) && player::hasSplitPair(s, audioOnly));
    CHECK(!player::playable(s, video) && !player::hasSplitPair(s, video));
    StreamFormat m = a;
    m.videoCodec = QStringLiteral("avc1.42001E");
    s.muxed << m;
    CHECK(player::playable(s, video) && !player::hasSplitPair(s, video));
    CHECK(!player::playable(StreamSet(), audioOnly));
}

static void res17_directHandOff() {
    StreamFormat f;
    f.formatId = QStringLiteral("137");
    f.url = QUrl(QStringLiteral("https://rr1.example/videoplayback?itag=137"));
    playback::StreamError e = playback::streamError(StreamErrorClass::Failed, false);
    CHECK(video::playeradapter::directUrl(f, &e) == f.url && e.ok());
    f.url = QUrl(QStringLiteral("http://rr1.example/videoplayback"));
    CHECK(video::playeradapter::directUrl(f, &e).isEmpty() && e.kind == StreamErrorClass::InvalidResponse);
}

static void codecFamilies() {
    CHECK(player::codecFamily(QStringLiteral("avc1.640028")) == QLatin1String("h264"));
    CHECK(player::codecFamily(QStringLiteral("vp09.02.51.10")) == QLatin1String("vp9"));
    CHECK(player::codecFamily(QStringLiteral("av01.0.08M.08")) == QLatin1String("av1"));
    CHECK(player::codecFamily(QStringLiteral("mp4a.40.2")) == QLatin1String("aac"));
    CHECK(player::codecFamily(QStringLiteral("opus")) == QLatin1String("opus"));
    CHECK(player::codecFamily(QStringLiteral("none")) == QLatin1String("none"));
    CHECK(player::codecFamily(QStringLiteral("xyz")).isEmpty());
    StreamRequest r;
    CHECK((player::videoCodecOrder(r) == QStringList{QStringLiteral("h264"), QStringLiteral("vp9")}));
    CHECK((player::audioCodecOrder(r) == QStringList{QStringLiteral("opus"), QStringLiteral("aac")}));
}

int main() {
    res10_cacheKeyCoversEveryOutputAffectingField();
    res09_finalClassification();
    sel02_videoLadderRespectsCeilingAndCodecOrder();
    sel04_audioLadderRankedAndDeduplicated();
    res22_nonHttpsFormatsDropped();
    res10_expireAndClenFromUrl();
    res19_liveAnswerUsesHlsAndIsNotCached();
    playableNeedsVideoOnlyWhenWanted();
    res17_directHandOff();
    codecFamilies();
    std::printf("resolve cache mapping tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
