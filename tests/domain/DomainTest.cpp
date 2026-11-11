// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Qt-free checks of the stream-selection policy (SEL-01..05, SEL-08), the
// live policy (RES-19, RES-09), the MP4 index reader and the DASH manifest
// builder. Build: tests/domain/run.sh
#include "Core/Media/Domain/AudioTrackPolicy.h"
#include "Core/Media/Domain/DashManifest.h"
#include "Core/Media/Domain/LivePolicy.h"
#include "Core/Media/Domain/MediaFormat.h"
#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Catalog/Domain/Video.h"

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace nordtile::video;
using namespace nordtile::video::media;

static int failures = 0;
static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static MediaFormat fmt(const char *id, const char *ext, const char *v, const char *a, int h, double fps = 30,
                       double kbps = 1000, Protocol p = Protocol::Https) {
    MediaFormat f;
    f.id = id; f.container = ext; f.protocol = p; f.url = std::string("https://media.example/") + id;
    f.videoCodecTag = v; f.audioCodecTag = a;
    f.videoCodec = parseVideoCodec(v); f.audioCodec = parseAudioCodec(a);
    f.height = h; f.width = h * 16 / 9; f.fps = fps; f.totalKbps = kbps;
    if (f.videoCodec == VideoCodec::None) f.audioKbps = kbps;
    return f;
}

static MediaFormat audioTrack(const char *id, const char *ext, const char *codec, double kbps,
                              const char *language, int preference, bool drc = false) {
    MediaFormat f = fmt(id, ext, "none", codec, 0, 0, kbps);
    f.language = language; f.languagePreference = preference; f.dynamicRangeCompressed = drc;
    return f;
}

// A typical VOD offer: one muxed 360p; H.264, VP9 and AV1 ladders at several
// heights (VP9 up to 2160p); AAC and Opus audio.
static SelectionInput typical() {
    SelectionInput in;
    in.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("134", "mp4", "avc1.4D401E", "none", 360, 30, 600), fmt("136", "mp4", "avc1.4D401F", "none", 720, 30, 1500),
        fmt("298", "mp4", "avc1.4D4020", "none", 720, 60, 2500), fmt("137", "mp4", "avc1.640028", "none", 1080, 30, 3000),
        fmt("299", "mp4", "avc1.64002A", "none", 1080, 60, 5000),
        fmt("243", "webm", "vp09.00.21.08", "none", 360, 30, 500), fmt("247", "webm", "vp09.00.31.08", "none", 720, 30, 1300),
        fmt("302", "webm", "vp09.00.40.08", "none", 720, 60, 2200), fmt("248", "webm", "vp09.00.40.08", "none", 1080, 30, 2600),
        fmt("303", "webm", "vp09.00.41.08", "none", 1080, 60, 4400), fmt("271", "webm", "vp09.00.50.08", "none", 1440, 30, 9000),
        fmt("313", "webm", "vp09.00.51.08", "none", 2160, 30, 18000),
        fmt("398", "mp4", "av01.0.05M.08", "none", 720, 30, 1000), fmt("399", "mp4", "av01.0.08M.08", "none", 1080, 30, 2000),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129), fmt("139", "m4a", "none", "mp4a.40.5", 0, 0, 48),
        fmt("251", "webm", "none", "opus", 0, 0, 135), fmt("250", "webm", "none", "opus", 0, 0, 70),
    };
    return in;
}

static const MediaFormat *find(const SelectionInput &in, const std::string &id) {
    for (const MediaFormat &f : in.formats) if (f.id == id) return &f;
    return nullptr;
}

static CodecSupport cs(bool available, bool hardware, bool software) {
    CodecSupport s; s.available = available; s.hardware = hardware; s.software = software; return s;
}

// Software decoders for everything, including an AV1 element (dav1d) whose
// real-time self-test has not run. No hardware.
static DecoderCapabilities softwareAll() {
    DecoderCapabilities c;
    c.h264 = cs(true, false, true); c.vp9 = cs(true, false, true); c.av1 = cs(true, false, true);
    c.hevc = cs(true, false, true); c.vp8 = cs(true, false, true);
    return c;
}

// Assumed phone profile: droidvdec claims H.264 and VP9 and passed the PLY-20
// probe; software VP9 from libgstvpx; no software H.264; no AV1.
static DecoderCapabilities device() {
    DecoderCapabilities c;
    c.h264 = cs(true, true, false); c.vp9 = cs(true, true, true);
    c.hardwareDecodeVerified = true;
    return c;
}

static PlaybackPreferences hardwareOn() { PlaybackPreferences p; p.hardwareDecode = true; return p; }

static bool hasVideoBranch(const StreamCandidate &c) { return !c.video.url.empty() || !c.muxed.url.empty(); }

static void codecParsing() {
    CHECK(parseVideoCodec("avc1.640028") == VideoCodec::H264);
    CHECK(parseVideoCodec("vp09.00.40.08") == VideoCodec::Vp9);
    CHECK(parseVideoCodec("vp9") == VideoCodec::Vp9);
    CHECK(parseVideoCodec("av01.0.08M.08") == VideoCodec::Av1);
    CHECK(parseVideoCodec("none") == VideoCodec::None);
    CHECK(parseVideoCodec("") == VideoCodec::Unknown);
    CHECK(parseAudioCodec("mp4a.40.2") == AudioCodec::Aac);
    CHECK(parseAudioCodec("opus") == AudioCodec::Opus);
    CHECK(parseAudioCodec("none") == AudioCodec::None);
    CHECK(containerOf(fmt("251", "webm", "none", "opus", 0)) == Container::WebM);
    CHECK(containerOf(fmt("140", "m4a", "none", "mp4a.40.2", 0)) == Container::Mp4);
    CHECK(containerOf(fmt("95", "mp4", "avc1.4D401F", "mp4a.40.2", 720, 30, 1, Protocol::Hls)) == Container::Hls);
    CHECK(premiumFormatNote("1080p Premium") && !premiumFormatNote("1080p60"));
    CHECK(catalog::validVideoId("dQw4w9WgXcQ"));
    CHECK(!catalog::validVideoId("dQw4w9WgXc"));
    CHECK(!catalog::validVideoId("dQw4w9WgXc/"));
    CHECK(catalog::validChannelId("UC_gpi1zIV_AfdZ0Bg1Vcxaw"));
    CHECK(!catalog::validChannelId("XC_gpi1zIV_AfdZ0Bg1Vcxaw"));
}

