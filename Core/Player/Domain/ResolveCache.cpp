// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ResolveCache.h"

#include <algorithm>

namespace nordtile { namespace player {

std::int64_t cacheableUntilMs(const CacheFacts &s, std::int64_t nowMs, const CachePolicy &p) {
    if (s.live) return 0;
    std::int64_t until = nowMs + p.maxLifetimeMs;
    if (s.expiresAtUnixMs > 0)
        until = std::min(until, s.expiresAtUnixMs - p.expiryMarginMs);
    else
        until = std::min(until, nowMs + p.unknownExpiryLifetimeMs);
    if (s.hls) until = std::min(until, nowMs + p.hlsLifetimeMs);
    return until - nowMs > p.minValidityMs ? until : 0;
}

}}
