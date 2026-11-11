// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// NFR-TST-06: Qt-free tests of the playback component's domain functions that had
// no test yet (Core/Media/Domain, Core/Player/Domain). Each test is named
// after the requirement the function's header cites and asserts the
// behaviour documented there. Build: tests/player_api_domain/run.sh
#include "Core/Media/Domain/AudioTrackPolicy.h"
#include "Core/Media/Domain/LivePolicy.h"
#include "Core/Media/Domain/MediaFormat.h"
#include "Core/Media/Domain/StreamSelection.h"
#include "Core/Player/Domain/AudioChain.h"
#include "Core/Player/Domain/AutoplayPolicy.h"
#include "Core/Player/Domain/BufferingPolicy.h"
#include "Core/Player/Domain/PipelinePlan.h"
#include "Core/Player/Domain/PositionPolicy.h"
#include "Core/Player/Domain/PrerollBarrier.h"
#include "Core/Player/Domain/SeekPolicy.h"
#include "Core/Player/Domain/SessionArbiter.h"
#include "Core/Player/Domain/TempDirPolicy.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

namespace media = nordtile::video::media;
namespace player = nordtile::player;

static int failures = 0;
static int checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static bool contains(const std::string &haystack, const std::string &needle) {
    return haystack.find(needle) != std::string::npos;
}

// ---- Core/Media/Domain ---------------------------------------------------------------

static media::MediaFormat audioFormat(const char *id, const char *ext, const char *codec, double kbps,
                                      const char *language = "", int preference = -1, bool drc = false) {
    media::MediaFormat f;
    f.id = id;
    f.container = ext;
    f.protocol = media::Protocol::Https;
    f.url = std::string("https://media.example/") + id;
    f.videoCodecTag = "none";
    f.audioCodecTag = codec;
    f.videoCodec = media::VideoCodec::None;
    f.audioCodec = media::parseAudioCodec(codec);
    f.audioKbps = kbps;
    f.totalKbps = kbps;
    f.language = language;
    f.languagePreference = preference;
    f.dynamicRangeCompressed = drc;
    return f;
}

static media::MediaFormat videoFormat(const char *id, const char *ext, const char *vcodec, const char *acodec,
                                      int height, media::Protocol protocol = media::Protocol::Https) {
    media::MediaFormat f;
    f.id = id;
    f.container = ext;
    f.protocol = protocol;
    f.url = std::string("https://media.example/") + id;
    f.videoCodecTag = vcodec;
    f.audioCodecTag = acodec;
    f.videoCodec = media::parseVideoCodec(vcodec);
    f.audioCodec = media::parseAudioCodec(acodec);
    f.height = height;
    f.width = height * 16 / 9;
    f.fps = 30;
    f.totalKbps = 1000;
    return f;
}

// SEL-04: the four ranking keys in order, each breaking only the ties of the
// one before, and a total order through the format id.
static void sel04_audioRanksBeforeKeys() {
    using media::audioRanksBefore;
    // (1) Language preference first: an original AAC beats a dubbed Opus of higher bitrate.
    const media::MediaFormat original = audioFormat("140", "m4a", "mp4a.40.2", 128, "en", 10);
    const media::MediaFormat dub = audioFormat("251-1", "webm", "opus", 160, "de", -1);
    CHECK(audioRanksBefore(original, dub));
    CHECK(!audioRanksBefore(dub, original));
    const media::MediaFormat described = audioFormat("251-2", "webm", "opus", 160, "en", -10);
    CHECK(audioRanksBefore(dub, described));
    // (2) Same language class: Opus before AAC even at a lower bitrate.
    const media::MediaFormat opusLow = audioFormat("250", "webm", "opus", 70, "en", 10);
    const media::MediaFormat aacHigh = audioFormat("140", "m4a", "mp4a.40.2", 129, "en", 10);
    CHECK(audioRanksBefore(opusLow, aacHigh));
    CHECK(!audioRanksBefore(aacHigh, opusLow));
    // (3) Same codec: non-DRC before DRC, even at a lower bitrate.
    const media::MediaFormat plain = audioFormat("251", "webm", "opus", 120, "en", 10, false);
    const media::MediaFormat drc = audioFormat("251-drc", "webm", "opus", 135, "en", 10, true);
    CHECK(audioRanksBefore(plain, drc));
    CHECK(!audioRanksBefore(drc, plain));
    // (4) Bitrate, higher first.
    const media::MediaFormat high = audioFormat("251", "webm", "opus", 135, "en", 10);
    const media::MediaFormat low = audioFormat("250", "webm", "opus", 70, "en", 10);
    CHECK(audioRanksBefore(high, low));
    CHECK(!audioRanksBefore(low, high));
    // Equal on every key: the id decides, so exactly one direction holds; irreflexive.
    const media::MediaFormat a = audioFormat("a", "webm", "opus", 100, "en", 10);
    const media::MediaFormat b = audioFormat("b", "webm", "opus", 100, "en", 10);
    CHECK(audioRanksBefore(a, b) != audioRanksBefore(b, a));
    CHECK(!audioRanksBefore(a, a));
}

