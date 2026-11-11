// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Display text the catalog generates itself where YouTube gave no text of its own (an RSS row has
// a timestamp, not "hace 3 días"; a count parsed from a structured field has no unit words).
// Built-in tables for the ten shipped languages (en, es, pt, fr, de, it, ru, ja, zh, ar) with
// their plural rules, keyed by the discovery language (`hl`, the device language), so the text
// matches what YouTube shows in the same lists. Unknown languages fall back to English.
// YouTube's own localized text, when a row carries it, is preferred by the callers. Qt-free.

#include <cstdint>
#include <string>

namespace nordtile { namespace video { namespace catalog {

// "3 days ago" / "hace 3 días" / "vor 3 Tagen" / "3 日前" / "منذ 3 أيام"; months are 30 days and
// years 365. Under a minute counts as one minute. Negative ages (clock skew) count as now.
std::string formatRelativeAge(std::int64_t ageSeconds, const std::string &hl);

// Compact count as YouTube writes it: "1.2M", "1,2 M", "1,2 Mio.", "120万", "1,2 млн".
std::string formatCompactCount(std::int64_t count, const std::string &hl);

// "1.2M views", "1,2 M de visualizaciones", "1,2 Mio. Aufrufe", "120万 回視聴".
std::string formatViews(std::int64_t count, const std::string &hl);
// "1.2M subscribers", "1,2 M de suscriptores", "チャンネル登録者数 120万人".
std::string formatSubscribers(std::int64_t count, const std::string &hl);
// "6.2K videos", "6,2 k vidéos", "6201 本の動画".
std::string formatVideoCount(std::int64_t count, const std::string &hl);

// The process-wide display language of generated list text (set once by the composition from
// the discovery locale; "en" until then). Main thread.
void setDisplayLanguage(const std::string &hl);
const std::string &displayLanguage();

}}}
