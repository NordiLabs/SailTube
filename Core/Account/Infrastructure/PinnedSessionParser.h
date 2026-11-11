// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-04: SailTune's session-import parser, used as is (pinned under
// Shared/SailTune/, never edited here), behind the SessionTextParser port.
// It accepts a Cookie header, copied request headers, "Copy as cURL" and a
// Netscape cookies.txt, keeps only allow-listed cookie names, discards a
// pasted Authorization header and never echoes a value. The result is the
// cookie set of the YouTube session (domain .youtube.com) for AccountStore,
// which applies the ACC-03/04 signing gate again before anything is stored.

#include "Core/Account/Infrastructure/SessionTextImport.h"
#include <functional>

namespace nordtile { namespace video { namespace account {

class PinnedSessionParser final : public SessionTextParser {
public:
    typedef std::function<qint64()> ClockSec;   // UTC seconds, for cookies.txt expiry
    explicit PinnedSessionParser(ClockSec clock = ClockSec());
    bool parse(const QString &text, SessionCookies *out, QString *reason) override;

private:
    ClockSec clock_;
};

}}}