// SEL-04 + SEL-05: the remembered language picks exact, then base language,
// then the original; descriptive tracks never match; empty pool -> nullptr.
static void sel05_bestAudioTrackForPreference() {
    CHECK(media::bestAudioTrack(std::vector<media::MediaFormat>(), "en") == nullptr);
    CHECK(media::bestAudioTrack(std::vector<media::MediaFormat>(), "") == nullptr);
    std::vector<media::MediaFormat> pool;
    pool.push_back(audioFormat("orig", "webm", "opus", 130, "en", 10));
    pool.push_back(audioFormat("pt-br", "webm", "opus", 130, "pt-BR", -1));
    pool.push_back(audioFormat("pt-pt", "webm", "opus", 130, "pt-PT", -1));
    pool.push_back(audioFormat("es-desc", "webm", "opus", 130, "es", -10));
    pool.push_back(audioFormat("fr-aac", "m4a", "mp4a.40.2", 128, "fr", -1));
    pool.push_back(audioFormat("fr-opus", "webm", "opus", 120, "fr", -1));
    const media::MediaFormat *t = media::bestAudioTrack(pool, "");
    CHECK(t && t->id == "orig");                     // no preference: the original
    t = media::bestAudioTrack(pool, "pt_BR");
    CHECK(t && t->id == "pt-br");                    // exact BCP-47 after normalisation
    t = media::bestAudioTrack(pool, "pt");
    CHECK(t && (t->id == "pt-br" || t->id == "pt-pt") && t->language.compare(0, 2, "pt") == 0);  // base language
    t = media::bestAudioTrack(pool, "pt-AO");
    CHECK(t && t->language.compare(0, 2, "pt") == 0);  // no exact: base language
    t = media::bestAudioTrack(pool, "es");
    CHECK(t && t->id == "orig");                     // only a described track: never chosen
    t = media::bestAudioTrack(pool, "ja");
    CHECK(t && t->id == "orig");                     // unknown language: the original
    t = media::bestAudioTrack(pool, "fr");
    CHECK(t && t->id == "fr-opus");                  // within the language, SEL-04 ranking (Opus first)
}

// RES-19: only muxed HLS with known codecs is a live candidate.
static void res19_muxedHlsOnlyForHlsWithBothKnownCodecs() {
    CHECK(media::muxedHls(videoFormat("95", "mp4", "avc1.4D401F", "mp4a.40.2", 720, media::Protocol::Hls)));
    CHECK(!media::muxedHls(videoFormat("95", "mp4", "avc1.4D401F", "none", 720, media::Protocol::Hls)));      // video only
    CHECK(!media::muxedHls(videoFormat("233", "mp4", "none", "mp4a.40.2", 0, media::Protocol::Hls)));        // audio only
    CHECK(!media::muxedHls(videoFormat("18", "mp4", "avc1.42001E", "mp4a.40.2", 360, media::Protocol::Https))); // not HLS
    CHECK(!media::muxedHls(videoFormat("95", "mp4", "", "mp4a.40.2", 720, media::Protocol::Hls)));            // unknown video codec
    CHECK(!media::muxedHls(videoFormat("95", "mp4", "avc1.4D401F", "ac-3", 720, media::Protocol::Hls)));      // unknown audio codec
}

