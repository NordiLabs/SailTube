// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Locale-aware parsing of the display text InnerTube returns where no structured field exists
// (I18N-02). Parsers call these only after the structured fields (viewCount, lengthSeconds,
// publishDate, badge styles) are absent. Every function returns -1 / Unknown on failure; the
// caller then shows the raw text and never drops the row. Nothing here depends on the process
// locale: tables are keyed by the request's `hl` (the language YouTube answered in).
//
// Supported table languages: the shipped catalogs en, es, pt, fr, de, it, ru, ja, zh (zh_CN),
// ar. Unknown `hl` values fall back to the English table plus language-neutral digit rules.

#include <cstdint>
#include <string>

namespace nordtile { namespace video { namespace catalog {

// "1:05:56" / "12:30" / "0:59" -> seconds. Also accepts the spelled accessibility form in
// English ("14 minutes, 54 seconds"). -1 when not a duration.
std::int64_t parseDurationText(const std::string &text);

// Counts with optional magnitude suffix and unit word: "7,898,556 views", "7.8M views", "3.6M",
// "No views" (0), "1 view", "11 Mio.", "1,2 Mio. Aufrufe", "363万", "1162万 回視聴", "84 watching".
// The decimal and grouping separators follow `hl`. -1 when no number is found.
std::int64_t parseCountText(const std::string &text, const std::string &hl);

// Relative ages: "3 days ago", "1y ago", "3d ago", "1mo ago", "Streamed 8 hours ago",
// "Premiered 2 weeks ago", "vor 2 Jahren", "1 年前", "hace 3 días". Month = 30 days, year = 365.
// -1 when the text is not a relative age.
std::int64_t parseAgeSeconds(const std::string &text, const std::string &hl);

// Absolute dates shown on the watch page or playlists: "Oct 24, 2009", "24.10.2009",
// "24 oct 2009", "2009/10/24", "2009年10月24日", ISO "2009-10-24". Returns UTC epoch seconds of
// that day's midnight, or -1.
std::int64_t parseAbsoluteDate(const std::string &text, const std::string &hl);

// What a lockup/metadata text part is, used where rows carry no field names (lockupViewModel).
// Count classes (views, watching, subscribers, video count) need a number in the text, so an
// owner label such as "Tech Review" stays Unknown; an age marker without a readable unit
// ("il y a longtemps") is still Age, so the row keeps the raw text.
enum class MetaPart { Unknown, Views, Watching, Age, Scheduled, VideoCount, Subscribers };
MetaPart classifyMetaPart(const std::string &text, const std::string &hl);

// "1.2M subscribers" / "1,2 Mio. Abonnenten" / "チャンネル登録者数 120万人" -> count.
std::int64_t parseSubscriberText(const std::string &text, const std::string &hl);

// Base language of an hl/BCP-47 code: "pt-BR" -> "pt", "zh-Hans" -> "zh", "en" -> "en".
std::string baseLanguage(const std::string &code);

}}}