// SEL-01: format ranking on a fixture with AV1, VP9 and H.264 at several heights.
static void sel01_codecPolicyRanksFormats() {
    const SelectionInput in = typical();
    PlaybackPreferences best; best.maxHeight = INT_MAX;
    // Software decode: 1080p at most, H.264 preferred, AV1 never chosen.
    auto chain = selectStreams(in, softwareAll(), best);
    CHECK(!chain.empty() && chain[0].video.id == "137");
    CHECK(chain.size() > 1 && chain[1].video.id == "248"); // other codec family next, still <= 1080
    for (const StreamCandidate &c : chain) CHECK(c.video.videoCodec != VideoCodec::Av1 && c.height <= 1080);
    // An available AV1 element alone is not enough: the self-test must pass.
    CHECK(!videoDecodable(*find(in, "399"), softwareAll(), best));
    DecoderCapabilities av1 = softwareAll(); av1.av1SelfTestPassed = true;
    CHECK(videoDecodable(*find(in, "399"), av1, best));
    SelectionInput av1Only;
    av1Only.formats = { *find(in, "399"), *find(in, "140") };
    CHECK(selectStreams(av1Only, softwareAll(), best).empty());
    CHECK(emptySelectionKind(av1Only, softwareAll(), best) == EmptySelection::NoDecodableVideo);
    CHECK(emptySelectionReason(av1Only, softwareAll(), best).find("codecs") != std::string::npos);
    // With AV1 verified it is decodable but ranks after H.264 and VP9.
    chain = selectStreams(in, av1, best);
    CHECK(!chain.empty() && chain[0].video.id == "137");
    // HEVC and VP8 are outside the policy even with decoders present.
    CHECK(!videoDecodable(fmt("hev", "mp4", "hvc1.1.6.L93.B0", "none", 720), softwareAll(), best));
    CHECK(!videoDecodable(fmt("vp8", "webm", "vp8", "none", 360), softwareAll(), best));
    // 1440p and above only with verified hardware decode, and only when raised.
    PlaybackPreferences hw = hardwareOn(); hw.maxHeight = INT_MAX;
    chain = selectStreams(in, device(), hw);
    CHECK(!chain.empty() && chain[0].video.id == "248");   // default hardware limit 1080
    hw.hardwareDecodeMaxHeight = 2160;
    chain = selectStreams(in, device(), hw);
    CHECK(!chain.empty() && chain[0].video.id == "313");
    DecoderCapabilities unverified = device(); unverified.hardwareDecodeVerified = false;
    chain = selectStreams(in, unverified, hw);
    CHECK(!chain.empty() && chain[0].video.id == "248");   // software VP9, capped at 1080
    PlaybackPreferences swBig = best; swBig.softwareDecodeMaxHeight = 2160;
    chain = selectStreams(in, softwareAll(), swBig);
    CHECK(!chain.empty() && chain[0].height == 1080);      // software never above 1080
    // Audio is Opus or AAC only.
    SelectionInput vorbis;
    vorbis.formats = { *find(in, "247"), fmt("172", "webm", "none", "vorbis", 0, 0, 192) };
    CHECK(selectStreams(vorbis, softwareAll(), PlaybackPreferences()).empty());
}