// PLY-04: container names used in hints and diagnostics.
static void ply04_containerNames() {
    CHECK(std::string(media::containerName(media::Container::None)) == "none");
    CHECK(std::string(media::containerName(media::Container::Mp4)) == "mp4");
    CHECK(std::string(media::containerName(media::Container::WebM)) == "webm");
    CHECK(std::string(media::containerName(media::Container::Hls)) == "hls");
    CHECK(std::string(media::containerName(media::Container::Other)) == "other");
    // Together with containerOf(): what a player hint for a branch reads.
    CHECK(std::string(media::containerName(media::containerOf(audioFormat("251", "webm", "opus", 130)))) == "webm");
    CHECK(std::string(media::containerName(media::containerOf(audioFormat("140", "m4a", "mp4a.40.2", 128)))) == "mp4");
}

// SEL-01: a software decoder exists when set, or when the only decoder is not hardware.
static void sel01_softwareAvailable() {
    media::CodecSupport s;
    CHECK(!s.softwareAvailable());                 // nothing at all
    s.available = true;
    CHECK(s.softwareAvailable());                  // a decoder that is not hardware
    s.hardware = true;
    CHECK(!s.softwareAvailable());                 // only the hardware decoder (no software H.264)
    s.software = true;
    CHECK(s.softwareAvailable());                  // both
    media::CodecSupport only;
    only.software = true;
    CHECK(only.softwareAvailable());               // set explicitly
}

// PLY-20: hardware decode is active only when requested AND verified on the device.
static void ply20_hardwareDecodeActive() {
    media::DecoderCapabilities caps;
    media::PlaybackPreferences prefs;
    CHECK(!media::hardwareDecodeActive(caps, prefs));  // default: off
    prefs.hardwareDecode = true;
    CHECK(!media::hardwareDecodeActive(caps, prefs));  // requested, not verified
    caps.hardwareDecodeVerified = true;
    CHECK(media::hardwareDecodeActive(caps, prefs));
    prefs.hardwareDecode = false;
    CHECK(!media::hardwareDecodeActive(caps, prefs));  // verified, not requested
    // SEL-02 consequence: the Auto ceiling follows the decode mode (720p sw, 1080p hw).
    CHECK(media::qualityCeiling(caps, prefs) == 720);
    prefs.hardwareDecode = true;
    CHECK(media::qualityCeiling(caps, prefs) == 1080);
}

static bool hasId(const std::vector<media::MediaFormat> &v, const std::string &id) {
    for (size_t i = 0; i < v.size(); ++i)
        if (v[i].id == id) return true;
    return false;
}

// SEL-04: playable audio = Opus or AAC audio-only over direct HTTPS in a demuxable container.
static void sel04_playableAudioFormats() {
    media::SelectionInput in;
    in.formats.push_back(audioFormat("251", "webm", "opus", 135));
    in.formats.push_back(audioFormat("140", "m4a", "mp4a.40.2", 129));
    in.formats.push_back(audioFormat("171", "webm", "vorbis", 128));               // SEL-01: Opus/AAC only
    media::MediaFormat hls = audioFormat("233", "mp4", "mp4a.40.2", 64);
    hls.protocol = media::Protocol::Hls;
    in.formats.push_back(hls);                                                     // HLS audio: not a dual branch
    media::MediaFormat nourl = audioFormat("250", "webm", "opus", 70);
    nourl.url.clear();
    in.formats.push_back(nourl);                                                   // no URL
    media::MediaFormat dash = audioFormat("249", "webm", "opus", 50);
    dash.protocol = media::Protocol::Dash;
    in.formats.push_back(dash);                                                    // not direct HTTPS
    in.formats.push_back(videoFormat("18", "mp4", "avc1.42001E", "mp4a.40.2", 360)); // muxed: not audio-only
    in.formats.push_back(videoFormat("137", "mp4", "avc1.640028", "none", 1080));    // video-only
    media::DecoderCapabilities caps;
    std::vector<media::MediaFormat> out = media::playableAudioFormats(in, caps);
    CHECK(out.size() == 2);
    CHECK(hasId(out, "251") && hasId(out, "140"));
    // Capabilities gate it: no Opus decoder, or no WebM demuxer, drops the Opus track.
    caps.opus = false;
    out = media::playableAudioFormats(in, caps);
    CHECK(out.size() == 1 && hasId(out, "140"));
    caps.opus = true;
    caps.webmDemux = false;
    out = media::playableAudioFormats(in, caps);
    CHECK(out.size() == 1 && hasId(out, "140"));
    caps.webmDemux = true;
    caps.mp4Demux = false;
    out = media::playableAudioFormats(in, caps);
    CHECK(out.size() == 1 && hasId(out, "251"));
    CHECK(media::playableAudioFormats(media::SelectionInput(), media::DecoderCapabilities()).empty());
}

