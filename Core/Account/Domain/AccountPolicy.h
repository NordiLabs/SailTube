// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free account decisions with the spec's numbers and orders (ACC-02, 03,
// 06, 07, 08, 09). tests/account_domain checks each rule.

#include "AccountTypes.h"
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace video { namespace account {

namespace limits {
const std::int64_t verifyIntervalMs = 15LL * 60 * 1000; // ACC-07: every 15 min while signed in
const int maxAuthUserProbe = 7;                          // ACC-09: X-Goog-AuthUser 0..7
const char origin[] = "https://www.youtube.com";
}

// ACC-02: the methods to offer, in rank order. WebView only when built in (ACC-05).
// Never an OAuth device flow.
std::vector<SignInMethod> signInMethods(BuildVariant variant, bool webViewBuiltIn);

// ACC-03 browser cookie databases, probed in this order (relative to $HOME,
// e.g. ".local/share/org.sailfishos/browser/.mozilla/cookies.sqlite").
std::vector<std::string> browserCookieDatabases();
// ACC-03: rows kept from moz_cookies: youtube.com, google.com or a subdomain,
// with or without the leading dot, case-insensitive ("notyoutube.com" is not).
bool importableCookieHost(const std::string &host);
// ACC-03 collision rule: of two cookies with the same name, the `.youtube.com`
// one wins (whatever its position); otherwise the first seen stays. The merged
// set keeps the order of each name's first appearance.
SessionCookies mergeImported(const std::vector<Cookie> &rows);
// ACC-03/04 gate: a session needs __Secure-3PAPISID or SAPISID.
bool hasSigningSecret(const SessionCookies &cookies);

// ACC-06: the tracked cookie names, in Cookie-header order (groups 1..6).
const std::vector<std::string> &trackedCookieNames();
bool isTrackedCookie(const std::string &name);
// "name=value; name=value" in the ACC-06 order, skipping missing names.
std::string cookieHeader(const SessionCookies &cookies);
// The secret used for SAPISIDHASH: __Secure-3PAPISID, else SAPISID; empty if none.
std::string signingSecret(const SessionCookies &cookies);
// "SAPISIDHASH <ts>_<hex sha1("<ts> <secret> <origin>")>". `sha1Hex` computes
// the lower-case hex SHA-1 of its input (Qt or SailTune's signer supplies it).
std::string sapisidHash(std::int64_t timestampSec, const std::string &secret, const std::string &origin,
                        const std::function<std::string(const std::string &)> &sha1Hex);

struct RequestHeaders {
    std::vector<std::pair<std::string, std::string>> headers; // in send order
};
// ACC-06: Cookie, Authorization, Origin, Referer, X-Goog-AuthUser (always),
// X-Goog-PageId (brand channel only). Empty when the session has no secret.
RequestHeaders authenticatedHeaders(const SessionCookies &cookies, const AccountSelection &selection,
                                    std::int64_t timestampSec,
                                    const std::function<std::string(const std::string &)> &sha1Hex);

// ACC-08: folds Set-Cookie values into the session. Updates stored names,
// adds a new one only if tracked, deletes on an expired/empty value for a
// stored name (a deletion for an unknown name is ignored). A rotation updates
// value, domain (when given) and expiry (when given). Returns true only on an
// actual change: the same value again returns false, so the caller writes nothing.
struct SetCookieValue {
    std::string name;
    std::string value;
    std::string domain;
    std::int64_t expiresSec = 0;   // 0 = none given
    bool expired = false;          // Max-Age<=0 or an Expires in the past
};
bool foldRotation(SessionCookies *session, const std::vector<SetCookieValue> &rotated);

// ACC-07 schedule: when the next periodic check is due, on the caller's clock.
// SignedIn/Checking: lastCheckMs < 0 (never checked) -> 0 (now), else
// lastCheckMs + 15 min. SignedOut/Expired/Unreachable -> -1 (no periodic check;
// an expired session is re-checked after re-import or on demand). The start,
// import, switch and on-demand checks are triggered by the service, not here.
std::int64_t nextVerificationMs(SessionState state, std::int64_t lastCheckMs);
// ACC-07: the state after a verification. Accepted -> SignedIn, Rejected ->
// Expired, Inconclusive keeps `current`, except Checking (no state known yet)
// which becomes Unreachable, never SignedOut.
SessionState stateAfter(SessionState current, Verification outcome);

// ACC-09: deduplicate on (gaia id, else datasync id, else name; plus page id).
std::string accountKey(const AccountEntry &e);
// ACC-09 probe stop rule: stop at an index that is logged out, empty, or whose
// entries are all duplicates of earlier ones. `known` = keys seen so far.
bool probeShouldStop(bool loggedOut, const std::vector<AccountEntry> &entries, const std::vector<std::string> &known);

}}}
