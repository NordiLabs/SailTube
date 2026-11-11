// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "../Domain/StreamTypes.h"
#include <QByteArray>
#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {

struct ParsedPlayer {
    domain::FailureKind failure = domain::FailureKind::None;
    std::string reason;          // fixed class text, never provider text
    domain::StreamSet set;
    int keptFormats = 0;         // direct addresses that survived
    int droppedFormats = 0;      // ciphered, DRM, segmented or foreign-host formats
};

// Reads the body of a /player answer into a ranked StreamSet.
//
// Kept: formats with a plain `url` on a *.googlevideo.com host over https. Dropped: anything that
// carries a signature cipher, DRM families, OTF or segment-style streams, other hosts. The answer
// must say playabilityStatus OK and name the requested video; a live broadcast is reported as
// Unplayable (the fast path serves recorded media only).
ParsedPlayer parsePlayerResponse(const QByteArray &body, const std::string &videoId,
                                 const domain::StreamRequest &request, std::int64_t nowUnixMs);

}}}}
