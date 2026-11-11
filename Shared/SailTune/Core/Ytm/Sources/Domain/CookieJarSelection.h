// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// From raw cookie-table rows to the text the account layer's import parser understands.
// Pure C++11, no Qt, no I/O.
//
// Behaviour (SPEC = clean-room specification FINTUNE_SPEC.md 4.1; INFERENCE = ours):
//  * only rows of the session allow-list (accounts::domain::isNeededCookie) are used; all other
//    cookies were never read from the database and are ignored here too;
//  * hosts: the cookies a request to music.youtube.com carries (domain cookies of youtube.com and
//    of music.youtube.com, host-only cookies of music.youtube.com) rank first; google.com /
//    .google.com rows fill in names the YouTube side lacks (SPEC "hosts ending youtube.com or
//    google.com"; the preference for the YouTube row is the SPEC's own advice because the same
//    name exists on both with different values). Matching is on whole host names:
//    "notyoutube.com" and the host-only cookies of www.youtube.com are not taken;
//  * path must be "/" (the sign-in cookies are site-wide; INFERENCE);
//  * expired rows are dropped (a value above 1e11 is read as milliseconds: INFERENCE, Gecko
//    versions differ);
//  * of several rows with one name: lowest host rank, then the plain browsing context before a
//    container / first-party-isolated one, then the longest expiry, then the most recently used;
//  * the gate (SPEC): one of the two sign-in cookies below must be among the usable rows,
//    otherwise "cookies but not a session";
//  * the Cookie header is rebuilt from the allowed names in a FIXED order (SPEC lists 17 names;
//    "PREF" is not on the session allow-list and is therefore not carried; the allow-list's
//    remaining names follow), so two imports of the same jar produce identical text.
//
// The composed text is SECRET. It is a bare `name=value; ...` line, optionally preceded by the
// two account-selection lines of the import parser's header-block grammar.
#pragma once

#include "../Ports/CookieJarReaderPort.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace domain {

struct SelectionOptions {
    std::int64_t nowUtcSeconds = 0;     // > 0 enables the expiry filter
    bool allowGoogleFallback = true;    // use google.com rows for names the YouTube side lacks
    // Account selection for multi-login / brand accounts (AccountEnumerator output). Empty = default.
    // authUser: 1-2 digits; pageId: 1-32 digits. An invalid value is ignored, never echoed.
    std::string authUser;
    std::string pageId;
};

struct SelectionResult {
    enum class Status { Ok, NoCookies, NotSignedIn };
    Status status = Status::NoCookies;
    std::string text;                           // SECRET; only when status == Ok
    std::vector<std::string> names;             // non-secret: cookie NAMES in the order they were written
    std::string gateName;                       // which sign-in cookie opened the gate (a name)
    int fromGoogleHost = 0;                     // names that came from a google.com row
    int skippedNotNeeded = 0, skippedHost = 0, skippedPath = 0, skippedExpired = 0, skippedInvalid = 0;
    int duplicatesDropped = 0;
    bool selectionApplied = false;              // the account-selection lines were written
    SelectionResult() = default;
    SelectionResult(const SelectionResult &) = default;
    SelectionResult(SelectionResult &&) = default;
    SelectionResult &operator=(const SelectionResult &) = default;
    SelectionResult &operator=(SelectionResult &&) = default;
    ~SelectionResult() { wipe(); }
    void wipe();
};

class CookieJarSelection {
public:
    // `matchingRows` is JarRead::matchingRows (YouTube/Google rows of any name in the file).
    static SelectionResult compose(const std::vector<ports::RawCookieRow> &rows, int matchingRows,
                                   const SelectionOptions &options);

    // The fixed order, exposed for the tests (every entry is on the session allow-list and the
    // table covers the whole allow-list).
    static std::size_t orderSize();
    static const char *orderAt(std::size_t index);
    // The two cookies of which one opens the gate.
    static bool isGateCookie(const std::string &name);

    // 0 = a host a request to music.youtube.com carries, 1 = google.com / .google.com, -1 = anything else.
    static int hostRank(const std::string &host);
    // Seconds since the epoch; a value above 1e11 is taken as milliseconds. 0 stays 0.
    static std::int64_t normaliseExpiry(std::int64_t expiry);
    static bool validAuthUser(const std::string &value);
    static bool validPageId(const std::string &value);
};

}}}}}
