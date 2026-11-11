// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Locale tables (I18N-02) and the per-locale fixture set (I18N-04, NFR-TST-01): for the ten
// shipped languages, search page 1, related videos (watch-next secondaryResults) and a channel's
// Videos tab parse with no dropped row and >= 90 % of video rows carrying views, age and
// duration; the exact display strings seen in the recordings parse per language; unparseable
// text keeps its raw form; results are identical under other process locales.
#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/TextParse.h"
#include "Core/Catalog/Domain/LocalizedText.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/RendererParser.h"

#include <QDir>
#include <QHash>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLocale>
#include <QProcess>
#include <QStringList>
#include <QTemporaryDir>
#include <clocale>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>

using namespace nordtile::video;
using namespace nordtile::video::catalog;

namespace {

const std::int64_t kMinute = 60, kHour = 3600, kDay = 86400, kWeek = 7 * kDay, kMonth = 30 * kDay, kYear = 365 * kDay;

struct LocaleSet {
    const char *hl;          // the request hl (what RendererParser gets)
    const char *search;
    const char *next;
    const char *channel;
};

const LocaleSet kLocales[] = {
    {"en", "search_en_p1.json", "next_en.json", "channel_videos.json"},
    {"de", "search_de_p1.json", "next_de.json", "channel_videos_de.json"},
    {"ja", "search_ja_p1.json", "next_ja.json", "channel_videos_ja.json"},
    {"es", "search_es_p1.json", "next_es.json", "channel_videos_es.json"},
    {"pt", "search_pt_p1.json", "next_pt.json", "channel_videos_pt.json"},
    {"fr", "search_fr_p1.json", "next_fr.json", "channel_videos_fr.json"},
    {"it", "search_it_p1.json", "next_it.json", "channel_videos_it.json"},
    {"ru", "search_ru_p1.json", "next_ru.json", "channel_videos_ru.json"},
    {"zh-CN", "search_zh_p1.json", "next_zh.json", "channel_videos_zh.json"},
    {"ar", "search_ar_p1.json", "next_ar.json", "channel_videos_ar.json"},
};

// Counts the row renderers below `v` the way a reader of the JSON would: every item renderer
// key (not descending into it), lockups only for the content types that are rows.
int countRowRenderers(const QJsonValue &v) {
    int n = 0;
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray()) n += countRowRenderers(x);
    } else if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            if (RendererParser::isItemRenderer(it.key()) && it.value().isObject()) {
                if (it.key() == QLatin1String("lockupViewModel")) {
                    const QString type = it.value().toObject().value(QStringLiteral("contentType")).toString();
                    if (type == QLatin1String("LOCKUP_CONTENT_TYPE_VIDEO") || type == QLatin1String("LOCKUP_CONTENT_TYPE_PLAYLIST") ||
                        type == QLatin1String("LOCKUP_CONTENT_TYPE_ALBUM") || type == QLatin1String("LOCKUP_CONTENT_TYPE_PODCAST") ||
                        type == QLatin1String("LOCKUP_CONTENT_TYPE_CHANNEL"))
                        ++n;
                } else {
                    ++n;
                }
                continue;
            }
            n += countRowRenderers(it.value());
        }
    }
    return n;
}

// Per video id, the texts a row shows besides its title: lockup metadata parts and the
// videoRenderer's publishedTimeText. Used to know whether a row shows an age at all.
void collectShownTexts(const QJsonValue &v, QHash<QString, QStringList> &out) {
    if (v.isArray()) {
        for (const QJsonValue &x : v.toArray()) collectShownTexts(x, out);
        return;
    }
    if (!v.isObject()) return;
    const QJsonObject o = v.toObject();
    for (auto it = o.begin(); it != o.end(); ++it) {
        const QJsonObject r = it.value().toObject();
        if (it.key() == QLatin1String("lockupViewModel")) {
            QStringList &texts = out[r.value(QStringLiteral("contentId")).toString()];
            const QJsonArray rows = json::at(r, QStringLiteral("metadata.lockupMetadataViewModel.metadata.contentMetadataViewModel.metadataRows")).toArray();
            for (const QJsonValue &row : rows)
                for (const QJsonValue &part : row.toObject().value(QStringLiteral("metadataParts")).toArray()) {
                    const QString t = json::text(part.toObject().value(QStringLiteral("text")));
                    if (!t.isEmpty()) texts << t;
                }
            continue;
        }
        if (it.key() == QLatin1String("videoRenderer") || it.key() == QLatin1String("compactVideoRenderer") ||
            it.key() == QLatin1String("gridVideoRenderer")) {
            const QString t = json::text(r.value(QStringLiteral("publishedTimeText")));
            if (!t.isEmpty()) out[r.value(QStringLiteral("videoId")).toString()] << t;
            continue;
        }
        collectShownTexts(it.value(), out);
    }
}