// SEL-02: ceilings per network class and decode mode, rung and tie rules.
static void sel02_qualityCeiling() {
    const SelectionInput in = typical();
    PlaybackPreferences prefs;
    CHECK(qualityCeiling(softwareAll(), prefs) == 720);
    CHECK(qualityCeiling(device(), prefs) == 720);         // hardware off by default (PLY-21)
    CHECK(qualityCeiling(device(), hardwareOn()) == 1080);
    DecoderCapabilities unverified = device(); unverified.hardwareDecodeVerified = false;
    CHECK(qualityCeiling(unverified, hardwareOn()) == 720);
    PlaybackPreferences mobile; mobile.network = NetworkClass::Mobile;
    CHECK(qualityCeiling(softwareAll(), mobile) == 480);
    // Software decode: 720p30 H.264 (not 720p60, not VP9).
    auto chain = selectStreams(in, softwareAll(), prefs);
    CHECK(!chain.empty() && chain[0].video.id == "136");
    // Verified hardware decode: 1080p, VP9 preferred.
    chain = selectStreams(in, device(), hardwareOn());
    CHECK(!chain.empty() && chain[0].video.id == "248");
    // The device has no software H.264: in software mode only VP9 is decodable.
    chain = selectStreams(in, device(), prefs);
    CHECK(chain.size() == 1 && chain[0].video.id == "247");
    // Switching from Wi-Fi to mobile applies the mobile ceiling to the next selection.
    PlaybackPreferences session;
    session.network = NetworkClass::Unmetered;
    chain = selectStreams(in, softwareAll(), session);
    CHECK(!chain.empty() && chain[0].height == 720);
    session.network = NetworkClass::Mobile;
    chain = selectStreams(in, softwareAll(), session);
    CHECK(!chain.empty() && chain[0].video.id == "134");   // highest rung <= 480 is 360p
    CHECK(!chain.empty() && chain.back().muxed.id == "18");
    chain = selectStreams(in, device(), [] { PlaybackPreferences p = hardwareOn(); p.network = NetworkClass::Mobile; return p; }());
    CHECK(!chain.empty() && chain[0].video.id == "243");   // VP9 with hardware
    // Nothing at or below the ceiling: the lowest rung.
    SelectionInput tall;
    tall.formats = { *find(in, "136"), *find(in, "137"), *find(in, "251") };
    chain = selectStreams(tall, softwareAll(), mobile);
    CHECK(!chain.empty() && chain[0].video.id == "136");
    // Equal height prefers the lower frame rate; high-frame-rate and Premium are never Auto.
    SelectionInput rungs;
    MediaFormat premium = fmt("616", "webm", "vp09.00.40.08", "none", 1080, 30, 6000); premium.premium = true;
    rungs.formats = { *find(in, "299"), *find(in, "303"), premium, *find(in, "137"), *find(in, "251") };
    PlaybackPreferences p1080; p1080.maxHeight = 1080;
    chain = selectStreams(rungs, softwareAll(), p1080);
    CHECK(!chain.empty() && chain[0].video.id == "137");
    for (const StreamCandidate &c : chain) CHECK(!c.video.premium && !c.video.highFrameRate());
    // Only high-frame-rate rungs offered: the lower frame rate wins.
    SelectionInput hfr;
    hfr.formats = { fmt("a", "webm", "vp09.00.40.08", "none", 720, 60), fmt("b", "webm", "vp09.00.40.08", "none", 720, 50),
                    *find(in, "251") };
    chain = selectStreams(hfr, softwareAll(), prefs);
    CHECK(!chain.empty() && chain[0].video.id == "b");
    // A requested height replaces the ceiling; below every offer takes the lowest rung.
    PlaybackPreferences noHfr; noHfr.allowHighFrameRate = false;
    chain = selectStreams(in, softwareAll(), noHfr, 1080);
    CHECK(!chain.empty() && chain[0].video.id == "137");
    chain = selectStreams(in, softwareAll(), noHfr, 240);
    CHECK(!chain.empty() && chain[0].video.id == "134");
    // A codec override wins over the decode-mode preference.
    PlaybackPreferences vp9; vp9.codec = CodecPreference::Vp9;
    chain = selectStreams(in, softwareAll(), vp9);
    CHECK(!chain.empty() && chain[0].video.id == "247");
}

// SEL-03: one entry per (height, high-fps, Premium), highlight, Auto, no muxed menu.
static void sel03_qualityMenu() {
    SelectionInput in;
    MediaFormat premiumVp9 = fmt("616", "webm", "vp09.00.40.08", "none", 1080, 30, 6000); premiumVp9.premium = true;
    MediaFormat premiumAvc = fmt("721", "mp4", "avc1.640028", "none", 1080, 30, 7000); premiumAvc.premium = true;
    in.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("136", "mp4", "avc1.4D401F", "none", 720), fmt("247", "webm", "vp09.00.31.08", "none", 720),
        fmt("137", "mp4", "avc1.640028", "none", 1080), fmt("248", "webm", "vp09.00.40.08", "none", 1080),
        fmt("299", "mp4", "avc1.64002A", "none", 1080, 60), fmt("303", "webm", "vp09.00.41.08", "none", 1080, 60),
        premiumVp9, premiumAvc,
        fmt("251", "webm", "none", "opus", 0, 0, 135),
    };
    const DecoderCapabilities caps = softwareAll();
    PlaybackPreferences prefs;
    const std::vector<QualityOption> options = qualityOptions(in, caps, prefs);
    CHECK(options.size() == 4);
    if (options.size() == 4) {
        CHECK(options[0].label == "1080p Premium" && options[0].premium);
        CHECK(options[1].label == "1080p60" && options[1].highFrameRate);
        CHECK(options[2].label == "1080p" && !options[2].premium && !options[2].highFrameRate);
        CHECK(options[3].label == "720p");
        for (const QualityOption &o : options) CHECK(o.formatIds.size() == 2 && o.kind == StreamKind::Adaptive);
        CHECK(options[2].formatId == "137");                 // software decode prefers H.264
    }
    // Auto plays the SEL-02 choice and the menu highlights it.
    auto chain = selectStreams(in, caps, prefs);
    CHECK(!chain.empty() && chain[0].kind == StreamKind::Adaptive && chain[0].video.id == "136");
    QualityMenu menu = qualityMenu(in, caps, prefs, chain[0]);
    CHECK(menu.entries.size() == 4 && menu.autoIndex == 3 && menu.playingIndex == 3);
    PlaybackPreferences p1080; p1080.maxHeight = 1080;
    CHECK(qualityMenu(in, caps, p1080, chain[0]).autoIndex == 2);
    // Explicit choices: 1080p60 and Premium are reachable only this way.
    if (options.size() == 4) {
        chain = selectStreams(in, caps, prefs, qualityChoice(options[1]));
        CHECK(!chain.empty() && chain[0].video.id == options[1].formatId && chain[0].video.highFrameRate());
        menu = qualityMenu(in, caps, prefs, chain[0]);
        CHECK(menu.playingIndex == 1 && menu.autoIndex == 3);
        chain = selectStreams(in, caps, prefs, qualityChoice(options[0]));
        CHECK(!chain.empty() && chain[0].video.premium);
        CHECK(chain.size() > 1 && chain[1].video.premium && chain[1].video.videoCodec != chain[0].video.videoCodec);
    }
    // The highlight follows the playing format id, whichever codec member it is.
    QualityChoice vp9; vp9.formatId = "248";
    chain = selectStreams(in, caps, prefs, vp9);
    CHECK(!chain.empty() && chain[0].video.id == "248");
    CHECK(qualityMenu(in, caps, prefs, chain[0]).playingIndex == 2);
    // No menu for a muxed play plan, a muxed-only offer or listen-only mode.
    CHECK(!chain.empty() && chain.back().kind == StreamKind::Progressive);
    CHECK(qualityMenu(in, caps, prefs, chain.back()).entries.empty());
    PlaybackPreferences muxedOnly; muxedOnly.allowAdaptive = false;
    CHECK(qualityOptions(in, caps, muxedOnly).empty());
    SelectionInput noAudio = in; noAudio.formats.pop_back();
    CHECK(qualityOptions(noAudio, caps, prefs).empty());
    PlaybackPreferences listen; listen.mode = PlaybackMode::AudioOnly;
    CHECK(qualityOptions(in, caps, listen).empty());
    // allowHighFrameRate=false hides the 60 fps entry.
    PlaybackPreferences noHfr; noHfr.allowHighFrameRate = false;
    CHECK(qualityOptions(in, caps, noHfr).size() == 3);
}

