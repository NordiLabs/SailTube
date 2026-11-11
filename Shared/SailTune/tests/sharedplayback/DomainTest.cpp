// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the shared playback component's decisions: the pinned request shape, format
// ladder, playability and error classes, circuit breaker, resolve cache and the HTTP parser.
#include "../../Core/SharedPlayback/Domain/CircuitBreaker.h"
#include "../../Core/SharedPlayback/Domain/FormatLadder.h"
#include "../../Core/SharedPlayback/Domain/HttpResponseParser.h"
#include "../../Core/SharedPlayback/Domain/InnerTubeClient.h"
#include "../../Core/SharedPlayback/Domain/Playability.h"
#include "../../Core/SharedPlayback/Domain/ResolveCache.h"
#include "../../Core/SharedPlayback/Domain/StreamTypes.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::music::sharedplayback::domain;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { ++failures; std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool has(const std::string &text, const char *needle) { return text.find(needle) != std::string::npos; }

static StreamFormat audioFormat(int itag, const char *mime, const char *codec, int bitrate) {
    StreamFormat f; f.itag = itag; f.url = "https://rr1.googlevideo.com/videoplayback?itag=" + std::to_string(itag);
    f.mimeType = mime; f.codec = codec; f.hasAudio = true; f.bitrate = bitrate; return f;
}
static StreamFormat videoFormat(int itag, const char *codec, int height, int bitrate, int fps = 25) {
    StreamFormat f; f.itag = itag; f.url = "https://rr1.googlevideo.com/videoplayback?itag=" + std::to_string(itag);
    f.mimeType = "video/mp4"; f.codec = codec; f.hasVideo = true; f.height = height; f.width = height; f.bitrate = bitrate; f.fps = fps; return f;
}

static void testRequestShape() {
    const ClientDefinition client = visionosClient();
    CHECK(client.usable() && client.id == 101 && client.name == "VISIONOS" && client.version == "1.02");
    CHECK(plainClient(client));
    const std::string body = playerRequestBody(client, "dQw4w9WgXcQ");
    // The whole body, pinned: any change to the shape is a deliberate decision.
    const std::string expected =
        "{\"videoId\":\"dQw4w9WgXcQ\",\"contentCheckOk\":true,\"racyCheckOk\":true,"
        "\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}},"
        "\"context\":{\"client\":{\"clientName\":\"VISIONOS\",\"clientVersion\":\"1.02\",\"deviceMake\":\"Apple\","
        "\"deviceModel\":\"RealityDevice17,1\",\"userAgent\":\"Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 "
        "(KHTML, like Gecko) Version/26.0 Safari/605.1.15\",\"osName\":\"visionOS\",\"osVersion\":\"26.5.23O471\","
        "\"hl\":\"en\",\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0}}}";
    CHECK(body == expected);
    // Things that made the media host refuse an address, or that this path must not carry.
    for (const char *forbidden : {"\"gl\"", "visitorData", "\"key\"", "signatureTimestamp", "poToken", "serviceIntegrity", "cookie", "Authorization"})
        CHECK(!has(body, forbidden));
    CHECK(playerRequestBody(client, "short").empty());
    CHECK(playerRequestBody(client, "dQw4w9WgXc\"").empty());
    CHECK(playerRequestBody(client, "dQw4w9WgXcQQ").empty());
    CHECK(playerRequestBody(client, "dQw4w9Wg/cQ").empty());
    ClientDefinition hostile = client; hostile.userAgent = "x\",\"gl\":\"US";
    CHECK(!plainClient(hostile) && playerRequestBody(hostile, "dQw4w9WgXcQ").empty());
    ClientDefinition incomplete = client; incomplete.id = 0;
    CHECK(!incomplete.usable() && playerRequestBody(incomplete, "dQw4w9WgXcQ").empty());

    const std::vector<Header> withVisitor = playerRequestHeaders(client, "VISITOR123");
    bool sawVisitor = false, sawName = false, sawIdentity = false, sawCookie = false, sawKey = false;
    for (std::size_t i = 0; i < withVisitor.size(); ++i) {
        if (withVisitor[i].first == "X-Goog-Visitor-Id" && withVisitor[i].second == "VISITOR123") sawVisitor = true;
        if (withVisitor[i].first == "X-YouTube-Client-Name" && withVisitor[i].second == "101") sawName = true;
        if (withVisitor[i].first == "Accept-Encoding" && withVisitor[i].second == "identity") sawIdentity = true;
        if (withVisitor[i].first == "Cookie" || withVisitor[i].first == "Authorization") sawCookie = true;
        if (withVisitor[i].first == "X-Goog-Api-Key") sawKey = true;
    }
    CHECK(sawVisitor && sawName && sawIdentity && !sawCookie && !sawKey);
    const std::vector<Header> without = playerRequestHeaders(client, "");
    for (std::size_t i = 0; i < without.size(); ++i) CHECK(without[i].first != "X-Goog-Visitor-Id");

    const std::vector<Header> media = mediaRequestHeaders("UA/1.0");
    CHECK(media.size() == 5 && media[0].first == "User-Agent" && media[0].second == "UA/1.0");
    CHECK(media[3].first == "Sec-Fetch-Mode" && media[3].second == "navigate");
    for (std::size_t i = 0; i < media.size(); ++i) CHECK(media[i].first != "Cookie" && media[i].first != "Range");
    CHECK(has(visitorRequestBody(), "\"clientName\":\"WEB\"") && !has(visitorRequestBody(), "visitorData"));
    CHECK(validVideoId("-1G_D_OaOTA") && validVideoId("mq5bW8xipYU") && !validVideoId("") && !validVideoId("has space!"));
}

