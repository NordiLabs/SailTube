// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PinnedSessionParser.h"

#include "Shared/SailTune/Core/Accounts/Infrastructure/SessionImportParser.h"
#include <QDateTime>

namespace nordtile { namespace video { namespace account {

namespace st = nordtile::music::accounts;

namespace {

// Fixed texts only: nothing from the input ever reaches the UI or a log.
QString reasonFor(st::ports::ImportError e) {
    switch (e) {
    case st::ports::ImportError::NoSigningCookie:
    case st::ports::ImportError::NoSessionCookie:
        return QStringLiteral("no-sign-in");
    case st::ports::ImportError::CookiesExpired:
        return QStringLiteral("expired");
    case st::ports::ImportError::TooLarge:
    case st::ports::ImportError::LineTooLong:
    case st::ports::ImportError::TooManyLines:
    case st::ports::ImportError::TooManyCookies:
        return QStringLiteral("too-large");
    default:
        return QStringLiteral("unrecognized");
    }
}

} // namespace

PinnedSessionParser::PinnedSessionParser(ClockSec clock) : clock_(clock) {}

bool PinnedSessionParser::parse(const QString &text, SessionCookies *out, QString *reason) {
    st::infrastructure::ImportOptions options;
    options.nowUtcSeconds = clock_ ? clock_() : QDateTime::currentMSecsSinceEpoch() / 1000;
    std::string input = text.toStdString();
    st::infrastructure::ImportResult r = st::infrastructure::SessionImportParser::parse(input, options);
    st::domain::wipeString(input);
    if (!r.ok()) {
        if (reason) *reason = reasonFor(r.error);
        r.wipe();
        return false;
    }
    for (const st::domain::SessionCookie &c : r.bundle.cookies) {
        Cookie cookie;
        cookie.name = c.name;
        cookie.value = c.value;
        cookie.domain = ".youtube.com";
        out->cookies.push_back(cookie);
    }
    r.wipe();
    return !out->empty();
}

}}}