// SEL-04: the four keys apply in order, each breaking only ties left by the one before.
static void sel04_audioRanking() {
    std::vector<MediaFormat> tracks;
    const char *classes[] = { "desc", "dub", "orig" };
    const int preference[] = { -10, -1, 10 };
    for (int l = 0; l < 3; ++l)
        for (int codec = 0; codec < 2; ++codec)
            for (int drc = 0; drc < 2; ++drc)
                for (int rate = 0; rate < 2; ++rate) {
                    const bool opus = codec == 0;
                    // AAC gets the higher bitrates, so Opus must win on key 2 alone.
                    const double kbps = opus ? (rate ? 160 : 64) : (rate ? 256 : 192);
                    const std::string id = std::string(classes[l]) + (opus ? "-opus" : "-aac") + (drc ? "-drc" : "") + (rate ? "-hi" : "-lo");
                    tracks.push_back(audioTrack(id.c_str(), opus ? "webm" : "m4a", opus ? "opus" : "mp4a.40.2", kbps,
                                                l == 2 ? "en" : "de", preference[l], drc != 0));
                }
    const std::vector<MediaFormat> ranked = rankAudio(tracks);
    CHECK(ranked.size() == 24);
    if (ranked.size() != 24) return;
    CHECK(ranked[0].id == "orig-opus-hi");
    for (std::size_t i = 0; i < 24; ++i) {
        CHECK(ranked[i].languagePreference == (i < 8 ? 10 : i < 16 ? -1 : -10));      // key 1
        CHECK((ranked[i].audioCodec == AudioCodec::Opus) == (i % 8 < 4));            // key 2
        CHECK(ranked[i].dynamicRangeCompressed == (i % 4 >= 2));                     // key 3
        if (i % 2 == 0) CHECK(ranked[i].audioKbps > ranked[i + 1].audioKbps);        // key 4
    }
    CHECK(descriptiveTrack(ranked[23]) && !descriptiveTrack(ranked[0]));
    // The dual plan takes the same top pick.
    SelectionInput in;
    in.formats = tracks;
    in.formats.push_back(fmt("247", "webm", "vp09.00.31.08", "none", 720));
    auto chain = selectStreams(in, softwareAll(), PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].audio.id == "orig-opus-hi");
}

// SEL-04 stable volume: bestEntryTrack keeps to one dub-picker entry, takes its DRC track first when
// asked (and only when the entry has one), then the requested codec, then the SEL-04 keys.
static void sel04_stableVolumeEntryTrack() {
    const std::vector<MediaFormat> pool = {
        audioTrack("en-opus", "webm", "opus", 135, "en", 10), audioTrack("en-opus-drc", "webm", "opus", 130, "en", 10, true),
        audioTrack("en-aac", "m4a", "mp4a.40.2", 129, "en", 10), audioTrack("en-aac-drc", "m4a", "mp4a.40.2", 128, "en", 10, true),
        audioTrack("de-opus", "webm", "opus", 135, "de", -1), audioTrack("de-aac-drc", "m4a", "mp4a.40.2", 128, "de", -1, true),
        audioTrack("en-ad", "webm", "opus", 135, "en", -10), audioTrack("pt-br", "webm", "opus", 135, "pt-BR", -1),
    };
    const MediaFormat *f = bestEntryTrack(pool, "en", false, false);
    CHECK(f && f->id == "en-opus");                      // SEL-04: non-DRC Opus
    f = bestEntryTrack(pool, "en", false, true);
    CHECK(f && f->id == "en-opus-drc");                  // stable volume: the DRC track of the entry
    f = bestEntryTrack(pool, "en", false, true, AudioCodec::Aac);
    CHECK(f && f->id == "en-aac-drc");                   // the candidate's codec next
    f = bestEntryTrack(pool, "de", false, true, AudioCodec::Opus);
    CHECK(f && f->id == "de-aac-drc");                   // DRC before the codec
    f = bestEntryTrack(pool, "de", false, false, AudioCodec::Opus);
    CHECK(f && f->id == "de-opus");
    f = bestEntryTrack(pool, "en", true, true);
    CHECK(f && f->id == "en-ad");                        // no DRC in the entry: its best track
    f = bestEntryTrack(pool, "PT_br", false, false);
    CHECK(f && f->id == "pt-br");                        // normalized tags
    CHECK(!bestEntryTrack(pool, "fr", false, true));     // not in the set
    CHECK(!bestEntryTrack(std::vector<MediaFormat>(), "en", false, false));
}