bool showsAge(const Item &i, const QHash<QString, QStringList> &shown) {
    for (const QString &t : shown.value(QString::fromStdString(i.id))) {
        const std::string s = t.toStdString();
        if (s != i.channel && s != i.viewText) return true;
    }
    return false;
}

bool isLiveish(const Item &i) {
    return i.liveStatus == media::LiveStatus::IsLive || i.liveStatus == media::LiveStatus::Upcoming;
}

struct Stats {
    int rows = 0, renderers = 0, unparsed = 0;
    int video = 0, views = 0, ageShown = 0, age = 0, timed = 0, duration = 0;
    int rawMissing = 0;   // a -1 number whose raw text is also empty although the row shows one
};

Stats measure(const RendererParser &parser, const QJsonValue &subtree) {
    Stats s;
    const ParsedItems r = parser.parse(subtree);
    s.rows = static_cast<int>(r.items.size());
    s.unparsed = r.unparsed;
    s.renderers = countRowRenderers(subtree);
    QHash<QString, QStringList> shown;
    collectShownTexts(subtree, shown);
    for (const Item &i : r.items) {
        if (i.kind != ItemKind::Video) continue;
        ++s.video;
        if (i.viewCount >= 0) ++s.views;
        if (showsAge(i, shown) && i.liveStatus != media::LiveStatus::Upcoming) {
            ++s.ageShown;
            if (i.publishedAgeSeconds >= 0) ++s.age;
        }
        if (!isLiveish(i)) {
            ++s.timed;
            if (i.durationSeconds > 0) ++s.duration;
        }
        // I18N-02: a number that did not parse keeps the text it came from.
        if (i.viewCount < 0 && i.viewText.empty() && !isLiveish(i)) ++s.rawMissing;
    }
    return s;
}

bool atLeast90(int ok, int total) { return total == 0 || ok * 10 >= total * 9; }

void checkStats(const char *label, const Stats &s, int minVideo) {
    std::printf("  %-26s rows %3d/%3d video %3d views %3d age %3d/%3d duration %3d/%3d\n", label, s.rows, s.renderers,
                s.video, s.views, s.age, s.ageShown, s.duration, s.timed);
    CHECK_EQ(s.rows, s.renderers);            // no row dropped
    CHECK_EQ(s.unparsed, 0);
    CHECK(s.video >= minVideo);
    CHECK(atLeast90(s.views, s.video));
    CHECK(atLeast90(s.age, s.ageShown));
    CHECK(atLeast90(s.duration, s.timed));
    CHECK_EQ(s.rawMissing, 0);
}

void localeFixtures() {
    std::printf("per-locale fixtures:\n");
    for (const LocaleSet &l : kLocales) {
        const RendererParser parser(QString::fromLatin1(l.hl));
        const QJsonObject search = catalog_test::fixtureJson(QString::fromLatin1(l.search));
        checkStats(l.search, measure(parser, search.value(QStringLiteral("contents"))), 10);
        const QJsonObject next = catalog_test::fixtureJson(QString::fromLatin1(l.next));
        checkStats(l.next, measure(parser, json::findFirst(next, QStringLiteral("secondaryResults"))), 10);
        const QJsonObject channel = catalog_test::fixtureJson(QString::fromLatin1(l.channel));
        checkStats(l.channel, measure(parser, channel.value(QStringLiteral("contents"))), 20);
        // The watch page's own date text parses as an absolute date (Dec 12, 2024).
        const QString date = json::text(json::findFirst(next, QStringLiteral("videoPrimaryInfoRenderer")).value(QStringLiteral("dateText")));
        CHECK(!date.isEmpty());
        CHECK_EQ(parseAbsoluteDate(date.toStdString(), l.hl), 1733961600);
        // The owner's subscriber line on the watch page.
        const QString subs = json::text(json::findFirst(next, QStringLiteral("videoOwnerRenderer")).value(QStringLiteral("subscriberCountText")));
        CHECK_EQ(parseSubscriberText(subs.toStdString(), l.hl), 25800000);
        CHECK(classifyMetaPart(subs.toStdString(), l.hl) == MetaPart::Subscribers);
    }
}


// Exact display strings from the recordings (search, related and channel Videos per locale).
struct CountCase { const char *hl; const char *text; std::int64_t value; MetaPart part; };
struct AgeCase { const char *hl; const char *text; std::int64_t seconds; };

