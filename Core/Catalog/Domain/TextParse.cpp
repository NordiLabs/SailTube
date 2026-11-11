// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "TextParse.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

namespace {

struct Word { const char *text; double factor; };

// Per-language tables, written from the display strings of the recorded fixtures (search,
// related and channel Videos per locale) plus the long forms of the same words. Words are
// matched as UTF-8 substrings of the normalized text (see normalize(): ASCII, Latin-1 and
// Cyrillic lowercased, Arabic diacritics and bidi marks removed), so entries are lowercase
// and written without Arabic diacritics. Magnitudes are tried in order (longer forms that
// share a prefix first); age units pick the longest match.
struct LocaleTable {
    const char *hl;
    char decimal;                      // '.' or ','
    std::vector<Word> magnitudes;      // suffix words after the number
    std::vector<Word> ageUnits;        // unit word -> seconds (Arabic duals carry 2x)
    std::vector<const char *> agoMarkers;
    std::vector<const char *> oneWords;    // "a", "an", "einem", ...: an implied 1
    std::vector<const char *> viewWords;
    std::vector<const char *> watchingWords;
    std::vector<const char *> scheduledWords;
    std::vector<const char *> videoCountWords;
    std::vector<const char *> subscriberWords;
    std::vector<const char *> noViewsWords;  // "no views" -> 0
};

const double kMin = 60, kHour = 3600, kDay = 86400, kWeek = 7 * 86400, kMonth = 30 * 86400, kYear = 365 * 86400;

const std::vector<LocaleTable> &tables() {
    static const std::vector<LocaleTable> t = {
        // "10,961,357 views", "3.6M views", "107K views", "1.02M subscribers", "6.2K videos",
        // "11 days ago", "11mo ago", "15h ago", "2w ago", "1y ago", "Dec 12, 2024".
        {"en", '.',
         {{"thousand", 1e3}, {"million", 1e6}, {"billion", 1e9}, {"k", 1e3}, {"m", 1e6}, {"b", 1e9}},
         {{"seconds", 1}, {"second", 1}, {"minutes", kMin}, {"minute", kMin}, {"hours", kHour}, {"hour", kHour},
          {"days", kDay}, {"day", kDay}, {"weeks", kWeek}, {"week", kWeek}, {"months", kMonth}, {"month", kMonth},
          {"years", kYear}, {"year", kYear}, {"mo", kMonth}, {"min", kMin}, {"sec", 1}, {"hr", kHour},
          {"y", kYear}, {"w", kWeek}, {"d", kDay}, {"h", kHour}, {"m", kMin}, {"s", 1}},
         {"ago"}, {"a ", "an "},
         {"view"}, {"watching", "waiting"}, {"scheduled", "premieres", "premiere"}, {"video"}, {"subscriber"}, {"no views"}},
        // "1.436.404 Aufrufe", "1,4 Mio. Aufrufe", "2,36 Mio. Abonnenten", "6201 Videos",
        // "vor 11 Tagen", "vor 15 Std.", "vor 10 Monaten", "vor 1 Jahr", "12.12.2024".
        {"de", ',',
         {{"tsd.", 1e3}, {"mio.", 1e6}, {"mrd.", 1e9}, {"tsd", 1e3}, {"mio", 1e6}, {"mrd", 1e9}, {"tausend", 1e3},
          {"millionen", 1e6}, {"million", 1e6}, {"milliarden", 1e9}, {"milliarde", 1e9}},
         {{"sekunden", 1}, {"sekunde", 1}, {"sek", 1}, {"minuten", kMin}, {"minute", kMin}, {"min", kMin},
          {"stunden", kHour}, {"stunde", kHour}, {"std", kHour}, {"tagen", kDay}, {"tag", kDay}, {"wochen", kWeek},
          {"woche", kWeek}, {"monaten", kMonth}, {"monat", kMonth}, {"jahren", kYear}, {"jahr", kYear}},
         {"vor "}, {"einem ", "einer "},
         {"aufruf"}, {"zuschauer", "wartend", "warten"}, {"geplant", "premiere"}, {"video"}, {"abonnent"}, {"keine aufrufe"}},
        // "1.381.345 visualizaciones", "1,3 M de visualizaciones", "370 K", "6,2 K vídeos",
        // "1,02 M de suscriptores", "hace 11 días", "hace 1 a", "hace 10 m", "hace 2 sem.",
        // "hace 3 d", "hace 18 h", "12 dic 2024".
        {"es", ',',
         {{"mil millones", 1e9}, {"mil m", 1e9}, {"millones", 1e6}, {"millón", 1e6}, {"mill.", 1e6}, {"mil", 1e3},
          {"m", 1e6}, {"k", 1e3}},
         {{"segundos", 1}, {"segundo", 1}, {"seg", 1}, {"minutos", kMin}, {"minuto", kMin}, {"min", kMin},
          {"horas", kHour}, {"hora", kHour}, {"días", kDay}, {"día", kDay}, {"semanas", kWeek}, {"semana", kWeek},
          {"sem", kWeek}, {"meses", kMonth}, {"mes", kMonth}, {"años", kYear}, {"año", kYear},
          {"a", kYear}, {"m", kMonth}, {"d", kDay}, {"h", kHour}, {"s", 1}},
         {"hace "}, {"un ", "una "},
         {"visualizaci", "vistas", "reproducci"}, {"espectador", "viendo", "esperando"}, {"programado", "estreno", "se estrena"},
         {"vídeo", "video"}, {"suscriptor"}, {"sin visualizaciones", "ninguna visualización"}},
        // "11.332.182 visualizações", "11 mi de visualizações", "346 mil", "6,2 mil vídeos",
        // "881 mil inscritos", "há 11 dias", "há 4 sem.", "há 1 mês", "12 de dez. de 2024".
        {"pt", ',',
         {{"mil mi", 1e9}, {"bilhões", 1e9}, {"bilhão", 1e9}, {"bi", 1e9}, {"milhões", 1e6}, {"milhão", 1e6},
          {"mi", 1e6}, {"mil", 1e3}},
         {{"segundos", 1}, {"segundo", 1}, {"seg", 1}, {"minutos", kMin}, {"minuto", kMin}, {"min", kMin},
          {"horas", kHour}, {"hora", kHour}, {"dias", kDay}, {"dia", kDay}, {"semanas", kWeek}, {"semana", kWeek},
          {"sem", kWeek}, {"meses", kMonth}, {"mês", kMonth}, {"anos", kYear}, {"ano", kYear},
          {"d", kDay}, {"h", kHour}},
         {"há ", "atrás"}, {"um ", "uma "},
         {"visualiza"}, {"assistindo", "aguardando", "espectador"}, {"programad", "estreia"}, {"vídeo", "video"},
         {"inscrito"}, {"nenhuma visualização", "sem visualizações"}},
        // "10 961 368 vues" (narrow no-break spaces), "3,6 M de vues", "107 k vues",
        // "1,02 M d'abonnés", "6,2 k vidéos", "il y a 11 jours", "il y a 1 a", "il y a 10 m.",
        // "il y a 3 j", "il y a 18 h", "il y a 2 sem.", "12 déc. 2024".
        {"fr", ',',
         {{"milliards", 1e9}, {"milliard", 1e9}, {"millions", 1e6}, {"million", 1e6}, {"mille", 1e3}, {"md", 1e9},
          {"k", 1e3}, {"m", 1e6}},
         {{"secondes", 1}, {"seconde", 1}, {"minutes", kMin}, {"minute", kMin}, {"min", kMin}, {"heures", kHour},
          {"heure", kHour}, {"jours", kDay}, {"jour", kDay}, {"semaines", kWeek}, {"semaine", kWeek}, {"sem", kWeek},
          {"mois", kMonth}, {"ans", kYear}, {"an", kYear},
          {"a", kYear}, {"m", kMonth}, {"j", kDay}, {"h", kHour}, {"s", 1}},
         {"il y a"}, {"un ", "une "},
         {"vue"}, {"spectateur", "regardent", "en attente"}, {"programmé", "prévu", "planifié", "première"},
         {"vidéo", "video"}, {"abonné"}, {"aucune vue"}},
        // "10.489.826 visualizzazioni", "10 Mln di visualizzazioni", "346K", "881K iscritti",
        // "6,2K video", "11 giorni fa", "3 gg fa", "2 sett. fa", "1 mese fa", "12 dic 2024".
        {"it", ',',
         {{"miliardi", 1e9}, {"miliardo", 1e9}, {"milioni", 1e6}, {"milione", 1e6}, {"mrd", 1e9}, {"mld", 1e9},
          {"mln", 1e6}, {"mila", 1e3}, {"k", 1e3}},
         {{"secondi", 1}, {"secondo", 1}, {"sec", 1}, {"minuti", kMin}, {"minuto", kMin}, {"min", kMin}, {"ore", kHour},
          {"ora", kHour}, {"giorni", kDay}, {"giorno", kDay}, {"gg", kDay}, {"settimane", kWeek}, {"settimana", kWeek},
          {"sett", kWeek}, {"mesi", kMonth}, {"mese", kMonth}, {"anni", kYear}, {"anno", kYear}, {"h", kHour}},
         {" fa"}, {"un ", "una "},
         {"visualizzazion"}, {"spettator", "in attesa", "stanno guardando"}, {"programmat", "prima visione"},
         {"video"}, {"iscritt"}, {"nessuna visualizzazione"}},
        // "13 936 981 просмотр", "3,6 млн просмотров", "370 тыс.", "6,2 тыс. видео",
        // "1,02 млн подписчиков", "11 дней назад", "18 ч назад", "2 дн. назад", "2 нед. назад",
        // "1 мес. назад", "1 г. назад", "5 л. назад", "час назад", "12 дек. 2024 г.".
        {"ru", ',',
         {{"тыс.", 1e3}, {"тыс", 1e3}, {"млрд", 1e9}, {"млн", 1e6}},
         {{"секунд", 1}, {"сек", 1}, {"минут", kMin}, {"мин", kMin}, {"час", kHour}, {"ч", kHour}, {"день", kDay},
          {"дней", kDay}, {"дня", kDay}, {"дн", kDay}, {"недел", kWeek}, {"нед", kWeek}, {"месяц", kMonth},
          {"мес", kMonth}, {"год", kYear}, {"г", kYear}, {"лет", kYear}, {"л", kYear}},
         {"назад"}, {},
         {"просмотр"}, {"зрител", "смотрят", "ожида"}, {"запланир", "премьера"}, {"видео"}, {"подписчик"},
         {"нет просмотров"}},
        // "10,489,680回視聴", "1048万回視聴", "1.8万回視聴", "チャンネル登録者数 14.5万人",
        // "6201 本の動画", "11 日前", "7 か月前", "15 時間前", "2 週間前", "2024/12/12".
        {"ja", '.',
         {{"万", 1e4}, {"億", 1e8}},
         {{"秒", 1}, {"分", kMin}, {"時間", kHour}, {"日", kDay}, {"週間", kWeek}, {"か月", kMonth}, {"ヶ月", kMonth},
          {"ヵ月", kMonth}, {"年", kYear}},
         {"前"}, {},
         {"回視聴", "視聴回数"}, {"視聴中", "待機中"}, {"公開予定", "プレミア"}, {"本の動画", "動画"}, {"登録者"}, {"視聴なし"}},
        // Simplified (zh-CN) and Traditional forms: "10,961,368次观看", "1096万次观看", "10万",
        // "102万位订阅者", "6201 个视频", "11天前", "10个月前", "2周前", "直播时间：4天前",
        // "2024年12月12日".
        {"zh", '.',
         {{"万", 1e4}, {"萬", 1e4}, {"亿", 1e8}, {"億", 1e8}, {"千", 1e3}},
         {{"秒", 1}, {"分钟", kMin}, {"分鐘", kMin}, {"小时", kHour}, {"小時", kHour}, {"天", kDay}, {"周", kWeek},
          {"週", kWeek}, {"星期", kWeek}, {"个月", kMonth}, {"個月", kMonth}, {"月", kMonth}, {"年", kYear}},
         {"前"}, {},
         {"次观看", "次觀看", "观看次数", "觀看次數", "次播放"}, {"正在观看", "正在觀看", "人在等待", "等待中"},
         {"预定", "預定", "首播"}, {"个视频", "部影片", "個影片"},
         {"订阅者", "訂閱者", "位订阅者"}, {"无人观看", "沒有人觀看", "没有观看"}},
        // "10,961,368 مشاهدة", "3.6 مليون مشاهدة", "782 ألف", "6.2 ألف من الفيديوهات",
        // "1.02 مليون مشترك", "قبل 11 يومًا", "قبل 10 أشهر", "قبل سنة واحدة", "قبل سنتين",
        // "قبل أسبوعين", "قبل شهر واحد", "قبل شهرين", "خلال 10 أشهر", "12‏/12‏/2024".
        // Arabic-Indic digits and separators are normalized to ASCII first.
        {"ar", '.',
         {{"مليار", 1e9}, {"مليون", 1e6}, {"ملايين", 1e6}, {"ألف", 1e3}, {"آلاف", 1e3}, {"الف", 1e3}},
         {{"ثانية", 1}, {"ثانيتين", 2}, {"ثانيتان", 2}, {"ثوان", 1},
          {"دقيقة", kMin}, {"دقيقتين", 2 * kMin}, {"دقيقتان", 2 * kMin}, {"دقائق", kMin},
          {"ساعة", kHour}, {"ساعتين", 2 * kHour}, {"ساعتان", 2 * kHour}, {"ساعات", kHour},
          {"يوم", kDay}, {"يومين", 2 * kDay}, {"يومان", 2 * kDay}, {"أيام", kDay},
          {"أسبوع", kWeek}, {"أسبوعين", 2 * kWeek}, {"أسبوعان", 2 * kWeek}, {"أسابيع", kWeek},
          {"شهر", kMonth}, {"شهرين", 2 * kMonth}, {"شهران", 2 * kMonth}, {"أشهر", kMonth}, {"شهور", kMonth},
          {"سنة", kYear}, {"سنتين", 2 * kYear}, {"سنتان", 2 * kYear}, {"سنوات", kYear}, {"سنين", kYear},
          {"عام", kYear}, {"عامين", 2 * kYear}, {"أعوام", kYear}},
         // "خلال" ("within") is what the related list shows for some past ages ("خلال 10 أشهر").
         {"قبل", "منذ", "خلال"}, {},
         {"مشاهدة", "مشاهدات"}, {"يشاهد", "في انتظار", "بانتظار"}, {"مجدول", "العرض الأول"}, {"فيديو"}, {"مشترك"},
         {"لا توجد مشاهدات", "لا مشاهدات", "بدون مشاهدات"}},
    };
    return t;
}

std::string lowerAscii(const std::string &s) {
    std::string r(s);
    for (char &c : r) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return r;
}

const LocaleTable &tableFor(const std::string &hl) {
    const std::string base = baseLanguage(lowerAscii(hl));
    for (const LocaleTable &t : tables()) if (base == t.hl) return t;
    return tables().front();
}

bool contains(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

bool isAsciiLetter(char c) { return c >= 'a' && c <= 'z'; }

// `needle` as a whole word: not glued to ASCII letters on either side ("ago" is not in "chicago").
bool containsWord(const std::string &hay, const char *needle) {
    const size_t len = std::strlen(needle);
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + 1)) {
        const bool before = p == 0 || needle[0] == ' ' || !isAsciiLetter(hay[p - 1]);
        const bool after = p + len >= hay.size() || needle[len - 1] == ' ' || !isAsciiLetter(hay[p + len]);
        if (before && after) return true;
    }
    return false;
}

