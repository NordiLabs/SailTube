// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SetCookie.h"
#include <QDateTime>
#include <QNetworkCookie>

namespace nordtile { namespace music { namespace ytm { namespace transport { namespace domain {

namespace {
const int kMaxCookies = 16;
const int kMaxHeaderBytes = 8192;
const int kMaxNameBytes = 128;
const int kMaxValueBytes = 4096;

bool tokenName(const QByteArray &name) {
    if (name.isEmpty() || name.size() > kMaxNameBytes) return false;
    for (char c : name) {
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        c == '_' || c == '-' || c == '.' || c == '!' || c == '#' || c == '$' || c == '%' ||
                        c == '&' || c == '\'' || c == '*' || c == '+' || c == '^' || c == '`' || c == '|' || c == '~';
        if (!ok) return false;
    }
    return true;
}

bool allowedDomain(const QString &domain) {
    if (domain.isEmpty()) return true;
    QString d = domain.toLower();
    if (d.startsWith(QLatin1Char('.'))) d = d.mid(1);
    return d == QLatin1String("youtube.com") || d.endsWith(QLatin1String(".youtube.com")) ||
           d == QLatin1String("google.com") || d.endsWith(QLatin1String(".google.com"));
}
}

std::vector<RotatedCookie> parseSetCookies(const QList<QByteArray> &headerValues, qint64 nowSeconds) {
    std::vector<RotatedCookie> out;
    for (const QByteArray &header : headerValues) {
        if (header.isEmpty() || header.size() > kMaxHeaderBytes) continue;
        const QList<QNetworkCookie> cookies = QNetworkCookie::parseCookies(header);
        for (const QNetworkCookie &cookie : cookies) {
            if (int(out.size()) >= kMaxCookies) return out;
            const QByteArray name = cookie.name();
            const QByteArray value = cookie.value();
            if (!tokenName(name) || value.size() > kMaxValueBytes) continue;
            if (!allowedDomain(cookie.domain())) continue;
            RotatedCookie c;
            c.name = name;
            c.value = value;
            c.domain = cookie.domain();
            c.path = cookie.path();
            c.secure = cookie.isSecure();
            c.httpOnly = cookie.isHttpOnly();
            if (!cookie.isSessionCookie() && cookie.expirationDate().isValid()) {
                c.expiresAtSeconds = cookie.expirationDate().toMSecsSinceEpoch() / 1000;
                if (c.expiresAtSeconds <= nowSeconds) c.deleted = true;
            }
            if (value.isEmpty()) c.deleted = true;
            out.push_back(c);
        }
    }
    return out;
}

}}}}}
