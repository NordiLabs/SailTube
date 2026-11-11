// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// Reading the provider's verdict on a video, and the extractor's error text. Fixed matching
// rules, case-insensitive. Provider text is only classified, never forwarded.
#include "StreamTypes.h"
#include <cstddef>
#include <string>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

namespace detail {
inline std::string lowerAscii(const std::string &text) {
    std::string out = text;
    for (std::size_t i = 0; i < out.size(); ++i) if (out[i] >= 'A' && out[i] <= 'Z') out[i] = char(out[i] - 'A' + 'a');
    return out;
}
inline bool contains(const std::string &lowered, const char *needle) { return lowered.find(needle) != std::string::npos; }
// A word, not a substring of another ("robot", "abbot"): bounded by non-letters.
inline bool containsWord(const std::string &lowered, const char *word) {
    const std::string w(word);
    for (std::size_t at = lowered.find(w); at != std::string::npos; at = lowered.find(w, at + 1)) {
        const bool leftOk = at == 0 || !(lowered[at - 1] >= 'a' && lowered[at - 1] <= 'z');
        const std::size_t end = at + w.size();
        const bool rightOk = end >= lowered.size() || !(lowered[end] >= 'a' && lowered[end] <= 'z');
        if (leftOk && rightOk) return true;
    }
    return false;
}
}  // namespace detail

// A reason that no retry, widening or second extractor can change: the item is gone or not yet
// there (removed, terminated, private, blocked in the country, a premiere). Never final: "try again later"
// (rate limiting can be worded as "unavailable") and a bare "video unavailable", which the one client
// this path speaks may be told where another client is not - those go on to the extractor.
inline bool isFinalReason(const std::string &reason) {
    const std::string r = detail::lowerAscii(reason);
    if (detail::contains(r, "try again later")) return false;
    static const char *const finals[] = {
        "video has been removed", "has been terminated", "live event will begin", "premieres in",
        "not made this video available in your country", "incomplete youtube id", "private video",
        "this video is no longer available"};
    for (std::size_t i = 0; i < sizeof(finals) / sizeof(finals[0]); ++i)
        if (detail::contains(r, finals[i])) return true;
    return false;
}

inline bool mentionsBotCheck(const std::string &reason) {
    const std::string r = detail::lowerAscii(reason);
    // "Sign in to confirm you're not a bot"; an age gate ("Sign in to confirm your age") is a different case.
    return detail::containsWord(r, "bot") || detail::contains(r, "not a robot") || detail::contains(r, "unusual traffic");
}

// playabilityStatus.status plus its reason -> what the host should do.
inline FailureKind classifyPlayability(const std::string &status, const std::string &reason) {
    if (status == "OK") return FailureKind::None;
    if (mentionsBotCheck(reason)) return FailureKind::BotWall;
    if (isFinalReason(reason)) return FailureKind::Final;
    return FailureKind::Unplayable;     // LOGIN_REQUIRED (age, members), UNPLAYABLE, ERROR ...
}

}}}}