// SEL-05: one entry per language, original first and marked; the Polish case.
static void sel05_dubsPolishCase() {
    const DecoderCapabilities caps = softwareAll();
    const MediaFormat video = fmt("247", "webm", "vp09.00.31.08", "none", 720);
    // Video A: original Polish, English dub, each in Opus and AAC.
    SelectionInput a;
    a.formats = { video, audioTrack("251-0", "webm", "opus", 135, "pl", 10), audioTrack("140-0", "m4a", "mp4a.40.2", 129, "pl", 10),
                  audioTrack("251-1", "webm", "opus", 135, "en", -1), audioTrack("140-1", "m4a", "mp4a.40.2", 129, "en", -1) };
    const std::vector<AudioTrackOption> optionsA = audioTrackOptions(a, caps);
    CHECK(optionsA.size() == 2);
    if (optionsA.size() != 2) return;
    CHECK(optionsA[0].language == "pl" && optionsA[0].original && optionsA[0].label == "pl (original)");
    CHECK(optionsA[1].language == "en" && !optionsA[1].original && optionsA[1].formatId == "251-1");
    CHECK(optionsA[0].formatIds.size() == 2);
    auto chain = selectStreams(a, caps, PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].audio.id == "251-0");
    CHECK(audioTrackIndex(optionsA, chain[0].audio.id) == 0);
    // The user picks the original: "no preference" is stored, not "pl".
    const std::string stored = storedLanguagePreference(optionsA[0]);
    CHECK(stored.empty());
    // Video B: original English with a Polish dub starts in English.
    SelectionInput b;
    b.formats = { video, audioTrack("251-0", "webm", "opus", 135, "en", 10), audioTrack("251-1", "webm", "opus", 135, "pl", -1) };
    PlaybackPreferences prefs; prefs.audioLanguage = stored;
    chain = selectStreams(b, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.language == "en");
    // The FinTube 1.5 regression, for contrast: storing "pl" starts B dubbed.
    prefs.audioLanguage = "pl";
    chain = selectStreams(b, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.language == "pl");
    // Picking a dub remembers its language.
    CHECK(storedLanguagePreference(optionsA[1]) == "en");
    // Exact BCP-47 first, then the base language, case- and separator-insensitive.
    SelectionInput c;
    c.formats = { video, audioTrack("o", "webm", "opus", 135, "en", 10), audioTrack("pt", "webm", "opus", 135, "pt-PT", -1),
                  audioTrack("br", "webm", "opus", 135, "pt-BR", -1) };
    prefs.audioLanguage = "PT_br";
    chain = selectStreams(c, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "br");
    c.formats.pop_back();
    chain = selectStreams(c, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "pt");
    prefs.audioLanguage = "es";
    chain = selectStreams(c, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "o");
    // Descriptive tracks get their own marked entry, last, and never match a remembered language.
    SelectionInput d;
    d.formats = { video, audioTrack("o", "webm", "opus", 135, "pl", 10), audioTrack("ad", "webm", "opus", 135, "en", -10),
                  audioTrack("dub", "webm", "opus", 135, "de", -1) };
    const std::vector<AudioTrackOption> optionsD = audioTrackOptions(d, caps);
    CHECK(optionsD.size() == 3 && optionsD[0].original && optionsD[1].language == "de"
          && optionsD[2].descriptive && optionsD[2].label == "en (described)");
    prefs.audioLanguage = "en";
    chain = selectStreams(d, caps, prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "o");
    CHECK(normalizedLanguage("pt_BR") == "pt-br" && baseLanguage("pt-BR") == "pt");
}

// SEL-08: listen-only builds no video branch and plays the SEL-04/05 best audio.
static void sel08_listenOnlyPicksBestAudio() {
    PlaybackPreferences prefs; prefs.mode = PlaybackMode::AudioOnly;
    auto chain = selectStreams(typical(), softwareAll(), prefs);
    CHECK(chain.size() == 2);
    CHECK(!chain.empty() && chain[0].kind == StreamKind::AudioOnly && chain[0].audio.id == "251");
    CHECK(chain.size() > 1 && chain[1].audio.id == "140");      // other codec as second attempt
    for (const StreamCandidate &c : chain) CHECK(c.kind == StreamKind::AudioOnly && !hasVideoBranch(c));
    CHECK(!chain.empty() && chain[0].audioContainer == Container::WebM && chain[0].videoContainer == Container::None);
    // Opus before AAC even at a lower bitrate.
    SelectionInput low;
    low.formats = { fmt("250", "webm", "none", "opus", 0, 0, 70), fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129) };
    chain = selectStreams(low, softwareAll(), prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "250");
    // A remembered dub applies in listen-only mode too.
    SelectionInput dubbed;
    dubbed.formats = { audioTrack("o", "webm", "opus", 135, "en", 10), audioTrack("d", "webm", "opus", 135, "es", -1) };
    prefs.audioLanguage = "es";
    chain = selectStreams(dubbed, softwareAll(), prefs);
    CHECK(!chain.empty() && chain[0].audio.id == "d");
    prefs.audioLanguage.clear();
    // A muxed stream only when no audio-only format exists.
    SelectionInput muxedOnly;
    muxedOnly.formats = { fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
                          fmt("22", "mp4", "avc1.64001F", "mp4a.40.2", 720, 30, 1500),
                          fmt("136", "mp4", "avc1.4D401F", "none", 720) };
    chain = selectStreams(muxedOnly, softwareAll(), prefs);
    CHECK(chain.size() == 1 && chain[0].kind == StreamKind::AudioOnly && chain[0].muxed.id == "18");
}