void directTables() {
    const CountCase counts[] = {
        {"en", "10,961,357 views", 10961357, MetaPart::Views},
        {"en", "3.6M views", 3600000, MetaPart::Views},
        {"en", "345K", 345000, MetaPart::Views},
        {"en", "1.02M subscribers", 1020000, MetaPart::Subscribers},
        {"en", "6.2K videos", 6200, MetaPart::VideoCount},
        {"de", "1.436.404 Aufrufe", 1436404, MetaPart::Views},
        {"de", "1,4 Mio. Aufrufe", 1400000, MetaPart::Views},
        {"de", "486.869", 486869, MetaPart::Views},
        {"de", "2,36 Mio. Abonnenten", 2360000, MetaPart::Subscribers},
        {"de", "6201 Videos", 6201, MetaPart::VideoCount},
        {"ja", "10,489,680回視聴", 10489680, MetaPart::Views},
        {"ja", "1048万回視聴", 10480000, MetaPart::Views},
        {"ja", "1.8万回視聴", 18000, MetaPart::Views},
        {"ja", "1097万", 10970000, MetaPart::Views},
        {"ja", "チャンネル登録者数 14.5万人", 145000, MetaPart::Subscribers},
        {"ja", "6201 本の動画", 6201, MetaPart::VideoCount},
        {"es", "1.381.345 visualizaciones", 1381345, MetaPart::Views},
        {"es", "1,3 M de visualizaciones", 1300000, MetaPart::Views},
        {"es", "370 K", 370000, MetaPart::Views},
        {"es", "107 K visualizaciones", 107000, MetaPart::Views},
        {"es", "1,02 M de suscriptores", 1020000, MetaPart::Subscribers},
        {"es", "6,2 K vídeos", 6200, MetaPart::VideoCount},
        {"es", "1,2 mil millones de visualizaciones", 1200000000, MetaPart::Views},
        {"pt", "11.332.182 visualizações", 11332182, MetaPart::Views},
        {"pt", "11 mi de visualizações", 11000000, MetaPart::Views},
        {"pt", "5,1 mi de visualizações", 5100000, MetaPart::Views},
        {"pt", "346 mil", 346000, MetaPart::Views},
        {"pt", "107 mil visualizações", 107000, MetaPart::Views},
        {"pt", "881 mil inscritos", 881000, MetaPart::Subscribers},
        {"pt", "1,02 mi de inscritos", 1020000, MetaPart::Subscribers},
        {"pt", "6,2 mil vídeos", 6200, MetaPart::VideoCount},
        {"fr", "10 961 368 vues", 10961368, MetaPart::Views},
        {"fr", "3,6 M de vues", 3600000, MetaPart::Views},
        {"fr", "107 k vues", 107000, MetaPart::Views},
        {"fr", "346 k", 346000, MetaPart::Views},
        {"fr", "22", 22, MetaPart::Views},
        {"fr", "1,02 M d’abonnés", 1020000, MetaPart::Subscribers},
        {"fr", "371 k abonnés", 371000, MetaPart::Subscribers},
        {"fr", "6,2 k vidéos", 6200, MetaPart::VideoCount},
        {"fr", "1,2 Md de vues", 1200000000, MetaPart::Views},
        {"it", "10.489.826 visualizzazioni", 10489826, MetaPart::Views},
        {"it", "10 Mln di visualizzazioni", 10000000, MetaPart::Views},
        {"it", "346K", 346000, MetaPart::Views},
        {"it", "1,6 Mln", 1600000, MetaPart::Views},
        {"it", "881K iscritti", 881000, MetaPart::Subscribers},
        {"it", "1,02 Mln di iscritti", 1020000, MetaPart::Subscribers},
        {"it", "6,2K video", 6200, MetaPart::VideoCount},
        {"ru", "13 936 981 просмотр", 13936981, MetaPart::Views},
        {"ru", "4 679 663 просмотра", 4679663, MetaPart::Views},
        {"ru", "3,6 млн просмотров", 3600000, MetaPart::Views},
        {"ru", "370 тыс.", 370000, MetaPart::Views},
        {"ru", "107 тыс. просмотров", 107000, MetaPart::Views},
        {"ru", "1,02 млн подписчиков", 1020000, MetaPart::Subscribers},
        {"ru", "6,2 тыс. видео", 6200, MetaPart::VideoCount},
        {"ru", "Нет просмотров", 0, MetaPart::Views},
        {"zh-CN", "10,961,368次观看", 10961368, MetaPart::Views},
        {"zh-CN", "1096万次观看", 10960000, MetaPart::Views},
        {"zh-CN", "1.8万次观看", 18000, MetaPart::Views},
        {"zh-CN", "10万", 100000, MetaPart::Views},
        {"zh-CN", "102万位订阅者", 1020000, MetaPart::Subscribers},
        {"zh-CN", "6201 个视频", 6201, MetaPart::VideoCount},
        {"ar", "10,961,368 مشاهدة", 10961368, MetaPart::Views},
        {"ar", "3.6 مليون مشاهدة", 3600000, MetaPart::Views},
        {"ar", "782 ألف", 782000, MetaPart::Views},
        {"ar", "107 ألف مشاهدة", 107000, MetaPart::Views},
        {"ar", "1.02 مليون مشترك", 1020000, MetaPart::Subscribers},
        {"ar", "6.2 ألف من الفيديوهات", 6200, MetaPart::VideoCount},
        // Arabic-Indic digits with the Arabic decimal and thousands separators.
        {"ar", "١٫٢ مليون مشاهدة", 1200000, MetaPart::Views},
        {"ar", "١٠٬٩٦١٬٣٦٨ مشاهدة", 10961368, MetaPart::Views},
    };
    for (const CountCase &c : counts) {
        const std::int64_t v = c.part == MetaPart::Subscribers ? parseSubscriberText(c.text, c.hl) : parseCountText(c.text, c.hl);
        if (v != c.value) std::printf("  count [%s] '%s' -> %lld\n", c.hl, c.text, static_cast<long long>(v));
        CHECK_EQ(v, c.value);
        if (classifyMetaPart(c.text, c.hl) != c.part) std::printf("  class [%s] '%s' -> %d\n", c.hl, c.text, static_cast<int>(classifyMetaPart(c.text, c.hl)));
        CHECK(classifyMetaPart(c.text, c.hl) == c.part);
    }

    const AgeCase ages[] = {
        {"en", "11 days ago", 11 * kDay}, {"en", "11mo ago", 11 * kMonth}, {"en", "15h ago", 15 * kHour},
        {"en", "2w ago", 2 * kWeek}, {"en", "1y ago", kYear}, {"en", "a day ago", kDay},
        {"de", "vor 11 Tagen", 11 * kDay}, {"de", "vor 15 Std.", 15 * kHour}, {"de", "vor 10 Monaten", 10 * kMonth},
        {"de", "vor 2 Wochen", 2 * kWeek}, {"de", "vor 1 Jahr", kYear}, {"de", "vor einem Monat", kMonth},
        {"ja", "11 日前", 11 * kDay}, {"ja", "7 か月前", 7 * kMonth}, {"ja", "15 時間前", 15 * kHour},
        {"ja", "2 週間前", 2 * kWeek}, {"ja", "1 年前", kYear},
        {"es", "hace 11 días", 11 * kDay}, {"es", "hace 1 a", kYear}, {"es", "hace 10 m", 10 * kMonth},
        {"es", "hace 2 sem.", 2 * kWeek}, {"es", "hace 3 d", 3 * kDay}, {"es", "hace 18 h", 18 * kHour},
        {"es", "hace 14 horas", 14 * kHour}, {"es", "hace 1 mes", kMonth}, {"es", "hace 5 min", 5 * kMinute},
        {"es", "hace un año", kYear},
        {"pt", "há 11 dias", 11 * kDay}, {"pt", "há 4 sem.", 4 * kWeek}, {"pt", "há 1 mês", kMonth},
        {"pt", "há 10 meses", 10 * kMonth}, {"pt", "há 14 horas", 14 * kHour}, {"pt", "há 2 anos", 2 * kYear},
        {"pt", "há um ano", kYear},
        {"fr", "il y a 11 jours", 11 * kDay}, {"fr", "il y a 1 a", kYear}, {"fr", "il y a 10 m.", 10 * kMonth},
        {"fr", "il y a 3 j", 3 * kDay}, {"fr", "il y a 18 h", 18 * kHour}, {"fr", "il y a 2 sem.", 2 * kWeek},
        {"fr", "il y a 1 mois", kMonth}, {"fr", "il y a 12 ans", 12 * kYear}, {"fr", "il y a un an", kYear},
        {"fr", "il y a une heure", kHour},
        {"it", "11 giorni fa", 11 * kDay}, {"it", "3 gg fa", 3 * kDay}, {"it", "2 sett. fa", 2 * kWeek},
        {"it", "1 mese fa", kMonth}, {"it", "10 mesi fa", 10 * kMonth}, {"it", "14 ore fa", 14 * kHour},
        {"it", "1 anno fa", kYear}, {"it", "un anno fa", kYear},
        {"ru", "11 дней назад", 11 * kDay}, {"ru", "18 ч назад", 18 * kHour}, {"ru", "2 дн. назад", 2 * kDay},
        {"ru", "2 нед. назад", 2 * kWeek}, {"ru", "1 мес. назад", kMonth}, {"ru", "1 г. назад", kYear},
        {"ru", "5 л. назад", 5 * kYear}, {"ru", "5 лет назад", 5 * kYear}, {"ru", "2 года назад", 2 * kYear},
        {"ru", "14 часов назад", 14 * kHour}, {"ru", "2 месяца назад", 2 * kMonth}, {"ru", "2 недели назад", 2 * kWeek},
        {"ru", "час назад", kHour}, {"ru", "неделю назад", kWeek}, {"ru", "год назад", kYear},
        {"zh-CN", "11天前", 11 * kDay}, {"zh-CN", "10个月前", 10 * kMonth}, {"zh-CN", "2周前", 2 * kWeek},
        {"zh-CN", "1年前", kYear}, {"zh-CN", "14小时前", 14 * kHour}, {"zh-CN", "直播时间：4天前", 4 * kDay},
        {"ar", "قبل 11 يومًا", 11 * kDay}, {"ar", "قبل 10 أشهر", 10 * kMonth}, {"ar", "قبل 3 سنوات", 3 * kYear},
        {"ar", "قبل سنة واحدة", kYear}, {"ar", "قبل سنتين", 2 * kYear}, {"ar", "قبل أسبوعين", 2 * kWeek},
        {"ar", "قبل شهر واحد", kMonth}, {"ar", "قبل شهرين", 2 * kMonth}, {"ar", "قبل 14 ساعة", 14 * kHour},
        {"ar", "قبل 3 أسابيع", 3 * kWeek}, {"ar", "قبل 3 أيام", 3 * kDay}, {"ar", "خلال 10 أشهر", 10 * kMonth},
        {"ar", "قبل يوم", kDay}, {"ar", "قبل ساعة", kHour}, {"ar", "قبل ٣ أيام", 3 * kDay},
    };
    for (const AgeCase &c : ages) {
        const std::int64_t v = parseAgeSeconds(c.text, c.hl);
        if (v != c.seconds) std::printf("  age [%s] '%s' -> %lld\n", c.hl, c.text, static_cast<long long>(v));
        CHECK_EQ(v, c.seconds);
        CHECK(classifyMetaPart(c.text, c.hl) == MetaPart::Age);
    }

    // Absolute dates (watch page dateText per locale, all 12 Dec 2024) and month names.
    const std::int64_t dec12 = 1733961600;
    const char *dates[][2] = {
        {"en", "Dec 12, 2024"}, {"de", "12.12.2024"}, {"ja", "2024/12/12"}, {"es", "12 dic 2024"},
        {"pt", "12 de dez. de 2024"}, {"fr", "12 déc. 2024"}, {"it", "12 dic 2024"},
        {"ru", "12 дек. 2024 г."}, {"zh-CN", "2024年12月12日"}, {"ar", "12‏/12‏/2024"},
        {"en", "Premiered Dec 12, 2024"}, {"es", "12 de diciembre de 2024"}, {"pt", "12 de dezembro de 2024"},
        {"fr", "12 décembre 2024"}, {"it", "12 dicembre 2024"}, {"de", "12. Dezember 2024"},
        {"ru", "12 декабря 2024 г."}, {"ar", "12 ديسمبر 2024"}, {"ar", "12 كانون الأول 2024"}, {"ar", "١٢/١٢/٢٠٢٤"},
    };
    for (const auto &d : dates) {
        const std::int64_t v = parseAbsoluteDate(d[1], d[0]);
        if (v != dec12) std::printf("  date [%s] '%s' -> %lld\n", d[0], d[1], static_cast<long long>(v));
        CHECK_EQ(v, dec12);
    }
    CHECK_EQ(parseAbsoluteDate("3 mars 2021", "fr"), parseAbsoluteDate("2021-03-03", "en"));
    CHECK_EQ(parseAbsoluteDate("3 мая 2021 г.", "ru"), parseAbsoluteDate("2021-05-03", "en"));
    CHECK_EQ(parseAbsoluteDate("3 de ago. de 2021", "pt"), parseAbsoluteDate("2021-08-03", "en"));
    CHECK_EQ(parseAbsoluteDate("3 mag 2021", "it"), parseAbsoluteDate("2021-05-03", "en"));
    CHECK_EQ(parseAbsoluteDate("3 أغسطس 2021", "ar"), parseAbsoluteDate("2021-08-03", "en"));

    // Owner labels are not counts or ages; unreadable text is -1 (the caller keeps it raw).
    CHECK(classifyMetaPart("Tech Review", "en") == MetaPart::Unknown);
    CHECK(classifyMetaPart("Chicago Video Lab", "en") == MetaPart::Unknown);
    CHECK(classifyMetaPart("Revue de presse", "fr") == MetaPart::Unknown);
    CHECK(classifyMetaPart("名前の動画チャンネル", "ja") == MetaPart::Unknown);
    CHECK(classifyMetaPart("il y a longtemps", "fr") == MetaPart::Age);
    CHECK_EQ(parseAgeSeconds("il y a longtemps", "fr"), -1);
    CHECK_EQ(parseCountText("muchas visualizaciones", "es"), -1);
    CHECK_EQ(parseDurationText("١:٠٥:٥٦"), 3956);
}

