// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Qt-free records of the optional YouTube account (SAILTUBE_REQUIREMENTS §3.6,
// ACC-01..11). Signed out is the normal state: nothing here is needed for
// playback, search, the feed or downloads (ACC-01).
//
// Secrets (cookie values) live only in SessionCookies and in the account
// store; they never enter logs, diagnostics or QML.

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace account {

struct Cookie {
    std::string name;
    std::string value;
    std::string domain;     // ".youtube.com", ".google.com", "www.youtube.com" (as imported)
    std::string path = "/";
    std::int64_t expiresSec = 0; // 0 = session cookie
    bool secure = true;
    bool httpOnly = false;
};

// The imported session: cookies by name after the ACC-03 collision rule.
struct SessionCookies {
    std::vector<Cookie> cookies;
    const Cookie *find(const std::string &name) const;
    bool empty() const { return cookies.empty(); }
};

// The selected login (X-Goog-AuthUser) and brand channel (X-Goog-PageId).
struct AccountSelection {
    int authUser = 0;
    std::string pageId;     // empty = the login's own channel
    bool operator==(const AccountSelection &o) const { return authUser == o.authUser && pageId == o.pageId; }
};

// One entry of account/accounts_list (ACC-09).
struct AccountEntry {
    int authUser = 0;
    std::string name;
    std::string handle;        // "@name" when shown
    std::string photoUrl;
    std::string pageId;        // pageIdToken.pageId (brand channel), empty for the login itself
    std::string datasyncId;    // datasyncIdToken.datasyncId
    std::string gaiaId;        // accountStateToken.obfuscatedGaiaId
    bool hasChannel = true;
};

// ACC-07 verification outcome.
enum class Verification { Accepted, Rejected, Inconclusive };

// What the UI shows. "Unreachable" = probe 0 failed on the network (ACC-09),
// never reported as signed out.
enum class SessionState { SignedOut = 0, Checking = 1, SignedIn = 2, Expired = 3, Unreachable = 4 };

// ACC-02 sign-in methods. Integer values are persisted nowhere; order comes
// from signInMethods().
enum class SignInMethod { BrowserImport, PasteOrFile, WebView };

enum class BuildVariant { Full, Store };

}}}