bool containsAny(const std::string &hay, const std::vector<const char *> &needles) {
    for (const char *n : needles) if (contains(hay, n)) return true;
    return false;
}

// Byte-level normalization, independent of the process locale:
//  * digits: Arabic-Indic (U+0660..0669), Extended Arabic-Indic (U+06F0..06F9) and full-width
//    (U+FF10..FF19) to ASCII; the Arabic decimal separator U+066B to '.', the Arabic thousands
//    separator U+066C to ',' (the Arabic table uses '.' as decimal);
//  * spaces: NBSP, narrow NBSP, thin/figure/punctuation spaces to ' ';
//  * removed: bidi marks (LRM, RLM, ALM), Arabic diacritics (U+064B..065F) and tatweel;
//  * lowercased: ASCII, Latin-1 capitals (U+00C0..00DE) and basic Cyrillic (U+0400..042F).
std::string normalize(const std::string &in) {
    std::string out;
    out.reserve(in.size());
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        const unsigned char d = i + 1 < n ? static_cast<unsigned char>(in[i + 1]) : 0;
        const unsigned char e = i + 2 < n ? static_cast<unsigned char>(in[i + 2]) : 0;
        if (c == 0xC2 && d == 0xA0) { out += ' '; i += 2; continue; }
        if (c == 0xE2 && d == 0x80) {
            if (e == 0xAF || (e >= 0x80 && e <= 0x8A)) { out += ' '; i += 3; continue; }   // U+2000..200A, U+202F
            if (e == 0x8E || e == 0x8F) { i += 3; continue; }                                // LRM, RLM
        }
        if (c == 0xD8 && d == 0x9C) { i += 2; continue; }                                    // ALM U+061C
        if (c == 0xD9) {
            if (d >= 0xA0 && d <= 0xA9) { out += static_cast<char>('0' + (d - 0xA0)); i += 2; continue; }
            if (d == 0xAB) { out += '.'; i += 2; continue; }
            if (d == 0xAC) { out += ','; i += 2; continue; }
            if ((d >= 0x8B && d <= 0x9F) || d == 0x80) { i += 2; continue; }               // harakat, tatweel
        }
        if (c == 0xDB && d >= 0xB0 && d <= 0xB9) { out += static_cast<char>('0' + (d - 0xB0)); i += 2; continue; }
        if (c == 0xEF && d == 0xBC && e >= 0x90 && e <= 0x99) { out += static_cast<char>('0' + (e - 0x90)); i += 3; continue; }
        if (c == 0xC3 && d >= 0x80 && d <= 0x9E && d != 0x97) {                               // À..Þ except ×
            out += static_cast<char>(c); out += static_cast<char>(d + 0x20); i += 2; continue;
        }
        if (c == 0xD0 && d >= 0x90 && d <= 0x9F) { out += static_cast<char>(0xD0); out += static_cast<char>(d + 0x20); i += 2; continue; }  // А..П
        if (c == 0xD0 && d >= 0xA0 && d <= 0xAF) { out += static_cast<char>(0xD1); out += static_cast<char>(d - 0x20); i += 2; continue; }  // Р..Я
        if (c == 0xD0 && d >= 0x80 && d <= 0x8F) { out += static_cast<char>(0xD1); out += static_cast<char>(d + 0x10); i += 2; continue; }  // Ѐ..Џ
        out += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : static_cast<char>(c);
        ++i;
    }
    return out;
}

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// Reads the first number in `s` honouring the table's decimal separator. Group separators
// (the other of '.'/',', spaces, apostrophes) are skipped while followed by exactly three
// digits. Sets `end` to the index after the number. Returns NaN when there is none. The value
// is assembled by hand: strtod/stod follow LC_NUMERIC and would read "1.5" as 1 under de_DE.
double readNumber(const std::string &s, char decimal, size_t &end, size_t from = 0) {
    size_t i = from;
    while (i < s.size() && !isDigit(s[i])) ++i;
    if (i >= s.size()) { end = s.size(); return std::nan(""); }
    const char group = decimal == '.' ? ',' : '.';
    double value = 0, scale = 1;
    bool seenDecimal = false;
    while (i < s.size()) {
        const char c = s[i];
        if (isDigit(c)) {
            if (seenDecimal) { scale /= 10; value += (c - '0') * scale; }
            else value = value * 10 + (c - '0');
            ++i;
            continue;
        }
        const bool threeDigitsFollow = i + 3 < s.size() && isDigit(s[i + 1]) && isDigit(s[i + 2]) && isDigit(s[i + 3]) &&
            (i + 4 >= s.size() || !isDigit(s[i + 4]));
        if ((c == group || c == ' ' || c == '\'') && !seenDecimal && threeDigitsFollow) { ++i; continue; }
        if (c == decimal && !seenDecimal && i + 1 < s.size() && isDigit(s[i + 1])) { seenDecimal = true; ++i; continue; }
        break;
    }
    end = i;
    return value;
}