static void testCacheKey() {
    StreamRequest a, b;
    CHECK(a.cacheKey("dQw4w9WgXcQ") == b.cacheKey("dQw4w9WgXcQ"));
    CHECK(a.cacheKey("dQw4w9WgXcQ") != a.cacheKey("dQw4w9WgXcR"));
    b.audioOnly = false; CHECK(a.cacheKey("x") != b.cacheKey("x"));
    b = a; b.maxAudioKbps = 128; CHECK(a.cacheKey("x") != b.cacheKey("x"));
    b = a; b.dubLanguage = "es"; CHECK(a.cacheKey("x") != b.cacheKey("x"));
    b = a; b.hardwareDecode = true; CHECK(a.cacheKey("x") != b.cacheKey("x"));
    b = a; b.codecPreference.push_back("aac"); CHECK(a.cacheKey("x") != b.cacheKey("x"));
    b = a; b.audioOnly = false; b.maxVideoHeight = 720; StreamRequest c = b; c.maxVideoHeight = 1080;
    CHECK(b.cacheKey("x") != c.cacheKey("x"));
    StreamRequest d = a; d.maxVideoHeight = 480;   // irrelevant for audio-only: same key
    CHECK(a.cacheKey("x") == d.cacheKey("x"));
}

static void testAudioLadder() {
    StreamRequest request;
    std::vector<StreamFormat> formats;
    formats.push_back(audioFormat(139, "audio/mp4", "mp4a.40.5", 50375));
    formats.push_back(audioFormat(140, "audio/mp4", "mp4a.40.2", 130905));
    formats.push_back(audioFormat(249, "audio/webm", "opus", 59071));
    formats.push_back(audioFormat(250, "audio/webm", "opus", 77730));
    formats.push_back(audioFormat(251, "audio/webm", "opus", 152672));
    StreamFormat drc = audioFormat(774, "audio/webm", "opus", 258000); drc.isDrc = true; formats.push_back(drc);
    StreamFormat withVideo = audioFormat(18, "video/mp4", "avc1", 500000); withVideo.hasVideo = true; formats.push_back(withVideo);
    StreamFormat noUrl = audioFormat(999, "audio/webm", "opus", 999999); noUrl.url.clear(); formats.push_back(noUrl);
    StreamFormat hls = audioFormat(95, "audio/mp4", "mp4a", 100000); hls.isHls = true; formats.push_back(hls);

    std::vector<StreamFormat> ladder = rankAudio(formats, request);
    // Opus before AAC, higher bitrate first inside a codec, DRC after the regular ones of its codec.
    std::vector<int> order; for (std::size_t i = 0; i < ladder.size(); ++i) order.push_back(ladder[i].itag);
    const int expected[] = {251, 250, 249, 774, 140, 139};
    CHECK(order.size() == 6);
    for (std::size_t i = 0; i < order.size() && i < 6; ++i) CHECK(order[i] == expected[i]);

    // Preference order is the request's: AAC first when asked.
    request.codecPreference.push_back("aac"); request.codecPreference.push_back("opus");
    ladder = rankAudio(formats, request);
    CHECK(!ladder.empty() && ladder[0].itag == 140);
    request.codecPreference.clear();

    // A ceiling removes what is above it, never the last format.
    request.maxAudioKbps = 100;
    ladder = rankAudio(formats, request);
    for (std::size_t i = 0; i < ladder.size(); ++i) CHECK(ladder[i].bitrate / 1000 <= 100);
    CHECK(!ladder.empty() && ladder[0].itag == 250);
    request.maxAudioKbps = 10;
    CHECK(!rankAudio(formats, request).empty());
    request.maxAudioKbps = 0;

    // Duplicates (same family and kbps within rounding) collapse.
    std::vector<StreamFormat> twins;
    twins.push_back(audioFormat(251, "audio/webm", "opus", 152672));
    twins.push_back(audioFormat(1251, "audio/webm", "opus", 152700));
    CHECK(rankAudio(twins, request).size() == 1);

    // Language: the original track first, a requested dub next, descriptive last.
    std::vector<StreamFormat> tracks;
    StreamFormat original = audioFormat(251, "audio/webm", "opus", 150000); original.audioLanguage = "en"; tracks.push_back(original);
    StreamFormat spanish = audioFormat(2510, "audio/webm", "opus", 150000); spanish.audioLanguage = "es"; spanish.isDefaultAudio = false; tracks.push_back(spanish);
    StreamFormat described = audioFormat(2511, "audio/webm", "opus", 151000); described.audioLanguage = "en"; described.isDefaultAudio = false; described.isDescriptive = true; tracks.push_back(described);
    ladder = rankAudio(tracks, request);
    CHECK(ladder.size() == 3 && ladder[0].itag == 251 && ladder[1].itag == 2510 && ladder[2].itag == 2511);
    request.dubLanguage = "es";
    ladder = rankAudio(tracks, request);
    CHECK(ladder.size() == 3 && ladder[0].itag == 2510 && ladder[2].itag == 2511);
    CHECK(rankAudio(std::vector<StreamFormat>(), request).empty());
}

