// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DIA-01: failures counted per area in separate classes. Qt-free.
//
// Only shape drift (an expected container or renderer is missing from an otherwise good answer)
// feeds the "parser update needed" hint. 429, 5xx, network errors, final verdicts and valid empty
// answers are never drift: they say nothing about the parser.

#include <array>
#include <cstdint>
#include <string>

namespace nordtile { namespace diagnostics {

enum class Area { Player, Search, WatchNext, Channel, Feed, Comments, Playlists, Account, Count };
enum class FailureClass { None, ShapeDrift, Http4xx, Http429, Http5xx, Network, Unplayable, EmptyValid, Count };

// What a request produced, as the caller saw it. Exactly what is known; nothing is guessed.
struct Outcome {
    int httpStatus = 0;          // 0: no HTTP answer (see networkError)
    bool networkError = false;   // DNS, TLS, reset, timeout before an answer
    enum class Parse { Ok, ShapeMissing, Empty } parse = Parse::Ok;  // only meaningful for a 2xx answer
    bool unplayable = false;     // a final verdict (removed, private, age gate, region...)
};

// Classification order: network, HTTP status class, final verdict, then the parse result of a 2xx.
inline FailureClass classify(const Outcome &o) {
    if (o.networkError) return FailureClass::Network;
    if (o.httpStatus == 429) return FailureClass::Http429;
    if (o.httpStatus >= 500 && o.httpStatus <= 599) return FailureClass::Http5xx;
    if (o.httpStatus >= 400 && o.httpStatus <= 499) return FailureClass::Http4xx;
    if (o.unplayable) return FailureClass::Unplayable;
    if (o.httpStatus != 0 && (o.httpStatus < 200 || o.httpStatus > 299)) return FailureClass::Http4xx;
    switch (o.parse) {
    case Outcome::Parse::ShapeMissing: return FailureClass::ShapeDrift;
    case Outcome::Parse::Empty: return FailureClass::EmptyValid;
    case Outcome::Parse::Ok: break;
    }
    return FailureClass::None;
}

inline const char *areaLabel(Area a) {
    static const char *const labels[] = {"player", "search", "watch-next", "channel", "feed", "comments",
                                         "playlists", "account"};
    return a < Area::Count ? labels[static_cast<int>(a)] : "unknown";
}

inline const char *classLabel(FailureClass c) {
    static const char *const labels[] = {"none", "shape-drift", "http-4xx", "http-429", "http-5xx", "network",
                                         "unplayable", "empty-valid"};
    return c < FailureClass::Count ? labels[static_cast<int>(c)] : "unknown";
}

class ErrorCounters {
public:
    // Counts the outcome; returns its class (None is not counted).
    FailureClass record(Area area, const Outcome &outcome) {
        const FailureClass c = classify(outcome);
        if (c != FailureClass::None && area < Area::Count) ++counts_[index(area)][static_cast<int>(c)];
        return c;
    }
    std::uint32_t count(Area area, FailureClass c) const {
        return area < Area::Count && c < FailureClass::Count ? counts_[index(area)][static_cast<int>(c)] : 0;
    }
    std::uint32_t total(FailureClass c) const {
        std::uint32_t sum = 0;
        for (int a = 0; a < static_cast<int>(Area::Count); ++a) sum += count(static_cast<Area>(a), c);
        return sum;
    }
    // The "parser update needed" hint: shape drift only, at least `threshold` times in one area.
    bool parserUpdateHint(Area area, std::uint32_t threshold = 3) const {
        return count(area, FailureClass::ShapeDrift) >= threshold;
    }
    bool anyParserUpdateHint(std::uint32_t threshold = 3) const {
        for (int a = 0; a < static_cast<int>(Area::Count); ++a)
            if (parserUpdateHint(static_cast<Area>(a), threshold)) return true;
        return false;
    }
    void reset() { counts_ = {}; }

private:
    static int index(Area a) { return static_cast<int>(a); }
    std::array<std::array<std::uint32_t, static_cast<int>(FailureClass::Count)>, static_cast<int>(Area::Count)> counts_{};
};

}}