// The same videos recorded in English and in each other locale parse to the same numbers:
// durations exactly, views within compact rounding, ages within rounding and recording drift.
void crossLocale() {
    std::map<std::string, Item> english;
    const RendererParser en(QStringLiteral("en"));
    for (const char *f : {"search_en_p1.json", "next_en.json", "channel_videos.json"})
        for (const Item &i : en.parse(catalog_test::fixtureJson(QString::fromLatin1(f))).items)
            if (i.kind == ItemKind::Video) english[i.id] = i;
    for (const LocaleSet &l : kLocales) {
        if (std::string(l.hl) == "en") continue;
        const RendererParser parser(QString::fromLatin1(l.hl));
        int compared = 0, viewsOk = 0, agesOk = 0, durationsOk = 0, ages = 0;
        for (const char *f : {l.search, l.next, l.channel}) {
            for (const Item &i : parser.parse(catalog_test::fixtureJson(QString::fromLatin1(f))).items) {
                auto it = english.find(i.id);
                if (i.kind != ItemKind::Video || it == english.end()) continue;
                const Item &e = it->second;
                ++compared;
                if (i.durationSeconds == e.durationSeconds) ++durationsOk;
                const double ratio = e.viewCount > 0 ? double(i.viewCount) / double(e.viewCount) : (i.viewCount == 0 ? 1 : 0);
                if (ratio > 0.85 && ratio < 1.15) ++viewsOk;
                else std::printf("  views [%s] %s '%s' %lld vs '%s' %lld\n", l.hl, i.id.c_str(), i.viewText.c_str(),
                                 static_cast<long long>(i.viewCount), e.viewText.c_str(), static_cast<long long>(e.viewCount));
                if (i.publishedAgeSeconds >= 0 && e.publishedAgeSeconds >= 0) {
                    ++ages;
                    const double a = double(i.publishedAgeSeconds), b = double(e.publishedAgeSeconds);
                    if (std::abs(a - b) <= std::max(2.0 * kDay, 0.2 * std::max(a, b))) ++agesOk;
                    else std::printf("  age [%s] %s '%s' vs '%s'\n", l.hl, i.id.c_str(), i.publishedText.c_str(), e.publishedText.c_str());
                }
            }
        }
        std::printf("  cross-locale %-6s compared %3d views %3d ages %3d/%3d durations %3d\n", l.hl, compared, viewsOk, agesOk, ages, durationsOk);
        CHECK(compared >= 30);
        CHECK(atLeast90(viewsOk, compared));
        CHECK(atLeast90(agesOk, ages));
        CHECK_EQ(durationsOk, compared);
    }
}