bool wordBoundaryAfter(const std::string &s, size_t pos) {
    if (pos >= s.size()) return true;
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    return !(c >= 'a' && c <= 'z');
}

double magnitudeAt(const std::string &s, size_t pos, const LocaleTable &t) {
    while (pos < s.size() && s[pos] == ' ') ++pos;
    for (const Word &w : t.magnitudes) {
        const size_t n = std::strlen(w.text);
        if (s.compare(pos, n, w.text) == 0) {
            const bool ascii = static_cast<unsigned char>(w.text[0]) < 0x80;
            if (!ascii || wordBoundaryAfter(s, pos + n) || w.text[n - 1] == '.') return w.factor;
        }
    }
    return 1;
}

std::int64_t countWith(const std::string &norm, const LocaleTable &t) {
    if (containsAny(norm, t.noViewsWords)) return 0;
    size_t end = 0;
    const double v = readNumber(norm, t.decimal, end);
    if (std::isnan(v)) return -1;
    return static_cast<std::int64_t>(std::llround(v * magnitudeAt(norm, end, t)));
}

std::int64_t ageWith(const std::string &norm, const LocaleTable &t) {
    if (!containsAny(norm, t.agoMarkers)) return -1;
    size_t end = 0;
    double n = readNumber(norm, '.', end);
    size_t unitFrom = end;
    if (std::isnan(n)) {
        // "a day ago", "vor einem Tag", "il y a un an": an implied one.
        bool implied = false;
        for (const char *w : t.oneWords) {
            const size_t p = norm.find(w);
            if (p != std::string::npos) { implied = true; unitFrom = p + std::strlen(w); break; }
        }
        if (!implied) {
            // Russian/Arabic singular forms carry no number ("час назад", "قبل يوم").
            unitFrom = 0;
        }
        n = 1;
    }
    // Longest unit word starting after the number (skipping spaces), else anywhere after it.
    size_t p = unitFrom;
    while (p < norm.size() && norm[p] == ' ') ++p;
    double best = -1; size_t bestLen = 0;
    for (const Word &w : t.ageUnits) {
        const size_t len = std::strlen(w.text);
        const bool ascii = static_cast<unsigned char>(w.text[0]) < 0x80;
        if (norm.compare(p, len, w.text) == 0 && (!ascii || wordBoundaryAfter(norm, p + len) || len > 2)) {
            if (len > bestLen) { best = w.factor; bestLen = len; }
        }
    }
    if (best < 0) {
        for (const Word &w : t.ageUnits) {
            const size_t len = std::strlen(w.text);
            if (len < 3) continue;  // single-letter compact units only directly after the number
            if (norm.find(w.text, unitFrom) != std::string::npos && len > bestLen) { best = w.factor; bestLen = len; }
        }
    }
    if (best < 0) return -1;
    return static_cast<std::int64_t>(std::llround(n * best));
}

