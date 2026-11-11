// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-04: paste and file import (Cookie header, copied request headers, "Copy
// as cURL", Netscape cookies.txt). The parsing is SailTune's session-import
// parser, used as is (pinned under Shared/SailTune/ by core); this port is the
// seam the account page calls, so the rest of the account code does not depend on the
// parser's own types. The ACC-03/04 gate (__Secure-3PAPISID or SAPISID) is
// applied by AccountStore::importSession, so a refused input stores nothing.

#include "Core/Account/Domain/AccountTypes.h"
#include <QString>

namespace nordtile { namespace video { namespace account {

class SessionTextParser {
public:
    virtual ~SessionTextParser() {}
    // false with a short, translatable reason when the text is no session at all.
    virtual bool parse(const QString &text, SessionCookies *out, QString *reason) = 0;
};

}}}
