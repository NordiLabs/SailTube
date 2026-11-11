// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Captions (SEL-06), SponsorBlock fetch (SB-01) and Return YouTube Dislike (RYD-01), offline
// against the recorded fixtures. Variants derived in code from a fixture say so where they
// are made.

#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/Captions.h"
#include "Core/Catalog/Domain/ExpiringCache.h"
#include "Core/Catalog/Domain/SponsorSegments.h"
#include "Core/Catalog/Infrastructure/CaptionService.h"
#include "Core/Catalog/Infrastructure/DislikeService.h"
#include "Core/Catalog/Infrastructure/SponsorBlockService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"
#include "Clients/Sailfish/Presentation/CaptionCues.h"
#include "Clients/Sailfish/Presentation/CaptionTracksModel.h"
#include "Clients/Sailfish/Presentation/DislikeInfo.h"
#include "Clients/Sailfish/Presentation/SponsorSegmentsModel.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSettings>
#include <QUrl>
#include <QFile>
#include <QSet>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <memory>

using namespace nordtile::video::catalog;
using namespace nordtile::video::sailfish;
using catalog_test::FakeTransport;
using catalog_test::fixture;
using catalog_test::fixtureJson;
using catalog_test::waitFor;

namespace {

const char *const kVideo = "9zCH37330f8";

std::string oc(Outcome o) { return outcomeName(o); }
int sz(std::size_t n) { return static_cast<int>(n); }

QByteArray toBytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }
QByteArray toBytes(const QJsonArray &a) { return QJsonDocument(a).toJson(QJsonDocument::Compact); }

std::vector<CaptionCue> parse(const QByteArray &body, Outcome *outcome = nullptr) {
    std::vector<CaptionCue> cues;
    const Outcome o = CaptionService::parseJson3(body, &cues);
    if (outcome) *outcome = o;
    return cues;
}

// Independent count of json3 events whose joined text is not blank.
int nonEmptyEvents(const QJsonObject &doc) {
    int n = 0;
    for (const QJsonValue &v : doc.value(QStringLiteral("events")).toArray()) {
        QString t;
        for (const QJsonValue &s : v.toObject().value(QStringLiteral("segs")).toArray()) t += s.toObject().value(QStringLiteral("utf8")).toString();
        if (!t.simplified().isEmpty()) ++n;
    }
    return n;
}

int rawOverlaps(const QJsonObject &doc) {
    int n = 0;
    qint64 lastEnd = -1;
    for (const QJsonValue &v : doc.value(QStringLiteral("events")).toArray()) {
        const QJsonObject e = v.toObject();
        if (!e.contains(QStringLiteral("segs"))) continue;
        QString t;
        for (const QJsonValue &s : e.value(QStringLiteral("segs")).toArray()) t += s.toObject().value(QStringLiteral("utf8")).toString();
        if (t.simplified().isEmpty()) continue;
        const qint64 start = static_cast<qint64>(e.value(QStringLiteral("tStartMs")).toDouble());
        if (lastEnd > start) ++n;
        lastEnd = start + static_cast<qint64>(e.value(QStringLiteral("dDurationMs")).toDouble(4000));
    }
    return n;
}

void checkCueInvariants(const std::vector<CaptionCue> &cues) {
    int shortCues = 0, overlaps = 0, unsorted = 0, badText = 0;
    for (std::size_t i = 0; i < cues.size(); ++i) {
        const CaptionCue &c = cues[i];
        if (c.endMs - c.startMs < 50) ++shortCues;
        if (c.text.empty() || c.text.find('\n') != std::string::npos || c.text.find("  ") != std::string::npos ||
            c.text[0] == ' ' || c.text[c.text.size() - 1] == ' ') ++badText;
        if (i + 1 < cues.size()) {
            if (c.endMs > cues[i + 1].startMs) ++overlaps;
            if (c.startMs > cues[i + 1].startMs) ++unsorted;
        }
    }
    CHECK_EQ(shortCues, 0);
    CHECK_EQ(overlaps, 0);
    CHECK_EQ(unsorted, 0);
    CHECK_EQ(badText, 0);
}

QJsonObject playerVariant(const QJsonArray &tracks, const QJsonArray &languages) {
    // Derived from player_visionos: the caption list replaced by the given tracks/languages.
    QJsonObject list;
    list.insert(QStringLiteral("captionTracks"), tracks);
    list.insert(QStringLiteral("translationLanguages"), languages);
    QJsonObject captions;
    captions.insert(QStringLiteral("playerCaptionsTracklistRenderer"), list);
    QJsonObject player = fixtureJson(QStringLiteral("player_visionos.json"));
    player.insert(QStringLiteral("captions"), captions);
    return player;
}

QJsonObject trackJson(const QString &code, const QString &name, bool asr = false, bool translatable = true) {
    QJsonObject t;
    t.insert(QStringLiteral("baseUrl"), QStringLiteral("https://www.youtube.com/api/timedtext?v=XXXXXXXXXXX&caps=asr&lang=") + code +
             (asr ? QStringLiteral("&kind=asr") : QString()));
    QJsonObject runName;
    QJsonArray runs;
    QJsonObject run;
    run.insert(QStringLiteral("text"), name);
    runs.append(run);
    runName.insert(QStringLiteral("runs"), runs);
    t.insert(QStringLiteral("name"), runName);
    t.insert(QStringLiteral("languageCode"), code);
    t.insert(QStringLiteral("isTranslatable"), translatable);
    if (asr) t.insert(QStringLiteral("kind"), QStringLiteral("asr"));
    return t;
}

QJsonArray visionosLanguages() {
    return fixtureJson(QStringLiteral("player_visionos.json"))
        .value(QStringLiteral("captions")).toObject()
        .value(QStringLiteral("playerCaptionsTracklistRenderer")).toObject()
        .value(QStringLiteral("translationLanguages")).toArray();
}

int countOf(const std::string &haystack, const std::string &needle) {
    int n = 0;
    for (std::size_t p = haystack.find(needle); p != std::string::npos; p = haystack.find(needle, p + 1)) ++n;
    return n;
}

// ---------------------------------------------------------------- cues

void testManualJson3() {
    const QByteArray body = fixture(QStringLiteral("timedtext_manual_json3.json"));
    Outcome o = Outcome::Network;
    const std::vector<CaptionCue> cues = parse(body, &o);
    CHECK_EQ(oc(o), std::string("ok"));
    const int expected = nonEmptyEvents(QJsonDocument::fromJson(body).object());
    CHECK(expected >= 200);
    CHECK_EQ(sz(cues.size()), expected);   // manual track: no repeats, no short cues
    checkCueInvariants(cues);
}

void testAsrJson3() {
    const QByteArray body = fixture(QStringLiteral("timedtext_asr_json3.json"));
    const QJsonObject doc = QJsonDocument::fromJson(body).object();
    Outcome o = Outcome::Network;
    const std::vector<CaptionCue> cues = parse(body, &o);
    CHECK_EQ(oc(o), std::string("ok"));
    CHECK(rawOverlaps(doc) > 100);           // rolling ASR: each line overlaps the next
    CHECK_EQ(sz(cues.size()), nonEmptyEvents(doc));   // the "\n" append events are dropped as empty
    CHECK(cues.size() > 250);
    checkCueInvariants(cues);
    CHECK(cues.size() > 2 && cues[0].endMs == cues[1].startMs);   // a cue ends where the next begins
}

