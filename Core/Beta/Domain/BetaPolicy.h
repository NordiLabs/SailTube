// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Time-limited public beta (docs/BETA.md). Qt-free decisions; the clock and storage live in
// Core/Beta/Infrastructure/BetaClock. Compiled only with CONFIG+=sailtube_beta.
//
// - The beta ends at the end of the expiry day in local time (the caller converts the date).
// - effectiveNow = max(system time, persisted high-water mark, build time): turning the clock back does
//   not revive an expired beta. Honest effort, not DRM: rebuilding from source bypasses it.
// - From 14 days before the end, a dismissible banner at most once per local day; in the last 3 days on
//   every launch. After the end only the "beta has ended" page runs.

#include <cstdint>
#include <cstdlib>
#include <string>

namespace nordtile { namespace video { namespace beta {

const char kLatestAllowedExpiry[] = "2026-12-30";
const std::int64_t kDayMs = 24LL * 3600 * 1000;

struct Date { int year = 0, month = 0, day = 0; };

// Strict YYYY-MM-DD with a real calendar day.
inline bool parseIsoDate(const std::string &s, Date *out) {
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    for (std::size_t i : {0u, 1u, 2u, 3u, 5u, 6u, 8u, 9u}) if (s[i] < '0' || s[i] > '9') return false;
    Date d;
    d.year = std::atoi(s.substr(0, 4).c_str());
    d.month = std::atoi(s.substr(5, 2).c_str());
    d.day = std::atoi(s.substr(8, 2).c_str());
    if (d.year < 2000 || d.month < 1 || d.month > 12 || d.day < 1) return false;
    static const int lengths[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (d.year % 4 == 0 && d.year % 100 != 0) || d.year % 400 == 0;
    const int max = lengths[d.month - 1] + (d.month == 2 && leap ? 1 : 0);
    if (d.day > max) return false;
    if (out) *out = d;
    return true;
}

inline bool notLaterThanLimit(const Date &d) {
    Date limit;
    parseIsoDate(kLatestAllowedExpiry, &limit);
    if (d.year != limit.year) return d.year < limit.year;
    if (d.month != limit.month) return d.month < limit.month;
    return d.day <= limit.day;
}

inline std::int64_t effectiveNow(std::int64_t systemMs, std::int64_t highWaterMs, std::int64_t buildMs) {
    std::int64_t n = systemMs;
    if (highWaterMs > n) n = highWaterMs;
    if (buildMs > n) n = buildMs;
    return n;
}

enum class Phase { Active, Warning, FinalDays, Expired };

struct Evaluation {
    Phase phase = Phase::Active;
    int daysLeft = 0;          // whole days until the end, rounded up; 0 when expired
    bool showBanner = false;
};

// expiryEndMs: the last millisecond of the beta (end of the expiry day, local time).
// todayIndex / lastBannerDay: local day numbers (e.g. days since a fixed date), -1 = never dismissed.
inline Evaluation evaluate(std::int64_t nowMs, std::int64_t expiryEndMs, std::int64_t todayIndex, std::int64_t lastBannerDay) {
    Evaluation e;
    if (nowMs > expiryEndMs) { e.phase = Phase::Expired; return e; }
    const std::int64_t left = expiryEndMs - nowMs;
    e.daysLeft = static_cast<int>((left + kDayMs - 1) / kDayMs);
    if (left < 3 * kDayMs) {
        e.phase = Phase::FinalDays;
        e.showBanner = true;                          // every launch
    } else if (left < 14 * kDayMs) {
        e.phase = Phase::Warning;
        e.showBanner = lastBannerDay != todayIndex;   // at most once a day
    }
    return e;
}

}}}