static void testVideoAndMuxedLadder() {
    StreamRequest request; request.audioOnly = false;
    std::vector<StreamFormat> formats;
    formats.push_back(videoFormat(137, "avc1.640020", 1080, 64000));
    formats.push_back(videoFormat(248, "vp9", 1080, 172000));
    formats.push_back(videoFormat(399, "av01.0.08M.08", 1080, 154000));
    formats.push_back(videoFormat(136, "avc1.4d401f", 720, 40000));
    formats.push_back(videoFormat(247, "vp9", 720, 90000));
    formats.push_back(videoFormat(403, "vp9", 2160, 900000));
    std::vector<StreamFormat> ranked = rankVideo(formats, request);
    for (std::size_t i = 0; i < ranked.size(); ++i) CHECK(ranked[i].codecFamily() != "av1" && ranked[i].height <= 1080);
    CHECK(ranked.size() == 4 && ranked[0].height == 1080);
    request.hardwareDecode = true;
    ranked = rankVideo(formats, request);
    CHECK(!ranked.empty() && ranked[0].itag == 137);           // H.264 first when the hardware decodes it
    request.maxVideoHeight = 720;
    ranked = rankVideo(formats, request);
    for (std::size_t i = 0; i < ranked.size(); ++i) CHECK(ranked[i].height <= 720);
    StreamFormat progressive = audioFormat(18, "video/mp4", "avc1.42001E", 500000); progressive.hasVideo = true; progressive.height = 360;
    StreamFormat better = progressive; better.itag = 22; better.height = 720;
    std::vector<StreamFormat> mixed(formats); mixed.push_back(progressive); mixed.push_back(better);
    const std::vector<StreamFormat> muxed = rankMuxed(mixed);
    CHECK(muxed.size() == 2 && muxed[0].itag == 22 && muxed[1].itag == 18);
    CHECK(videoFormat(1, "avc1.640020", 1, 1).codecFamily() == "h264" && videoFormat(1, "vp9", 1, 1).codecFamily() == "vp9");
    CHECK(audioFormat(1, "audio/mp4", "mp4a.40.2", 1).codecFamily() == "aac" && audioFormat(1, "audio/webm", "opus", 1).container() == "webm");
}