void testDefaultDuration() {
    // Derived from timedtext_manual_json3: the last event loses dDurationMs.
    QJsonObject doc = QJsonDocument::fromJson(fixture(QStringLiteral("timedtext_manual_json3.json"))).object();
    QJsonArray events = doc.value(QStringLiteral("events")).toArray();
    QJsonObject last = events.last().toObject();
    last.remove(QStringLiteral("dDurationMs"));
    events.replace(events.size() - 1, last);
    doc.insert(QStringLiteral("events"), events);
    const std::vector<CaptionCue> cues = parse(toBytes(doc));
    CHECK(!cues.empty());
    CHECK_EQ(cues.back().endMs - cues.back().startMs, static_cast<std::int64_t>(4000));

    // Without a duration in the middle: 4 s, cut where the next cue begins.
    std::vector<CaptionEvent> ev;
    ev.push_back(CaptionEvent(0, -1, "first"));
    ev.push_back(CaptionEvent(10000, 1000, "second"));
    ev.push_back(CaptionEvent(11000, -1, "third"));
    ev.push_back(CaptionEvent(13000, 500, "fourth"));
    const std::vector<CaptionCue> c = buildCues(ev);
    CHECK_EQ(sz(c.size()), 4);
    if (c.size() == 4) {
        CHECK_EQ(c[0].endMs, static_cast<std::int64_t>(4000));
        CHECK_EQ(c[2].endMs, static_cast<std::int64_t>(13000));
    }
}

void testRollingMerge() {
    std::vector<CaptionEvent> ev;
    ev.push_back(CaptionEvent(0, 1000, "hello  world"));
    ev.push_back(CaptionEvent(1030, 970, "hello\nworld"));   // same line, 30 ms after: merged
    ev.push_back(CaptionEvent(2000, 1000, "next line"));
    ev.push_back(CaptionEvent(3200, 800, "next line"));      // repeated 200 ms later: kept apart
    const std::vector<CaptionCue> c = buildCues(ev);
    CHECK_EQ(sz(c.size()), 3);
    if (c.size() == 3) {
        CHECK_EQ(c[0].text, std::string("hello world"));
        CHECK_EQ(c[0].startMs, static_cast<std::int64_t>(0));
        CHECK_EQ(c[0].endMs, static_cast<std::int64_t>(2000));
        CHECK_EQ(c[2].startMs, static_cast<std::int64_t>(3200));
    }

    // Derived from timedtext_asr_json3: every text event is re-emitted 20 ms later (a rolling
    // window repeating its line). The cue list must not change in count.
    const QByteArray body = fixture(QStringLiteral("timedtext_asr_json3.json"));
    const std::vector<CaptionCue> original = parse(body);
    QJsonObject doc = QJsonDocument::fromJson(body).object();
    QJsonArray events = doc.value(QStringLiteral("events")).toArray();
    QJsonArray doubled;
    for (const QJsonValue &v : events) {
        doubled.append(v);
        QJsonObject e = v.toObject();
        if (!e.contains(QStringLiteral("segs"))) continue;
        e.insert(QStringLiteral("tStartMs"), e.value(QStringLiteral("tStartMs")).toDouble() + 20);
        doubled.append(e);
    }
    doc.insert(QStringLiteral("events"), doubled);
    const std::vector<CaptionCue> merged = parse(toBytes(doc));
    CHECK_EQ(sz(merged.size()), sz(original.size()));
    checkCueInvariants(merged);
}

void testShortAndEmpty() {
    std::vector<CaptionEvent> ev;
    ev.push_back(CaptionEvent(0, 30, "too short"));
    ev.push_back(CaptionEvent(100, 1000, "cut short"));
    ev.push_back(CaptionEvent(120, 1000, "kept"));       // the previous cue now lasts 20 ms: dropped
    ev.push_back(CaptionEvent(5000, 1000, " \n\t "));      // blank: dropped
    const std::vector<CaptionCue> c = buildCues(ev);
    CHECK_EQ(sz(c.size()), 1);
    if (!c.empty()) CHECK_EQ(c[0].text, std::string("kept"));

    Outcome o = Outcome::Ok;
    parse(QByteArray(), &o);
    CHECK_EQ(oc(o), std::string("unavailable"));          // timedtext refusing with an empty 200
    parse("<html>", &o);
    CHECK_EQ(oc(o), std::string("drift"));
    parse("{\"wireMagic\":\"pb3\"}", &o);
    CHECK_EQ(oc(o), std::string("empty"));
    parse("{\"wireMagic\":\"pb3\",\"events\":[]}", &o);
    CHECK_EQ(oc(o), std::string("empty"));
    parse("{\"other\":1}", &o);
    CHECK_EQ(oc(o), std::string("drift"));
}

void testCursor() {
    const std::vector<CaptionCue> cues = parse(fixture(QStringLiteral("timedtext_manual_json3.json")));
    CueCursor cursor;
    int mismatches = 0;
    for (std::int64_t ms = 0; ms < cues.back().endMs + 2000; ms += 250)
        if (cursor.at(cues, ms) != cueIndexAt(cues, ms)) ++mismatches;
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(cursor.rescans(), 0);
    const std::int64_t back = cues[10].startMs + 10;
    CHECK_EQ(cursor.at(cues, back), 10);                    // backward jump: rescanned
    CHECK_EQ(cursor.rescans(), 1);
    CHECK_EQ(cursor.at(cues, cues[200].startMs), 200);      // forward jump
    CHECK_EQ(cursor.at(cues, cues[200].startMs + 1), 200);
    CHECK_EQ(cursor.rescans(), 1);
    CHECK_EQ(cursor.at(cues, -5), -1);
    CHECK_EQ(cueIndexAt(cues, cues[0].startMs - 1), -1);
}

// ---------------------------------------------------------------- tracks

void testUrlRewrite() {
    CHECK_EQ(json3Url("https://h/api/timedtext?v=a&fmt=srv3&lang=en"), std::string("https://h/api/timedtext?v=a&lang=en&fmt=json3"));
    CHECK_EQ(json3Url("//h/api/timedtext?fmt=vtt"), std::string("https://h/api/timedtext?fmt=json3"));
    CHECK_EQ(json3Url("/api/timedtext?v=a"), std::string("https://www.youtube.com/api/timedtext?v=a&fmt=json3"));
    CHECK_EQ(translationUrl("https://h/t?v=a&tlang=de&fmt=json3", "fr"), std::string("https://h/t?v=a&tlang=fr&fmt=json3"));
    CHECK_EQ(baseLanguage("pt-BR"), std::string("pt"));
    CHECK_EQ(baseLanguage("en-orig"), std::string("en"));
    CHECK_EQ(baseLanguage("es-419"), std::string("es"));
    CHECK_EQ(baseLanguage("zh_Hant"), std::string("zh"));
}