// ---- Core/Player/Domain ---------------------------------------------------------------

// PLY-22: every band clamped to the UI range, NaN -> 0 dB, in-range kept.
static void ply22_clampEqGains() {
    player::EqGains in = {{-30, -12, -6.5, 0, 3, 12, 12.5, 40, std::numeric_limits<double>::quiet_NaN(), 7}};
    const player::EqGains out = player::clampEqGains(in);
    CHECK(out[0] == player::kEqMinDb);
    CHECK(out[1] == -12);
    CHECK(out[2] == -6.5);
    CHECK(out[3] == 0);
    CHECK(out[4] == 3);
    CHECK(out[5] == 12);
    CHECK(out[6] == player::kEqMaxDb);
    CHECK(out[7] == player::kEqMaxDb);
    CHECK(out[8] == 0 && !std::isnan(out[8]));
    CHECK(out[9] == 7);
    const player::EqGains flat = player::clampEqGains(player::flatEq());
    bool allZero = true;
    for (int i = 0; i < player::kEqBandCount; ++i) allZero = allZero && flat[i] == 0;
    CHECK(allZero);
}

// PLY-25: clearing the queue drops every item, the current index and a running countdown.
static void ply25_clearQueue() {
    player::AutoplayPolicy p;
    std::vector<std::string> ids;
    ids.push_back("aaaaaaaaaaa"); ids.push_back("bbbbbbbbbbb"); ids.push_back("ccccccccccc");
    p.setQueue(ids, 1);
    CHECK(p.hasQueue() && p.canGoNext() && p.currentIndex() == 1);
    p.clearQueue();
    CHECK(p.queue().empty());
    CHECK(p.currentIndex() == -1);
    CHECK(!p.hasQueue() && !p.canGoNext() && !p.canGoPrevious());
    // Without a queue the natural end falls back to the related countdown (PLY-25)...
    p.setRelatedCandidate("rrrrrrrrrrr");
    player::AutoplayDecision d = p.onEnded(1000, true);
    CHECK(d.kind == player::AutoplayDecision::StartCountdown && d.videoId == "rrrrrrrrrrr");
    CHECK(p.countdownActive());
    // ...and clearQueue cancels that countdown too.
    p.clearQueue();
    CHECK(!p.countdownActive());
    CHECK(p.nextDeadlineMs() == -1);
    CHECK(p.advance(7000).kind == player::AutoplayDecision::None);
}

// PLY-08: the user intent is tracked separately from buffering and survives reset().
static void ply08_wantsPlayingTracksUserIntent() {
    player::BufferingPolicy b;
    CHECK(!b.wantsPlaying());
    b.reset(true, false);
    b.onUserIntent(true);
    CHECK(b.wantsPlaying());
    // A buffering pause does not change the intent; a user play while buffering only records it.
    player::BufferingAction a = b.onBufferingPercent(player::Branch::Video, 40);
    CHECK(a.pausePipeline && b.buffering() && b.wantsPlaying());
    a = b.onUserIntent(false);
    CHECK(!b.wantsPlaying() && a.pausePipeline);
    a = b.onUserIntent(true);
    CHECK(b.wantsPlaying() && !a.resumePipeline);  // still buffering
    a = b.onBufferingPercent(player::Branch::Video, 100);
    CHECK(a.resumePipeline && !b.buffering());
    // reset() clears levels but keeps the intent.
    b.reset(true, false);
    CHECK(b.wantsPlaying());
    b.onUserIntent(false);
    b.reset(false, false);
    CHECK(!b.wantsPlaying());
}