// I18N-02: text that does not parse keeps the row and its raw text. Variants derived from real
// fixtures by replacing display strings (search: videoRenderer fields; related: lockup parts).
void rawTextKept() {
    QByteArray search = catalog_test::fixture(QStringLiteral("search_es_p1.json"));
    search.replace("1.381.345 visualizaciones", "muchas visualizaciones");
    search.replace("hace 11 días", "en algún momento");
    const RendererParser es(QStringLiteral("es"));
    const QJsonObject original = catalog_test::fixtureJson(QStringLiteral("search_es_p1.json"));
    const ParsedItems before = es.parse(original.value(QStringLiteral("contents")));
    const ParsedItems after = es.parse(QJsonDocument::fromJson(search).object().value(QStringLiteral("contents")));
    CHECK_EQ(after.items.size(), before.items.size());
    int rawViews = 0, rawAges = 0;
    for (const Item &i : after.items) {
        if (i.viewText == "muchas visualizaciones") { ++rawViews; CHECK_EQ(i.viewCount, -1); }
        if (i.publishedText == "en algún momento") { ++rawAges; CHECK_EQ(i.publishedAgeSeconds, -1); }
    }
    CHECK(rawViews >= 1);
    CHECK(rawAges >= 1);

    QByteArray next = catalog_test::fixture(QStringLiteral("next_fr.json"));
    next.replace("il y a 3 j", "il y a longtemps");
    const RendererParser fr(QStringLiteral("fr"));
    const ParsedItems related = fr.parse(json::findFirst(QJsonDocument::fromJson(next).object(), QStringLiteral("secondaryResults")));
    CHECK_EQ(static_cast<int>(related.items.size()), 20);
    int rawLockups = 0;
    for (const Item &i : related.items)
        if (i.publishedText == "il y a longtemps") { ++rawLockups; CHECK_EQ(i.publishedAgeSeconds, -1); CHECK(!i.channel.empty()); }
    CHECK(rawLockups >= 1);
}

