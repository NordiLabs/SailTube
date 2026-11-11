// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QtGlobal>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace transport { namespace domain {

// One cookie from a Set-Cookie response header, kept only to be handed to the
// session layer for rotation (DD-2). The value is a secret: it is never logged,
// never put in an error string and never persisted by the transport.
struct RotatedCookie {
    QByteArray name;
    QByteArray value;
    QString domain;           // empty = host-only
    QString path;
    qint64 expiresAtSeconds = 0;   // 0 = session cookie
    bool secure = false;
    bool httpOnly = false;
    bool deleted = false;     // empty value or already expired: the server clears it
};

// Bounds: at most 16 cookies, name <= 128, value <= 4096 bytes. Cookies whose
// Domain attribute is outside youtube.com / google.com are dropped, so a hostile or
// misconfigured answer cannot plant cookies for another site. Pure, no I/O.
std::vector<RotatedCookie> parseSetCookies(const QList<QByteArray> &headerValues, qint64 nowSeconds);

}}}}}