std::int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

struct MonthName { const char *name; int month; };
const std::vector<MonthName> &monthNames() {
    static const std::vector<MonthName> m = {
        // en
        {"january", 1}, {"february", 2}, {"march", 3}, {"april", 4}, {"may", 5}, {"june", 6}, {"july", 7},
        {"august", 8}, {"september", 9}, {"october", 10}, {"november", 11}, {"december", 12},
        {"jan", 1}, {"feb", 2}, {"mar", 3}, {"apr", 4}, {"jun", 6}, {"jul", 7}, {"aug", 8}, {"sep", 9}, {"oct", 10},
        {"nov", 11}, {"dec", 12},
        // de
        {"januar", 1}, {"jänner", 1}, {"februar", 2}, {"märz", 3}, {"mai", 5}, {"juni", 6}, {"juli", 7},
        {"oktober", 10}, {"dezember", 12}, {"okt", 10}, {"dez", 12}, {"mär", 3},
        // es / pt / it / fr
        {"enero", 1}, {"febrero", 2}, {"marzo", 3}, {"abril", 4}, {"mayo", 5}, {"junio", 6}, {"julio", 7},
        {"agosto", 8}, {"septiembre", 9}, {"setiembre", 9}, {"octubre", 10}, {"noviembre", 11}, {"diciembre", 12},
        {"ene", 1}, {"abr", 4}, {"ago", 8}, {"dic", 12},
        {"janeiro", 1}, {"fevereiro", 2}, {"março", 3}, {"maio", 5}, {"junho", 6}, {"julho", 7}, {"setembro", 9},
        {"outubro", 10}, {"novembro", 11}, {"dezembro", 12}, {"fev", 2}, {"set", 9}, {"out", 10},
        {"gennaio", 1}, {"febbraio", 2}, {"aprile", 4}, {"maggio", 5}, {"giugno", 6}, {"luglio", 7},
        {"settembre", 9}, {"ottobre", 10}, {"dicembre", 12}, {"gen", 1}, {"mag", 5}, {"giu", 6}, {"lug", 7}, {"ott", 10},
        {"janvier", 1}, {"février", 2}, {"mars", 3}, {"avril", 4}, {"juin", 6}, {"juillet", 7}, {"août", 8},
        {"septembre", 9}, {"octobre", 10}, {"novembre", 11}, {"décembre", 12}, {"janv", 1}, {"févr", 2}, {"avr", 4},
        {"juil", 7}, {"sept", 9}, {"déc", 12},
        // ru (genitive and short forms)
        {"января", 1}, {"февраля", 2}, {"марта", 3}, {"апреля", 4}, {"мая", 5}, {"июня", 6}, {"июля", 7},
        {"августа", 8}, {"сентября", 9}, {"октября", 10}, {"ноября", 11}, {"декабря", 12},
        {"янв", 1}, {"февр", 2}, {"фев", 2}, {"мар", 3}, {"апр", 4}, {"май", 5}, {"июн", 6}, {"июл", 7},
        {"авг", 8}, {"сент", 9}, {"сен", 9}, {"окт", 10}, {"нояб", 11}, {"ноя", 11}, {"дек", 12},
        // ar (Egypt/Gulf and Levant names)
        {"يناير", 1}, {"فبراير", 2}, {"مارس", 3}, {"أبريل", 4}, {"إبريل", 4}, {"مايو", 5}, {"يونيو", 6},
        {"يوليو", 7}, {"أغسطس", 8}, {"سبتمبر", 9}, {"أكتوبر", 10}, {"نوفمبر", 11}, {"ديسمبر", 12},
        {"كانون الثاني", 1}, {"شباط", 2}, {"آذار", 3}, {"نيسان", 4}, {"أيار", 5}, {"حزيران", 6}, {"تموز", 7},
        {"آب", 8}, {"أيلول", 9}, {"تشرين الأول", 10}, {"تشرين الثاني", 11}, {"كانون الأول", 12},
        // ja / zh dates are numeric ("2024年12月12日", "2024/12/12").
    };
    return m;
}