static void testPlayability() {
    CHECK(classifyPlayability("OK", "") == FailureKind::None);
    CHECK(classifyPlayability("LOGIN_REQUIRED", "Sign in to confirm you\xE2\x80\x99re not a bot") == FailureKind::BotWall);
    CHECK(classifyPlayability("LOGIN_REQUIRED", "Sign in to confirm you're not a bot. Use cookies") == FailureKind::BotWall);
    CHECK(classifyPlayability("ERROR", "Confirm you are not a robot") == FailureKind::BotWall);
    CHECK(classifyPlayability("LOGIN_REQUIRED", "Sign in to confirm your age") == FailureKind::Unplayable);
    CHECK(classifyPlayability("LOGIN_REQUIRED", "This video may be inappropriate for some users.") == FailureKind::Unplayable);
    CHECK(classifyPlayability("UNPLAYABLE", "") == FailureKind::Unplayable);
    CHECK(classifyPlayability("UNPLAYABLE", "This video has been removed by the uploader") == FailureKind::Final);
    CHECK(classifyPlayability("UNPLAYABLE", "The uploader has not made this video available in your country") == FailureKind::Final);
    CHECK(classifyPlayability("LIVE_STREAM_OFFLINE", "Premieres in 3 days") == FailureKind::Final);
    CHECK(classifyPlayability("UNPLAYABLE", "Private video") == FailureKind::Final);
    // Rate limiting is worded as unavailable and is never final.
    CHECK(classifyPlayability("ERROR", "This content is unavailable. Please try again later") == FailureKind::Unplayable);
    CHECK(!isFinalReason("Video unavailable, try again later"));
    CHECK(!isFinalReason("Video unavailable. This video is not available"));     // one client's answer: the extractor decides
    CHECK(classifyPlayability("ERROR", "Video unavailable. This video is not available") == FailureKind::Unplayable);
    // A word, not a substring.
    CHECK(!mentionsBotCheck("The abbot's robotic arm") && mentionsBotCheck("Bot check") && mentionsBotCheck("you are a bot."));
    CHECK(isFinalReason("ERROR: [youtube] x: Video unavailable. This video has been removed for violating") ||
          isFinalReason("This video has been removed"));
    CHECK(isFinalReason("Incomplete YouTube ID abc") && isFinalReason("The live event will begin in 2 hours"));
    CHECK(!isFinalReason("") && !isFinalReason("Sign in to confirm you're not a bot"));
}

static void testBreaker() {
    CircuitBreaker breaker;
    CHECK(breaker.allows(0));
    breaker.recordStrike(1000);
    CHECK(breaker.allows(1001));                                   // one strike: still on
    breaker.recordStrike(1000 + CircuitBreaker::strikeWindowMs + 1);   // the first one aged out
    CHECK(breaker.allows(2000 + CircuitBreaker::strikeWindowMs));
    breaker.recordStrike(1000 + CircuitBreaker::strikeWindowMs + 5000);   // two within the window
    CHECK(!breaker.allows(1000 + CircuitBreaker::strikeWindowMs + 5001));
    const std::int64_t openedAt = 1000 + CircuitBreaker::strikeWindowMs + 5000;
    CHECK(!breaker.allows(openedAt + CircuitBreaker::openForMs - 1));
    CHECK(breaker.allows(openedAt + CircuitBreaker::openForMs));    // back on after thirty minutes
    breaker.recordStrike(openedAt + CircuitBreaker::openForMs + 10);
    CHECK(breaker.allows(openedAt + CircuitBreaker::openForMs + 11));   // counters restarted

    CircuitBreaker bot;
    bot.recordBotWall(10);
    CHECK(bot.allows(11) && bot.botWalls() == 1);
    bot.recordSuccess();                                           // any success resets the run of walls
    bot.recordBotWall(20);
    CHECK(bot.allows(21));
    bot.recordBotWall(30);
    CHECK(!bot.allows(31));
    CHECK(bot.allows(30 + CircuitBreaker::openForMs));
    bot.reset(); CHECK(bot.allows(0) && bot.botWalls() == 0);
}

static StreamSet cacheSet(const char *id, std::int64_t expiresAt) {
    StreamSet s; s.videoId = id; s.expiresAtUnixMs = expiresAt; s.audio.push_back(audioFormat(251, "audio/webm", "opus", 152000)); return s;
}