// Dual pairs: WebM or MP4 per branch, container hints, muxed fallback, no DASH dependency.
static void dualPairsAnyContainer() {
    DecoderCapabilities caps = softwareAll();
    caps.adaptive = false; // no DASH demuxer: dual does not need one
    auto chain = selectStreams(typical(), caps, PlaybackPreferences());
    CHECK(chain.size() == 3);
    if (chain.size() == 3) {
        CHECK(chain[0].kind == StreamKind::Adaptive && chain[0].video.id == "136" && chain[0].audio.id == "251");
        CHECK(chain[0].videoContainer == Container::Mp4 && chain[0].audioContainer == Container::WebM);
        CHECK(chain[0].muxedContainer == Container::None && !chain[0].bypassProxyAndProbe);
        CHECK(chain[0].label == "720p H.264 + Opus (dual)");
        CHECK(chain[1].kind == StreamKind::Adaptive && chain[1].video.id == "247");
        CHECK(chain[1].videoContainer == Container::WebM);
        CHECK(chain[2].kind == StreamKind::Progressive && chain[2].muxed.id == "18");
        CHECK(chain[2].muxedContainer == Container::Mp4 && chain[2].videoContainer == Container::None);
    }
    caps.adaptive = true;
    auto same = selectStreams(typical(), caps, PlaybackPreferences());
    CHECK(same.size() == chain.size() && !same.empty() && same[0].video.id == chain[0].video.id);
    // The device in software mode: an all-WebM pair (VP9 + Opus).
    chain = selectStreams(typical(), device(), PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].videoContainer == Container::WebM && chain[0].audioContainer == Container::WebM);
    // Without Opus the pair uses AAC in MP4.
    caps.opus = false;
    chain = selectStreams(typical(), caps, PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].audio.id == "140" && chain[0].audioContainer == Container::Mp4);
    // Without a WebM demuxer neither VP9 nor Opus is offered.
    caps = softwareAll(); caps.webmDemux = false;
    chain = selectStreams(typical(), caps, PlaybackPreferences());
    CHECK(chain.size() == 2 && chain[0].audio.id == "140" && chain[1].kind == StreamKind::Progressive);
    // Dual disabled: muxed only.
    PlaybackPreferences off; off.allowAdaptive = false;
    chain = selectStreams(typical(), softwareAll(), off);
    CHECK(chain.size() == 1 && chain[0].muxed.id == "18");
    // HDR rungs are skipped.
    SelectionInput hdr = typical();
    for (MediaFormat &f : hdr.formats) if (f.id == "136") f.highDynamicRange = true;
    chain = selectStreams(hdr, softwareAll(), PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].video.id == "247");
}

static SelectionInput liveOffer() {
    SelectionInput in;
    in.liveStatus = LiveStatus::IsLive;
    in.manifestUrl = "https://manifest.example/master.m3u8";
    const char *ids[] = { "91", "92", "93", "94", "95", "96" };
    const int heights[] = { 144, 240, 360, 480, 720, 1080 };
    for (int i = 0; i < 6; ++i)
        in.formats.push_back(fmt(ids[i], "mp4", "avc1.4D401F", "mp4a.40.2", heights[i], 30, 300.0 * (i + 1), Protocol::Hls));
    in.formats.push_back(fmt("300", "mp4", "avc1.4D4020", "mp4a.40.2", 720, 60, 3000, Protocol::Hls));
    in.formats.push_back(fmt("229", "mp4", "avc1.4D401F", "none", 720, 30, 2000, Protocol::Hls)); // split-audio HLS
    in.formats.push_back(fmt("233", "mp4", "none", "mp4a.40.2", 0, 0, 48, Protocol::Hls));
    return in;
}

// RES-19: running livestreams play muxed HLS (91-96 first), unproxied and unprobed.
static void res19_liveAndUpcoming() {
    const SelectionInput in = liveOffer();
    auto chain = selectStreams(in, softwareAll(), PlaybackPreferences());
    CHECK(chain.size() == 3);
    if (chain.size() == 3) {
        CHECK(chain[0].kind == StreamKind::Live && chain[0].muxed.id == "95");
        CHECK(chain[1].muxed.id == "300");
        CHECK(chain[2].muxed.url == in.manifestUrl);
    }
    for (const StreamCandidate &c : chain) {
        CHECK(c.kind == StreamKind::Live && c.bypassProxyAndProbe && c.muxedContainer == Container::Hls);
        CHECK(c.muxed.id != "229" && c.video.url.empty() && c.audio.url.empty());
    }
    PlaybackPreferences mobile; mobile.network = NetworkClass::Mobile;
    chain = selectStreams(in, softwareAll(), mobile);
    CHECK(!chain.empty() && chain[0].muxed.id == "94");
    CHECK(qualityOptions(in, softwareAll(), PlaybackPreferences()).empty());
    // Listen-only on a live stream: the audio rendition.
    PlaybackPreferences listen; listen.mode = PlaybackMode::AudioOnly;
    chain = selectStreams(in, softwareAll(), listen);
    CHECK(chain.size() == 1 && chain[0].muxed.id == "233" && chain[0].bypassProxyAndProbe);
    CHECK(leadingItag("95") == 95 && leadingItag("95-1") == 95 && leadingItag("95x") == 0 && leadingItag("hls") == 0);
    CHECK(classicMuxedHlsItag("91") && classicMuxedHlsItag("96-0"));
    CHECK(!classicMuxedHlsItag("90") && !classicMuxedHlsItag("97") && !classicMuxedHlsItag("300"));
    // Upcoming: no candidates, its scheduled start, no resolve before then.
    SelectionInput upcoming = in;
    upcoming.liveStatus = LiveStatus::Upcoming;
    upcoming.scheduledStartUnix = 1800000000;
    CHECK(selectStreams(upcoming, softwareAll(), PlaybackPreferences()).empty());
    CHECK(emptySelectionKind(upcoming, softwareAll(), PlaybackPreferences()) == EmptySelection::Upcoming);
    CHECK(emptySelectionReason(upcoming, softwareAll(), PlaybackPreferences()).find("not started") != std::string::npos);
    CHECK(!mayResolve(upcoming.scheduledStartUnix - 60, upcoming.scheduledStartUnix));
    CHECK(mayResolve(upcoming.scheduledStartUnix, upcoming.scheduledStartUnix));
    CHECK(mayResolve(1700000000, 0));
}