static player::PlanInput localInput() {
    player::PlanInput in;
    in.mode = player::PlayMode::Local;
    in.muxed.uri = "file:///media/clip.webm";
    in.muxed.container = player::Container::Matroska;
    const std::vector<std::string> all = player::elementsToProbe();
    in.available = std::set<std::string>(all.begin(), all.end());
    in.available.erase("droidvdec");
    in.available.erase("droideglsink");
    return in;
}

// PLY-20: the per-build fallback input: elements present gate the probe and the hardware mode.
static void ply20_hardwareElementsPresent() {
    player::DecodeModePolicy p("device|os|gst|app");
    CHECK(!p.hardwareElementsPresent());
    p.setHardwareRequested(true);
    CHECK(!p.needsProbe());                          // no droid elements: never probe
    CHECK(p.modeForVideo("v") == player::DecodeMode::Software);
    p.setHardwareElementsPresent(true);
    CHECK(p.hardwareElementsPresent());
    CHECK(p.needsProbe());
    p.onProbeStarted(0);
    CHECK(p.onProbeFrame(500) == player::DecodeEvent::ProbePassed);
    CHECK(p.modeForVideo("v") == player::DecodeMode::Hardware);
    p.setHardwareElementsPresent(false);             // e.g. an OS update removed gst-droid
    CHECK(!p.hardwareElementsPresent());
    CHECK(p.modeForVideo("v") == player::DecodeMode::Software);
}

// PLY-01 diagnostics: describePlan renders every branch and route gst-launch style.
static void ply01_describePlan() {
    CHECK(player::describePlan(player::PipelinePlan()).empty());
    const player::PipelinePlan local = player::buildPipelinePlan(localInput());
    CHECK(local.ok);
    const std::string d = player::describePlan(local);
    CHECK(d.compare(0, std::string("uridecodebin name=muxed-src").size(), "uridecodebin name=muxed-src") == 0);
    CHECK(contains(d, " ! "));
    CHECK(contains(d, "name=video-sink") && contains(d, "name=audio-sink"));
    CHECK(contains(d, " muxed-src. ! "));            // the second route of the same source
    CHECK(!contains(d, " | "));                      // one branch
    // Every element of every route appears as "factory name=name"; the
    // unmatched-pad template is made on demand and is not part of the line.
    for (size_t r = 0; r < local.branches[0].routes.size(); ++r) {
        const std::vector<player::ElementPlan> &chain = local.branches[0].routes[r].chain;
        for (size_t e = 0; e < chain.size(); ++e) CHECK(contains(d, chain[e].factory + " name=" + chain[e].name));
    }
    CHECK(!contains(d, "unmatched-sink"));
    player::PlanInput dual = localInput();
    dual.mode = player::PlayMode::Dual;
    dual.video.uri = "file:///media/v.webm";
    dual.video.container = player::Container::Matroska;
    dual.audio.uri = "file:///media/a.webm";
    dual.audio.container = player::Container::Matroska;
    dual.muxed = player::SourceSpec();
    const player::PipelinePlan dp = player::buildPipelinePlan(dual);
    CHECK(dp.ok);
    const std::string dd = player::describePlan(dp);
    CHECK(contains(dd, "name=video-src") && contains(dd, " | ") && contains(dd, "name=audio-src"));
    CHECK(dd.find("name=video-src") < dd.find(" | "));
}