static void testCache() {
    const std::int64_t now = 1000000000000LL;
    ResolveCache cache;
    // Valid until expiry - 120 s, but at most 20 minutes.
    CHECK(ResolveCache::validUntil(cacheSet("a", now + 6 * 3600 * 1000), now) == now + ResolveCache::maximumLifeMs);
    CHECK(ResolveCache::validUntil(cacheSet("a", now + 600 * 1000), now) == now + 600 * 1000 - ResolveCache::expiryMarginMs);
    CHECK(ResolveCache::validUntil(cacheSet("a", 0), now) == now + ResolveCache::unknownExpiryLifeMs);
    // Stored only if still valid for more than five seconds.
    CHECK(!cache.put("short", cacheSet("short", now + ResolveCache::expiryMarginMs + 5000), now));
    CHECK(cache.put("long", cacheSet("long", now + ResolveCache::expiryMarginMs + 6000), now));
    CHECK(cache.get("long", now) != nullptr && cache.get("short", now) == nullptr);
    CHECK(cache.get("long", now + 5000) != nullptr);
    CHECK(cache.get("long", now + 6001) == nullptr);                // expired by then
    // Capacity and least recently used.
    ResolveCache lru;
    for (int i = 0; i < 24; ++i) CHECK(lru.put("k" + std::to_string(i), cacheSet(("v" + std::to_string(i)).c_str(), 0), now));
    CHECK(lru.size() == 24);
    CHECK(lru.get("k0", now) != nullptr);                           // k0 is now the most recently used
    CHECK(lru.put("k24", cacheSet("v24", 0), now));
    CHECK(lru.size() == 24 && lru.get("k0", now) != nullptr && lru.get("k1", now) == nullptr);
    // Invalidation drops every key of a video.
    ResolveCache keys;
    keys.put("v|a", cacheSet("vid", 0), now); keys.put("v|b", cacheSet("vid", 0), now); keys.put("w|a", cacheSet("other", 0), now);
    keys.invalidateVideo("vid");
    CHECK(keys.get("v|a", now) == nullptr && keys.get("v|b", now) == nullptr && keys.get("w|a", now) != nullptr);
    // peek() is a read: valid entries are returned, expired ones are not, and neither pruning nor the use order changes.
    ResolveCache peeked;
    for (int i = 0; i < 24; ++i) peeked.put("k" + std::to_string(i), cacheSet(("v" + std::to_string(i)).c_str(), 0), now);
    StreamSet seen;
    CHECK(peeked.peek("k0", now, &seen) && seen.videoId == "v0" && peeked.peek("k0", now, nullptr));
    CHECK(!peeked.peek("nope", now, &seen) && !peeked.peek("k0", now + ResolveCache::unknownExpiryLifeMs, &seen));
    CHECK(peeked.size() == 24);                                     // the expired entry is still stored: nothing was pruned
    CHECK(peeked.put("k24", cacheSet("v24", 0), now));
    CHECK(peeked.get("k0", now) == nullptr);                        // k0 stayed the least recently used: peeking did not refresh it
    // Replacing a key keeps one entry.
    ResolveCache again;
    again.put("k", cacheSet("v", 0), now); again.put("k", cacheSet("v", 0), now);
    CHECK(again.size() == 1);
}