void testTrackShaping() {
    const QJsonObject player = fixtureJson(QStringLiteral("player_visionos.json"));
    const CaptionTrackSet set = CaptionService::tracksFromPlayer(player);
    CHECK_EQ(sz(set.real.size()), 5);   // manual hr/en/it/ja + ASR en
    QStringList keys;
    int asr = 0, badUrls = 0;
    QSet<QString> realCodes;
    for (const CaptionTrack &t : set.real) {
        keys << QString::fromStdString(t.key);
        realCodes.insert(QString::fromStdString(t.code));
        if (t.autoGenerated) ++asr;
        if (countOf(t.url, "fmt=") != 1 || countOf(t.url, "fmt=json3") != 1 || t.url.find("tlang=") != std::string::npos) ++badUrls;
    }
    CHECK_EQ(keys.join(QLatin1Char(',')), QStringLiteral("hr,en,asr:en,it,ja"));
    CHECK_EQ(asr, 1);
    CHECK_EQ(badUrls, 0);

    const QJsonArray languages = visionosLanguages();
    int expected = 0;
    for (const QJsonValue &v : languages)
        if (!realCodes.contains(v.toObject().value(QStringLiteral("languageCode")).toString())) ++expected;
    CHECK(languages.size() >= 150);
    CHECK_EQ(sz(set.translations.size()), expected);   // dropped where a real track exists
    int clash = 0, badT = 0;
    for (const CaptionTrack &t : set.translations) {
        if (realCodes.contains(QString::fromStdString(t.code))) ++clash;
        if (!t.translation || t.url.find("tlang=" + t.code + "&") == std::string::npos ||
            countOf(t.url, "fmt=json3") != 1 || t.url.find("kind=asr") == std::string::npos || t.name.empty()) ++badT;
    }
    CHECK_EQ(clash, 0);
    CHECK_EQ(badT, 0);

    // Derived: a live_chat entry and a second ASR track for English are added.
    QJsonArray tracks = player.value(QStringLiteral("captions")).toObject()
        .value(QStringLiteral("playerCaptionsTracklistRenderer")).toObject().value(QStringLiteral("captionTracks")).toArray();
    tracks.append(trackJson(QStringLiteral("en"), QStringLiteral("English (auto 2)"), true));
    tracks.append(trackJson(QStringLiteral("live_chat"), QStringLiteral("Live chat")));
    const CaptionTrackSet more = CaptionService::tracksFromPlayer(playerVariant(tracks, languages));
    CHECK_EQ(sz(more.real.size()), 5);
    CHECK(!more.find("live_chat") && !more.find("tlang:live_chat"));

    // A video without captions.
    QJsonObject bare = player;
    bare.remove(QStringLiteral("captions"));
    CHECK(CaptionService::tracksFromPlayer(bare).empty());
}

void testYtDlpTracks() {
    // Synthetic yt-dlp info dict built from its documented fields (subtitles /
    // automatic_captions: {lang: [{ext, url, name}]}); no recorded yt-dlp fixture exists.
    const QString base = QStringLiteral("https://www.youtube.com/api/timedtext?v=9zCH37330f8&caps=asr");
    auto entry = [](const QString &ext, const QString &url, const QString &name) {
        QJsonObject e;
        e.insert(QStringLiteral("ext"), ext);
        e.insert(QStringLiteral("url"), url);
        e.insert(QStringLiteral("name"), name);
        return e;
    };
    QJsonObject subtitles;
    subtitles.insert(QStringLiteral("en"), QJsonArray() << entry(QStringLiteral("vtt"), base + QStringLiteral("&lang=en&fmt=vtt"), QStringLiteral("English"))
                                                      << entry(QStringLiteral("json3"), base + QStringLiteral("&lang=en&fmt=json3"), QStringLiteral("English")));
    subtitles.insert(QStringLiteral("live_chat"), QJsonArray() << entry(QStringLiteral("json"), QStringLiteral("https://www.youtube.com/watch?v=9zCH37330f8"), QString()));
    QJsonObject automatic;
    automatic.insert(QStringLiteral("en-orig"), QJsonArray() << entry(QStringLiteral("srv3"), base + QStringLiteral("&kind=asr&lang=en&fmt=srv3"), QStringLiteral("English (Original)")));
    automatic.insert(QStringLiteral("en"), QJsonArray() << entry(QStringLiteral("json3"), base + QStringLiteral("&kind=asr&lang=en&tlang=en&fmt=json3"), QStringLiteral("English")));
    automatic.insert(QStringLiteral("fr"), QJsonArray() << entry(QStringLiteral("json3"), base + QStringLiteral("&kind=asr&lang=en&tlang=fr&fmt=json3"), QStringLiteral("French")));
    automatic.insert(QStringLiteral("de"), QJsonArray() << entry(QStringLiteral("json3"), base + QStringLiteral("&kind=asr&lang=en&tlang=de&fmt=json3"), QStringLiteral("German")));
    QJsonObject info;
    info.insert(QStringLiteral("id"), QString::fromLatin1(kVideo));
    info.insert(QStringLiteral("subtitles"), subtitles);
    info.insert(QStringLiteral("automatic_captions"), automatic);

    const CaptionTrackSet set = CaptionService::tracksFromYtDlp(info);
    CHECK_EQ(sz(set.real.size()), 2);
    CHECK(set.find("en") && !set.find("en")->autoGenerated);
    CHECK(set.find("asr:en") && set.find("asr:en")->code == "en");
    if (set.find("asr:en")) CHECK_EQ(countOf(set.find("asr:en")->url, "fmt=json3"), 1);
    CHECK_EQ(sz(set.translations.size()), 2);   // de, fr; the "en" translation is dropped
    CHECK(set.find("tlang:fr") && set.find("tlang:de") && !set.find("tlang:en"));
    CHECK(!set.find("live_chat"));
    CHECK_EQ(autoSelectCaption(set, "en-orig").key, std::string("en"));
}

void testAutoSelectOrder() {
    const CaptionTrackSet set = CaptionService::tracksFromPlayer(fixtureJson(QStringLiteral("player_visionos.json")));
    struct Case { const char *pref; CaptionMatch match; const char *key; };
    const Case cases[] = {
        {"it", CaptionMatch::ExactReal, "it"},
        {"en", CaptionMatch::ExactReal, "en"},              // manual beats ASR
        {"fr", CaptionMatch::ExactTranslation, "tlang:fr"},
        {"pt-PT", CaptionMatch::ExactTranslation, "tlang:pt-PT"},
        {"en-GB", CaptionMatch::BaseReal, "en"},
        {"en-orig", CaptionMatch::BaseReal, "en"},
        {"ja_JP", CaptionMatch::BaseReal, "ja"},
        {"pt-BR", CaptionMatch::BaseTranslation, "tlang:pt"},   // pt beats pt-PT
        {"es-419", CaptionMatch::BaseTranslation, "tlang:es"},
        {"zh-TW", CaptionMatch::BaseTranslation, "tlang:zh-Hans"},
        {"xx", CaptionMatch::None, ""},
        {"off", CaptionMatch::None, ""},
        {"", CaptionMatch::None, ""},
    };
    int wrong = 0;
    for (const Case &c : cases) {
        const CaptionAutoPick p = autoSelectCaption(set, c.pref);
        if (p.match != c.match || p.key != c.key) {
            std::printf("  auto-select %s -> %s (match %d)\n", c.pref, p.key.c_str(), static_cast<int>(p.match));
            ++wrong;
        }
    }
    CHECK_EQ(wrong, 0);

    // Base matches among real tracks rank manual before ASR: ASR-only English vs manual en-US.
    QJsonArray tracks;
    tracks.append(trackJson(QStringLiteral("en"), QStringLiteral("English (auto-generated)"), true));
    tracks.append(trackJson(QStringLiteral("en-US"), QStringLiteral("English (United States)")));
    const CaptionTrackSet asrFirst = CaptionService::tracksFromPlayer(playerVariant(tracks, QJsonArray()));
    CHECK_EQ(autoSelectCaption(asrFirst, "en-GB").key, std::string("en-US"));
    CHECK_EQ(autoSelectCaption(asrFirst, "en").key, std::string("asr:en"));   // step 1 still wins
}