// PLT-12: pulsesink stream-properties carry media.role as a quoted string.
static void plt12_streamPropertiesStructure() {
    CHECK(player::streamPropertiesStructure("x-maemo") == "props, media.role=(string)\"x-maemo\"");
    CHECK(player::streamPropertiesStructure("") == "props, media.role=(string)\"\"");
    CHECK(player::streamPropertiesStructure("a\"b\\c") == "props, media.role=(string)\"a\\\"b\\\\c\"");
    // The plan applies the same structure to the pulse sink.
    const player::PipelinePlan p = player::buildPipelinePlan(localInput());
    const player::ElementPlan *sink = p.find(player::elementnames::kAudioSink);
    CHECK(sink && sink->factory == "pulsesink");
    CHECK(sink && sink->property("stream-properties") == player::streamPropertiesStructure(player::kDefaultMediaRole));
}

// PLY-24: durationKnown only for a pipeline duration, never for the metadata fallback.
static void ply24_durationKnown() {
    player::PositionPolicy p;
    CHECK(!p.durationKnown() && p.needsDurationPoll());
    p.setMetadataDurationMs(60000);
    CHECK(p.durationMs() == 60000);
    CHECK(!p.durationKnown());                       // metadata only: keep polling
    CHECK(p.needsDurationPoll());
    p.onDurationQuery(false, 59000);
    CHECK(!p.durationKnown());
    p.onDurationQuery(true, 0);
    CHECK(!p.durationKnown());
    p.onDurationQuery(true, 59500);
    CHECK(p.durationKnown() && !p.needsDurationPoll() && p.durationMs() == 59500);
    // Paused with a known duration: no timer (PLY-24).
    CHECK(p.pollIntervalMs(true, true, false) == 0);
    p.reset(true);
    CHECK(!p.durationKnown() && p.durationMs() == 60000);
}

// PLY-24: while seeking, the target is shown and position queries are ignored.
static void ply24_seekingHoldsTheTarget() {
    player::PositionPolicy p;
    p.onDurationQuery(true, 100000);
    p.onPositionQuery(true, 1000);
    CHECK(!p.seeking());
    p.onSeekRequested(50000);
    CHECK(p.seeking() && p.positionMs() == 50000);
    p.onPositionQuery(true, 1200);                   // stale query: ignored
    CHECK(p.positionMs() == 50000);
    p.onSeekLanded(true, 48000);
    CHECK(!p.seeking() && p.positionMs() == 48000);
    p.onSeekRequested(70000);
    CHECK(p.seeking());
    p.onSeekRefused();
    CHECK(!p.seeking());
    p.onPositionQuery(true, 48500);                  // queries resume
    CHECK(p.positionMs() == 48500);
    p.onSeekRequested(90000);
    p.onEos();
    CHECK(!p.seeking() && p.ended() && p.positionMs() == 100000);
    p.onSeekRequested(10000);
    p.reset();
    CHECK(!p.seeking());
}

// PLY-06/PLY-07: the rate held until the first async-done, sanitised.
static void ply07_pendingRate() {
    player::PrerollBarrier b;
    CHECK(b.pendingRate() == 1.0);
    b.arm(0, 1.5);
    CHECK(b.pendingRate() == 1.5);
    CHECK(b.deferRate(2.0));
    CHECK(b.pendingRate() == 2.0);                   // latest wins
    CHECK(b.deferRate(-3.0));
    CHECK(b.pendingRate() == 1.0);                   // <= 0 -> 1.0
    CHECK(b.deferRate(std::numeric_limits<double>::quiet_NaN()));
    CHECK(b.pendingRate() == 1.0);                   // NaN -> 1.0
    CHECK(b.deferRate(1.25));
    player::DeferredSeek d;
    CHECK(b.onAsyncDone(&d));
    CHECK(d.rate == 1.25);
    CHECK(!b.deferRate(0.5));                        // after preroll: nothing deferred
    CHECK(b.pendingRate() == 1.25);
    b.arm(0, 0);
    CHECK(b.pendingRate() == 1.0);
    b.arm(0, std::numeric_limits<double>::quiet_NaN());
    CHECK(b.pendingRate() == 1.0);
}

