// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// QtCore checks of the yt-dlp JSON parsers, argument construction and the pure
// part of the GStreamer probe. Build: tests/resolver_qt/run.sh (needs Qt5Core).
#include "Core/Platform/Infrastructure/GstCapabilityProbe.h"
#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"
#include "Core/Media/Domain/StreamSelection.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>
#include <vector>

using namespace nordtile::video;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QByteArray fixture(const char *name) {
    QFile f(QStringLiteral(SAILTUBE_FIXTURES "/") + QString::fromLatin1(name));
    if (!f.open(QIODevice::ReadOnly)) { std::printf("missing fixture %s\n", name); ++failures; return QByteArray(); }
    return f.readAll();
}

static void listing() {
    const auto items = resolver::parseListing(fixture("listing.jsonl"), 20);
    CHECK(items.size() == 2);
    CHECK(items[0].videoId == "aBcDeFgHiJk");
    CHECK(items[0].thumbnailUrl == "https://img.example/1/b.jpg");
    CHECK(items[0].durationSeconds == 61);
    CHECK(items[0].viewCount == 1500);
    CHECK(items[0].channelId == "UC0123456789abcdefABCDEF");
    CHECK(items[1].channel == "Two");
    CHECK(items[1].liveStatus == media::LiveStatus::IsLive);
    CHECK(items[1].viewCount == -1);
    CHECK(resolver::parseListing(fixture("listing.jsonl"), 1).size() == 1);
}

static void details() {
    catalog::VideoDetails d;
    CHECK(resolver::parseDetails(fixture("details_vod.json"), "aBcDeFgHiJk", &d) == resolver::ErrorCode::None);
    CHECK(d.summary.title == "Sailing the archipelago");
    CHECK(d.summary.durationSeconds == 212);
    CHECK(d.likeCount == 4321);
    CHECK(d.channelFollowerCount == 12345);
    CHECK(d.uploadDate == "20260917");
    CHECK(d.chapters.size() == 2);
    CHECK(d.tags.size() == 2);
    CHECK(d.summary.liveStatus == media::LiveStatus::NotLive);
    // nourl, plain http, the storyboard, a look-alike host and a wrong path are dropped.
    CHECK(d.formats.size() == 8);
    const media::MediaFormat *f137 = nullptr, *f140 = nullptr, *drc = nullptr, *hdr = nullptr, *hls = nullptr;
    for (const media::MediaFormat &f : d.formats) {
        if (f.id == "137") f137 = &f;
        if (f.id == "140") f140 = &f;
        if (f.id == "140-drc") drc = &f;
        if (f.id == "337") hdr = &f;
        if (f.id == "96") hls = &f;
    }
    CHECK(f137 && f137->videoCodec == media::VideoCodec::H264 && f137->audioCodec == media::AudioCodec::None);
    CHECK(f137 && f137->height == 1080 && f137->availableAtUnixSeconds == 1790000000);
    CHECK(f137 && f137->headers.count("User-Agent") == 1); // inherited top-level headers
    CHECK(f140 && f140->languagePreference == 10 && f140->audioSampleRate == 44100);
    CHECK(drc && drc->dynamicRangeCompressed);
    CHECK(hdr && hdr->highDynamicRange);
    CHECK(hls && hls->protocol == media::Protocol::Hls);
    // Wrong identity is rejected.
    CHECK(resolver::parseDetails(fixture("details_vod.json"), "zzzzzzzzzzz", &d) == resolver::ErrorCode::InvalidResponse);
    CHECK(resolver::parseDetails("not json", "aBcDeFgHiJk", &d) == resolver::ErrorCode::InvalidResponse);
}