void testPersistence() {
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/p.conf"), QSettings::IniFormat);
    const QString key = QLatin1String(CaptionTracksModel::kSettingsKey);
    const QJsonObject player = fixtureJson(QStringLiteral("player_visionos.json"));

    CaptionTracksModel model(&settings);
    model.setFromPlayerAnswer(QString::fromLatin1(kVideo), player);
    CHECK(model.off());                              // never chosen: captions start off
    CHECK(!settings.contains(key));
    CHECK_EQ(model.count(), 5);

    // An auto match is never written back.
    settings.setValue(key, QStringLiteral("en-GB"));
    CaptionTracksModel autoModel(&settings);
    autoModel.setFromPlayerAnswer(QString::fromLatin1(kVideo), player);
    CHECK_EQ(autoModel.currentKey(), QStringLiteral("en"));
    CHECK(autoModel.lastMatch() == CaptionMatch::BaseReal);
    CHECK_EQ(settings.value(key).toString(), QStringLiteral("en-GB"));

    // An explicit pick persists its code; Off persists the sentinel.
    CHECK(autoModel.pick(QStringLiteral("asr:en")));
    CHECK_EQ(settings.value(key).toString(), QStringLiteral("en"));
    CHECK_EQ(autoModel.currentTrack().value(QStringLiteral("key")).toString(), QStringLiteral("asr:en"));
    CHECK(!autoModel.pick(QStringLiteral("tlang:nope")));
    autoModel.pickOff();
    CHECK(autoModel.off());
    CHECK_EQ(settings.value(key).toString(), QStringLiteral("off"));
    autoModel.setFromPlayerAnswer(QStringLiteral("XXXXXXXXXXX"), player);
    CHECK(autoModel.off());                          // Off carries to the next video
    CaptionTracksModel restarted(&settings);
    CHECK_EQ(restarted.preferred(), QStringLiteral("off"));

    // Translation filter: name substring or code prefix.
    CaptionTranslationsModel *tr = model.translationsModel();
    const int total = tr->count();
    tr->setFilter(QStringLiteral("germ"));
    CHECK_EQ(tr->count(), 1);
    CHECK_EQ(tr->get(0).value(QStringLiteral("code")).toString(), QStringLiteral("de"));
    tr->setFilter(QStringLiteral("PT"));
    bool hasPt = false, allMatch = true;
    for (int i = 0; i < tr->count(); ++i) {
        const QVariantMap m = tr->get(i);
        if (m.value(QStringLiteral("code")).toString() == QLatin1String("pt-PT")) hasPt = true;
        if (!m.value(QStringLiteral("name")).toString().contains(QStringLiteral("pt"), Qt::CaseInsensitive) &&
            !m.value(QStringLiteral("code")).toString().startsWith(QStringLiteral("pt"), Qt::CaseInsensitive)) allMatch = false;
    }
    CHECK(hasPt);
    CHECK(allMatch);
    tr->setFilter(QString());
    CHECK_EQ(tr->count(), total);
    CHECK(total > 140);
}

// §6 P9 as a decision test: pick a translation on one video, then open others.
void testP9Scenario() {
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/p9.conf"), QSettings::IniFormat);
    const QString key = QLatin1String(CaptionTracksModel::kSettingsKey);
    CaptionTracksModel model(&settings);
    model.setFromPlayerAnswer(QString::fromLatin1(kVideo), fixtureJson(QStringLiteral("player_visionos.json")));
    CHECK(model.pick(QStringLiteral("tlang:de")));
    CHECK(model.currentIsTranslation());
    CHECK_EQ(settings.value(key).toString(), QStringLiteral("de"));

    const QJsonArray languages = visionosLanguages();
    // Next videos are derived from player_visionos with other caption lists.
    struct Next { QJsonArray tracks; QJsonArray languages; CaptionMatch match; QString key; };
    std::vector<Next> next;
    {
        Next n;   // German as a real track: step 1
        n.tracks << trackJson(QStringLiteral("en"), QStringLiteral("English")) << trackJson(QStringLiteral("de"), QStringLiteral("German"));
        n.languages = languages; n.match = CaptionMatch::ExactReal; n.key = QStringLiteral("de");
        next.push_back(n);
    }
    {
        Next n;   // only de-DE real, "de" offered as a translation: step 2 beats step 3
        n.tracks << trackJson(QStringLiteral("en"), QStringLiteral("English")) << trackJson(QStringLiteral("de-DE"), QStringLiteral("German (Germany)"));
        n.languages = languages; n.match = CaptionMatch::ExactTranslation; n.key = QStringLiteral("tlang:de");
        next.push_back(n);
    }
    {
        Next n;   // de-AT real and no translations: step 3
        n.tracks << trackJson(QStringLiteral("en"), QStringLiteral("English"), false, false) << trackJson(QStringLiteral("de-AT"), QStringLiteral("German (Austria)"), false, false);
        n.match = CaptionMatch::BaseReal; n.key = QStringLiteral("de-AT");
        next.push_back(n);
    }
    {
        Next n;   // only de-CH as a translation: step 4
        QJsonObject ch; ch.insert(QStringLiteral("languageCode"), QStringLiteral("de-CH"));
        QJsonObject chName; chName.insert(QStringLiteral("simpleText"), QStringLiteral("Swiss German")); ch.insert(QStringLiteral("languageName"), chName);
        QJsonObject fr; fr.insert(QStringLiteral("languageCode"), QStringLiteral("fr"));
        n.tracks << trackJson(QStringLiteral("ja"), QStringLiteral("Japanese"));
        n.languages << fr << ch; n.match = CaptionMatch::BaseTranslation; n.key = QStringLiteral("tlang:de-CH");
        next.push_back(n);
    }
    {
        Next n;   // no German at all: off for this video
        n.tracks << trackJson(QStringLiteral("en"), QStringLiteral("English"), false, false);
        n.match = CaptionMatch::None; n.key = QString();
        next.push_back(n);
    }
    int wrong = 0;
    for (std::size_t i = 0; i < next.size(); ++i) {
        model.setFromPlayerAnswer(QStringLiteral("video%1xxxx").arg(i), playerVariant(next[i].tracks, next[i].languages));
        if (model.lastMatch() != next[i].match || model.currentKey() != next[i].key) {
            std::printf("  P9 video %d -> %s\n", static_cast<int>(i), qPrintable(model.currentKey()));
            ++wrong;
        }
    }
    CHECK_EQ(wrong, 0);
    CHECK_EQ(settings.value(key).toString(), QStringLiteral("de"));   // auto picks never written back
    CHECK_EQ(model.preferred(), QStringLiteral("de"));
}

// ---------------------------------------------------------------- cue fetching