static void testHttpParser() {
    {   // Content-Length, fed byte by byte.
        const std::string wire = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 5\r\n\r\nhello";
        HttpResponseParser parser;
        HttpResponseParser::State state = HttpResponseParser::NeedMore;
        for (std::size_t i = 0; i < wire.size(); ++i) state = parser.feed(&wire[i], 1);
        CHECK(state == HttpResponseParser::Done && parser.statusCode() == 200 && parser.body() == "hello");
        CHECK(parser.keepAlive() && parser.header("content-type") == "application/json");
    }
    {   // Chunked, with an extension, split across feeds.
        const std::string wire = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5;ext=1\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n";
        for (std::size_t split = 1; split < wire.size(); split += 7) {
            HttpResponseParser parser;
            parser.feed(wire.data(), split);
            const HttpResponseParser::State state = parser.feed(wire.data() + split, wire.size() - split);
            CHECK(state == HttpResponseParser::Done && parser.body() == "hello world");
        }
    }
    {   // Until close.
        HttpResponseParser parser;
        const std::string head = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\npartial";
        CHECK(parser.feed(head.data(), head.size()) == HttpResponseParser::HeadersDone);
        CHECK(!parser.keepAlive());
        CHECK(parser.closed() == HttpResponseParser::Done && parser.body() == "partial");
    }
    {   // A status with no body, and headers only for a 206 probe.
        HttpResponseParser none;
        const std::string wire = "HTTP/1.1 204 No Content\r\n\r\n";
        CHECK(none.feed(wire.data(), wire.size()) == HttpResponseParser::Done && none.statusCode() == 204);
        HttpResponseParser probe(16 * 1024, 64 * 1024, true);   // headers only: the declared 10 MiB is not read
        const std::string partial = "HTTP/1.1 206 Partial Content\r\nContent-Length: 10485760\r\nContent-Range: bytes 0-10485759/20000000\r\n\r\nab";
        CHECK(probe.feed(partial.data(), partial.size()) == HttpResponseParser::HeadersDone);
        CHECK(probe.statusCode() == 206 && probe.headersComplete() && probe.header("content-range").compare(0, 7, "bytes 0") == 0);
    }
    {   // An interim response (103 Early Hints, 100 Continue) is not the answer: the final one follows.
        HttpResponseParser parser;
        const std::string wire = "HTTP/1.1 103 Early Hints\r\nLink: </a.css>; rel=preload\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi";
        CHECK(parser.feed(wire.data(), wire.size()) == HttpResponseParser::Done && parser.statusCode() == 200 && parser.body() == "hi");
        CHECK(parser.header("link").empty() && parser.header("content-length") == "2");
        HttpResponseParser probe(16 * 1024, 64 * 1024, true);
        const std::string hinted = "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 206 Partial Content\r\nContent-Range: bytes 0-9/100\r\nContent-Length: 10\r\n\r\n";
        CHECK(probe.feed(hinted.data(), hinted.size()) == HttpResponseParser::HeadersDone && probe.statusCode() == 206);
        for (std::size_t split = 1; split < wire.size(); split += 5) {        // and split anywhere
            HttpResponseParser cut;
            cut.feed(wire.data(), split);
            CHECK(cut.feed(wire.data() + split, wire.size() - split) == HttpResponseParser::Done && cut.statusCode() == 200 && cut.body() == "hi");
        }
        HttpResponseParser flood;
        std::string many; for (int i = 0; i < 9; ++i) many += "HTTP/1.1 103 Early Hints\r\n\r\n";
        CHECK(flood.feed(many.data(), many.size()) == HttpResponseParser::Error);       // endless interim answers are an error
    }
    {   // Refusals and bad wire bytes.
        HttpResponseParser refused;
        const std::string wire = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
        CHECK(refused.feed(wire.data(), wire.size()) == HttpResponseParser::Done && refused.statusCode() == 403);
        HttpResponseParser garbage;
        const std::string junk = "NOT HTTP AT ALL\r\n\r\n";
        CHECK(garbage.feed(junk.data(), junk.size()) == HttpResponseParser::Error);
        HttpResponseParser badChunk;
        const std::string chunk = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZ\r\n";
        CHECK(badChunk.feed(chunk.data(), chunk.size()) == HttpResponseParser::Error);
        HttpResponseParser tooBig(100);
        const std::string big = "HTTP/1.1 200 OK\r\nContent-Length: 101\r\n\r\n";
        CHECK(tooBig.feed(big.data(), big.size()) == HttpResponseParser::Error);
        HttpResponseParser bigHeader(100, 64);
        const std::string endless = "HTTP/1.1 200 OK\r\nX: " + std::string(200, 'a');
        CHECK(bigHeader.feed(endless.data(), endless.size()) == HttpResponseParser::Error);
        HttpResponseParser cut;
        const std::string half = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
        cut.feed(half.data(), half.size());
        CHECK(cut.closed() == HttpResponseParser::Error);          // closed early: not a response
    }
}

static void testLabels() {
    CHECK(std::string(failureLabel(FailureKind::BotWall)) == "bot_wall" && std::string(pathLabel(ResolvePath::Fast)) == "fast");
    CHECK(std::string(failureLabel(FailureKind::None)) == "none" && std::string(pathLabel(ResolvePath::Cache)) == "cache");
}

int main() {
    testRequestShape();
    testCacheKey();
    testAudioLadder();
    testVideoAndMuxedLadder();
    testPlayability();
    testBreaker();
    testCache();
    testHttpParser();
    testLabels();
    if (failures) { std::fprintf(stderr, "%d check(s) failed\n", failures); return 1; }
    std::printf("sharedplayback DomainTest: all checks passed\n");
    return 0;
}