// PLY-03: per-container seek flags; ISO-BMFF never accurate.
static void ply03_seekFlagsFor() {
    const player::SeekFlags mkv = player::seekFlagsFor(player::Container::Matroska);
    CHECK(mkv == player::SeekFlags::flushAccurate());
    CHECK(mkv.flush && mkv.accurate && !mkv.keyUnit);
    const player::SeekFlags mp4 = player::seekFlagsFor(player::Container::IsoBmff);
    CHECK(mp4 == player::SeekFlags::flushKeyUnit());
    CHECK(mp4.flush && !mp4.accurate && mp4.keyUnit);
    const player::SeekFlags unknown = player::seekFlagsFor(player::Container::Unknown);
    CHECK(unknown == player::SeekFlags::flushKeyUnit());  // the safe flags
    // planSeek uses the same table for an AudioOnly branch.
    player::SeekRequest r;
    r.mode = player::PlayMode::AudioOnly;
    r.audio = player::Container::Matroska;
    r.targetMs = 1000;
    const player::SeekPlan plan = player::planSeek(r);
    CHECK(plan.now.size() == 1 && plan.now[0].flags == player::seekFlagsFor(player::Container::Matroska));
}

// PLY-10: the top of the context stack is the most recently opened context.
static void ply10_topContext() {
    player::SessionArbiter s;
    CHECK(s.topContext() == player::kNoContext);
    s.open(1, "aaaaaaaaaaa");
    CHECK(s.topContext() == 1);
    s.open(2, "bbbbbbbbbbb");
    CHECK(s.topContext() == 2 && s.contextCount() == 2);
    s.open(1, "ccccccccccc");                        // reusing an id changes it in place
    CHECK(s.topContext() == 2 && s.contextCount() == 2);
    s.activate(1);                                   // reclaiming ownership does not reorder
    CHECK(s.topContext() == 2 && s.owner() == 1);
    s.open(player::kAppContext, "ddddddddddd");      // the app context is never a page
    CHECK(s.topContext() == 2);
    s.close(2, player::PlaybackSnapshot(false, 0));
    CHECK(s.topContext() == 1);
    s.close(1, player::PlaybackSnapshot(false, 0));
    CHECK(s.topContext() == player::kNoContext && s.contextCount() == 0);
}

// PLT-09: lexical normalisation of absolute paths.
static void plt09_normalizeAbsolutePath() {
    using player::normalizeAbsolutePath;
    CHECK(normalizeAbsolutePath("").empty());
    CHECK(normalizeAbsolutePath("relative/path").empty());
    CHECK(normalizeAbsolutePath("./tmp").empty());
    CHECK(normalizeAbsolutePath("/") == "/");
    CHECK(normalizeAbsolutePath("//") == "/");
    CHECK(normalizeAbsolutePath("/a//b/") == "/a/b");
    CHECK(normalizeAbsolutePath("/a/./b/../c") == "/a/c");
    CHECK(normalizeAbsolutePath("/..") == "/");
    CHECK(normalizeAbsolutePath("/a/b/../../..") == "/");
    CHECK(normalizeAbsolutePath("/home/u/.cache/x/../tmp/./spool") == "/home/u/.cache/tmp/spool");
    // The RAM-tree check relies on it: a dotted escape into /tmp is caught.
    CHECK(normalizeAbsolutePath("/home/../tmp/x") == "/tmp/x");
    CHECK(!player::isAllowedSpoolLocation("/home/../tmp/x"));
}

int main() {
    sel04_audioRanksBeforeKeys();
    sel05_bestAudioTrackForPreference();
    res19_muxedHlsOnlyForHlsWithBothKnownCodecs();
    ply04_containerNames();
    sel01_softwareAvailable();
    ply20_hardwareDecodeActive();
    sel04_playableAudioFormats();
    ply22_clampEqGains();
    ply25_clearQueue();
    ply08_wantsPlayingTracksUserIntent();
    ply20_hardwareElementsPresent();
    ply01_describePlan();
    plt12_streamPropertiesStructure();
    ply24_durationKnown();
    ply24_seekingHoldsTheTarget();
    ply07_pendingRate();
    ply03_seekFlagsFor();
    ply10_topContext();
    plt09_normalizeAbsolutePath();
    std::printf("player api domain tests: %d checks, %d failure%s\n", checks, failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