// I18N-02: results do not depend on the process locale. Every table string and every fixture
// row is parsed under C, then again after setlocale(LC_ALL, ...) and QLocale::setDefault(...)
// for de_DE, ar_EG and ja_JP. glibc locales are compiled into a private LOCPATH with localedef
// when the system lacks them (the SDK container ships only C and en_US).
std::string digest() {
    std::ostringstream out;
    for (const LocaleSet &l : kLocales) {
        const RendererParser parser(QString::fromLatin1(l.hl));
        for (const char *f : {l.search, l.next, l.channel})
            for (const Item &i : parser.parse(catalog_test::fixtureJson(QString::fromLatin1(f))).items)
                out << i.id << ' ' << i.viewCount << ' ' << i.publishedAgeSeconds << ' ' << i.durationSeconds << ' '
                    << i.subscriberCount << ' ' << i.videoCount << ' ' << static_cast<int>(i.liveStatus) << '\n';
    }
    const char *texts[] = {"1,4 Mio. Aufrufe", "3.6M views", "1.02 مليون مشترك", "١٫٢ مليون مشاهدة",
                           "チャンネル登録者数 14.5万人", "1,02 млн подписчиков", "10 961 368 vues"};
    for (const char *hl : {"en", "de", "ar", "ja", "ru", "fr"})
        for (const char *t : texts) out << parseCountText(t, hl) << ' ' << static_cast<int>(classifyMetaPart(t, hl)) << '\n';
    out << parseAbsoluteDate("12 dic 2024", "es") << parseAbsoluteDate("12.12.2024", "de") << parseAgeSeconds("vor 15 Std.", "de");
    return out.str();
}

