// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionExpiryDetector.h"

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
using domain::SessionVerdict;

const std::size_t kExamined = 8192;

Classification make(SessionVerdict verdict, ExpiryReason reason) {
    Classification c; c.verdict = verdict; c.reason = reason; return c;
}
std::string lower(const std::string &in, std::size_t limit) {
    std::string out; const std::size_t n = in.size() < limit ? in.size() : limit;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        char c = in[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        out.push_back(c);
    }
    return out;
}
bool has(const std::string &hay, const char *needle) { return hay.find(needle) != std::string::npos; }

// First significant position: skip a UTF-8 BOM and ASCII whitespace.
std::size_t skip(const std::string &s) {
    std::size_t i = 0;
    if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF && static_cast<unsigned char>(s[1]) == 0xBB &&
        static_cast<unsigned char>(s[2]) == 0xBF) i = 3;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) ++i;
    return i;
}
bool startsWithAt(const std::string &s, std::size_t at, const char *prefix) {
    for (std::size_t i = 0; prefix[i]; ++i) if (at + i >= s.size() || s[at + i] != prefix[i]) return false;
    return true;
}
bool hostIs(const std::string &host, const char *name) {
    const std::string n(name);
    if (host == n) return true;
    return host.size() > n.size() && host.compare(host.size() - n.size(), n.size(), n) == 0 && host[host.size() - n.size() - 1] == '.';
}

enum Kind { Html, Json, Other, Empty };
Kind kindOf(const std::string &contentTypeLower, const std::string &bodyLower) {
    const std::size_t at = skip(bodyLower);
    if (at >= bodyLower.size()) return Empty;
    const bool htmlType = has(contentTypeLower, "html");
    const bool htmlBody = startsWithAt(bodyLower, at, "<!doctype html") || startsWithAt(bodyLower, at, "<html") ||
                          startsWithAt(bodyLower, at, "<head") || startsWithAt(bodyLower, at, "<body") ||
                          startsWithAt(bodyLower, at, "<!--");
    if (htmlType || htmlBody) return Html;
    std::size_t p = at;
    if (startsWithAt(bodyLower, p, ")]}'")) { p += 4; while (p < bodyLower.size() && (bodyLower[p] == '\n' || bodyLower[p] == '\r')) ++p; }
    if (p < bodyLower.size() && (bodyLower[p] == '{' || bodyLower[p] == '[')) return Json;
    if (has(contentTypeLower, "json")) return Json;
    return Other;
}
bool challengeWording(const std::string &t) {
    return has(t, "unusual traffic") || has(t, "/sorry/") || has(t, "captcha") || has(t, "not a robot") ||
           has(t, "not a bot") || has(t, "our systems have detected") ||
           // the bot check of the service ("Sign in to confirm you're not a bot"; it also reaches us inside an error
           // document, where it may even carry a permission status). "not a bot" covers the full sentence; this covers
           // the explicit signal on its own. An age gate ("sign in to confirm your age") belongs to watch pages, which
           // this client never requests.
           has(t, "sign in to confirm");
}
bool consentWording(const std::string &t) {
    return has(t, "consent.youtube.com") || has(t, "consent.google.com") || has(t, "before you continue");
}
bool signInWording(const std::string &t) {
    return has(t, "accounts.google.com") || has(t, "servicelogin") || has(t, "sign in") || has(t, "signin");
}

// ASCII whitespace removed, so `"error" : {` and `"error":{` are one pattern.
std::string squeezed(const std::string &t) {
    std::string out; out.reserve(t.size());
    for (std::size_t i = 0; i < t.size(); ++i) if (t[i] != ' ' && t[i] != '\t' && t[i] != '\r' && t[i] != '\n') out.push_back(t[i]);
    return out;
}

// An error DOCUMENT (JSON with an `error` member) that names a permission failure: the canonical status
// PERMISSION_DENIED, a `forbidden` / `insufficientPermissions` reason, or an insufficient-scope wording.
// INFERENCE (not observed on a real account): the markers are those of Google's public error model. An
// authentication marker (UNAUTHENTICATED, LOGIN_REQUIRED) is never a permission error: that is a sign-in
// matter, so such a body stays an unexplained 403. The caller has already ruled out challenge and consent
// wording. `lowered` is the lower-cased examined prefix (at most 8 KiB).
bool permissionErrorDocument(const std::string &lowered) {
    if (!has(squeezed(lowered), "\"error\":")) return false;
    if (has(lowered, "unauthenticated") || has(lowered, "login_required")) return false;
    return has(lowered, "permission_denied") || has(lowered, "permission denied") || has(lowered, "does not have permission") ||
           has(lowered, "do not have permission") || has(lowered, "insufficientpermission") || has(lowered, "insufficient permission") ||
           has(lowered, "insufficient authentication scopes") || has(lowered, "insufficient scope") || has(lowered, "insufficientscope") ||
           has(lowered, "scope_insufficient") || has(lowered, "forbidden");
}
}