// RES-09: a post-live recording with only segment formats is "still processing".
static void res09_postLiveStillProcessing() {
    SelectionInput in;
    in.liveStatus = LiveStatus::PostLive;
    in.formats = { fmt("137", "mp4", "avc1.640028", "none", 1080, 30, 3000, Protocol::Dash),
                   fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129, Protocol::Dash) };
    in.formats[1].url.clear(); // fragments only
    CHECK(selectStreams(in, softwareAll(), PlaybackPreferences()).empty());
    CHECK(stillProcessing(in.liveStatus, in.formats));
    CHECK(emptySelectionKind(in, softwareAll(), PlaybackPreferences()) == EmptySelection::StillProcessing);
    CHECK(emptySelectionReason(in, softwareAll(), PlaybackPreferences()).find("still being processed") != std::string::npos);
    // Not for a normal video, nor once a direct format is offered.
    CHECK(!stillProcessing(LiveStatus::NotLive, in.formats));
    SelectionInput ready = in;
    ready.formats.push_back(fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500));
    CHECK(!stillProcessing(ready.liveStatus, ready.formats));
    auto chain = selectStreams(ready, softwareAll(), PlaybackPreferences());
    CHECK(chain.size() == 1 && chain[0].muxed.id == "18");
    // A just-ended stream offered only as HLS still plays through the live path.
    SelectionInput hlsOnly = liveOffer();
    hlsOnly.liveStatus = LiveStatus::PostLive;
    chain = selectStreams(hlsOnly, softwareAll(), PlaybackPreferences());
    CHECK(!chain.empty() && chain[0].muxed.id == "95" && chain[0].bypassProxyAndProbe);
    SelectionInput empty;
    CHECK(emptySelectionKind(empty, softwareAll(), PlaybackPreferences()) == EmptySelection::NoFormats);
}

static std::vector<unsigned char> box(const char *type, std::size_t payload) {
    std::vector<unsigned char> b(8 + payload, 0);
    const std::size_t size = b.size();
    b[0] = (size >> 24) & 0xff; b[1] = (size >> 16) & 0xff; b[2] = (size >> 8) & 0xff; b[3] = size & 0xff;
    for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<unsigned char>(type[i]);
    return b;
}

static void mp4Index() {
    std::vector<unsigned char> file;
    for (auto part : { box("ftyp", 16), box("moov", 600), box("sidx", 100), box("moof", 40) })
        file.insert(file.end(), part.begin(), part.end());
    Mp4Index index = parseMp4Index(file.data(), file.size());
    CHECK(index.status == Mp4IndexStatus::Ok);
    CHECK(index.initializationEnd == 24 + 608 - 1);
    CHECK(index.indexStart == 24 + 608);
    CHECK(index.indexEnd == 24 + 608 + 108 - 1);
    Mp4Index partial = parseMp4Index(file.data(), 100);
    CHECK(partial.status == Mp4IndexStatus::NeedMoreData && partial.bytesNeeded > 100);
    std::vector<unsigned char> noIndex;
    for (auto part : { box("ftyp", 16), box("moov", 60), box("mdat", 100) })
        noIndex.insert(noIndex.end(), part.begin(), part.end());
    CHECK(parseMp4Index(noIndex.data(), noIndex.size()).status == Mp4IndexStatus::Invalid);
    std::vector<unsigned char> garbage(64, 0);
    CHECK(parseMp4Index(garbage.data(), garbage.size()).status == Mp4IndexStatus::Invalid);
}

static void manifest() {
    DashRepresentation v, a;
    v.id = "137"; v.mimeType = "video/mp4"; v.codecs = "avc1.640028"; v.url = "https://m.example/v?a=1&b=2";
    v.bandwidthBps = 4000000; v.width = 1920; v.height = 1080; v.frameRate = 29.97;
    v.initializationEnd = 740; v.indexStart = 741; v.indexEnd = 2000;
    a.id = "140"; a.mimeType = "audio/mp4"; a.codecs = "mp4a.40.2"; a.url = "https://m.example/a";
    a.bandwidthBps = 130000; a.audioSampleRate = 44100; a.initializationEnd = 600; a.indexStart = 601; a.indexEnd = 900;
    const std::string mpd = buildOnDemandMpd(212.5, v, a);
    CHECK(mpd.find("mediaPresentationDuration=\"PT212.500S\"") != std::string::npos);
    CHECK(mpd.find("v?a=1&amp;b=2") != std::string::npos);
    CHECK(mpd.find("a=1&b") == std::string::npos);
    CHECK(mpd.find("indexRange=\"741-2000\"") != std::string::npos);
    CHECK(mpd.find("<Initialization range=\"0-740\"/>") != std::string::npos);
    CHECK(mpd.find("frameRate=\"30\"") != std::string::npos);
    CHECK(mpd.find("audioSamplingRate=\"44100\"") != std::string::npos);
    CHECK(xmlEscape("<\"'&>") == "&lt;&quot;&apos;&amp;&gt;");
}



