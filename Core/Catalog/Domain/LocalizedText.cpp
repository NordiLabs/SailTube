// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LocalizedText.h"

#include "TextParse.h"

#include <cstdio>

namespace nordtile { namespace video { namespace catalog {

namespace {

enum class Lang { En, Es, Pt, Fr, De, It, Ru, Ja, Zh, Ar };

Lang langOf(const std::string &hl) {
    const std::string b = baseLanguage(hl);
    if (b == "es") return Lang::Es;
    if (b == "pt") return Lang::Pt;
    if (b == "fr") return Lang::Fr;
    if (b == "de") return Lang::De;
    if (b == "it") return Lang::It;
    if (b == "ru") return Lang::Ru;
    if (b == "ja") return Lang::Ja;
    if (b == "zh") return Lang::Zh;
    if (b == "ar") return Lang::Ar;
    return Lang::En;
}

std::string num(std::int64_t n) { return std::to_string(static_cast<long long>(n)); }

// Russian plural class of n: 0 one (1, 21, 31...), 1 few (2-4, 22-24...), 2 many.
int ruForm(std::int64_t n) {
    const std::int64_t d = n % 10, h = n % 100;
    if (d == 1 && h != 11) return 0;
    if (d >= 2 && d <= 4 && (h < 12 || h > 14)) return 1;
    return 2;
}

enum Unit { Minute, Hour, Day, Week, Month, Year };

struct Words { const char *one; const char *other; };

std::string ageText(Lang lang, Unit unit, std::int64_t n) {
    switch (lang) {
    case Lang::En: {
        static const Words w[] = {{"minute", "minutes"}, {"hour", "hours"}, {"day", "days"},
                                  {"week", "weeks"}, {"month", "months"}, {"year", "years"}};
        return num(n) + " " + (n == 1 ? w[unit].one : w[unit].other) + " ago";
    }
    case Lang::Es: {
        static const Words w[] = {{"minuto", "minutos"}, {"hora", "horas"}, {"d\xC3\xAD" "a", "d\xC3\xAD" "as"},
                                  {"semana", "semanas"}, {"mes", "meses"}, {"a\xC3\xB1o", "a\xC3\xB1os"}};
        return "hace " + num(n) + " " + (n == 1 ? w[unit].one : w[unit].other);
    }
    case Lang::Pt: {
        static const Words w[] = {{"minuto", "minutos"}, {"hora", "horas"}, {"dia", "dias"},
                                  {"semana", "semanas"}, {"m\xC3\xAAs", "meses"}, {"ano", "anos"}};
        return "h\xC3\xA1 " + num(n) + " " + (n == 1 ? w[unit].one : w[unit].other);
    }
    case Lang::Fr: {
        static const Words w[] = {{"minute", "minutes"}, {"heure", "heures"}, {"jour", "jours"},
                                  {"semaine", "semaines"}, {"mois", "mois"}, {"an", "ans"}};
        return "il y a " + num(n) + " " + (n <= 1 ? w[unit].one : w[unit].other);
    }
    case Lang::De: {
        // Dative after "vor": "vor 1 Tag", "vor 3 Tagen".
        static const Words w[] = {{"Minute", "Minuten"}, {"Stunde", "Stunden"}, {"Tag", "Tagen"},
                                  {"Woche", "Wochen"}, {"Monat", "Monaten"}, {"Jahr", "Jahren"}};
        return "vor " + num(n) + " " + (n == 1 ? w[unit].one : w[unit].other);
    }
    case Lang::It: {
        static const Words w[] = {{"minuto", "minuti"}, {"ora", "ore"}, {"giorno", "giorni"},
                                  {"settimana", "settimane"}, {"mese", "mesi"}, {"anno", "anni"}};
        return num(n) + " " + (n == 1 ? w[unit].one : w[unit].other) + " fa";
    }
    case Lang::Ru: {
        // Accusative after a number: "1 минуту", "2 минуты", "5 минут".
        static const char *const w[][3] = {
            {"\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82\xD1\x83", "\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82\xD1\x8B", "\xD0\xBC\xD0\xB8\xD0\xBD\xD1\x83\xD1\x82"},
            {"\xD1\x87\xD0\xB0\xD1\x81", "\xD1\x87\xD0\xB0\xD1\x81\xD0\xB0", "\xD1\x87\xD0\xB0\xD1\x81\xD0\xBE\xD0\xB2"},
            {"\xD0\xB4\xD0\xB5\xD0\xBD\xD1\x8C", "\xD0\xB4\xD0\xBD\xD1\x8F", "\xD0\xB4\xD0\xBD\xD0\xB5\xD0\xB9"},
            {"\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8E", "\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD0\xB8", "\xD0\xBD\xD0\xB5\xD0\xB4\xD0\xB5\xD0\xBB\xD1\x8C"},
            {"\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86\xD0\xB0", "\xD0\xBC\xD0\xB5\xD1\x81\xD1\x8F\xD1\x86\xD0\xB5\xD0\xB2"},
            {"\xD0\xB3\xD0\xBE\xD0\xB4", "\xD0\xB3\xD0\xBE\xD0\xB4\xD0\xB0", "\xD0\xBB\xD0\xB5\xD1\x82"}};
        return num(n) + " " + w[unit][ruForm(n)] + " \xD0\xBD\xD0\xB0\xD0\xB7\xD0\xB0\xD0\xB4";   // назад
    }
    case Lang::Ja: {
        static const char *const w[] = {"\xE5\x88\x86", "\xE6\x99\x82\xE9\x96\x93", "\xE6\x97\xA5",
                                        "\xE9\x80\xB1\xE9\x96\x93", "\xE3\x81\x8B\xE6\x9C\x88", "\xE5\xB9\xB4"};
        return num(n) + " " + w[unit] + "\xE5\x89\x8D";   // 前
    }
    case Lang::Zh: {
        static const char *const w[] = {"\xE5\x88\x86\xE9\x92\x9F", "\xE5\xB0\x8F\xE6\x97\xB6", "\xE5\xA4\xA9",
                                        "\xE5\x91\xA8", "\xE4\xB8\xAA\xE6\x9C\x88", "\xE5\xB9\xB4"};
        return num(n) + w[unit] + "\xE5\x89\x8D";
    }
    case Lang::Ar: {
        // singular, dual, plural (3-10), accusative singular (11+).
        static const char *const w[][4] = {
            {"\xD8\xAF\xD9\x82\xD9\x8A\xD9\x82\xD8\xA9", "\xD8\xAF\xD9\x82\xD9\x8A\xD9\x82\xD8\xAA\xD9\x8A\xD9\x86", "\xD8\xAF\xD9\x82\xD8\xA7\xD8\xA6\xD9\x82", "\xD8\xAF\xD9\x82\xD9\x8A\xD9\x82\xD8\xA9"},
            {"\xD8\xB3\xD8\xA7\xD8\xB9\xD8\xA9", "\xD8\xB3\xD8\xA7\xD8\xB9\xD8\xAA\xD9\x8A\xD9\x86", "\xD8\xB3\xD8\xA7\xD8\xB9\xD8\xA7\xD8\xAA", "\xD8\xB3\xD8\xA7\xD8\xB9\xD8\xA9"},
            {"\xD9\x8A\xD9\x88\xD9\x85", "\xD9\x8A\xD9\x88\xD9\x85\xD9\x8A\xD9\x86", "\xD8\xA3\xD9\x8A\xD8\xA7\xD9\x85", "\xD9\x8A\xD9\x88\xD9\x85\xD9\x8B\xD8\xA7"},
            {"\xD8\xA3\xD8\xB3\xD8\xA8\xD9\x88\xD8\xB9", "\xD8\xA3\xD8\xB3\xD8\xA8\xD9\x88\xD8\xB9\xD9\x8A\xD9\x86", "\xD8\xA3\xD8\xB3\xD8\xA7\xD8\xA8\xD9\x8A\xD8\xB9", "\xD8\xA3\xD8\xB3\xD8\xA8\xD9\x88\xD8\xB9\xD9\x8B\xD8\xA7"},
            {"\xD8\xB4\xD9\x87\xD8\xB1", "\xD8\xB4\xD9\x87\xD8\xB1\xD9\x8A\xD9\x86", "\xD8\xA3\xD8\xB4\xD9\x87\xD8\xB1", "\xD8\xB4\xD9\x87\xD8\xB1\xD9\x8B\xD8\xA7"},
            {"\xD8\xB3\xD9\x86\xD8\xA9", "\xD8\xB3\xD9\x86\xD8\xAA\xD9\x8A\xD9\x86", "\xD8\xB3\xD9\x86\xD9\x88\xD8\xA7\xD8\xAA", "\xD8\xB3\xD9\x86\xD8\xA9"}};
        const std::string since = "\xD9\x85\xD9\x86\xD8\xB0 ";   // منذ
        if (n == 1) return since + w[unit][0];
        if (n == 2) return since + w[unit][1];
        if (n >= 3 && n <= 10) return since + num(n) + " " + w[unit][2];
        return since + num(n) + " " + w[unit][3];
    }
    }
    return num(n);
}

struct Compact {
    const char *thousand, *million, *billion;
    char decimal;
};

Compact compactOf(Lang lang) {
    switch (lang) {
    case Lang::Es: return Compact{" mil", " M", " mil M", ','};
    case Lang::Pt: return Compact{" mil", " mi", " bi", ','};
    case Lang::Fr: return Compact{" k", " M", " Md", ','};
    case Lang::De: return Compact{" Tsd.", " Mio.", " Mrd.", ','};
    case Lang::It: return Compact{"K", " Mln", " Mrd", ','};
    case Lang::Ru: return Compact{" \xD1\x82\xD1\x8B\xD1\x81.", " \xD0\xBC\xD0\xBB\xD0\xBD", " \xD0\xBC\xD0\xBB\xD1\x80\xD0\xB4", ','};
    case Lang::Ar: return Compact{" \xD8\xA3\xD9\x84\xD9\x81", " \xD9\x85\xD9\x84\xD9\x8A\xD9\x88\xD9\x86", " \xD9\x85\xD9\x84\xD9\x8A\xD8\xA7\xD8\xB1", '.'};
    default: return Compact{"K", "M", "B", '.'};
    }
}

// value / unit truncated to one decimal below 10 ("1.2"), to an integer above ("12", "120").
std::string scaled(std::int64_t n, std::int64_t unit, char decimal) {
    const std::int64_t whole = n / unit;
    if (whole >= 10) return num(whole);
    const std::int64_t tenth = (n % unit) * 10 / unit;
    if (tenth == 0) return num(whole);
    return num(whole) + decimal + num(tenth);
}

bool isWordUnit(Lang lang, std::int64_t n) {
    // es / pt / fr put "de" between a word unit (M, mi, Md...) and the noun.
    return (lang == Lang::Es || lang == Lang::Pt || lang == Lang::Fr) && n >= 1000000;
}

}  // namespace

namespace { std::string &displayLanguageRef() { static std::string hl = "en"; return hl; } }
void setDisplayLanguage(const std::string &hl) { displayLanguageRef() = hl.empty() ? std::string("en") : hl; }
const std::string &displayLanguage() { return displayLanguageRef(); }

std::string formatRelativeAge(std::int64_t age, const std::string &hl) {
    const Lang lang = langOf(hl);
    if (age < 60) age = 60;
    const std::int64_t minutes = age / 60, hours = age / 3600, days = age / 86400;
    if (hours < 1) return ageText(lang, Minute, minutes);
    if (days < 1) return ageText(lang, Hour, hours);
    if (days < 7) return ageText(lang, Day, days);
    if (days < 30) return ageText(lang, Week, days / 7);
    if (days < 365) return ageText(lang, Month, days / 30);
    return ageText(lang, Year, days / 365);
}

std::string formatCompactCount(std::int64_t n, const std::string &hl) {
    if (n < 0) return std::string();
    const Lang lang = langOf(hl);
    if (lang == Lang::Ja || lang == Lang::Zh) {
        // 万 = 10^4, 億 / 亿 = 10^8.
        if (n < 10000) return num(n);
        if (n < 100000000LL) return scaled(n, 10000, '.') + "\xE4\xB8\x87";
        return scaled(n, 100000000LL, '.') + (lang == Lang::Ja ? "\xE5\x84\x84" : "\xE4\xBA\xBF");
    }
    const Compact c = compactOf(lang);
    if (n < 1000) return num(n);
    if (n < 1000000) return scaled(n, 1000, c.decimal) + c.thousand;
    if (n < 1000000000LL) return scaled(n, 1000000, c.decimal) + c.million;
    return scaled(n, 1000000000LL, c.decimal) + c.billion;
}

std::string formatViews(std::int64_t n, const std::string &hl) {
    if (n < 0) return std::string();
    const Lang lang = langOf(hl);
    const std::string c = formatCompactCount(n, hl);
    const std::string de = isWordUnit(lang, n) ? " de" : "";
    switch (lang) {
    case Lang::Es: return c + de + (n == 1 ? " visualizaci\xC3\xB3n" : " visualizaciones");
    case Lang::Pt: return c + de + (n == 1 ? " visualiza\xC3\xA7\xC3\xA3o" : " visualiza\xC3\xA7\xC3\xB5" "es");
    case Lang::Fr: return c + de + (n <= 1 ? " vue" : " vues");
    case Lang::De: return c + (n == 1 ? " Aufruf" : " Aufrufe");
    case Lang::It: return c + (n == 1 ? " visualizzazione" : " visualizzazioni");
    case Lang::Ru: {
        static const char *const w[] = {"\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBC\xD0\xBE\xD1\x82\xD1\x80",
                                        "\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBC\xD0\xBE\xD1\x82\xD1\x80\xD0\xB0",
                                        "\xD0\xBF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBC\xD0\xBE\xD1\x82\xD1\x80\xD0\xBE\xD0\xB2"};
        return c + " " + w[n < 1000 ? ruForm(n) : 2];
    }
    case Lang::Ja: return c + " \xE5\x9B\x9E\xE8\xA6\x96\xE8\x81\xB4";   // 回視聴
    case Lang::Zh: return c + "\xE6\xAC\xA1\xE8\xA7\x82\xE7\x9C\x8B";    // 次观看
    case Lang::Ar: return c + " \xD9\x85\xD8\xB4\xD8\xA7\xD9\x87\xD8\xAF\xD8\xA9";   // مشاهدة
    default: return c + (n == 1 ? " view" : " views");
    }
}

std::string formatSubscribers(std::int64_t n, const std::string &hl) {
    if (n < 0) return std::string();
    const Lang lang = langOf(hl);
    const std::string c = formatCompactCount(n, hl);
    const std::string de = isWordUnit(lang, n) ? " de" : "";
    switch (lang) {
    case Lang::Es: return c + de + (n == 1 ? " suscriptor" : " suscriptores");
    case Lang::Pt: return c + de + (n == 1 ? " inscrito" : " inscritos");
    case Lang::Fr: return c + de + (n <= 1 ? " abonn\xC3\xA9" : " abonn\xC3\xA9s");
    case Lang::De: return c + (n == 1 ? " Abonnent" : " Abonnenten");
    case Lang::It: return c + (n == 1 ? " iscritto" : " iscritti");
    case Lang::Ru: {
        static const char *const w[] = {"\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD1\x87\xD0\xB8\xD0\xBA",
                                        "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD1\x87\xD0\xB8\xD0\xBA\xD0\xB0",
                                        "\xD0\xBF\xD0\xBE\xD0\xB4\xD0\xBF\xD0\xB8\xD1\x81\xD1\x87\xD0\xB8\xD0\xBA\xD0\xBE\xD0\xB2"};
        return c + " " + w[n < 1000 ? ruForm(n) : 2];
    }
    case Lang::Ja: return "\xE3\x83\x81\xE3\x83\xA3\xE3\x83\xB3\xE3\x83\x8D\xE3\x83\xAB\xE7\x99\xBB\xE9\x8C\xB2\xE8\x80\x85\xE6\x95\xB0 " + c + "\xE4\xBA\xBA";
    case Lang::Zh: return c + "\xE4\xBD\x8D\xE8\xAE\xA2\xE9\x98\x85\xE8\x80\x85";   // 位订阅者
    case Lang::Ar: return c + " \xD9\x85\xD8\xB4\xD8\xAA\xD8\xB1\xD9\x83";            // مشترك
    default: return c + (n == 1 ? " subscriber" : " subscribers");
    }
}

std::string formatVideoCount(std::int64_t n, const std::string &hl) {
    if (n < 0) return std::string();
    const Lang lang = langOf(hl);
    const std::string c = formatCompactCount(n, hl);
    switch (lang) {
    case Lang::Es: return c + (n == 1 ? " video" : " videos");
    case Lang::Pt: return c + (n == 1 ? " v\xC3\xAD" "deo" : " v\xC3\xAD" "deos");
    case Lang::Fr: return c + (n <= 1 ? " vid\xC3\xA9o" : " vid\xC3\xA9os");
    case Lang::De: return c + (n == 1 ? " Video" : " Videos");
    case Lang::It: return c + " video";
    case Lang::Ru: return c + " \xD0\xB2\xD0\xB8\xD0\xB4\xD0\xB5\xD0\xBE";            // видео
    case Lang::Ja: return c + " \xE6\x9C\xAC\xE3\x81\xAE\xE5\x8B\x95\xE7\x94\xBB";      // 本の動画
    case Lang::Zh: return c + "\xE4\xB8\xAA\xE8\xA7\x86\xE9\xA2\x91";                  // 个视频
    case Lang::Ar: return c + " \xD9\x81\xD9\x8A\xD8\xAF\xD9\x8A\xD9\x88";            // فيديو
    default: return c + (n == 1 ? " video" : " videos");
    }
}

}}}
