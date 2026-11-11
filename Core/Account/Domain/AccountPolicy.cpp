// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountPolicy.h"

#include <algorithm>

namespace nordtile { namespace video { namespace account {

namespace {

std::string asciiLower(const std::string &s) {
    std::string out(s);
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = char(out[i] - 'A' + 'a');
    }
    return out;
}

// "youtube.com" or any "<label>.youtube.com"; a leading dot is part of the
// suffix match, so "notyoutube.com" is refused.
bool isDomainOrSubdomain(const std::string &host, const std::string &domain) {
    if (host == domain) return true;
    const std::string dotted = "." + domain;
    return host.size() > dotted.size() && host.compare(host.size() - dotted.size(), dotted.size(), dotted) == 0;
}

bool sameCookie(const Cookie &a, const SetCookieValue &b) {
    return a.value == b.value && (b.domain.empty() || a.domain == b.domain)
        && (b.expiresSec == 0 || a.expiresSec == b.expiresSec);
}

} // namespace

const Cookie *SessionCookies::find(const std::string &name) const {
    for (std::size_t i = 0; i < cookies.size(); ++i) {
        if (cookies[i].name == name) return &cookies[i];
    }
    return nullptr;
}

std::vector<SignInMethod> signInMethods(BuildVariant variant, bool webViewBuiltIn) {
    std::vector<SignInMethod> out;
    if (variant == BuildVariant::Full) out.push_back(SignInMethod::BrowserImport);
    out.push_back(SignInMethod::PasteOrFile);
    if (webViewBuiltIn) out.push_back(SignInMethod::WebView);
    return out;
}

std::vector<std::string> browserCookieDatabases() {
    std::vector<std::string> out;
    out.push_back(".local/share/org.sailfishos/browser/.mozilla/cookies.sqlite");
    out.push_back(".mozilla/mozembed/cookies.sqlite");
    out.push_back(".local/share/org.sailfishos/sailfish-browser/.mozilla/cookies.sqlite");
    return out;
}

bool importableCookieHost(const std::string &host) {
    std::string h = asciiLower(host);
    if (!h.empty() && h[0] == '.') h.erase(0, 1);
    return isDomainOrSubdomain(h, "youtube.com") || isDomainOrSubdomain(h, "google.com");
}

SessionCookies mergeImported(const std::vector<Cookie> &rows) {
    SessionCookies out;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const Cookie &row = rows[i];
        Cookie *existing = nullptr;
        for (std::size_t j = 0; j < out.cookies.size(); ++j) {
            if (out.cookies[j].name == row.name) { existing = &out.cookies[j]; break; }
        }
        if (!existing) {
            out.cookies.push_back(row);
        } else if (asciiLower(row.domain) == ".youtube.com" && asciiLower(existing->domain) != ".youtube.com") {
            *existing = row; // keeps the slot of the first appearance
        }
    }
    return out;
}

bool hasSigningSecret(const SessionCookies &cookies) {
    return !signingSecret(cookies).empty();
}

const std::vector<std::string> &trackedCookieNames() {
    static const char *const names[] = {
        "__Secure-3PAPISID", "SAPISID", "__Secure-1PAPISID",
        "__Secure-3PSID", "__Secure-1PSID",
        "SID", "HSID", "SSID", "APISID",
        "SIDCC", "__Secure-3PSIDCC", "__Secure-1PSIDCC",
        "LOGIN_INFO", "PREF", "VISITOR_INFO1_LIVE",
        "__Secure-3PSIDTS", "__Secure-1PSIDTS",
    };
    static const std::vector<std::string> list(names, names + sizeof(names) / sizeof(names[0]));
    return list;
}

bool isTrackedCookie(const std::string &name) {
    const std::vector<std::string> &names = trackedCookieNames();
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::string cookieHeader(const SessionCookies &cookies) {
    std::string out;
    const std::vector<std::string> &names = trackedCookieNames();
    for (std::size_t i = 0; i < names.size(); ++i) {
        const Cookie *c = cookies.find(names[i]);
        if (!c) continue;
        if (!out.empty()) out += "; ";
        out += c->name + "=" + c->value;
    }
    return out;
}

std::string signingSecret(const SessionCookies &cookies) {
    const Cookie *c = cookies.find("__Secure-3PAPISID");
    if (c && !c->value.empty()) return c->value;
    c = cookies.find("SAPISID");
    if (c && !c->value.empty()) return c->value;
    return std::string();
}

std::string sapisidHash(std::int64_t timestampSec, const std::string &secret, const std::string &origin,
                        const std::function<std::string(const std::string &)> &sha1Hex) {
    const std::string ts = std::to_string(static_cast<long long>(timestampSec));
    return "SAPISIDHASH " + ts + "_" + sha1Hex(ts + " " + secret + " " + origin);
}

RequestHeaders authenticatedHeaders(const SessionCookies &cookies, const AccountSelection &selection,
                                    std::int64_t timestampSec,
                                    const std::function<std::string(const std::string &)> &sha1Hex) {
    RequestHeaders out;
    const std::string secret = signingSecret(cookies);
    if (secret.empty()) return out;
    const std::string origin(limits::origin);
    out.headers.push_back(std::make_pair(std::string("Cookie"), cookieHeader(cookies)));
    out.headers.push_back(std::make_pair(std::string("Authorization"), sapisidHash(timestampSec, secret, origin, sha1Hex)));
    out.headers.push_back(std::make_pair(std::string("Origin"), origin));
    out.headers.push_back(std::make_pair(std::string("Referer"), origin + "/"));
    out.headers.push_back(std::make_pair(std::string("X-Goog-AuthUser"), std::to_string(selection.authUser)));
    if (!selection.pageId.empty())
        out.headers.push_back(std::make_pair(std::string("X-Goog-PageId"), selection.pageId));
    return out;
}

bool foldRotation(SessionCookies *session, const std::vector<SetCookieValue> &rotated) {
    if (!session) return false;
    bool changed = false;
    for (std::size_t i = 0; i < rotated.size(); ++i) {
        const SetCookieValue &r = rotated[i];
        if (r.name.empty()) continue;
        std::vector<Cookie>::iterator it = session->cookies.begin();
        for (; it != session->cookies.end(); ++it) {
            if (it->name == r.name) break;
        }
        const bool removal = r.expired || r.value.empty();
        if (it == session->cookies.end()) {
            if (removal || !isTrackedCookie(r.name)) continue;
            Cookie c;
            c.name = r.name;
            c.value = r.value;
            c.domain = r.domain.empty() ? std::string(".youtube.com") : r.domain;
            c.expiresSec = r.expiresSec;
            session->cookies.push_back(c);
            changed = true;
        } else if (removal) {
            session->cookies.erase(it);
            changed = true;
        } else if (!sameCookie(*it, r)) {
            it->value = r.value;
            if (!r.domain.empty()) it->domain = r.domain;
            if (r.expiresSec != 0) it->expiresSec = r.expiresSec;
            changed = true;
        }
    }
    return changed;
}

std::int64_t nextVerificationMs(SessionState state, std::int64_t lastCheckMs) {
    if (state != SessionState::SignedIn && state != SessionState::Checking) return -1;
    if (lastCheckMs < 0) return 0;
    return lastCheckMs + limits::verifyIntervalMs;
}

SessionState stateAfter(SessionState current, Verification outcome) {
    switch (outcome) {
    case Verification::Accepted: return SessionState::SignedIn;
    case Verification::Rejected: return SessionState::Expired;
    case Verification::Inconclusive: break;
    }
    return current == SessionState::Checking ? SessionState::Unreachable : current;
}

std::string accountKey(const AccountEntry &e) {
    const std::string &id = !e.gaiaId.empty() ? e.gaiaId : (!e.datasyncId.empty() ? e.datasyncId : e.name);
    return id + "|" + e.pageId;
}

bool probeShouldStop(bool loggedOut, const std::vector<AccountEntry> &entries, const std::vector<std::string> &known) {
    if (loggedOut || entries.empty()) return true;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (std::find(known.begin(), known.end(), accountKey(entries[i])) == known.end()) return false;
    }
    return true;
}

}}}