// Interim decoder rule (decision log 2026-11-08): H.264-only offer, droidvdec
// only, hardware decode off -> the item falls back to hardware; with a VP9
// software path present nothing changes.
static void interimHardwareFallback() {
    SelectionInput in;
    in.formats = {
        fmt("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, 30, 500),
        fmt("136", "mp4", "avc1.4D401F", "none", 720),
        fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129),
    };
    DecoderCapabilities droidOnly;
    droidOnly.h264 = cs(true, true, false); // hardware only, no software H.264
    droidOnly.vp9 = cs(true, false, true);
    PlaybackPreferences prefs; // hardware decode off (SET-01 default)
    CHECK(selectStreams(in, droidOnly, prefs).empty());
    const DecoderPlan plan = decoderPlanFor(in, droidOnly, prefs);
    CHECK(plan.hardwareFallback);
    const auto chain = selectStreamsWithFallback(in, droidOnly, prefs, QualityChoice());
    CHECK(!chain.empty() && chain[0].hardwareDecodeFallback);
    CHECK(!chain.empty() && chain[0].video.id == "136");
    CHECK(!chain.empty() && chain.back().muxed.id == "18");
    // A VP9 rung decodable in software: no fallback.
    in.formats.push_back(fmt("247", "webm", "vp09.00.31.08", "none", 720));
    in.formats.push_back(fmt("251", "webm", "none", "opus", 0, 0, 135));
    CHECK(!decoderPlanFor(in, droidOnly, prefs).hardwareFallback);
    const auto sw = selectStreamsWithFallback(in, droidOnly, prefs, QualityChoice());
    CHECK(!sw.empty() && !sw[0].hardwareDecodeFallback && sw[0].video.id == "247");
    // Listen-only never needs a video decoder.
    PlaybackPreferences listen; listen.mode = PlaybackMode::AudioOnly;
    CHECK(!decoderPlanFor(in, droidOnly, listen).hardwareFallback);
    // No hardware decoder at all: stays empty, no fallback.
    SelectionInput h264Only; h264Only.formats = { fmt("136", "mp4", "avc1.4D401F", "none", 720), fmt("140", "m4a", "none", "mp4a.40.2", 0, 0, 129) };
    DecoderCapabilities none; none.vp9 = cs(true, false, true);
    CHECK(!decoderPlanFor(h264Only, none, prefs).hardwareFallback);
}

// Core's quality API (PlaybackSession::setQuality(height, highFrameRate)):
// the >30 fps rung only when explicitly asked for, never automatic.
static void explicitHighFrameRate() {
    auto chain = selectStreams(typical(), softwareAll(), PlaybackPreferences(), 1080, true);
    CHECK(!chain.empty() && chain[0].video.id == "299");
    chain = selectStreams(typical(), softwareAll(), PlaybackPreferences(), 720, true);
    CHECK(!chain.empty() && chain[0].video.id == "298");
    chain = selectStreams(typical(), softwareAll(), PlaybackPreferences(), 1080);
    CHECK(!chain.empty() && chain[0].video.id == "137");
    PlaybackPreferences noHfr; noHfr.allowHighFrameRate = false;
    chain = selectStreams(typical(), softwareAll(), noHfr, 1080, true);
    CHECK(!chain.empty() && chain[0].video.id == "137");
}

// PLY-06 deep start: at equal height a WebM (VP9, Cues index) rung beats fragmented MP4 (H.264)
// with software decode; without the flag H.264 stays first (SEL-02).
static void ply06_deepStartPrefersIndexedContainer() {
    const SelectionInput in = typical();
    PlaybackPreferences p;
    auto chain = selectStreams(in, softwareAll(), p);
    CHECK(!chain.empty() && chain[0].video.videoCodec == VideoCodec::H264);
    p.preferSeekIndex = true;
    chain = selectStreams(in, softwareAll(), p);
    CHECK(!chain.empty() && chain[0].video.videoCodec == VideoCodec::Vp9 && containerOf(chain[0].video) == Container::WebM);
    CHECK(!chain.empty() && chain[0].height == 720);  // same ceiling (720p software)
    // An explicit codec choice wins over the deep-start preference.
    p.codec = CodecPreference::H264;
    chain = selectStreams(in, softwareAll(), p);
    CHECK(!chain.empty() && chain[0].video.videoCodec == VideoCodec::H264);
}

int main() {
    interimHardwareFallback();
    explicitHighFrameRate();
    ply06_deepStartPrefersIndexedContainer();
    codecParsing();
    sel01_codecPolicyRanksFormats();
    sel02_qualityCeiling();
    sel03_qualityMenu();
    sel04_audioRanking();
    sel04_stableVolumeEntryTrack();
    sel05_dubsPolishCase();
    sel08_listenOnlyPicksBestAudio();
    dualPairsAnyContainer();
    res19_liveAndUpcoming();
    res09_postLiveStillProcessing();
    mp4Index();
    manifest();
    std::printf("domain tests: %s (%d checks, %d failure%s)\n", failures ? "FAIL" : "PASS", checks, failures,
                failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