Classification SessionExpiryDetector::classify(const ExpiryObservation &o) {
    if (o.networkError || o.httpStatus <= 0) return make(SessionVerdict::Transient, ExpiryReason::NetworkError);
    if (o.oversized) return make(SessionVerdict::ProtocolChanged, ExpiryReason::OversizedBody);

    const int status = o.httpStatus;
    if (status == 429) return make(SessionVerdict::RateLimited, ExpiryReason::Http429);
    if (status == 401) return make(SessionVerdict::SessionExpired, ExpiryReason::Http401);

    const std::string body = lower(o.bodyPrefix, kExamined);
    const std::string type = lower(o.contentType, 256);

    if (status >= 300 && status < 400) {
        const std::string host = lower(o.redirectHost, 256);
        const std::string path = lower(o.redirectPath, 256);
        if (hostIs(host, "accounts.google.com") || hostIs(host, "accounts.youtube.com")) return make(SessionVerdict::SessionExpired, ExpiryReason::SignInRedirect);
        if (host.compare(0, 8, "consent.") == 0) return make(SessionVerdict::Blocked, ExpiryReason::ConsentRedirect);
        if ((hostIs(host, "google.com")) && path.compare(0, 6, "/sorry") == 0)
            return make(SessionVerdict::Blocked, ExpiryReason::ChallengeRedirect);
        return make(SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedRedirect);
    }
    if (status == 403) {
        // Order matters: a bot check or a consent wall that happens to be phrased like a permission error is
        // still a block. Only what is left AND explains itself as a permission error is operation-scoped.
        if (challengeWording(body)) return make(SessionVerdict::Blocked, ExpiryReason::Http403Challenge);
        if (consentWording(body)) return make(SessionVerdict::Blocked, ExpiryReason::Http403);
        if (kindOf(type, body) == Json && permissionErrorDocument(body)) return make(SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden);
        return make(SessionVerdict::Blocked, ExpiryReason::Http403);
    }
    if (status == 408) return make(SessionVerdict::Transient, ExpiryReason::Http408);
    if (status >= 500 && status < 600) return make(SessionVerdict::Transient, ExpiryReason::Http5xx);
    if (status == 400) return make(SessionVerdict::ProtocolChanged, ExpiryReason::BadRequest);
    if (status != 200) return make(SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedStatus);

    switch (kindOf(type, body)) {
    case Empty: return make(SessionVerdict::ProtocolChanged, ExpiryReason::EmptyBody);
    case Other: return make(SessionVerdict::ProtocolChanged, ExpiryReason::NotJson);
    case Html:
        if (challengeWording(body)) return make(SessionVerdict::Blocked, ExpiryReason::HtmlChallenge);
        if (consentWording(body)) return make(SessionVerdict::Blocked, ExpiryReason::HtmlConsent);
        if (signInWording(body)) return make(SessionVerdict::SessionExpired, ExpiryReason::HtmlSignIn);
        return make(SessionVerdict::SessionExpired, ExpiryReason::HtmlUnknown);
    case Json:
        switch (o.shape) {
        case BodyShape::SignedOut: return make(SessionVerdict::SessionExpired, ExpiryReason::SignedOutShape);
        case BodyShape::MissingRequired: return make(SessionVerdict::ProtocolChanged, ExpiryReason::MissingRequiredFields);
        case BodyShape::SignedIn: return make(SessionVerdict::Ok, ExpiryReason::Ok);
        case BodyShape::NotChecked: return make(SessionVerdict::Ok, ExpiryReason::OkShapeUnchecked);
        }
    }
    return make(SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedStatus);
}

const char *SessionExpiryDetector::reasonName(ExpiryReason r) {
    switch (r) {
    case ExpiryReason::Ok: return "Ok";
    case ExpiryReason::OkShapeUnchecked: return "OkShapeUnchecked";
    case ExpiryReason::Http401: return "Http401";
    case ExpiryReason::SignInRedirect: return "SignInRedirect";
    case ExpiryReason::HtmlSignIn: return "HtmlSignIn";
    case ExpiryReason::HtmlUnknown: return "HtmlUnknown";
    case ExpiryReason::SignedOutShape: return "SignedOutShape";
    case ExpiryReason::ConsentRedirect: return "ConsentRedirect";
    case ExpiryReason::ChallengeRedirect: return "ChallengeRedirect";
    case ExpiryReason::HtmlConsent: return "HtmlConsent";
    case ExpiryReason::HtmlChallenge: return "HtmlChallenge";
    case ExpiryReason::Http403: return "Http403";
    case ExpiryReason::Http403Challenge: return "Http403Challenge";
    case ExpiryReason::Http429: return "Http429";
    case ExpiryReason::NetworkError: return "NetworkError";
    case ExpiryReason::Http5xx: return "Http5xx";
    case ExpiryReason::Http408: return "Http408";
    case ExpiryReason::UnexpectedRedirect: return "UnexpectedRedirect";
    case ExpiryReason::OversizedBody: return "OversizedBody";
    case ExpiryReason::BadRequest: return "BadRequest";
    case ExpiryReason::UnexpectedStatus: return "UnexpectedStatus";
    case ExpiryReason::EmptyBody: return "EmptyBody";
    case ExpiryReason::NotJson: return "NotJson";
    case ExpiryReason::MissingRequiredFields: return "MissingRequiredFields";
    case ExpiryReason::Http403Forbidden: return "Http403Forbidden";
    }
    return "Unknown";
}

}}}}