void testCueCache() {
    FakeTransport net;
    net.on(QStringLiteral("lang=fail"), QByteArray(), 429, QByteArray(), 0, 1);
    net.on(QStringLiteral("api/timedtext"), QByteArray(), 200, fixture(QStringLiteral("timedtext_manual_json3.json")));
    WorkLane lane(QStringLiteral("test"));
    CaptionService service(&net, &lane);
    CatalogCounters counters;
    service.setCounters(&counters);
    QObject context;

    auto fetch = [&](const QString &url) {
        CueResult got;
        bool done = false;
        service.fetchCues(url, &context, [&](const CueResult &r) { got = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        return got;
    };
    auto url = [](int i) { return QStringLiteral("https://www.youtube.com/api/timedtext?v=9zCH37330f8&lang=l%1&fmt=json3").arg(i); };

    const CueResult first = fetch(url(0));
    CHECK_EQ(oc(first.outcome), std::string("ok"));
    CHECK(first.cues && first.cues->size() > 200);
    CHECK_EQ(net.requests.size(), 1);
    const HttpRequest &req = net.requests.first();
    CHECK_EQ(req.timeoutMs, 15000);
    bool browserUa = false, cookie = false;
    for (const auto &h : req.headers) {
        if (h.first == "User-Agent" && h.second.startsWith("Mozilla/5.0")) browserUa = true;
        if (h.first.toLower() == "cookie") cookie = true;
    }
    CHECK(browserUa);
    CHECK(!cookie);

    CHECK(fetch(url(0)).fromCache);
    CHECK_EQ(net.requests.size(), 1);
    for (int i = 1; i <= 11; ++i) fetch(url(i));   // 12 URLs: the cache is full
    CHECK(fetch(url(0)).fromCache);                // 0 becomes the most recently used
    fetch(url(12));                                // the 13th URL evicts one entry
    CHECK_EQ(net.requests.size(), 13);
    CHECK(!service.isCached(url(1)));            // least recently used (0 was read again)
    CHECK(service.isCached(url(0)));
    CHECK(service.isCached(url(12)));
    fetch(url(1));
    CHECK_EQ(net.requests.size(), 14);

    // Failures are never cached: 429, then a network error / timeout.
    const QString failing = QStringLiteral("https://www.youtube.com/api/timedtext?v=9zCH37330f8&lang=fail&fmt=json3");
    CHECK_EQ(oc(fetch(failing).outcome), std::string("http429"));
    CHECK(!service.isCached(failing));
    CHECK_EQ(oc(fetch(failing).outcome), std::string("ok"));
    CHECK(service.isCached(failing));
    const int before = net.requests.size();
    const QString unreachable = QStringLiteral("https://unreachable.example/api/x?lang=none");
    CHECK_EQ(oc(fetch(unreachable).outcome), std::string("network"));
    CHECK_EQ(oc(fetch(unreachable).outcome), std::string("network"));
    CHECK_EQ(net.requests.size(), before + 2);
    CHECK_EQ(counters.value(Area::Captions, Outcome::RateLimited), static_cast<std::int64_t>(1));
    CHECK(counters.value(Area::Captions, Outcome::Ok) >= 14);
}

void testCueLateAnswer() {
    FakeTransport net;
    net.on(QStringLiteral("lang=slow"), QByteArray(), 200, fixture(QStringLiteral("timedtext_manual_json3.json")), 300);
    net.on(QStringLiteral("lang=fast"), QByteArray(), 200, fixture(QStringLiteral("timedtext_asr_json3.json")), 10);
    CaptionService service(&net, nullptr);
    CaptionCues cues(&service);
    const QString slow = QStringLiteral("https://www.youtube.com/api/timedtext?v=9zCH37330f8&lang=slow&fmt=json3");
    const QString fast = QStringLiteral("https://www.youtube.com/api/timedtext?v=9zCH37330f8&lang=fast&fmt=json3");
    const int asrCount = sz(parse(fixture(QStringLiteral("timedtext_asr_json3.json"))).size());

    cues.load(slow);
    CHECK(cues.loading());
    cues.setPosition(1000);
    cues.load(fast);   // the user switched language before the first answer
    CHECK(waitFor([&] { return cues.ready(); }));
    CHECK(waitFor([&] { return service.isCached(slow); }, 2000));   // the late answer arrived...
    QCoreApplication::processEvents();
    CHECK_EQ(cues.url(), fast);                                      // ...and was dropped here
    CHECK_EQ(cues.count(), asrCount);
    CHECK(!cues.currentText().isEmpty());
    CHECK_EQ(cues.currentText(), cues.textAt(1000));

    // Positions: forward ticks and a backward jump keep currentText right.
    const std::vector<CaptionCue> &list = cues.cues();
    cues.setPosition(list[100].startMs + 5);
    CHECK_EQ(cues.currentText(), QString::fromStdString(list[100].text));
    cues.setPosition(list[3].startMs + 5);
    CHECK_EQ(cues.currentText(), QString::fromStdString(list[3].text));
    cues.clear();
    CHECK(!cues.ready());
    CHECK(cues.currentText().isEmpty());

    // Following a tracks model: an explicit pick loads that track.
    FakeTransport net2;
    net2.on(QStringLiteral("api/timedtext"), QByteArray(), 200, fixture(QStringLiteral("timedtext_manual_json3.json")));
    CaptionService service2(&net2, nullptr);
    CaptionCues followed(&service2);
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/f.conf"), QSettings::IniFormat);
    CaptionTracksModel tracks(&settings);
    followed.follow(&tracks);
    tracks.setFromPlayerAnswer(QString::fromLatin1(kVideo), fixtureJson(QStringLiteral("player_visionos.json")));
    CHECK(followed.url().isEmpty());
    tracks.pick(QStringLiteral("en"));
    CHECK(waitFor([&] { return followed.ready(); }));
    CHECK_EQ(followed.url(), tracks.currentUrl());
    CHECK(followed.url().contains(QStringLiteral("fmt=json3")));
    tracks.pickOff();
    CHECK(followed.url().isEmpty());
    CHECK(!followed.ready());
}

// ---------------------------------------------------------------- SponsorBlock

void testSha256() {
    const char *inputs[] = {"", "abc", "9zCH37330f8", "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"};
    int wrong = 0;
    for (const char *in : inputs) {
        const QByteArray reference = QCryptographicHash::hash(QByteArray(in), QCryptographicHash::Sha256).toHex();
        if (sha256Hex(in) != reference.toStdString()) ++wrong;
    }
    const std::string longInput(1000, 'a');
    if (sha256Hex(longInput) != QCryptographicHash::hash(QByteArray::fromStdString(longInput), QCryptographicHash::Sha256).toHex().toStdString()) ++wrong;
    CHECK_EQ(wrong, 0);
    CHECK_EQ(sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    const QByteArray independent = QCryptographicHash::hash(QByteArray(kVideo), QCryptographicHash::Sha256).toHex().left(4);
    CHECK_EQ(sponsorHashPrefix(kVideo), independent.toStdString());
    CHECK_EQ(sponsorHashPrefix(kVideo), std::string("5752"));   // the recorded request path
    CHECK_EQ(sz(sponsorCategories().size()), 8);
    CHECK_EQ(sponsorCategoriesJson(false),
             std::string("[\"sponsor\",\"selfpromo\",\"interaction\",\"intro\",\"outro\",\"preview\",\"filler\",\"music_offtopic\"]"));
}

// Independent filter of the recorded prefix answer for one id.
int expectedSegments(const QJsonArray &answer, const QString &id) {
    const QStringList categories = QStringList() << "sponsor" << "selfpromo" << "interaction" << "intro" << "outro" << "preview" << "filler" << "music_offtopic";
    int n = 0;
    for (const QJsonValue &v : answer) {
        if (v.toObject().value(QStringLiteral("videoID")).toString() != id) continue;
        for (const QJsonValue &s : v.toObject().value(QStringLiteral("segments")).toArray()) {
            const QJsonArray r = s.toObject().value(QStringLiteral("segment")).toArray();
            if (categories.contains(s.toObject().value(QStringLiteral("category")).toString()) && r.at(1).toDouble() > r.at(0).toDouble()) ++n;
        }
    }
    return n;
}

void testSponsorFetch() {
    const QByteArray body = fixture(QStringLiteral("sponsorblock_prefix.json"));
    const QJsonArray answer = QJsonDocument::fromJson(body).array();
    CHECK(answer.size() > 1);   // the prefix answer holds other videos too
    FakeTransport net;
    net.on(QStringLiteral("/api/skipSegments/5752"), QByteArray(), 200, body);
    WorkLane lane(QStringLiteral("sb"));
    SponsorBlockService service(&net, &lane);
    std::int64_t now = 1000000;
    service.setClock([&now] { return now; });
    QObject context;
    auto fetch = [&](const QString &id) {
        SponsorResult got;
        bool done = false;
        service.fetch(id, &context, [&](const SponsorResult &r) { got = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        return got;
    };

    const SponsorResult r = fetch(QString::fromLatin1(kVideo));
    CHECK_EQ(oc(r.outcome), std::string("ok"));
    CHECK_EQ(sz(r.list.segments.size()), expectedSegments(answer, QString::fromLatin1(kVideo)));
    CHECK(!r.list.segments.empty());
    int bad = 0;
    for (std::size_t i = 0; i < r.list.segments.size(); ++i) {
        if (!(r.list.segments[i].endSeconds > r.list.segments[i].startSeconds)) ++bad;
        if (i + 1 < r.list.segments.size() && r.list.segments[i].startSeconds > r.list.segments[i + 1].startSeconds) ++bad;
    }
    CHECK_EQ(bad, 0);

    CHECK_EQ(net.requests.size(), 1);
    const HttpRequest req = net.requests.first();
    CHECK_EQ(req.url.host(), QStringLiteral("sponsor.ajay.app"));
    CHECK_EQ(req.url.path(), QStringLiteral("/api/skipSegments/5752"));
    CHECK_EQ(req.timeoutMs, 8000);
    const QString query = QUrlQuery(req.url).queryItemValue(QStringLiteral("categories"), QUrl::FullyDecoded);
    int missing = 0;
    for (const std::string &c : sponsorCategories())
        if (!query.contains(QLatin1Char('"') + QString::fromStdString(c) + QLatin1Char('"'))) ++missing;
    CHECK_EQ(missing, 0);

    // Cache: 10 minutes.
    CHECK(fetch(QString::fromLatin1(kVideo)).fromCache);
    now += 10 * 60 * 1000 - 1;
    CHECK(fetch(QString::fromLatin1(kVideo)).fromCache);
    CHECK_EQ(net.requests.size(), 1);
    now += 2;
    CHECK(!fetch(QString::fromLatin1(kVideo)).fromCache);
    CHECK_EQ(net.requests.size(), 2);

    // Derived from sponsorblock_prefix: the target entry gets a reversed order, an inverted
    // segment and a zero-length one; another id gets a segment that must not leak.
    QJsonArray derived;
    for (const QJsonValue &v : answer) {
        QJsonObject o = v.toObject();
        QJsonArray segs = o.value(QStringLiteral("segments")).toArray();
        auto seg = [](double a, double b, const char *cat) {
            QJsonObject s;
            s.insert(QStringLiteral("segment"), QJsonArray() << a << b);
            s.insert(QStringLiteral("category"), QString::fromLatin1(cat));
            s.insert(QStringLiteral("actionType"), QStringLiteral("skip"));
            return s;
        };
        if (o.value(QStringLiteral("videoID")).toString() == QLatin1String(kVideo)) {
            QJsonArray reversed;
            for (int i = segs.size() - 1; i >= 0; --i) reversed.append(segs.at(i));
            reversed.append(seg(50, 40, "sponsor"));
            reversed.append(seg(60, 60, "sponsor"));
            reversed.append(seg(1, 2, "poi_highlight"));
            segs = reversed;
        } else {
            segs.append(seg(0.5, 3, "sponsor"));
        }
        o.insert(QStringLiteral("segments"), segs);
        derived.append(o);
    }
    SponsorSegmentList list;
    CHECK_EQ(oc(SponsorBlockService::parseAnswer(200, toBytes(derived), kVideo, &list)), std::string("ok"));
    CHECK_EQ(sz(list.segments.size()), expectedSegments(answer, QString::fromLatin1(kVideo)));
    bool sorted = true;
    for (std::size_t i = 0; i + 1 < list.segments.size(); ++i)
        if (list.segments[i].startSeconds > list.segments[i + 1].startSeconds) sorted = false;
    CHECK(sorted);
    CHECK(list.hasHighlight);
    CHECK_EQ(oc(SponsorBlockService::parseAnswer(200, "{}", kVideo, &list)), std::string("drift"));
    CHECK_EQ(oc(SponsorBlockService::parseAnswer(200, "[]", kVideo, &list)), std::string("empty"));
    CHECK_EQ(oc(SponsorBlockService::parseAnswer(0, QByteArray(), kVideo, &list)), std::string("network"));
}

void testSponsorCacheAndFailures() {
    FakeTransport net;
    net.on(QStringLiteral("/api/skipSegments/"), QByteArray(), 500, QByteArray(), 0, 1);
    net.on(QStringLiteral("/api/skipSegments/"), QByteArray(), 404, QByteArray("Not Found"));
    SponsorBlockService service(&net, nullptr);
    QObject context;
    auto fetch = [&](const QString &id) {
        SponsorResult got;
        bool done = false;
        service.fetch(id, &context, [&](const SponsorResult &r) { got = r; done = true; });
        CHECK(waitFor([&] { return done; }));
        return got;
    };
    auto id = [](int i) { return QStringLiteral("vid%1").arg(i, 8, 10, QLatin1Char('0')); };

    CHECK_EQ(oc(fetch(id(0)).outcome), std::string("http5xx"));   // not cached
    const SponsorResult none = fetch(id(0));
    CHECK_EQ(oc(none.outcome), std::string("empty"));             // 404: no segments
    CHECK(none.list.segments.empty());
    CHECK_EQ(net.requests.size(), 2);
    CHECK(fetch(id(0)).fromCache);
    for (int i = 1; i < 65; ++i) fetch(id(i));                    // the 65th video evicts one entry
    CHECK_EQ(net.requests.size(), 66);
    CHECK(fetch(id(64)).fromCache);
    CHECK(!fetch(id(0)).fromCache);
    CHECK_EQ(net.requests.size(), 67);

    // A transport failure (timeout or network: status 0) is not cached either.
    FakeTransport slow;
    SponsorBlockService timeouts(&slow, nullptr);
    bool done = false;
    SponsorResult got;
    timeouts.fetch(QString::fromLatin1(kVideo), &context, [&](const SponsorResult &r) { got = r; done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK_EQ(oc(got.outcome), std::string("network"));
    done = false;
    timeouts.fetch(QString::fromLatin1(kVideo), &context, [&](const SponsorResult &r) { got = r; done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK_EQ(slow.requests.size(), 2);

    // No request ever carries a full video id.
    int leaks = 0;
    for (const HttpRequest &r : net.requests) {
        for (int i = 0; i < 65; ++i) if (r.url.toString().contains(id(i))) ++leaks;
    }
    for (const HttpRequest &r : slow.requests) if (r.url.toString().contains(QLatin1String(kVideo))) ++leaks;
    CHECK_EQ(leaks, 0);

    // Invalid ids are rejected without a request.
    const int before = net.requests.size();
    CHECK_EQ(oc(fetch(QStringLiteral("short")).outcome), std::string("http4xx"));
    CHECK_EQ(net.requests.size(), before);
}

// DL-07: offline playback uses the segments saved with the download; no request is made and a
// fetch of the same video is a no-op; another video fetches again.
void testSponsorLocal() {
    FakeTransport net;
    net.on(QStringLiteral("/api/skipSegments/"), QByteArray(), 200, fixture(QStringLiteral("sponsorblock_prefix.json")));
    SponsorBlockService service(&net, nullptr);
    SponsorSegmentsModel model(&service);
    QVariantList saved;
    QVariantMap a; a.insert(QStringLiteral("category"), QStringLiteral("sponsor")); a.insert(QStringLiteral("actionType"), QStringLiteral("skip"));
    a.insert(QStringLiteral("start"), 12.5); a.insert(QStringLiteral("end"), 40.0);
    QVariantMap h; h.insert(QStringLiteral("category"), QStringLiteral("poi_highlight")); h.insert(QStringLiteral("start"), 95.0); h.insert(QStringLiteral("end"), 95.0);
    QVariantMap bad; bad.insert(QStringLiteral("category"), QStringLiteral("intro")); bad.insert(QStringLiteral("start"), 50.0); bad.insert(QStringLiteral("end"), 49.0);
    saved << a << h << bad;
    model.setLocal(QString::fromLatin1(kVideo), saved);
    CHECK(model.local());
    CHECK_EQ(model.videoId(), QString::fromLatin1(kVideo));
    CHECK_EQ(model.count(), 1);                                          // the empty range is dropped
    const QVariantMap first = model.segments().first().toMap();
    CHECK_EQ(first.value(QStringLiteral("start")).toDouble(), 12500.0);  // seconds -> ms
    CHECK_EQ(first.value(QStringLiteral("end")).toDouble(), 40000.0);
    CHECK_EQ(first.value(QStringLiteral("category")).toString(), QStringLiteral("sponsor"));
    CHECK_EQ(model.highlight(), 95000.0);
    model.fetch(QString::fromLatin1(kVideo));
    CHECK(!model.loading());
    CHECK_EQ(static_cast<int>(net.requests.size()), 0);
    // Another (online) video: fetched as usual.
    model.fetch(QStringLiteral("aaaaaaaaaaa"));
    CHECK(!model.local());
    CHECK(waitFor([&] { return !model.loading(); }));
    CHECK_EQ(static_cast<int>(net.requests.size()), 1);
}

void testSponsorModel() {
    FakeTransport net;
    net.on(QStringLiteral("/api/skipSegments/5752"), QByteArray(), 200, fixture(QStringLiteral("sponsorblock_prefix.json")), 200);
    net.on(QStringLiteral("/api/skipSegments/"), QByteArray(), 404, QByteArray(), 10);
    SponsorBlockService service(&net, nullptr);
    SponsorSegmentsModel model(&service);
    CHECK_EQ(model.categories().size(), 8);
    model.fetch(QString::fromLatin1(kVideo));
    CHECK(model.loading());
    CHECK(waitFor([&] { return !model.loading(); }));
    CHECK(model.count() > 0);
    const QVariantMap first = model.segments().first().toMap();
    CHECK(first.value(QStringLiteral("end")).toDouble() > first.value(QStringLiteral("start")).toDouble());
    CHECK(!first.value(QStringLiteral("category")).toString().isEmpty());
    CHECK(first.value(QStringLiteral("start")).toDouble() >= 0);

    // A late answer for the previous video is dropped.
    service.clearCache();
    model.clear();
    model.fetch(QString::fromLatin1(kVideo));        // slow (200 ms)
    model.fetch(QStringLiteral("AAAAAAAAAAA"));      // fast 404
    CHECK(waitFor([&] { return !model.loading(); }));
    catalog_test::waitFor([] { return false; }, 300);
    CHECK_EQ(model.videoId(), QStringLiteral("AAAAAAAAAAA"));
    CHECK_EQ(model.count(), 0);
}

// ---------------------------------------------------------------- Return YouTube Dislike

void testDislikes() {
    FakeTransport net;
    net.on(QStringLiteral("returnyoutubedislikeapi.com/votes"), QByteArray(), 200, fixture(QStringLiteral("ryd_votes.json")));
    DislikeService service(&net);
    std::int64_t now = 5000000;
    service.setClock([&now] { return now; });
    QObject context;

    // Disabled (the default): nothing is sent and no callback arrives.
    CHECK(!service.enabled());
    bool called = false;
    CHECK_EQ(service.fetch(QString::fromLatin1(kVideo), &context, [&](const DislikeResult &) { called = true; }), static_cast<quint64>(0));
    catalog_test::waitFor([] { return false; }, 50);
    CHECK(!called);
    CHECK_EQ(net.requests.size(), 0);

    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/ryd.conf"), QSettings::IniFormat);
    DislikeInfo info(&service, &settings);
    CHECK(!info.enabled());
    info.fetch(QString::fromLatin1(kVideo));
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(net.requests.size(), 0);
    CHECK(!info.available());
    CHECK(info.disclosure().contains(QStringLiteral("third-party")));
    CHECK(info.disclosure().contains(QStringLiteral("estimates")));
    CHECK(info.estimateLabel().contains(QStringLiteral("Estimated")));

    // Enabled: one request, parsed, reused for 10 minutes.
    info.setEnabled(true);
    CHECK_EQ(settings.value(QLatin1String(DislikeInfo::kSettingsKey)).toBool(), true);
    CHECK(waitFor([&] { return info.available(); }));
    CHECK_EQ(info.estimate(), 1533.0);
    CHECK_EQ(net.requests.size(), 1);
    const HttpRequest req = net.requests.first();
    CHECK_EQ(req.url.host(), QStringLiteral("returnyoutubedislikeapi.com"));
    CHECK_EQ(QUrlQuery(req.url).queryItemValue(QStringLiteral("videoId")), QString::fromLatin1(kVideo));

    DislikeResult got;
    bool done = false;
    service.fetch(QString::fromLatin1(kVideo), &context, [&](const DislikeResult &r) { got = r; done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK(got.fromCache);
    CHECK_EQ(got.votes.likes, static_cast<std::int64_t>(219807));
    CHECK_EQ(net.requests.size(), 1);
    now += 10 * 60 * 1000;
    done = false;
    service.fetch(QString::fromLatin1(kVideo), &context, [&](const DislikeResult &r) { got = r; done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK(!got.fromCache);
    CHECK_EQ(net.requests.size(), 2);

    // Turning it off again stops all requests.
    info.setEnabled(false);
    CHECK(!info.available());
    info.fetch(QStringLiteral("BBBBBBBBBBB"));
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(net.requests.size(), 2);
    DislikeInfo restarted(&service, &settings);
    CHECK(!restarted.enabled());

    DislikeVotes v;
    CHECK_EQ(oc(DislikeService::parseVotes(200, "{\"id\":\"x\"}", &v)), std::string("drift"));
    CHECK_EQ(oc(DislikeService::parseVotes(429, QByteArray(), &v)), std::string("http429"));
}

void testExpiringCache() {
    std::int64_t now = 0;
    ExpiringCache<int, int> cache(2, 100, [&now] { return now; });
    cache.put(1, 10);
    cache.put(2, 20);
    int v = 0;
    CHECK(cache.get(1, &v) && v == 10);
    cache.put(3, 30);                 // evicts 2 (1 was read more recently)
    CHECK(!cache.contains(2));
    CHECK(cache.contains(1));
    now = 100;
    CHECK(!cache.contains(1));        // expired
    CHECK_EQ(sz(cache.size()), 1);
    ExpiringCache<int, int> session(1, 0, [&now] { return now; });
    session.put(1, 1);
    now = 1000000000;
    CHECK(session.contains(1));
}

}  // namespace

// DL-07 / W4: a download's saved tracks. setLocalTracks() builds the set from localMedia().captions
// (auto-pick rules unchanged); the cues are read from the saved json3 files on the lane, with no
// request; a missing file fails like an unavailable track.
void testLocalTracks() {
    QTemporaryDir dir;
    const QString manual = dir.path() + QStringLiteral("/9zCH37330f8.en.json3");
    const QString asr = dir.path() + QStringLiteral("/9zCH37330f8.asr-en.json3");
    { QFile f(manual); f.open(QIODevice::WriteOnly); f.write(fixture(QStringLiteral("timedtext_manual_json3.json"))); }
    { QFile f(asr); f.open(QIODevice::WriteOnly); f.write(fixture(QStringLiteral("timedtext_asr_json3.json"))); }
    const int manualCount = sz(parse(fixture(QStringLiteral("timedtext_manual_json3.json"))).size());

    FakeTransport net;
    WorkLane lane(QStringLiteral("local-captions"), 1);
    CaptionService service(&net, &lane);
    QSettings settings(dir.path() + QStringLiteral("/local.conf"), QSettings::IniFormat);
    CaptionTracksModel tracks(&settings);
    CaptionCues cues(&service);
    cues.follow(&tracks);

    QVariantList saved;
    QVariantMap a; a.insert(QStringLiteral("languageCode"), QStringLiteral("en")); a.insert(QStringLiteral("name"), QStringLiteral("English"));
    a.insert(QStringLiteral("autoGenerated"), false); a.insert(QStringLiteral("path"), manual);
    QVariantMap b; b.insert(QStringLiteral("languageCode"), QStringLiteral("en")); b.insert(QStringLiteral("name"), QStringLiteral("English (auto)"));
    b.insert(QStringLiteral("autoGenerated"), true); b.insert(QStringLiteral("path"), asr);
    QVariantMap broken; broken.insert(QStringLiteral("languageCode"), QString()); broken.insert(QStringLiteral("path"), manual);
    saved << a << b << broken;
    tracks.setLocalTracks(QStringLiteral("9zCH37330f8"), saved);
    CHECK_EQ(tracks.count(), 2);                       // the entry without a language is skipped
    CHECK(tracks.available());
    CHECK(tracks.pick(QStringLiteral("en")));
    CHECK(tracks.currentUrl().startsWith(QLatin1String("file://")));
    CHECK(waitFor([&] { return cues.ready(); }));
    CHECK_EQ(cues.count(), manualCount);
    CHECK_EQ(static_cast<int>(net.requests.size()), 0);   // nothing fetched
    CHECK(tracks.pick(QStringLiteral("asr:en")));
    CHECK(waitFor([&] { return cues.ready() && cues.url().contains(QLatin1String("asr-en")); }));
    CHECK(cues.count() > 0);

    // A saved file that disappeared: failed, not a crash, still no request.
    QFile::remove(manual);
    service.clearCache();
    CueResult got;
    bool done = false;
    QObject context;
    service.fetchCues(QUrl::fromLocalFile(manual).toString(), &context, [&](const CueResult &r) { got = r; done = true; });
    CHECK(waitFor([&] { return done; }));
    CHECK(got.outcome == Outcome::Unavailable);
    CHECK(got.cues && got.cues->empty());
    CHECK_EQ(static_cast<int>(net.requests.size()), 0);
}

// The shipped state of the translation switch (kCaptionTranslationsOffered, false for beta 3):
// no auto-translation is listed, chosen or picked automatically. Runs before main() turns the
// switch on for the tests of the translation code, which stays and stays tested.
void testTranslationsSwitch() {
    CHECK(!kCaptionTranslationsOffered);
    CHECK(!captionTranslationsOffered());
    const QJsonObject player = fixtureJson(QStringLiteral("player_visionos.json"));

    const CaptionTrackSet offered = CaptionService::tracksFromPlayer(player);
    CHECK_EQ(sz(offered.real.size()), 5);
    CHECK(offered.translations.empty());
    CHECK(offered.find("tlang:de") == nullptr);
    CHECK(offered.find("en") != nullptr);

    // yt-dlp's translated entries are left out the same way.
    QJsonObject entry;
    entry.insert(QStringLiteral("ext"), QStringLiteral("json3"));
    entry.insert(QStringLiteral("url"), QStringLiteral("https://www.youtube.com/api/timedtext?v=1&lang=en&kind=asr&fmt=json3"));
    QJsonObject translated = entry;
    translated.insert(QStringLiteral("url"), QStringLiteral("https://www.youtube.com/api/timedtext?v=1&lang=en&kind=asr&tlang=de&fmt=json3"));
    QJsonObject automatic;
    automatic.insert(QStringLiteral("en"), QJsonArray() << entry);
    automatic.insert(QStringLiteral("de"), QJsonArray() << translated);
    QJsonObject info;
    info.insert(QStringLiteral("automatic_captions"), automatic);
    const CaptionTrackSet fromYtDlp = CaptionService::tracksFromYtDlp(info);
    CHECK_EQ(sz(fromYtDlp.real.size()), 1);
    CHECK(fromYtDlp.translations.empty());

    // The model: nothing to choose, and a stored language that only a translation has stays off.
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/switch.conf"), QSettings::IniFormat);
    settings.setValue(QLatin1String(CaptionTracksModel::kSettingsKey), QStringLiteral("de"));
    CaptionTracksModel model(&settings);
    model.setFromPlayerAnswer(QString::fromLatin1(kVideo), player);
    CHECK_EQ(model.count(), 5);
    CHECK_EQ(model.translationsModel()->total(), 0);
    CHECK_EQ(model.translationsModel()->count(), 0);
    CHECK(model.off());
    CHECK(model.lastMatch() == CaptionMatch::None);
    CHECK(!model.pick(QStringLiteral("tlang:de")));
    CHECK(model.off());
    CHECK_EQ(settings.value(QLatin1String(CaptionTracksModel::kSettingsKey)).toString(), QStringLiteral("de"));   // untouched
    CHECK(model.pick(QStringLiteral("en")));       // real tracks are chosen as before

    // The shaping itself still builds them; only the offer leaves them out.
    CaptionTrackSet whole;
    CaptionTrack real;
    real.key = "en"; real.code = "en"; real.name = "English"; real.url = "https://example.invalid/en";
    CaptionTrack tr;
    tr.key = "tlang:de"; tr.code = "de"; tr.name = "German"; tr.url = "https://example.invalid/en?tlang=de"; tr.translation = true;
    whole.real.push_back(real);
    whole.translations.push_back(tr);
    const CaptionTrackSet cut = offeredCaptionTracks(whole);
    CHECK_EQ(sz(cut.real.size()), 1);
    CHECK(cut.translations.empty());

    // On: the same answers list them again (what flipping the constant does).
    setCaptionTranslationsOffered(true);
    CHECK(captionTranslationsOffered());
    CHECK_EQ(sz(offeredCaptionTracks(whole).translations.size()), 1);
    const CaptionTrackSet all = CaptionService::tracksFromPlayer(player);
    CHECK(sz(all.translations.size()) > 140);
    CHECK(all.find("tlang:de") != nullptr);
    CHECK_EQ(sz(CaptionService::tracksFromYtDlp(info).translations.size()), 1);
    model.setFromPlayerAnswer(QString::fromLatin1(kVideo), player);
    CHECK(model.translationsModel()->total() > 140);
    setCaptionTranslationsOffered(false);
    CHECK(!captionTranslationsOffered());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("org.nordilabs.sailtube.test"));
    app.setApplicationName(QStringLiteral("catalog_captions"));
    testTranslationsSwitch();
    // From here on the switch is on: the translation code keeps its tests although the shipped
    // lists leave translations out.
    setCaptionTranslationsOffered(true);
    testManualJson3();
    testAsrJson3();
    testDefaultDuration();
    testRollingMerge();
    testShortAndEmpty();
    testCursor();
    testUrlRewrite();
    testTrackShaping();
    testYtDlpTracks();
    testAutoSelectOrder();
    testPersistence();
    testP9Scenario();
    testCueCache();
    testCueLateAnswer();
    testSha256();
    testSponsorFetch();
    testSponsorCacheAndFailures();
    testSponsorModel();
    testDislikes();
    testExpiringCache();
    testLocalTracks();
    testSponsorLocal();
    return catalog_test::finish("catalog_captions");
}