static void selectionOnFixture() {
    catalog::VideoDetails d;
    resolver::parseDetails(fixture("details_vod.json"), "aBcDeFgHiJk", &d);
    media::SelectionInput in;
    in.formats = d.formats;
    in.liveStatus = d.summary.liveStatus;
    const media::DecoderCapabilities caps = platform::capabilitiesFrom(
        QStringList() << "droidvdec" << "qtdemux" << "matroskademux" << "hlsdemux" << "opusdec" << "avdec_aac" << "vp9dec" << "avdec_h264",
        QStringLiteral("video/x-h264, stream-format=(string)byte-stream; video/x-vp9"), QString(), true);
    const auto chain = media::selectStreams(in, caps, media::PlaybackPreferences());
    // Playback component (SEL-01/02/04): software decode by default (avdec_h264 here);
    // the only SDR rung is 1080p (above the 720p ceiling, so the lowest rung is
    // taken); audio ranks language first (140 is the marked original,
    // 251 carries no language), so AAC 140 beats Opus here (SEL-04); muxed 18 as the last resort; 2160p HDR VP9 is out.
    CHECK(!chain.empty() && chain[0].kind == media::StreamKind::Adaptive);
    CHECK(!chain.empty() && chain[0].video.id == "137" && chain[0].audio.id == "140");
    CHECK(!chain.empty() && chain.back().kind == media::StreamKind::Progressive && chain.back().muxed.id == "18");
    // Without a software H.264 decoder (droidvdec only) nothing here is playable
    // while hardware decode is off (PLY-21, PLT-08).
    const media::DecoderCapabilities droidOnly = platform::capabilitiesFrom(
        QStringList() << "droidvdec" << "qtdemux" << "matroskademux" << "opusdec" << "avdec_aac" << "vp9dec",
        QStringLiteral("video/x-h264, stream-format=(string)byte-stream; video/x-vp9"), QString(), true);
    CHECK(media::selectStreams(in, droidOnly, media::PlaybackPreferences()).empty());
}