void processLocale() {
    const std::string reference = digest();
    QTemporaryDir locales;
    for (const char *name : {"de_DE", "ar_EG", "ja_JP"}) {
        const QByteArray full = QByteArray(name) + ".UTF-8";
        bool applied = std::setlocale(LC_ALL, full.constData()) != nullptr;
        if (!applied && locales.isValid()) {
            QProcess::execute(QStringLiteral("localedef"), QStringList() << QStringLiteral("-i") << QString::fromLatin1(name)
                              << QStringLiteral("-f") << QStringLiteral("UTF-8") << locales.path() + QLatin1Char('/') + QString::fromLatin1(full));
            qputenv("LOCPATH", locales.path().toLocal8Bit());
            applied = std::setlocale(LC_ALL, full.constData()) != nullptr;
        }
        QLocale::setDefault(QLocale(QString::fromLatin1(name)));
        if (std::string(name) == "de_DE" && applied) CHECK(std::strtod("1.5", nullptr) == 1.0);   // the trap is armed
        std::printf("  process locale %s: setlocale %s, QLocale %s\n", name, applied ? "applied" : "unavailable",
                    qPrintable(QLocale().name()));
        CHECK(digest() == reference);
        std::setlocale(LC_ALL, "C");
        QLocale::setDefault(QLocale::c());
        qunsetenv("LOCPATH");
    }
}

}  // namespace