int monthIn(const std::string &norm) {
    int best = 0; size_t bestLen = 0;
    for (const MonthName &m : monthNames()) {
        const size_t len = std::strlen(m.name);
        size_t p = norm.find(m.name);
        while (p != std::string::npos) {
            const bool asciiName = static_cast<unsigned char>(m.name[0]) < 0x80;
            const bool startOk = p == 0 || !asciiName || !(norm[p - 1] >= 'a' && norm[p - 1] <= 'z');
            const bool endOk = !asciiName || wordBoundaryAfter(norm, p + len) || norm[p + len] == '.';
            if (startOk && endOk && len > bestLen) { best = m.month; bestLen = len; }
            p = norm.find(m.name, p + 1);
        }
    }
    return best;
}

std::vector<long> allNumbers(const std::string &s) {
    std::vector<long> out;
    size_t i = 0;
    while (i < s.size()) {
        if (isDigit(s[i])) {
            long v = 0;
            while (i < s.size() && isDigit(s[i])) { v = v * 10 + (s[i] - '0'); ++i; }
            out.push_back(v);
        } else {
            ++i;
        }
    }
    return out;
}

}  // namespace

std::string baseLanguage(const std::string &code) {
    std::string base;
    for (char c : code) {
        if (c == '-' || c == '_') break;
        base += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return base;
}

std::int64_t parseDurationText(const std::string &text) {
    const std::string s = normalize(text);
    std::vector<long> parts;
    size_t i = 0;
    while (i < s.size() && !isDigit(s[i])) ++i;
    const size_t start = i;
    long cur = -1;
    for (; i < s.size(); ++i) {
        if (isDigit(s[i])) { cur = (cur < 0 ? 0 : cur * 10) + (s[i] - '0'); continue; }
        if ((s[i] == ':' || s[i] == '.') && cur >= 0) { parts.push_back(cur); cur = -1; continue; }
        break;
    }
    if (cur >= 0) parts.push_back(cur);
    if (parts.size() >= 2 && parts.size() <= 3 && i > start) {
        for (size_t k = 1; k < parts.size(); ++k) if (parts[k] > 59) return -1;
        std::int64_t total = 0;
        for (long p : parts) total = total * 60 + p;
        return total;
    }
    // English accessibility form: "1 hour, 5 minutes, 56 seconds".
    std::int64_t total = 0; bool any = false;
    size_t end = 0, from = 0;
    while (true) {
        const double n = readNumber(s, '.', end, from);
        if (std::isnan(n)) break;
        size_t p = end; while (p < s.size() && s[p] == ' ') ++p;
        if (s.compare(p, 4, "hour") == 0) total += static_cast<std::int64_t>(n) * 3600;
        else if (s.compare(p, 6, "minute") == 0) total += static_cast<std::int64_t>(n) * 60;
        else if (s.compare(p, 6, "second") == 0) total += static_cast<std::int64_t>(n);
        else return -1;
        any = true;
        from = p;
        while (from < s.size() && !isDigit(s[from])) ++from;
        if (from >= s.size()) break;
    }
    return any ? total : -1;
}

std::int64_t parseCountText(const std::string &text, const std::string &hl) {
    const std::string norm = normalize(text);
    const LocaleTable &t = tableFor(hl);
    std::int64_t v = countWith(norm, t);
    if (v < 0 && &t != &tables().front()) v = countWith(norm, tables().front());
    return v;
}

std::int64_t parseAgeSeconds(const std::string &text, const std::string &hl) {
    const std::string norm = normalize(text);
    const LocaleTable &t = tableFor(hl);
    std::int64_t v = ageWith(norm, t);
    if (v >= 0) return v;
    // YouTube sometimes answers a row in another language than requested; try the others.
    for (const LocaleTable &other : tables()) {
        if (&other == &t) continue;
        v = ageWith(norm, other);
        if (v >= 0) return v;
    }
    return -1;
}

std::int64_t parseAbsoluteDate(const std::string &text, const std::string &hl) {
    const std::string norm = normalize(text);
    const std::vector<long> nums = allNumbers(norm);
    int y = 0, m = 0, d = 0;
    const int named = monthIn(norm);
    if (named) {
        m = named;
        for (long n : nums) {
            if (n >= 1900 && n <= 2200) y = static_cast<int>(n);
            else if (n >= 1 && n <= 31 && !d) d = static_cast<int>(n);
        }
    } else if (nums.size() >= 3) {
        if (nums[0] >= 1900) {           // 2009/10/24, 2009-10-24, 2009年10月24日
            y = static_cast<int>(nums[0]); m = static_cast<int>(nums[1]); d = static_cast<int>(nums[2]);
        } else if (nums[2] >= 1900) {    // 24.10.2009 (day first) or 10/24/2009 (en-US)
            y = static_cast<int>(nums[2]);
            const bool monthFirst = baseLanguage(hl) == "en" && norm.find('/') != std::string::npos;
            m = static_cast<int>(monthFirst ? nums[0] : nums[1]);
            d = static_cast<int>(monthFirst ? nums[1] : nums[0]);
        }
    }
    if (y < 1900 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    return daysFromCivil(y, static_cast<unsigned>(m), static_cast<unsigned>(d)) * 86400;
}

MetaPart classifyMetaPart(const std::string &text, const std::string &hl) {
    const std::string norm = normalize(text);
    const LocaleTable &t = tableFor(hl);
    const LocaleTable &en = tables().front();
    auto has = [&](const std::vector<const char *> LocaleTable::*field) {
        return containsAny(norm, t.*field) || containsAny(norm, en.*field);
    };
    // Count words only classify a part that carries a number, so an owner label such as
    // "Tech Review" or "Video Lab" stays the owner.
    const bool digit = std::any_of(norm.begin(), norm.end(), isDigit);
    if (containsAny(norm, t.noViewsWords) || containsAny(norm, en.noViewsWords)) return MetaPart::Views;
    if (digit && has(&LocaleTable::watchingWords)) return MetaPart::Watching;
    if (has(&LocaleTable::scheduledWords)) return MetaPart::Scheduled;
    if (digit && has(&LocaleTable::subscriberWords)) return MetaPart::Subscribers;
    if (digit && has(&LocaleTable::viewWords)) return MetaPart::Views;
    if (parseAgeSeconds(text, hl) >= 0) return MetaPart::Age;
    // An age the tables cannot read ("il y a longtemps") is still an age: the row keeps its
    // raw text (I18N-02). Word markers only; the CJK marker 前 also occurs in owner names.
    for (const char *m : t.agoMarkers)
        if (static_cast<unsigned char>(m[0]) < 0xE0 && containsWord(norm, m)) return MetaPart::Age;
    if (digit && has(&LocaleTable::videoCountWords)) return MetaPart::VideoCount;
    // A bare count ("3.6M", "363万") in a lockup's stats row is the view count.
    size_t end = 0;
    const double n = readNumber(norm, t.decimal, end);
    if (!std::isnan(n)) {
        size_t p = end; while (p < norm.size() && norm[p] == ' ') ++p;
        const double mag = magnitudeAt(norm, end, t);
        if (p >= norm.size() || mag > 1) return MetaPart::Views;
    }
    return MetaPart::Unknown;
}

std::int64_t parseSubscriberText(const std::string &text, const std::string &hl) {
    return parseCountText(text, hl);
}

}}}