static void urlAllowList() {
    using media::Protocol;
    CHECK(resolver::acceptedMediaUrl("https://rr1---sn-abc.googlevideo.com/videoplayback?itag=18", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("http://rr1---sn-abc.googlevideo.com/videoplayback?itag=18", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://googlevideo.com.evil.example/videoplayback", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://evilgooglevideo.com/videoplayback", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://user@rr1.googlevideo.com/videoplayback", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://rr1.googlevideo.com:8443/videoplayback", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://rr1.googlevideo.com/api/manifest/hls", Protocol::Https));
    CHECK(resolver::acceptedMediaUrl("https://manifest.googlevideo.com/api/manifest/hls_variant/x.m3u8", Protocol::Hls));
    CHECK(resolver::acceptedMediaUrl("https://www.youtube.com/api/manifest/hls_playlist/x.m3u8", Protocol::Hls));
    CHECK(!resolver::acceptedMediaUrl("https://127.0.0.1/videoplayback", Protocol::Https));
    CHECK(!resolver::acceptedMediaUrl("https://rr1.googlevideo.com/videoplayback", Protocol::Dash));
}

static void failures_() {
    CHECK(resolver::classifyFailure("ERROR: [youtube] x: Sign in to confirm you\xe2\x80\x99re not a bot.") == resolver::ErrorCode::BotCheck);
    CHECK(resolver::classifyFailure("ERROR: [youtube] x: Private video. Sign in") == resolver::ErrorCode::PrivateVideo);
    CHECK(resolver::classifyFailure("ERROR: [youtube] x: Video unavailable") == resolver::ErrorCode::VideoUnavailable);
    CHECK(resolver::classifyFailure("ERROR: [youtube] x: This live event will begin in 3 hours.") == resolver::ErrorCode::Upcoming);
    CHECK(resolver::classifyFailure("ERROR: Unable to download API page: timed out") == resolver::ErrorCode::Network);
    CHECK(resolver::classifyFailure("ERROR: packaged resolver temporary storage is unavailable.") == resolver::ErrorCode::RuntimeUnavailable);
    CHECK(resolver::classifyFailure("ERROR: something new") == resolver::ErrorCode::Failed);
}

// RES-09: rate limiting is never "video unavailable"; the final reasons are told apart from a bare
// "unavailable"; the runtime's temporary-file budget is a retryable extraction failure.
static void res09_failureReasons() {
    using resolver::ErrorCode;
    using resolver::FailureReason;
    using resolver::classifyFailure;
    using resolver::classifyFailureReason;
    // Under HTTP 429 the watch page is missing and the private video falls through to "unavailable".
    const QByteArray limited = "WARNING: [youtube] x: Unable to download webpage: HTTP Error 429: Too Many Requests\n"
                               "ERROR: [youtube] x: Video unavailable";
    CHECK(classifyFailure(limited) == ErrorCode::Network);
    CHECK(classifyFailureReason(limited) == FailureReason::RateLimited);
    const QByteArray tryLater = "ERROR: [youtube] x: This content isn't available, try again later. The current session has been rate-limited";
    CHECK(classifyFailure(tryLater) == ErrorCode::Network && classifyFailureReason(tryLater) == FailureReason::RateLimited);
    // A final reason under 429 is still the provider's verdict.
    const QByteArray removedUnder429 = "WARNING: HTTP Error 429: Too Many Requests\nERROR: [youtube] x: This video has been removed by the uploader";
    CHECK(classifyFailure(removedUnder429) == ErrorCode::VideoUnavailable);
    CHECK(classifyFailureReason(removedUnder429) == FailureReason::Removed);
    struct Row { const char *text; ErrorCode code; FailureReason reason; };
    const Row rows[] = {
        {"ERROR: [youtube] x: Video unavailable", ErrorCode::VideoUnavailable, FailureReason::BareUnavailable},
        {"ERROR: [youtube] x: This video is not available", ErrorCode::VideoUnavailable, FailureReason::BareUnavailable},
        // The app and TV clients' wording (android_vr, tv): seen for a video that does not exist.
        {"WARNING: [youtube] x: This video is unavailable", ErrorCode::VideoUnavailable, FailureReason::BareUnavailable},
        {"ERROR: [youtube] x: This video is private", ErrorCode::PrivateVideo, FailureReason::Private},
        {"WARNING: [youtube] x: android_vr client https formats require a GVS PO Token which was not provided. They will "
         "be skipped as they may yield HTTP Error 403.", ErrorCode::Failed, FailureReason::DeliveryChanged},
        // Not "unavailable": a server error stays a failure.
        {"ERROR: [youtube] x: Unable to download API page: HTTP Error 503: Service Unavailable", ErrorCode::Network,
         FailureReason::None},
        {"ERROR: [youtube] x: Video unavailable. This video has been removed for violating YouTube's Terms of Service",
         ErrorCode::VideoUnavailable, FailureReason::Removed},
        {"ERROR: [youtube] x: Video unavailable. This video is no longer available due to a copyright claim by Someone",
         ErrorCode::VideoUnavailable, FailureReason::Removed},
        {"ERROR: [youtube] x: Video unavailable. This video is no longer available because the YouTube account "
         "associated with this video has been terminated.", ErrorCode::VideoUnavailable, FailureReason::Terminated},
        {"ERROR: [youtube] x: Video unavailable. The uploader has not made this video available in your country",
         ErrorCode::VideoUnavailable, FailureReason::GeoBlocked},
        {"ERROR: [youtube] x: Private video. Sign in if you've been granted access to this video", ErrorCode::PrivateVideo,
         FailureReason::Private},
        {"ERROR: [youtube] x: This live event will begin in 3 days.", ErrorCode::Upcoming, FailureReason::Upcoming},
        {"ERROR: [youtube] x: Join this channel to get access to members-only content like this video",
         ErrorCode::Failed, FailureReason::MembersOnly},
        {"ERROR: [youtube] x: Video unavailable. Join this channel to get access to members-only content",
         ErrorCode::VideoUnavailable, FailureReason::MembersOnly},
        {"ERROR: packaged resolver temporary storage limit exceeded", ErrorCode::Failed, FailureReason::StorageLimit},
        {"WARNING: [youtube] x: Some tv client https formats have been skipped as they are missing a URL. YouTube may "
         "have enabled the SABR-only streaming experiment", ErrorCode::Failed, FailureReason::DeliveryChanged},
        {"WARNING: [youtube] x: n challenge solving failed: Some formats may be missing.", ErrorCode::Failed,
         FailureReason::DeliveryChanged},
        {"WARNING: [youtube] x: Signature solving failed: Some formats may be missing.", ErrorCode::Failed,
         FailureReason::DeliveryChanged},
        {"ERROR: [youtube] x: Requested format is not available", ErrorCode::Failed, FailureReason::None},
        {"ERROR: something new", ErrorCode::Failed, FailureReason::None},
    };
    for (const Row &r : rows) {
        CHECK(classifyFailure(r.text) == r.code);
        CHECK(classifyFailureReason(r.text) == r.reason);
    }
    // The temporary-storage refusal of a missing directory still means the runtime is missing.
    CHECK(classifyFailureReason("ERROR: packaged resolver temporary storage is unavailable.") == FailureReason::None);
    // Fixed labels, one per reason.
    CHECK(std::string(resolver::failureReasonLabel(FailureReason::RateLimited)) == "rate_limited");
    CHECK(std::string(resolver::failureReasonLabel(FailureReason::BareUnavailable)) == "bare_unavailable");
    CHECK(std::string(resolver::failureReasonLabel(FailureReason::StorageLimit)) == "storage_limit");
    CHECK(std::string(resolver::failureReasonLabel(FailureReason::None)) == "none");
}

// RES-19: an upcoming answer carries yt-dlp's release_timestamp (DetailsExtras); others do not.
static void res19_upcomingStartParsed() {
    catalog::VideoDetails d;
    resolver::DetailsExtras x;
    const QByteArray upcoming = "{\"id\":\"aBcDeFgHiJk\",\"title\":\"Soon\",\"live_status\":\"is_upcoming\","
                                "\"release_timestamp\":1790000000,\"formats\":[]}\n";
    CHECK(resolver::parseDetails(upcoming, "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    CHECK(d.summary.liveStatus == media::LiveStatus::Upcoming && d.formats.empty());
    CHECK(x.scheduledStartUnix == 1790000000);
    const QByteArray vod = "{\"id\":\"aBcDeFgHiJk\",\"title\":\"Done\",\"live_status\":\"not_live\","
                           "\"release_timestamp\":1790000000,\"formats\":[]}";
    CHECK(resolver::parseDetails(vod, "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None && x.scheduledStartUnix == 0);
    CHECK(resolver::parseDetails(upcoming, "aBcDeFgHiJk", &d, nullptr) == resolver::ErrorCode::None);
    // Formats listed without a usable URL are counted (the storyboard is not a media format).
    CHECK(resolver::parseDetails(fixture("details_vod.json"), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    CHECK(d.formats.size() == 8 && x.formatsWithoutUrl == 4);  // nourl, http, lookalike, wrongpath
    CHECK(resolver::parseDetails(upcoming, "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None && x.formatsWithoutUrl == 0);
}

// Scrub preview: the storyboard formats (sb0..sb3) of a details answer. The level closest above the
// preview's cell width is kept; sheets only from https *.ytimg.com/sb/; malformed levels are dropped.
static QByteArray storyboardFormat(const char *id, int width, int height, int columns, int rows, const QByteArray &fragments) {
    return QByteArray("{\"format_id\":\"") + id + "\",\"format_note\":\"storyboard\",\"ext\":\"mhtml\",\"protocol\":\"mhtml\","
           "\"vcodec\":\"none\",\"acodec\":\"none\",\"url\":\"https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M$M.jpg?sqp=x&sigh=y\","
           "\"width\":" + QByteArray::number(width) + ",\"height\":" + QByteArray::number(height) + ",\"columns\":"
           + QByteArray::number(columns) + ",\"rows\":" + QByteArray::number(rows) + ",\"fragments\":[" + fragments + "]}";
}
static QByteArray sheet(const char *level, int index, const char *seconds, const char *host = "i.ytimg.com") {
    return QByteArray("{\"url\":\"https://") + host + "/sb/aBcDeFgHiJk/storyboard3_" + level + "/M" + QByteArray::number(index)
           + ".jpg?sqp=x&sigh=y\",\"duration\":" + seconds + "}";
}
static QByteArray detailsWith(const QByteArray &formats) {
    return "{\"id\":\"aBcDeFgHiJk\",\"title\":\"Boards\",\"live_status\":\"not_live\",\"duration\":212,\"formats\":[" + formats + "]}";
}
static void sb_storyboardLevels() {
    using resolver::StoryboardLevel;
    catalog::VideoDetails d;
    resolver::DetailsExtras x;
    const QByteArray l3 = storyboardFormat("sb0", 320, 180, 3, 3, sheet("L3", 0, "90.0") + "," + sheet("L3", 1, "90.0") + "," + sheet("L3", 2, "32.0"));
    const QByteArray l2 = storyboardFormat("sb1", 160, 90, 5, 5, sheet("L2", 0, "200.5") + "," + sheet("L2", 1, "11.5"));
    const QByteArray l1 = storyboardFormat("sb2", 80, 45, 10, 10, sheet("L1", 0, "212"));
    const QByteArray l0 = storyboardFormat("sb3", 48, 27, 10, 10, sheet("L0", 0, "212"));
    CHECK(resolver::parseDetails(detailsWith(l3 + "," + l2 + "," + l1 + "," + l0), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    // 160 px cells: the level the preview asks for (kStoryboardCellWidth), not the largest.
    CHECK(resolver::kStoryboardCellWidth == 160);
    CHECK(x.storyboard.width == 160 && x.storyboard.height == 90 && x.storyboard.columns == 5 && x.storyboard.rows == 5);
    CHECK(x.storyboard.sheets.size() == 2);
    CHECK(x.storyboard.sheets.size() == 2 && x.storyboard.sheets[0].durationMs == 200500 && x.storyboard.sheets[1].durationMs == 11500);
    CHECK(x.storyboard.sheets.size() == 2
          && x.storyboard.sheets[1].url == "https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M1.jpg?sqp=x&sigh=y");
    // Storyboards are not media formats and are not counted as formats without a URL.
    CHECK(d.formats.empty() && x.formatsWithoutUrl == 0);
    // Without the 160 level: the narrowest level above it; with only smaller ones: the widest.
    CHECK(resolver::parseDetails(detailsWith(l3 + "," + l1 + "," + l0), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    CHECK(x.storyboard.width == 320 && x.storyboard.sheets.size() == 3);
    CHECK(resolver::parseDetails(detailsWith(l0 + "," + l1), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    CHECK(x.storyboard.width == 80);
    // No storyboard formats (and the fixture's bare sb0 without sheets): none.
    CHECK(resolver::parseDetails(detailsWith(""), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None && x.storyboard.empty());
    CHECK(resolver::parseDetails(fixture("details_vod.json"), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None && x.storyboard.empty());
    // A level with a sheet outside the allow-list is dropped as a whole; the next level serves.
    const QByteArray foreign = storyboardFormat("sb1", 160, 90, 5, 5, sheet("L2", 0, "200.5") + "," + sheet("L2", 1, "11.5", "img.example"));
    CHECK(resolver::parseDetails(detailsWith(l3 + "," + foreign), "aBcDeFgHiJk", &d, &x) == resolver::ErrorCode::None);
    CHECK(x.storyboard.width == 320);
    const auto level = [](const QByteArray &format) {
        return resolver::parseStoryboard(QJsonDocument::fromJson(format).object());
    };
    CHECK(level(l2).sheets.size() == 2);
    // An empty last sheet (yt-dlp's rounding) is dropped; an empty sheet elsewhere is malformed.
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 5, sheet("L2", 0, "212") + "," + sheet("L2", 1, "0.0"))).sheets.size() == 1);
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 5, sheet("L2", 0, "0") + "," + sheet("L2", 1, "212"))).empty());
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 5, sheet("L2", 0, "0"))).empty());
    // Sizes and counts are bounded.
    CHECK(level(storyboardFormat("sb1", 160, 90, 0, 5, sheet("L2", 0, "212"))).empty());
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 64, sheet("L2", 0, "212"))).empty());
    CHECK(level(storyboardFormat("sb1", 4096, 90, 5, 5, sheet("L2", 0, "212"))).empty());
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 5, "")).empty());
    QByteArray many;
    for (int i = 0; i < 2049; ++i) many += (i ? "," : "") + sheet("L2", i, "1");
    CHECK(level(storyboardFormat("sb1", 160, 90, 5, 5, many)).empty());
    // Only entries marked as storyboards.
    QByteArray unmarked = l2;
    unmarked.replace("\"format_note\":\"storyboard\"", "\"format_note\":\"x\"").replace("\"protocol\":\"mhtml\"", "\"protocol\":\"https\"");
    CHECK(level(unmarked).empty());
    // The allow-list: https, a ytimg.com host, the /sb/ path, no credentials or other port.
    CHECK(resolver::acceptedStoryboardUrl("https://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M0.jpg?sigh=y"));
    CHECK(resolver::acceptedStoryboardUrl("https://i9.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M0.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("http://i.ytimg.com/sb/aBcDeFgHiJk/storyboard3_L2/M0.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("https://i.ytimg.com/vi/aBcDeFgHiJk/mqdefault.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("https://ytimg.com.example/sb/a/M0.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("https://evilytimg.com/sb/a/M0.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("https://user@i.ytimg.com/sb/a/M0.jpg"));
    CHECK(!resolver::acceptedStoryboardUrl("https://i.ytimg.com:8443/sb/a/M0.jpg"));
    // pickStoryboard on its own: empty levels are skipped; no level, no storyboard.
    std::vector<StoryboardLevel> levels;
    CHECK(resolver::pickStoryboard(levels, 160).empty());
    levels.push_back(StoryboardLevel());
    levels.push_back(level(l3));
    CHECK(resolver::pickStoryboard(levels, 160).width == 320 && resolver::pickStoryboard(levels, 400).width == 320);
}

// RES-08 step 1 vs 2: the token-free pass names android_vr + visionos in the one youtube
// extractor-args value; the widened pass keeps yt-dlp's default clients.
static void res08_passClients() {
    using resolver::YtDlpVideoService;
    const QString tokenFree = QStringLiteral("youtube:skip=dash;player_client=android_vr,visionos");
    CHECK(YtDlpVideoService::youtubeExtractorArguments(true) == tokenFree);
    CHECK(YtDlpVideoService::youtubeExtractorArguments(false) == QLatin1String("youtube:skip=dash"));
    const QStringList common = QStringList() << "--ignore-config";
    const QStringList a = YtDlpVideoService::passArguments(common, QStringLiteral("aBcDeFgHiJk"), true);
    const int at = a.indexOf(QStringLiteral("--extractor-args"));
    CHECK(at >= 0 && a.value(at + 1) == tokenFree && a.count(QStringLiteral("--extractor-args")) == 1);
    const QStringList w = YtDlpVideoService::passArguments(common, QStringLiteral("aBcDeFgHiJk"), false);
    CHECK(w.contains(QStringLiteral("youtube:skip=dash")) && !w.join(QLatin1Char(' ')).contains(QLatin1String("player_client")));
    CHECK(YtDlpVideoService::detailsArguments(common, QStringLiteral("aBcDeFgHiJk")) == w);
}

static void arguments() {
    const QStringList common = QStringList() << "--ignore-config";
    const QStringList s = resolver::YtDlpVideoService::searchArguments(common, QStringLiteral("sail boats"), 20, 20);
    CHECK(s.first() == "--ignore-config");
    CHECK(s.contains("--flat-playlist"));
    CHECK(s.indexOf("--playlist-items") >= 0 && s.at(s.indexOf("--playlist-items") + 1) == "21:40");
    CHECK(s.last() == "ytsearch40:sail boats");
    CHECK(s.at(s.size() - 2) == "--");
    const QStringList d = resolver::YtDlpVideoService::detailsArguments(common, QStringLiteral("aBcDeFgHiJk"));
    CHECK(d.last() == "https://www.youtube.com/watch?v=aBcDeFgHiJk");
    CHECK(d.contains("--simulate") && d.contains("--ignore-no-formats-error"));
    CHECK(resolver::YtDlpVideoService::normalizedQuery(QStringLiteral("  a\n\tb  ")) == "a b");
    CHECK(resolver::YtDlpVideoService::normalizedQuery(QString(500, QLatin1Char('x'))).size() == 200);
}

static void probeMapping() {
    const auto none = platform::capabilitiesFrom(QStringList() << "qtdemux", QString(), QString(), true);
    CHECK(!none.h264.available && !none.aac && none.adaptive);
    const auto droid = platform::capabilitiesFrom(QStringList() << "droidvdec" << "droidadec" << "qtdemux",
        QStringLiteral("video/x-h264; video/x-h265"), QStringLiteral("audio/mpeg, mpegversion=(int)4"), false);
    CHECK(droid.h264.hardware && droid.h264.available && droid.hevc.hardware);
    CHECK(!droid.vp9.available && !droid.av1.available && droid.aac && !droid.adaptive);
    const auto sw = platform::capabilitiesFrom(QStringList() << "vp9dec" << "dav1ddec", QString(), QString(), false);
    CHECK(sw.vp9.available && !sw.vp9.hardware && sw.av1.available && !sw.mp4Demux);
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    listing();
    details();
    selectionOnFixture();
    urlAllowList();
    failures_();
    res09_failureReasons();
    res19_upcomingStartParsed();
    sb_storyboardLevels();
    res08_passClients();
    arguments();
    probeMapping();
    std::printf("resolver tests: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
