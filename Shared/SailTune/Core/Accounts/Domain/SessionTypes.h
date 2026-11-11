// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Value types of the YouTube Music cookie-session provider ("ytm-session").
// Pure C++11 (no Qt). Everything here is either non-secret or explicitly wipeable.
// Evidence tags follow docs/ytm_hybrid/06-design.md: VERIFIED / INFERENCE.
#pragma once

#include "Account.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace accounts { namespace domain {

inline const char *ytmSessionProvider() { return "ytm-session"; }

// Best-effort zeroing (std::string may have made transient copies elsewhere; that is the
// same caveat as ports::SecretBytes). Never optimised away: writes go through volatile.
inline void wipeString(std::string &value) {
    if (!value.empty()) {
        volatile char *data = &value[0];
        for (std::size_t i = 0; i < value.size(); ++i) data[i] = 0;
    }
    value.clear();
}

// Outcome classes of one authenticated HTTP exchange (design 3.6). Pure taxonomy, shared by
// the expiry detector (this slice) and the transport/parsers slices.
//
// Forbidden (appended; ytm-write finding, docs/ytm_hybrid/impl/session.md section 13): an explicit
// permission error for ONE operation (a JSON error document of an HTTP 403 that names a permission,
// "forbidden" or an insufficient scope). It says nothing about the health of the session or of the
// service, so it must never be treated like Blocked (bot check / consent wall: stop everything for a
// cool-down). Bot-check, captcha, consent and "sign in to confirm" signals stay Blocked; an
// UNEXPLAINED 403 (HTML, empty, JSON without a permission marker) stays Blocked as well (conservative).
enum class SessionVerdict { Ok, SessionExpired, Blocked, ProtocolChanged, RateLimited, Transient, Forbidden };

inline const char *verdictName(SessionVerdict verdict) {
    switch (verdict) {
    case SessionVerdict::Ok: return "Ok";
    case SessionVerdict::SessionExpired: return "SessionExpired";
    case SessionVerdict::Blocked: return "Blocked";
    case SessionVerdict::ProtocolChanged: return "ProtocolChanged";
    case SessionVerdict::RateLimited: return "RateLimited";
    case SessionVerdict::Transient: return "Transient";
    case SessionVerdict::Forbidden: return "Forbidden";
    }
    return "Unknown";
}
// Account-level error for a non-Ok verdict. Transient maps to Network so the UI can show
// "YouTube can't be reached" (code Network) apart from "Sign in again" (AuthorizationRequired).
inline ErrorCode errorCodeFor(SessionVerdict verdict) {
    switch (verdict) {
    case SessionVerdict::Ok: return ErrorCode::None;
    case SessionVerdict::SessionExpired: return ErrorCode::AuthorizationRequired;
    case SessionVerdict::Blocked: return ErrorCode::Blocked;
    case SessionVerdict::ProtocolChanged: return ErrorCode::ProtocolChanged;
    case SessionVerdict::RateLimited: return ErrorCode::RateLimited;
    case SessionVerdict::Transient: return ErrorCode::Network;
    case SessionVerdict::Forbidden: return ErrorCode::PermissionDenied;
    }
    return ErrorCode::InvalidResponse;
}
// Fixed, safe application messages (never provider text).
inline const char *verdictMessage(SessionVerdict verdict) {
    switch (verdict) {
    case SessionVerdict::Ok: return "";
    case SessionVerdict::SessionExpired: return "YouTube no longer accepts this sign-in.";
    case SessionVerdict::Blocked: return "YouTube blocked the request. The connection is paused.";
    case SessionVerdict::ProtocolChanged: return "YouTube changed how it works. Sync is paused.";
    case SessionVerdict::RateLimited: return "YouTube is limiting requests. Try again later.";
    case SessionVerdict::Transient: return "YouTube could not be reached.";
    case SessionVerdict::Forbidden: return "YouTube did not allow that request.";
    }
    return "";
}

enum class SessionSourceKind { Unknown, Paste, EmbeddedWebView, SystemBrowser };

inline const char *sourceKindName(SessionSourceKind kind) {
    switch (kind) {
    case SessionSourceKind::Paste: return "paste";
    case SessionSourceKind::EmbeddedWebView: return "webview";
    case SessionSourceKind::SystemBrowser: return "browser";
    case SessionSourceKind::Unknown: break;
    }
    return "unknown";
}
inline SessionSourceKind parseSourceKind(const std::string &name) {
    if (name == "paste") return SessionSourceKind::Paste;
    if (name == "webview") return SessionSourceKind::EmbeddedWebView;
    if (name == "browser") return SessionSourceKind::SystemBrowser;
    return SessionSourceKind::Unknown;
}

// ---------------------------------------------------------------------------------------
// Cookie allow-list. Only these names are ever kept, staged, stored or merged; everything
// else in a pasted header or cookies.txt is dropped at once. INFERENCE: the minimum set the
// API needs is unknown until observed on a device (docs/ytm_hybrid/04 open question 3), so
// this is a table, not a contract. The signing roles follow docs/ytm_hybrid/02 section 3.3.
enum class CookieRole { None, Signing, Identity, Client };