// Generated display text (LocalizedText): ages, compact counts and unit words in the ten
// languages, with their plural rules, and a round trip through the parsers of TextParse (the
// generated text must read back as the same value, as YouTube's own text does).
static void generatedText() {
    using namespace nordtile::video::catalog;
    const std::int64_t day = 86400;
    struct Age { const char *hl; std::int64_t seconds; const char *text; };
    const Age ages[] = {
        {"en", 3 * day, "3 days ago"}, {"en", day, "1 day ago"}, {"en", 30, "1 minute ago"},
        {"es", 3 * day, "hace 3 d\xC3\xAD" "as"}, {"es", 3600, "hace 1 hora"},
        {"pt", 2 * 3600, "h\xC3\xA1 2 horas"}, {"fr", 3 * day, "il y a 3 jours"},
        {"de", 3 * day, "vor 3 Tagen"}, {"de", day, "vor 1 Tag"}, {"it", 3 * day, "3 giorni fa"},
        {"ru", day, "1 \xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4"},
        {"ru", 3 * day, "3 \xD0\xB4\xD0\xBD\xD1\x8F \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4"},
        {"ru", 5 * day, "5 \xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9 \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4"},
        {"ru", 21 * 60, "21 \xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82\xD1\x83 \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4"},
        {"ja", 3 * day, "3 \xE6\x97\xA5\xE5\x89\x8D"}, {"zh", 3 * day, "3\xE5\xA4\xA9\xE5\x89\x8D"},
        {"ar", 3 * day, "\xD9\x85\xD9\x86\xD8\xB0 3 \xD8\xA3\xD9\x8A\xD8\xA7\xD9\x85"},
        {"ar", 2 * day, "\xD9\x85\xD9\x86\xD8\xB0 \xD9\x8A\xD9\x88\xD9\x85\xD9\x8A\xD9\x86"},
        {"es-419", 14 * day, "hace 2 semanas"}, {"xx", 400 * day, "1 year ago"},
    };
    for (const Age &a : ages) CHECK_EQ(formatRelativeAge(a.seconds, a.hl), std::string(a.text));
    // Every language's generated age reads back as an age through the parser tables.
    const char *const langs[] = {"en", "es", "pt", "fr", "de", "it", "ru", "ja", "zh", "ar"};
    for (const char *hl : langs) {
        for (std::int64_t s : {std::int64_t(5 * 60), std::int64_t(3 * 3600), std::int64_t(3 * day), std::int64_t(2 * 7 * day),
                               std::int64_t(3 * 30 * day), std::int64_t(2 * 365 * day)}) {
            const std::string t = formatRelativeAge(s, hl);
            const std::int64_t back = parseAgeSeconds(t, hl);
            if (back != s) std::printf("age round trip %s: '%s' -> %lld (want %lld)\n", hl, t.c_str(), static_cast<long long>(back), static_cast<long long>(s));
            CHECK_EQ(back, s);
        }
    }
    // Compact counts and unit words.
    CHECK_EQ(formatCompactCount(1299, "en"), std::string("1.2K"));
    CHECK_EQ(formatCompactCount(12999, "en"), std::string("12K"));
    CHECK_EQ(formatCompactCount(999, "en"), std::string("999"));
    CHECK_EQ(formatCompactCount(1020000, "es"), std::string("1 M"));
    CHECK_EQ(formatCompactCount(1250000, "de"), std::string("1,2 Mio."));
    CHECK_EQ(formatCompactCount(1200000, "ja"), std::string("120\xE4\xB8\x87"));
    CHECK_EQ(formatCompactCount(-1, "en"), std::string());
    CHECK_EQ(formatViews(1, "en"), std::string("1 view"));
    CHECK_EQ(formatViews(1234567, "es"), std::string("1,2 M de visualizaciones"));
    CHECK_EQ(formatViews(5, "ru"), std::string("5 \xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBC\xD0\xBE\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2"));
    CHECK_EQ(formatSubscribers(1020000, "es"), std::string("1 M de suscriptores"));
    CHECK_EQ(formatSubscribers(1200000, "ja"), std::string("\xE3\x83\x81\xE3\x83\xA3\xE3\x83\xB3\xE3\x83\x8D\xE3\x83\xAB\xE7\x99\xBB\xE9\x8C\xB2\xE8\x80\x85\xE6\x95\xB0 120\xE4\xB8\x87\xE4\xBA\xBA"));
    CHECK_EQ(formatVideoCount(6201, "fr"), std::string("6,2 k vid\xC3\xA9os"));
    // Counts read back through the count parser in every language.
    for (const char *hl : langs) {
        for (std::int64_t n : {std::int64_t(7), std::int64_t(1500), std::int64_t(25000), std::int64_t(3000000)}) {
            const std::string v = formatViews(n, hl);
            const std::int64_t back = parseCountText(v, hl);
            if (back != n) std::printf("count round trip %s: '%s' -> %lld (want %lld)\n", hl, v.c_str(), static_cast<long long>(back), static_cast<long long>(n));
            CHECK_EQ(back, n);
        }
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    directTables();
    localeFixtures();
    crossLocale();
    rawTextKept();
    processLocale();
    generatedText();
    return catalog_test::finish("catalog_i18n");
}