inline CookieRole cookieRole(const std::string &name) {
    static const char *const signing[] = {"SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID"};
    static const char *const identity[] = {"SID", "HSID", "SSID", "APISID", "__Secure-1PSID", "__Secure-3PSID",
        "__Secure-1PSIDTS", "__Secure-3PSIDTS", "__Secure-1PSIDCC", "__Secure-3PSIDCC", "SIDCC", "LOGIN_INFO"};
    static const char *const client[] = {"VISITOR_INFO1_LIVE", "VISITOR_PRIVACY_METADATA", "SOCS", "CONSENT",
        "__Secure-ROLLOUT_TOKEN", "YSC"};
    for (std::size_t i = 0; i < sizeof(signing) / sizeof(signing[0]); ++i) if (name == signing[i]) return CookieRole::Signing;
    for (std::size_t i = 0; i < sizeof(identity) / sizeof(identity[0]); ++i) if (name == identity[i]) return CookieRole::Identity;
    for (std::size_t i = 0; i < sizeof(client) / sizeof(client[0]); ++i) if (name == client[i]) return CookieRole::Client;
    return CookieRole::None;
}
inline bool isNeededCookie(const std::string &name) { return cookieRole(name) != CookieRole::None; }
// A value that is safe to place into a Cookie header and into the line-based bundle:
// printable ASCII without ';' (the cookie separator). Quotes are legal (RFC 6265 allows a
// quoted value and browsers send it back verbatim).
inline bool validCookieValue(const std::string &value) {
    if (value.empty() || value.size() > 4096) return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(value[i]);
        if (c < 0x21 || c > 0x7e || c == ';') return false;
    }
    return true;
}
inline bool validCookieName(const std::string &name) {
    if (name.empty() || name.size() > 64) return false;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}
inline bool digitsOnly(const std::string &value, std::size_t minLen, std::size_t maxLen) {
    if (value.size() < minLen || value.size() > maxLen) return false;
    for (std::size_t i = 0; i < value.size(); ++i) if (value[i] < '0' || value[i] > '9') return false;
    return true;
}

struct SessionCookie {
    std::string name;
    std::string value;   // SECRET
};

// What the vault holds (serialised by SessionBundleCodec) and what the in-memory holder
// keeps. Cookies are unique by name and kept sorted by name (deterministic header order).
struct SessionBundle {
    enum { kMaxCookies = 32, kMaxEncodedBytes = 60000 };   // the vault limit is 64 KiB
    std::vector<SessionCookie> cookies;
    // X-Goog-AuthUser (zero-based index of the signed-in Google account, VERIFIED
    // docs/ytm_hybrid/02 3.4) and the brand-account page id (X-Goog-PageId, INFERENCE from
    // community material). Non-secret but kept inside the bundle with the cookies.
    std::string authUser;
    std::string pageId;
    SessionSourceKind source = SessionSourceKind::Unknown;
    std::int64_t importedAtUtcSeconds = 0;

    SessionBundle() = default;
    SessionBundle(const SessionBundle &) = default;
    SessionBundle(SessionBundle &&) = default;
    SessionBundle &operator=(const SessionBundle &) = default;
    SessionBundle &operator=(SessionBundle &&) = default;
    ~SessionBundle() { wipe(); }

    bool empty() const { return cookies.empty(); }
    const SessionCookie *find(const std::string &name) const {
        for (std::size_t i = 0; i < cookies.size(); ++i) if (cookies[i].name == name) return &cookies[i];
        return nullptr;
    }
    // Inserts or replaces, keeping the vector sorted. False when invalid or the cap is hit.
    bool set(const std::string &name, const std::string &value) {
        if (!validCookieName(name) || !validCookieValue(value)) return false;
        std::size_t at = 0;
        while (at < cookies.size() && cookies[at].name < name) ++at;
        if (at < cookies.size() && cookies[at].name == name) { wipeString(cookies[at].value); cookies[at].value = value; return true; }
        if (cookies.size() >= static_cast<std::size_t>(kMaxCookies)) return false;
        SessionCookie cookie; cookie.name = name; cookie.value = value;
        cookies.insert(cookies.begin() + static_cast<std::ptrdiff_t>(at), cookie);
        return true;
    }
    bool erase(const std::string &name) {
        for (std::size_t i = 0; i < cookies.size(); ++i)
            if (cookies[i].name == name) { wipeString(cookies[i].value); cookies.erase(cookies.begin() + static_cast<std::ptrdiff_t>(i)); return true; }
        return false;
    }
    bool hasRole(CookieRole role) const {
        for (std::size_t i = 0; i < cookies.size(); ++i) if (cookieRole(cookies[i].name) == role) return true;
        return false;
    }
    // Required for any request: a cookie to sign with and a cookie that identifies the session.
    bool usable() const { return hasRole(CookieRole::Signing) && hasRole(CookieRole::Identity); }
    void wipe() {
        for (std::size_t i = 0; i < cookies.size(); ++i) { wipeString(cookies[i].name); wipeString(cookies[i].value); }
        cookies.clear(); wipeString(authUser); wipeString(pageId);
        source = SessionSourceKind::Unknown; importedAtUtcSeconds = 0;
    }
};

}}}}
