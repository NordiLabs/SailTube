// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// C++11-only tests of the ytm-sources slice: row selection and text composition (through the REAL
// import parser), the multi-login enumeration policy, BrowserCookieJarSource over fakes (consent,
// availability, busy, cancel, destruction mid-read, worker thread, no value in any message) and the
// POSIX scratch helpers (modes, symlink refusal, zeroing, cleanup). No Qt, no network.
#include "check.h"
#include "Fakes.h"
#include "Core/Accounts/Infrastructure/SessionImportParser.h"
#include "Core/Ytm/Sources/Application/BrowserCookieJarSource.h"
#include "Core/Ytm/Sources/Infrastructure/PrivateScratch.h"
#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <random>
#include <set>
#include <sys/stat.h>
#include <unistd.h>

namespace sel = src::domain;
namespace sc = src::infrastructure::scratch;
using src::ports::RawCookieRow;
typedef std::vector<RawCookieRow> Rows;

static const std::int64_t kNow = 1700000000;

static sel::SelectionOptions opts() { sel::SelectionOptions o; o.nowUtcSeconds = kNow; return o; }
static sel::SelectionResult compose(const Rows &rows, int matching = -1, const sel::SelectionOptions &o = opts()) {
    return sel::CookieJarSelection::compose(rows, matching < 0 ? static_cast<int>(rows.size()) : matching, o);
}
static bool hasName(const sel::SelectionResult &r, const char *name) { return std::find(r.names.begin(), r.names.end(), name) != r.names.end(); }

// ---------------------------------------------------------------------------------------------
// selection
// ---------------------------------------------------------------------------------------------
static void testOrderTable() {
    const char *allowed[] = {"SAPISID", "__Secure-1PAPISID", "__Secure-3PAPISID", "SID", "HSID", "SSID", "APISID", "__Secure-1PSID",
        "__Secure-3PSID", "__Secure-1PSIDTS", "__Secure-3PSIDTS", "__Secure-1PSIDCC", "__Secure-3PSIDCC", "SIDCC", "LOGIN_INFO",
        "VISITOR_INFO1_LIVE", "VISITOR_PRIVACY_METADATA", "SOCS", "CONSENT", "__Secure-ROLLOUT_TOKEN", "YSC"};
    const std::size_t n = sizeof(allowed) / sizeof(allowed[0]);
    CHECK_EQ(sel::CookieJarSelection::orderSize(), n);
    std::set<std::string> seen;
    for (std::size_t i = 0; i < sel::CookieJarSelection::orderSize(); ++i) {
        const std::string name = sel::CookieJarSelection::orderAt(i);
        CHECK(acc::domain::isNeededCookie(name));        // only allowed names
        CHECK(seen.insert(name).second);                 // no duplicates
    }
    for (std::size_t i = 0; i < n; ++i) CHECK(seen.count(allowed[i]) == 1);   // the table covers the allow-list
    CHECK(!acc::domain::isNeededCookie("PREF"));                              // the specification's 17th name is not carried
    CHECK(std::string(sel::CookieJarSelection::orderAt(9999)).empty());
    // the specification's order for the names it lists
    const char *spec[] = {"__Secure-3PAPISID", "SAPISID", "__Secure-1PAPISID", "__Secure-3PSID", "__Secure-1PSID", "SID", "HSID", "SSID",
        "APISID", "SIDCC", "__Secure-3PSIDCC", "__Secure-1PSIDCC", "LOGIN_INFO", "VISITOR_INFO1_LIVE", "__Secure-3PSIDTS", "__Secure-1PSIDTS"};
    for (std::size_t i = 0; i < sizeof(spec) / sizeof(spec[0]); ++i) CHECK_EQ(std::string(sel::CookieJarSelection::orderAt(i)), std::string(spec[i]));
    CHECK(sel::CookieJarSelection::isGateCookie("__Secure-3PAPISID"));
    CHECK(sel::CookieJarSelection::isGateCookie("SAPISID"));
    CHECK(!sel::CookieJarSelection::isGateCookie("__Secure-1PAPISID"));
    CHECK(!sel::CookieJarSelection::isGateCookie("SID"));
}

static void testHostRank() {
    typedef sel::CookieJarSelection S;
    CHECK_EQ(S::hostRank(".youtube.com"), 0);
    CHECK_EQ(S::hostRank(".YouTube.COM"), 0);
    CHECK_EQ(S::hostRank("music.youtube.com"), 0);
    CHECK_EQ(S::hostRank(".music.youtube.com"), 0);
    CHECK_EQ(S::hostRank(".google.com"), 1);
    CHECK_EQ(S::hostRank("google.com"), 1);
    // host-only cookies of the apex / www are not sent to music.youtube.com; look-alikes are other sites
    CHECK_EQ(S::hostRank("youtube.com"), -1);
    CHECK_EQ(S::hostRank("www.youtube.com"), -1);
    CHECK_EQ(S::hostRank(".www.youtube.com"), -1);
    CHECK_EQ(S::hostRank("notyoutube.com"), -1);
    CHECK_EQ(S::hostRank(".notyoutube.com"), -1);
    CHECK_EQ(S::hostRank("youtube.com.evil.net"), -1);
    CHECK_EQ(S::hostRank(".youtube.com.evil.net"), -1);
    CHECK_EQ(S::hostRank("evilgoogle.com"), -1);
    CHECK_EQ(S::hostRank("accounts.google.com"), -1);
    CHECK_EQ(S::hostRank(".google.co.uk"), -1);
    CHECK_EQ(S::hostRank(""), -1);
    CHECK_EQ(S::hostRank("."), -1);
    CHECK_EQ(S::hostRank(std::string(300, 'a') + ".youtube.com"), -1);
    CHECK_EQ(S::normaliseExpiry(1700000000), 1700000000);
    CHECK_EQ(S::normaliseExpiry(1700000000000LL), 1700000000);
    CHECK_EQ(S::normaliseExpiry(0), 0);
    CHECK(S::validAuthUser("0")); CHECK(S::validAuthUser("12")); CHECK(!S::validAuthUser("")); CHECK(!S::validAuthUser("123")); CHECK(!S::validAuthUser("a"));
    CHECK(S::validPageId("1")); CHECK(S::validPageId(std::string(32, '9'))); CHECK(!S::validPageId(std::string(33, '9'))); CHECK(!S::validPageId("12a"));
}

static void testComposeExactText() {
    const sel::SelectionResult r = compose(goodRows());
    CHECK(r.status == sel::SelectionResult::Status::Ok);
    const std::string expected = std::string("__Secure-3PAPISID=") + kV3PAPISID + "; SAPISID=" + kVSAPISID + "; __Secure-1PAPISID=" + kV1PAPISID +
        "; __Secure-3PSID=" + kV3PSID + "; __Secure-1PSID=" + kV1PSID + "; SID=" + kVSID + "; HSID=" + kVHSID + "; SSID=" + kVSSID +
        "; APISID=" + kVAPISID + "; SIDCC=" + kVSIDCC + "; __Secure-3PSIDCC=" + kV3PSIDCC + "; __Secure-1PSIDCC=" + kV1PSIDCC +
        "; LOGIN_INFO=" + kVLOGIN + "; VISITOR_INFO1_LIVE=" + kVVISITOR + "; __Secure-3PSIDTS=" + kV3PSIDTS + "; __Secure-1PSIDTS=" + kV1PSIDTS +
        "; SOCS=" + kVSOCS;
    CHECK_EQ(r.text, expected);
    CHECK_EQ(r.names.size(), static_cast<std::size_t>(17));
    CHECK_EQ(r.gateName, std::string("__Secure-3PAPISID"));
    CHECK(!r.selectionApplied);
    CHECK_EQ(r.fromGoogleHost, 0);
    // the real parser accepts it and keeps every cookie
    const acc::infrastructure::ImportResult p = acc::infrastructure::SessionImportParser::parse(r.text);
    CHECK(p.ok());
    CHECK_EQ(p.summary.cookiesKept, 17);
    CHECK(p.bundle.usable());
    CHECK(p.summary.hasSigningCookie && p.summary.hasSessionCookie);
    CHECK(p.summary.format == acc::ports::ImportFormat::CookieHeader);
    const acc::domain::SessionCookie *c = p.bundle.find("SID");
    CHECK(c && c->value == kVSID);
    // names only, in the written order, never a value
    for (std::size_t i = 0; i < r.names.size(); ++i) CHECK(!containsSentinel(r.names[i]));
}

static void testGateAndStatus() {
    {   // nothing at all
        const sel::SelectionResult r = compose(Rows(), 0);
        CHECK(r.status == sel::SelectionResult::Status::NoCookies);
        CHECK(r.text.empty());
    }
    {   // cookies exist but no allow-listed row
        const sel::SelectionResult r = compose(Rows(), 12);
        CHECK(r.status == sel::SelectionResult::Status::NotSignedIn);
    }
    {   // only the consent / visitor cookies: "cookies but not a session"
        Rows rows; rows.push_back(row(".youtube.com", "SOCS", kVSOCS)); rows.push_back(row(".youtube.com", "VISITOR_INFO1_LIVE", kVVISITOR));
        const sel::SelectionResult r = compose(rows);
        CHECK(r.status == sel::SelectionResult::Status::NotSignedIn);
        CHECK(r.text.empty());
    }
    {   // the first-party cookie alone does not open the gate (the gate is the two third-party/legacy names)
        Rows rows = goodRows();
        rows.erase(rows.begin(), rows.begin() + 2);          // drop 3PAPISID and SAPISID
        CHECK(compose(rows).status == sel::SelectionResult::Status::NotSignedIn);
    }
    {   // SAPISID alone opens it
        Rows rows = goodRows();
        rows.erase(rows.begin());                            // drop 3PAPISID
        const sel::SelectionResult r = compose(rows);
        CHECK(r.status == sel::SelectionResult::Status::Ok);
        CHECK_EQ(r.gateName, std::string("SAPISID"));
    }
    {   // 3PAPISID alone opens it
        Rows rows = goodRows();
        rows.erase(rows.begin() + 1);                        // drop SAPISID
        const sel::SelectionResult r = compose(rows);
        CHECK(r.status == sel::SelectionResult::Status::Ok);
        CHECK_EQ(r.gateName, std::string("__Secure-3PAPISID"));
    }
    {   // an expired gate cookie does not open the gate
        Rows rows = goodRows();
        rows[0].expiry = 1600000000; rows[1].expiry = 1600000000;
        CHECK(compose(rows).status == sel::SelectionResult::Status::NotSignedIn);
    }
}

static void testRowFilters() {
    {   // names the allow-list does not carry are ignored and counted
        Rows rows = goodRows();
        rows.push_back(row(".youtube.com", "PREF", kVOther));
        rows.push_back(row(".youtube.com", "NID", kVOther));
        rows.push_back(row(".youtube.com", "__Host-GAPS", kVOther));
        const sel::SelectionResult r = compose(rows);
        CHECK_EQ(r.skippedNotNeeded, 3);
        CHECK(!hasName(r, "PREF"));
        CHECK(r.text.find(kVOther) == std::string::npos);
    }
    {   // hosts: look-alikes and unrelated sites never contribute
        Rows rows = goodRows();
        rows.push_back(row("notyoutube.com", "SID", "SNTLSRC_LOOKALIKE_1"));
        rows.push_back(row("www.youtube.com", "HSID", "SNTLSRC_WWW_2"));
        rows.push_back(row("example.org", "SSID", "SNTLSRC_FOREIGN_3"));
        rows.push_back(row("youtube.com.evil.net", "APISID", "SNTLSRC_EVIL_4"));
        const sel::SelectionResult r = compose(rows);
        CHECK_EQ(r.skippedHost, 4);
        CHECK(r.text.find("LOOKALIKE") == std::string::npos && r.text.find("WWW") == std::string::npos);
        CHECK(r.text.find("FOREIGN") == std::string::npos && r.text.find("EVIL") == std::string::npos);
    }
    {   // paths
        Rows rows = goodRows();
        rows.push_back(row(".youtube.com", "YSC", "SNTLSRC_PATH_5", "/embed"));
        rows.push_back(row(".youtube.com", "CONSENT", "SNTLSRC_PATH_6", ""));
        const sel::SelectionResult r = compose(rows);
        CHECK_EQ(r.skippedPath, 2);
        CHECK(!hasName(r, "YSC") && !hasName(r, "CONSENT"));
    }
    {   // expiry: seconds, milliseconds, unknown, filter off
        Rows rows = goodRows();
        rows.push_back(row(".youtube.com", "YSC", "SNTLSRC_EXP_PAST", "/", 1600000000));                 // expired (seconds)
        rows.push_back(row(".youtube.com", "CONSENT", "SNTLSRC_EXP_PASTMS", "/", 1600000000000LL));      // expired (milliseconds)
        rows.push_back(row(".youtube.com", "VISITOR_PRIVACY_METADATA", "SNTLSRC_EXP_FUTUREMS", "/", 4102444800000LL));   // valid (ms)
        RawCookieRow noExpiry = row(".youtube.com", "__Secure-ROLLOUT_TOKEN", "SNTLSRC_EXP_NONE"); noExpiry.hasExpiry = false;
        rows.push_back(noExpiry);
        const sel::SelectionResult r = compose(rows);
        CHECK_EQ(r.skippedExpired, 2);
        CHECK(!hasName(r, "YSC") && !hasName(r, "CONSENT"));
        CHECK(hasName(r, "VISITOR_PRIVACY_METADATA") && hasName(r, "__Secure-ROLLOUT_TOKEN"));
        sel::SelectionOptions off = opts(); off.nowUtcSeconds = 0;       // filter disabled
        const sel::SelectionResult all = compose(rows, -1, off);
        CHECK_EQ(all.skippedExpired, 0);
        CHECK(hasName(all, "YSC") && hasName(all, "CONSENT"));
    }
    {   // values
        Rows rows = goodRows();
        rows.push_back(row(".youtube.com", "YSC", "has;semicolon"));
        rows.push_back(row(".youtube.com", "CONSENT", "has space"));
        rows.push_back(row(".youtube.com", "VISITOR_PRIVACY_METADATA", ""));
        rows.push_back(row(".youtube.com", "__Secure-ROLLOUT_TOKEN", std::string(5000, 'x').c_str()));
        rows.push_back(row(".youtube.com", "SOCS", "line\nbreak"));      // a second SOCS row; it must lose to nothing and be skipped
        RawCookieRow na = row(".youtube.com", "APISID", "caf\xc3\xa9"); rows.push_back(na);
        rows.push_back(row(".youtube.com", "CONSENT", "\"quoted\""));      // a quote is legal in a cookie value
        const sel::SelectionResult r = compose(rows);
        CHECK_EQ(r.skippedInvalid, 6);
        CHECK(!hasName(r, "YSC") && !hasName(r, "VISITOR_PRIVACY_METADATA") && !hasName(r, "__Secure-ROLLOUT_TOKEN"));
        CHECK(hasName(r, "CONSENT") && r.text.find("\"quoted\"") != std::string::npos);
    }
}

static void testDuplicatePreference() {
    {   // YouTube beats google.com whatever the order, and the google value never appears
        Rows a; a.push_back(row(".google.com", "SAPISID", kVSAPISIDGoogle)); a.push_back(row(".youtube.com", "SAPISID", kVSAPISID));
        Rows b(a.rbegin(), a.rend());
        for (int i = 0; i < 2; ++i) {
            const sel::SelectionResult r = compose(i ? b : a);
            CHECK(r.status == sel::SelectionResult::Status::Ok);
            CHECK(r.text.find(kVSAPISID) != std::string::npos);
            CHECK(r.text.find(kVSAPISIDGoogle) == std::string::npos);
            CHECK_EQ(r.fromGoogleHost, 0);
            CHECK_EQ(r.duplicatesDropped, 1);
        }
    }
    {   // a name that exists only on google.com fills in (and can open the gate)
        Rows rows; rows.push_back(row(".google.com", "SAPISID", kVSAPISIDGoogle));
        const sel::SelectionResult r = compose(rows);
        CHECK(r.status == sel::SelectionResult::Status::Ok);
        CHECK_EQ(r.fromGoogleHost, 1);
        sel::SelectionOptions noFallback = opts(); noFallback.allowGoogleFallback = false;
        const sel::SelectionResult n = compose(rows, -1, noFallback);
        CHECK(n.status == sel::SelectionResult::Status::NotSignedIn);
        CHECK_EQ(n.skippedHost, 1);
    }
    {   // the ordinary browsing context beats a container / first-party-isolated one, in either order
        Rows a; a.push_back(row(".youtube.com", "SID", kVContainer, "/", 4102444800LL, "^userContextId=2")); a.push_back(row(".youtube.com", "SID", kVSID));
        Rows b(a.rbegin(), a.rend());
        for (int i = 0; i < 2; ++i) {
            Rows rows = i ? b : a; rows.push_back(row(".youtube.com", "SAPISID", kVSAPISID));
            const sel::SelectionResult r = compose(rows);
            CHECK(r.text.find(kVSID) != std::string::npos);
            CHECK(r.text.find(kVContainer) == std::string::npos);
        }
    }
    {   // longer expiry wins, then the most recently used
        Rows rows; rows.push_back(row(".youtube.com", "SAPISID", kVSAPISID));
        rows.push_back(row(".youtube.com", "SID", "SNTLSRC_SID_SHORT", "/", 1800000000));
        rows.push_back(row(".youtube.com", "SID", "SNTLSRC_SID_LONG", "/", 4100000000LL));
        const sel::SelectionResult r = compose(rows);
        CHECK(r.text.find("SID_LONG") != std::string::npos && r.text.find("SID_SHORT") == std::string::npos);
        Rows rows2; rows2.push_back(row(".youtube.com", "SAPISID", kVSAPISID));
        rows2.push_back(row(".youtube.com", "SID", "SNTLSRC_SID_OLD", "/", 4100000000LL, "", 5));
        rows2.push_back(row(".youtube.com", "SID", "SNTLSRC_SID_NEW", "/", 4100000000LL, "", 9));
        const sel::SelectionResult r2 = compose(rows2);
        CHECK(r2.text.find("SID_NEW") != std::string::npos && r2.text.find("SID_OLD") == std::string::npos);
    }
    {   // the order of the rows does not change the text (no exact ties here)
        Rows rows = goodRows();
        rows.push_back(row(".google.com", "SID", "SNTLSRC_SID_G"));
        rows.push_back(row(".youtube.com", "HSID", "SNTLSRC_HSID_OLD", "/", 1800000000));
        std::mt19937 rng(7);
        const std::string reference = compose(rows).text;
        for (int i = 0; i < 200; ++i) {
            std::shuffle(rows.begin(), rows.end(), rng);
            CHECK_EQ(compose(rows).text, reference);
        }
    }
}

static void testCapAndSelection() {
    {   // a hostile jar of 4000-byte values: the line stays bounded and the gate cookies survive
        Rows rows;
        const std::string big(4000, 'v');
        for (std::size_t i = 0; i < sel::CookieJarSelection::orderSize(); ++i)
            rows.push_back(row(".youtube.com", sel::CookieJarSelection::orderAt(i), big.c_str()));
        const sel::SelectionResult r = compose(rows);
        CHECK(r.status == sel::SelectionResult::Status::Ok);
        CHECK(r.text.size() <= static_cast<std::size_t>(src::domain::Limits::kMaxComposedBytes));
        CHECK(r.names.size() < sel::CookieJarSelection::orderSize());
        CHECK(!r.names.empty() && r.names[0] == "__Secure-3PAPISID");
        const acc::infrastructure::ImportResult p = acc::infrastructure::SessionImportParser::parse(r.text);
        CHECK(p.ok());
    }
    {   // account selection lines: accepted by the real parser, applied to the bundle
        sel::SelectionOptions o = opts(); o.authUser = "2"; o.pageId = "109876543210987654321";
        const sel::SelectionResult r = compose(goodRows(), -1, o);
        CHECK(r.selectionApplied);
        CHECK(r.text.compare(0, 18, "X-Goog-AuthUser: 2") == 0);
        const acc::infrastructure::ImportResult p = acc::infrastructure::SessionImportParser::parse(r.text);
        CHECK(p.ok());
        CHECK(p.summary.format == acc::ports::ImportFormat::HeaderBlock);
        CHECK_EQ(p.bundle.authUser, std::string("2"));
        CHECK_EQ(p.bundle.pageId, std::string("109876543210987654321"));
        CHECK_EQ(p.summary.cookiesKept, 17);
        CHECK(p.bundle.usable());
        sel::SelectionOptions onlyPage = opts(); onlyPage.pageId = "42";
        const acc::infrastructure::ImportResult q = acc::infrastructure::SessionImportParser::parse(compose(goodRows(), -1, onlyPage).text);
        CHECK(q.ok() && q.bundle.pageId == "42" && q.bundle.authUser.empty());
    }
    {   // an invalid selection is ignored, never echoed
        const char *badUser[] = {"abc", "123", "-1", " 1", "1 ", "1;2", "\n1"};
        for (std::size_t i = 0; i < sizeof(badUser) / sizeof(badUser[0]); ++i) {
            sel::SelectionOptions o = opts(); o.authUser = badUser[i];
            const sel::SelectionResult r = compose(goodRows(), -1, o);
            CHECK(!r.selectionApplied);
            CHECK(r.text.find("X-Goog") == std::string::npos);
        }
        const std::string badPage[] = {"abc", "-1", " 1", "1 ", "1;2", std::string(33, '1'), "1\n2"};
        for (std::size_t i = 0; i < sizeof(badPage) / sizeof(badPage[0]); ++i) {
            sel::SelectionOptions o = opts(); o.pageId = badPage[i];
            const sel::SelectionResult r = compose(goodRows(), -1, o);
            CHECK(!r.selectionApplied);
            CHECK(r.text.find("X-Goog") == std::string::npos);
        }
        sel::SelectionOptions mixed = opts(); mixed.authUser = "abc"; mixed.pageId = "77";     // one valid half is still applied
        const sel::SelectionResult m = compose(goodRows(), -1, mixed);
        CHECK(m.selectionApplied && m.text.find("X-Goog-PageId: 77") != std::string::npos && m.text.find("AuthUser") == std::string::npos);
    }
}

static void testWipe() {
    sel::SelectionResult r = compose(goodRows());
    const char *p = r.text.data();
    const std::size_t n = r.text.size();
    CHECK(n > 100);
    r.wipe();
    CHECK(r.text.empty() && r.names.empty());
    bool zero = true;
    for (std::size_t i = 0; i < n; ++i) if (p[i] != 0) zero = false;      // the (retained) buffer was zeroed, not just forgotten
    CHECK(zero);
    RawCookieRow one = row(".youtube.com", "SID", kVSID);
    const char *v = one.value.data();
    const std::size_t vn = one.value.size();
    CHECK(vn > 16);
    one.wipe();
    bool vz = true;
    for (std::size_t i = 0; i < vn; ++i) if (v[i] != 0) vz = false;       // the retained buffer holds zeros
    CHECK(vz && one.value.empty());
    src::ports::JarRead jar = okJar(goodRows());
    CHECK(!jar.rows.empty());
    jar.wipe();
    CHECK(jar.rows.empty());
}

static void testSelectionFuzz() {
    std::mt19937 rng(12345);
    const char *names[] = {"__Secure-3PAPISID", "SAPISID", "__Secure-1PAPISID", "SID", "HSID", "SSID", "APISID", "__Secure-3PSID", "__Secure-1PSID",
        "LOGIN_INFO", "SOCS", "YSC", "PREF", "NID", "bad name", "", "x;y", "__Secure-3PSIDTS"};
    const char *hosts[] = {".youtube.com", "music.youtube.com", ".music.youtube.com", ".google.com", "google.com", "youtube.com", "www.youtube.com",
        "notyoutube.com", "", ".GOOGLE.com", "example.org"};
    const char *values[] = {"aaaa1111", "SNTLSRC_x", "bad;value", "bad value", "", "\"q\"", "caf\xc3\xa9", "0123456789abcdef0123456789abcdef"};
    const char *paths[] = {"/", "/", "/", "/x", ""};
    const char *origins[] = {"", "", "", "^userContextId=1", "^firstPartyDomain=a.com"};
    const std::int64_t expiries[] = {0, 1600000000, 1800000000, 4102444800LL, 4102444800000LL, 1600000000000LL, -5};
    for (int iter = 0; iter < 20000; ++iter) {
        Rows rows;
        const int count = static_cast<int>(rng() % 41);
        for (int i = 0; i < count; ++i) {
            RawCookieRow r;
            r.name = names[rng() % (sizeof(names) / sizeof(names[0]))];
            r.host = hosts[rng() % (sizeof(hosts) / sizeof(hosts[0]))];
            r.value = values[rng() % (sizeof(values) / sizeof(values[0]))];
            r.path = paths[rng() % (sizeof(paths) / sizeof(paths[0]))];
            r.originAttributes = origins[rng() % (sizeof(origins) / sizeof(origins[0]))];
            r.expiry = expiries[rng() % (sizeof(expiries) / sizeof(expiries[0]))];
            r.hasExpiry = (rng() % 4) != 0;
            r.lastAccessed = static_cast<std::int64_t>(rng() % 5);
            rows.push_back(r);
        }
        sel::SelectionOptions o;
        o.nowUtcSeconds = (rng() % 3) ? kNow : 0;
        o.allowGoogleFallback = (rng() % 2) != 0;
        if (rng() % 3 == 0) { o.authUser = (rng() % 2) ? "1" : "abc"; }
        if (rng() % 3 == 0) { o.pageId = (rng() % 2) ? "123456789" : "9x"; }
        const sel::SelectionResult r = sel::CookieJarSelection::compose(rows, static_cast<int>(rows.size()), o);
        if (r.status != sel::SelectionResult::Status::Ok) { CHECK(r.text.empty()); CHECK(r.names.empty()); continue; }
        const acc::infrastructure::ImportResult p = acc::infrastructure::SessionImportParser::parse(r.text);
        // the gate is open; the only thing the parser may still refuse is a jar without any identity cookie (the sink says so)
        CHECK(p.ok() || p.error == acc::ports::ImportError::NoSessionCookie);
        if (!p.ok()) continue;
        CHECK_EQ(static_cast<std::size_t>(p.summary.cookiesKept), r.names.size());
        CHECK(p.bundle.hasRole(acc::domain::CookieRole::Signing));
        CHECK(sel::CookieJarSelection::isGateCookie(r.gateName));
        // the names are unique, allowed, and in the table's order
        std::size_t last = 0; bool first = true;
        std::set<std::string> uniq;
        for (std::size_t i = 0; i < r.names.size(); ++i) {
            CHECK(uniq.insert(r.names[i]).second);
            CHECK(acc::domain::isNeededCookie(r.names[i]));
            std::size_t idx = 0;
            while (idx < sel::CookieJarSelection::orderSize() && r.names[i] != sel::CookieJarSelection::orderAt(idx)) ++idx;
            CHECK(idx < sel::CookieJarSelection::orderSize());
            if (!first) CHECK(idx > last);
            last = idx; first = false;
        }
        CHECK(r.text.size() <= static_cast<std::size_t>(src::domain::Limits::kMaxComposedBytes) + 128);
        if (!o.allowGoogleFallback) CHECK_EQ(r.fromGoogleHost, 0);
    }
}

// ---------------------------------------------------------------------------------------------
// enumeration
// ---------------------------------------------------------------------------------------------
typedef ScriptedProbe::Result PR;
typedef std::vector<src::ports::LoginEntry> Entries;

static src::domain::EnumerationResult walk(ScriptedProbe &probe, int *doneCalls = nullptr) {
    src::domain::AccountEnumerator e(probe);
    src::domain::EnumerationResult out; int calls = 0;
    const bool started = e.start([&](const src::domain::EnumerationResult &r) { out = r; ++calls; });
    CHECK(started);
    if (probe.synchronous) { CHECK_EQ(calls, 1); CHECK(!e.busy()); }
    if (doneCalls) *doneCalls = calls;
    return out;
}

static void testEnumerationPolicy() {
    using namespace src::domain;
    {   // three logins, then the first logged-out slot ends the walk
        ScriptedProbe p;
        p.script[0] = okAnswer(Entries(1, entry("g0", "d0", "Zero")));
        p.script[1] = okAnswer(Entries(1, entry("g1", "d1", "One")));
        p.script[2] = okAnswer(Entries(1, entry("g2", "d2", "Two")));
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::Ok);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(3));
        CHECK_EQ(r.probes, 4);
        CHECK(r.stop == StopReason::LoggedOut);
        CHECK(!r.incomplete);
        CHECK(p.asked == std::vector<int>({0, 1, 2, 3}));
        for (int i = 0; i < 3; ++i) { CHECK_EQ(r.choices[i].authUser, i); CHECK(r.choices[i].primary); CHECK(r.choices[i].pageId.empty()); }
    }
    {   // brand channels: same login, page ids; then a slot that repeats the last login answers only with duplicates
        ScriptedProbe p;
        Entries zero; zero.push_back(entry("g0", "d0", "Main")); zero.push_back(entry("g0", "d0b", "Brand A", "111")); zero.push_back(entry("g0", "d0c", "Brand B", "222"));
        Entries one; one.push_back(entry("g1", "d1", "Other")); one.push_back(entry("g1", "d1b", "Brand C", "333"));
        p.script[0] = okAnswer(zero); p.script[1] = okAnswer(one); p.script[2] = okAnswer(one);
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(5));
        CHECK(r.stop == StopReason::OnlyDuplicates);
        CHECK_EQ(r.probes, 3);
        CHECK(r.choices[1].authUser == 0 && r.choices[1].pageId == "111" && !r.choices[1].primary);
        CHECK(r.choices[4].authUser == 1 && r.choices[4].pageId == "333");
        std::set<std::string> keys; for (std::size_t i = 0; i < r.choices.size(); ++i) keys.insert(r.choices[i].key);
        CHECK_EQ(keys.size(), r.choices.size());
    }
    {   // Google keeps answering with the last valid login for every later slot
        ScriptedProbe p; p.synchronous = true;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        p.fallback = okAnswer(Entries(1, entry("g0", "", "Zero"))); p.useFallback = true;
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(1));
        CHECK(r.stop == StopReason::OnlyDuplicates);
        CHECK_EQ(r.probes, 2);
    }
    {   // eight distinct logins: slot 7 is the last that can be selected, there is no ninth request
        ScriptedProbe p;
        for (int i = 0; i < 12; ++i) { const std::string g = "g" + std::string(1, char('a' + i)); p.script[i] = okAnswer(Entries(1, entry(g.c_str(), "", "N"))); }
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(8));
        CHECK_EQ(r.probes, 8);
        CHECK(r.stop == StopReason::SlotLimit);
        CHECK(p.asked.size() == 8 && p.asked.back() == 7);
        CHECK(!r.incomplete);
    }
    {   // a network failure on probe 0 is "unreachable", never "signed out"
        ScriptedProbe p; p.script[0] = statusAnswer(PR::Status::Network);
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::Unreachable);
        CHECK(r.choices.empty());
        CHECK_EQ(r.probes, 1);
        CHECK(r.stop == StopReason::Network);
    }
    {   // logged out on probe 0
        ScriptedProbe p; p.script[0] = statusAnswer(PR::Status::LoggedOut);
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::SignedOut);
        CHECK_EQ(r.probes, 1);
    }
    {   // unexpected answer on probe 0
        ScriptedProbe p; p.script[0] = statusAnswer(PR::Status::Failed);
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::Failed);
    }
    {   // a network failure after good probes keeps what was found, flagged incomplete
        ScriptedProbe p;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero"))); p.script[1] = okAnswer(Entries(1, entry("g1", "", "One")));
        p.script[2] = statusAnswer(PR::Status::Network);
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::Ok);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(2));
        CHECK(r.incomplete);
        CHECK(r.stop == StopReason::Network);
        ScriptedProbe q;
        q.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero"))); q.script[1] = statusAnswer(PR::Status::Failed);
        const EnumerationResult s = walk(q);
        CHECK(s.status == EnumerationStatus::Ok && s.incomplete && s.choices.size() == 1);
    }
    {   // an answer with no usable item
        ScriptedProbe p; p.script[0] = okAnswer(Entries());
        const EnumerationResult r = walk(p);
        CHECK(r.status == EnumerationStatus::NoAccounts);
        CHECK(r.stop == StopReason::NoItems);
        ScriptedProbe q; q.script[0] = okAnswer(Entries(1, entry("g", "", "n", "not-digits")));     // a page id the account layer would refuse
        const EnumerationResult s = walk(q);
        CHECK(s.status == EnumerationStatus::NoAccounts);
        CHECK_EQ(s.droppedEntries, 1);
        ScriptedProbe t; t.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero"))); t.script[1] = okAnswer(Entries());
        const EnumerationResult u = walk(t);
        CHECK(u.status == EnumerationStatus::Ok && u.stop == StopReason::NoItems && u.choices.size() == 1);
    }
}

static void testEnumerationDedupAndBounds() {
    using namespace src::domain;
    typedef AccountEnumerator E;
    // the key: Gaia id, else data-sync id, else name; plus the page id
    CHECK_EQ(E::dedupKey(entry("g", "d", "n")), E::dedupKey(entry("g", "other", "other")));
    CHECK(E::dedupKey(entry("g", "d", "n")) != E::dedupKey(entry("g", "d", "n", "5")));
    CHECK_EQ(E::dedupKey(entry("", "d", "n")), E::dedupKey(entry("", "d", "x")));
    CHECK(E::dedupKey(entry("", "d", "n")) != E::dedupKey(entry("", "e", "n")));
    CHECK_EQ(E::dedupKey(entry("", "", "same")), E::dedupKey(entry("", "", "same")));
    CHECK(E::dedupKey(entry("", "", "a")) != E::dedupKey(entry("", "", "b")));
    CHECK(E::usablePageId("1") && E::usablePageId(std::string(32, '7')) && !E::usablePageId("") && !E::usablePageId(std::string(33, '7')) && !E::usablePageId("1a"));
    {   // more entries than one probe may carry
        Entries many; for (int i = 0; i < 40; ++i) { const std::string g = "g" + std::to_string(i); many.push_back(entry(g.c_str(), "", "n")); }
        ScriptedProbe p; p.script[0] = okAnswer(many);
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(EnumerationLimits::kEntriesPerProbe));
        CHECK_EQ(r.droppedEntries, 8);
        CHECK(r.incomplete);
    }
    {   // the overall cap
        ScriptedProbe p;
        for (int slot = 0; slot < 4; ++slot) {
            Entries es; for (int i = 0; i < 32; ++i) { const std::string g = "g" + std::to_string(slot) + "_" + std::to_string(i); es.push_back(entry(g.c_str(), "", "n")); }
            p.script[slot] = okAnswer(es);
        }
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(EnumerationLimits::kMaxChoices));
        CHECK(r.stop == StopReason::Cap && r.incomplete);
        CHECK_EQ(r.probes, 3);
    }
    {   // strings: control characters, over-long names on a UTF-8 boundary, bad ids
        std::string longName; for (int i = 0; i < 100; ++i) longName += "\xc3\xa9\xc3\xa9";       // 400 bytes of 2-byte characters
        ScriptedProbe p;
        src::ports::LoginEntry e = entry("g 1", "d\n2", "Na\tme\x01 with\ncontrols"); // ids with blanks/controls are not ids
        Entries es; es.push_back(e); es.push_back(entry("g2", "", longName.c_str())); es.push_back(entry("g3", "", "  padded  "));
        p.script[0] = okAnswer(es);
        const EnumerationResult r = walk(p);
        CHECK_EQ(r.choices.size(), static_cast<std::size_t>(3));
        CHECK(r.choices[0].label.find('\t') == std::string::npos && r.choices[0].label.find('\n') == std::string::npos && r.choices[0].label.find('\x01') == std::string::npos);
        CHECK(r.choices[0].key.compare(0, 2, "n:") == 0);                       // both ids were unusable, so the key fell back to the name
        CHECK(r.choices[1].label.size() <= static_cast<std::size_t>(EnumerationLimits::kMaxNameBytes));
        bool valid = true;     // never half a character
        for (std::size_t i = 0; i < r.choices[1].label.size();) {
            const unsigned char c = static_cast<unsigned char>(r.choices[1].label[i]);
            const std::size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
            if (i + len > r.choices[1].label.size()) { valid = false; break; }
            i += len;
        }
        CHECK(valid);
        CHECK_EQ(r.choices[2].label, std::string("padded"));
    }
}

static void testEnumerationAsyncAndLifetime() {
    using namespace src::domain;
    {   // asynchronous port: busy, a second start is refused, one completion
        ScriptedProbe p; p.synchronous = false;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero"))); p.script[1] = okAnswer(Entries(1, entry("g1", "", "One")));
        AccountEnumerator e(p);
        int calls = 0; EnumerationResult out;
        CHECK(e.start([&](const EnumerationResult &r) { ++calls; out = r; }));
        CHECK(e.busy());
        CHECK(!e.start([&](const EnumerationResult &) { ++calls; }));
        CHECK_EQ(p.asked.size(), static_cast<std::size_t>(1));                  // one request in flight
        CHECK(p.answerOne());
        CHECK_EQ(p.asked.size(), static_cast<std::size_t>(2));
        CHECK(p.answerOne());                                                   // slot 1 ok -> slot 2 asked
        CHECK(p.answerOne());                                                   // slot 2: logged out
        CHECK_EQ(calls, 1); CHECK(!e.busy());
        CHECK_EQ(out.choices.size(), static_cast<std::size_t>(2));
        // reusable
        CHECK(e.start([&](const EnumerationResult &) { ++calls; }));
        CHECK(e.busy());
    }
    {   // cancel: done never runs, the port is told, a late answer is ignored even after a new walk started
        ScriptedProbe p; p.synchronous = false;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        AccountEnumerator e(p);
        int calls = 0;
        CHECK(e.start([&](const EnumerationResult &) { ++calls; }));
        ScriptedProbe::Done stale = p.pending.front().second;
        e.cancel();
        CHECK(!e.busy()); CHECK_EQ(p.cancels, 1);
        e.cancel();                                                             // idempotent
        CHECK_EQ(p.cancels, 1);
        stale(p.answerFor(0));                                                  // the cancelled probe answers late
        CHECK_EQ(calls, 0);
        int calls2 = 0; EnumerationResult out2;
        CHECK(e.start([&](const EnumerationResult &r) { ++calls2; out2 = r; }));
        stale(p.answerFor(0));                                                  // stale answer while a new walk waits: ignored
        CHECK_EQ(calls2, 0); CHECK_EQ(p.asked.size(), static_cast<std::size_t>(2));
        CHECK(p.pending.size() == 2);
        p.pending.pop_front();                                                  // the old probe's slot in the fake
        CHECK(p.answerOne());
        CHECK_EQ(calls, 0);
    }
    {   // destruction mid-walk
        ScriptedProbe p; p.synchronous = false;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        int calls = 0;
        ScriptedProbe::Done late;
        {
            AccountEnumerator e(p);
            CHECK(e.start([&](const EnumerationResult &) { ++calls; }));
            late = p.pending.front().second;
        }
        CHECK_EQ(p.cancels, 1);
        late(p.answerFor(0));
        CHECK_EQ(calls, 0);
    }
    {   // done starts another walk (synchronous port): works, nothing left running afterwards
        ScriptedProbe p;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        AccountEnumerator e(p);
        int calls = 0;
        std::function<void(const EnumerationResult &)> again = [&](const EnumerationResult &) { if (++calls < 5) e.start(again); };
        CHECK(e.start(again));
        CHECK_EQ(calls, 5); CHECK(!e.busy());
    }
    {   // done destroys the enumerator
        ScriptedProbe p;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        AccountEnumerator *e = new AccountEnumerator(p);
        int calls = 0;
        CHECK(e->start([&](const EnumerationResult &) { ++calls; delete e; }));
        CHECK_EQ(calls, 1);
    }
    {   // a port that answers twice
        ScriptedProbe p; p.synchronous = false;
        p.script[0] = okAnswer(Entries(1, entry("g0", "", "Zero")));
        AccountEnumerator e(p);
        int calls = 0;
        CHECK(e.start([&](const EnumerationResult &) { ++calls; }));
        ScriptedProbe::Done first = p.pending.front().second;
        first(p.answerFor(0));                                                  // slot 1 is asked now
        first(p.answerFor(0));                                                  // duplicate completion of slot 0: ignored
        CHECK_EQ(p.asked.size(), static_cast<std::size_t>(2));
        CHECK_EQ(calls, 0);
    }
}

static void testEnumerationFuzz() {
    using namespace src::domain;
    std::mt19937 rng(99);
    for (int iter = 0; iter < 5000; ++iter) {
        ScriptedProbe p;
        p.synchronous = (rng() % 2) != 0;
        const int slots = static_cast<int>(rng() % 12);
        for (int s = 0; s < slots; ++s) {
            PR r;
            switch (rng() % 6) {
            case 0: r.status = PR::Status::LoggedOut; break;
            case 1: r.status = PR::Status::Network; break;
            case 2: r.status = PR::Status::Failed; break;
            default: {
                r.status = PR::Status::Ok;
                const int count = static_cast<int>(rng() % 5);
                for (int i = 0; i < count; ++i) {
                    const std::string g = (rng() % 3) ? "g" + std::to_string(rng() % 6) : std::string();
                    const std::string page = (rng() % 3 == 0) ? std::to_string(rng() % 4) : ((rng() % 7 == 0) ? "x" : "");
                    r.entries.push_back(entry(g.c_str(), (rng() % 2) ? "d" : "", (rng() % 2) ? "n" : "", page.c_str()));
                }
            }
            }
            p.script[s] = r;
        }
        AccountEnumerator e(p);
        EnumerationResult out; int calls = 0;
        e.start([&](const EnumerationResult &r) { out = r; ++calls; });
        while (e.busy()) { if (!p.answerOne()) break; }
        CHECK_EQ(calls, 1);
        CHECK(out.probes >= 1 && out.probes <= EnumerationLimits::kSlots);
        CHECK_EQ(p.asked.size(), static_cast<std::size_t>(out.probes));
        for (std::size_t i = 0; i < p.asked.size(); ++i) CHECK_EQ(p.asked[i], static_cast<int>(i));
        CHECK(out.choices.size() <= static_cast<std::size_t>(EnumerationLimits::kMaxChoices));
        std::set<std::string> keys;
        for (std::size_t i = 0; i < out.choices.size(); ++i) {
            CHECK(keys.insert(out.choices[i].key).second);
            CHECK(out.choices[i].authUser >= 0 && out.choices[i].authUser < EnumerationLimits::kSlots);
            CHECK(out.choices[i].pageId.empty() || AccountEnumerator::usablePageId(out.choices[i].pageId));
            CHECK_EQ(out.choices[i].primary, out.choices[i].pageId.empty());
        }
        if (!out.choices.empty()) CHECK(out.status == EnumerationStatus::Ok);
        if (out.status == EnumerationStatus::Unreachable) CHECK(out.probes == 1 && out.choices.empty());
        if (out.status == EnumerationStatus::SignedOut) CHECK(out.probes == 1 && out.choices.empty());
    }
}

// ---------------------------------------------------------------------------------------------
// the source over fakes
// ---------------------------------------------------------------------------------------------
typedef src::application::BrowserCookieJarSource Source;

static Source::Config baseConfig(FakeReader &reader, acc::ports::WorkerPort *worker) {
    Source::Config cfg;
    cfg.reader = &reader; cfg.worker = worker;
    cfg.consentGranted = [] { return true; };
    cfg.nowUtcSeconds = [] { return kNow; };
    return cfg;
}

static std::string goodLine() {
    return compose(goodRows()).text;
}

static void testSourceHappyPath() {
    FakeReader reader; reader.next = okJar(goodRows(), 1);
    acc::ports::InlineWorker worker;
    Source source(baseConfig(reader, &worker));
    CHECK(source.kind() == acc::domain::SessionSourceKind::SystemBrowser);
    CHECK_EQ(std::string(acc::domain::sourceKindName(source.kind())), std::string("browser"));
    CHECK(source.status().available);
    CHECK(source.status().reason.empty());
    ParserSink sink; int calls = 0; acc::ports::SessionStageResult got;
    const acc::domain::Result r = source.start(sink, [&](const acc::ports::SessionStageResult &s) { ++calls; got = s; });
    CHECK(r.ok());
    CHECK_EQ(calls, 1);                              // inline worker: done ran before start() returned
    CHECK(got.ok());
    CHECK_EQ(sink.stageCalls, 1);
    CHECK(sink.lastKind == acc::domain::SessionSourceKind::SystemBrowser);
    CHECK_EQ(sink.lastText, goodLine());
    CHECK(sink.last.ok() && sink.last.bundle.usable());
    CHECK(got.summary.hasSigningCookie && got.summary.hasSessionCookie);
    CHECK_EQ(got.summary.cookiesKept, 17);
    CHECK(source.outcome() == src::domain::Outcome::Staged);
    const Source::Report rep = source.report();
    CHECK_EQ(rep.cookiesUsed, 17); CHECK_EQ(rep.candidate, 1); CHECK(!rep.selectionApplied); CHECK_EQ(rep.names.size(), static_cast<std::size_t>(17));
    CHECK(!source.busy());
    CHECK_EQ(reader.reads, 1);
    CHECK(sink.hasStaged());
}

static void testSourceConsentAndAvailability() {
    acc::ports::InlineWorker worker;
    {   // no consent: nothing is probed, read or staged
        FakeReader reader; reader.next = okJar(goodRows());
        Source::Config cfg = baseConfig(reader, &worker); cfg.consentGranted = [] { return false; };
        Source source(cfg); ParserSink sink; int calls = 0;
        const acc::domain::Result r = source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; });
        CHECK(r.code == acc::domain::ErrorCode::PermissionDenied);
        CHECK_EQ(calls, 0); CHECK_EQ(reader.reads, 0); CHECK_EQ(reader.probes, 0); CHECK_EQ(sink.stageCalls, 0);
        CHECK(source.outcome() == src::domain::Outcome::ConsentMissing);
        CHECK(!source.busy());
    }
    {   // no consent callback at all: fails closed
        FakeReader reader; reader.next = okJar(goodRows());
        Source::Config cfg = baseConfig(reader, &worker); cfg.consentGranted = std::function<bool()>();
        Source source(cfg); ParserSink sink;
        CHECK(source.start(sink, Source::Completion()).code == acc::domain::ErrorCode::PermissionDenied);
        CHECK_EQ(reader.reads, 0); CHECK_EQ(reader.probes, 0);
    }
    {   // consent withdrawn between two attempts
        FakeReader reader; reader.next = okJar(goodRows());
        bool consent = true;
        Source::Config cfg = baseConfig(reader, &worker); cfg.consentGranted = [&] { return consent; };
        Source source(cfg); ParserSink sink;
        CHECK(source.start(sink, Source::Completion()).ok());
        consent = false;
        CHECK(source.start(sink, Source::Completion()).code == acc::domain::ErrorCode::PermissionDenied);
        CHECK_EQ(reader.reads, 1);
    }
    {   // every availability maps to a precise reason and a synchronous refusal
        struct Case { src::domain::Availability a; acc::domain::ErrorCode code; src::domain::Outcome outcome; };
        const Case cases[] = {
            {src::domain::Availability::NotFound, acc::domain::ErrorCode::Unsupported, src::domain::Outcome::NotFound},
            {src::domain::Availability::NotReadable, acc::domain::ErrorCode::PermissionDenied, src::domain::Outcome::NotReadable},
            {src::domain::Availability::Unsafe, acc::domain::ErrorCode::PermissionDenied, src::domain::Outcome::Unsafe},
            {src::domain::Availability::NoHome, acc::domain::ErrorCode::Unsupported, src::domain::Outcome::NotFound}};
        std::set<std::string> reasons;
        for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            FakeReader reader; reader.availability = cases[i].a; reader.next = okJar(goodRows());
            Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0;
            const Source::Status st = source.status();
            CHECK(!st.available);
            CHECK(!st.reason.empty());
            reasons.insert(st.reason);
            CHECK_EQ(st.reason, std::string(src::domain::reasonFor(cases[i].a)));
            const acc::domain::Result r = source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; });
            CHECK(r.code == cases[i].code);
            CHECK_EQ(r.message, st.reason);
            CHECK_EQ(calls, 0); CHECK_EQ(reader.reads, 0); CHECK_EQ(sink.stageCalls, 0);
            CHECK(source.outcome() == cases[i].outcome);
            CHECK(!containsSentinel(st.reason));
        }
        CHECK_EQ(reasons.size(), static_cast<std::size_t>(4));
        CHECK(std::string(src::domain::reasonFor(src::domain::Availability::NotFound)).find("unsandboxed build (OpenRepos profile)") != std::string::npos);
        CHECK(std::string(src::domain::reasonFor(src::domain::Availability::NotReadable)).find("unsandboxed build (OpenRepos profile)") != std::string::npos);
        CHECK(std::string(src::domain::reasonFor(src::domain::Availability::Available)).empty());
    }
    {   // no reader configured
        Source::Config cfg; cfg.consentGranted = [] { return true; };
        Source source(cfg); ParserSink sink;
        CHECK(!source.status().available);
        CHECK(source.start(sink, Source::Completion()).code == acc::domain::ErrorCode::Unsupported);
    }
}

static void testSourceOutcomes() {
    acc::ports::InlineWorker worker;
    struct Case { src::domain::ReadStatus status; src::domain::Outcome outcome; acc::domain::ErrorCode code; };
    const Case cases[] = {
        {src::domain::ReadStatus::NotFound, src::domain::Outcome::NotFound, acc::domain::ErrorCode::Unsupported},
        {src::domain::ReadStatus::NotReadable, src::domain::Outcome::NotReadable, acc::domain::ErrorCode::PermissionDenied},
        {src::domain::ReadStatus::Unsafe, src::domain::Outcome::Unsafe, acc::domain::ErrorCode::PermissionDenied},
        {src::domain::ReadStatus::TooLarge, src::domain::Outcome::TooLarge, acc::domain::ErrorCode::InvalidResponse},
        {src::domain::ReadStatus::ChangedWhileReading, src::domain::Outcome::ChangedWhileReading, acc::domain::ErrorCode::Busy},
        {src::domain::ReadStatus::CopyFailed, src::domain::Outcome::CopyFailed, acc::domain::ErrorCode::InvalidResponse},
        {src::domain::ReadStatus::NotADatabase, src::domain::Outcome::NotADatabase, acc::domain::ErrorCode::InvalidResponse},
        {src::domain::ReadStatus::UnexpectedSchema, src::domain::Outcome::UnexpectedSchema, acc::domain::ErrorCode::InvalidResponse},
        {src::domain::ReadStatus::TooManyRows, src::domain::Outcome::TooManyRows, acc::domain::ErrorCode::InvalidResponse},
        {src::domain::ReadStatus::NoCookies, src::domain::Outcome::NoCookies, acc::domain::ErrorCode::AuthorizationRequired}};
    for (std::size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        FakeReader reader; reader.next.status = cases[i].status; reader.next.rows = goodRows();   // rows must be ignored when the status is not Ok
        Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0; acc::ports::SessionStageResult got;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &s) { ++calls; got = s; }).ok());
        CHECK_EQ(calls, 1);
        CHECK(!got.ok());
        CHECK(got.result.code == cases[i].code);
        CHECK(source.outcome() == cases[i].outcome);
        CHECK_EQ(sink.stageCalls, 0);
        CHECK(!containsSentinel(got.result.message));
        CHECK(!source.busy());
    }
    {   // a reader that says Ok but found nothing at all (no matching row of any name): "no cookies", not "not signed in"
        FakeReader reader; reader.next.status = src::domain::ReadStatus::Ok; reader.next.matchingRows = 0;
        Source source(baseConfig(reader, &worker)); ParserSink sink; acc::ports::SessionStageResult got;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &s) { got = s; }).ok());
        CHECK(source.outcome() == src::domain::Outcome::NoCookies);
        CHECK(got.error == acc::ports::ImportError::NoCookies && got.result.code == acc::domain::ErrorCode::AuthorizationRequired);
        CHECK_EQ(sink.stageCalls, 0);
    }
    {   // cookies but not signed in -> the existing taxonomy, nothing staged
        Rows rows; rows.push_back(row(".youtube.com", "SOCS", kVSOCS));
        FakeReader reader; reader.next = okJar(rows);
        Source source(baseConfig(reader, &worker)); ParserSink sink; acc::ports::SessionStageResult got;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &s) { got = s; }).ok());
        CHECK(source.outcome() == src::domain::Outcome::NotSignedIn);
        CHECK(got.result.code == acc::domain::ErrorCode::AuthorizationRequired && got.error == acc::ports::ImportError::NoSigningCookie);
        CHECK_EQ(sink.stageCalls, 0);
    }
    {   // the sink refuses (no identity cookie): Rejected, with the sink's reason, and no value anywhere
        Rows rows; rows.push_back(row(".youtube.com", "__Secure-3PAPISID", kV3PAPISID));
        FakeReader reader; reader.next = okJar(rows);
        Source source(baseConfig(reader, &worker)); ParserSink sink; acc::ports::SessionStageResult got;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &s) { got = s; }).ok());
        CHECK(source.outcome() == src::domain::Outcome::Rejected);
        CHECK(!got.ok());
        CHECK(got.error == acc::ports::ImportError::NoSessionCookie);
        CHECK_EQ(sink.stageCalls, 1);
        CHECK(!containsSentinel(got.result.message));
        CHECK(!sink.hasStaged());
    }
    {   // the sink says consent is missing (it checks too)
        FakeReader reader; reader.next = okJar(goodRows());
        Source source(baseConfig(reader, &worker)); ParserSink sink; sink.consent = false; acc::ports::SessionStageResult got;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &s) { got = s; }).ok());
        CHECK(source.outcome() == src::domain::Outcome::Rejected && got.error == acc::ports::ImportError::ConsentRequired);
    }
    {   // account selection reaches the staged text
        FakeReader reader; reader.next = okJar(goodRows());
        Source source(baseConfig(reader, &worker)); ParserSink sink;
        CHECK(!source.setAccountSelection("abc", ""));
        CHECK(!source.setAccountSelection("1", "12x"));
        CHECK(!source.setAccountSelection("123", ""));
        CHECK(source.setAccountSelection("3", "4455667788"));
        CHECK(source.start(sink, Source::Completion()).ok());
        CHECK(sink.last.ok());
        CHECK_EQ(sink.last.bundle.authUser, std::string("3"));
        CHECK_EQ(sink.last.bundle.pageId, std::string("4455667788"));
        CHECK(source.report().selectionApplied);
        source.clearAccountSelection();
        CHECK(source.start(sink, Source::Completion()).ok());
        CHECK(sink.last.bundle.authUser.empty() && sink.last.bundle.pageId.empty());
        CHECK(!source.report().selectionApplied);
    }
    {   // the clock decides expiry: the same jar is "not signed in" a century later
        FakeReader reader; reader.next = okJar(goodRows());
        Source::Config cfg = baseConfig(reader, &worker); cfg.nowUtcSeconds = [] { return static_cast<std::int64_t>(4200000000LL); };
        Source source(cfg); ParserSink sink;
        CHECK(source.start(sink, Source::Completion()).ok());
        CHECK(source.outcome() == src::domain::Outcome::NotSignedIn);
    }
    {   // google.com fallback switch
        Rows rows = goodRows(); rows.erase(rows.begin() + 1);                         // drop SAPISID (youtube)
        rows.erase(rows.begin());                                                    // drop 3PAPISID
        rows.push_back(row(".google.com", "SAPISID", kVSAPISIDGoogle));
        FakeReader reader; reader.next = okJar(rows);
        Source::Config cfg = baseConfig(reader, &worker);
        { Source source(cfg); ParserSink sink; source.start(sink, Source::Completion()); CHECK(source.outcome() == src::domain::Outcome::Staged); CHECK_EQ(source.report().fromGoogleHost, 1); }
        cfg.allowGoogleFallback = false;
        { Source source(cfg); ParserSink sink; source.start(sink, Source::Completion()); CHECK(source.outcome() == src::domain::Outcome::NotSignedIn); }
    }
}

static void testSourceLifecycle() {
    {   // busy, then completion, then reusable
        FakeReader reader; reader.next = okJar(goodRows());
        ManualWorker worker;
        Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        CHECK(source.busy());
        CHECK_EQ(calls, 0); CHECK_EQ(reader.reads, 0);                              // nothing ran yet
        CHECK(source.start(sink, Source::Completion()).code == acc::domain::ErrorCode::Busy);
        worker.runAll();
        CHECK_EQ(calls, 1); CHECK(!source.busy()); CHECK_EQ(reader.reads, 1);
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        worker.runAll();
        CHECK_EQ(calls, 2);
    }
    {   // cancel before the work runs: nothing is read as far as results go, nothing staged, done never called
        FakeReader reader; reader.next = okJar(goodRows());
        ManualWorker worker;
        Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        source.cancel();
        CHECK(!source.busy());
        CHECK(source.outcome() == src::domain::Outcome::Cancelled);
        worker.runAll();                                                            // the job still runs; its result is thrown away
        CHECK_EQ(calls, 0); CHECK_EQ(sink.stageCalls, 0);
        CHECK(source.outcome() == src::domain::Outcome::Cancelled);                 // not overwritten by the stale completion
        source.cancel();                                                            // idempotent
        // a new run is not confused by the stale one
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        worker.runAll();
        CHECK_EQ(calls, 1); CHECK_EQ(sink.stageCalls, 1);
    }
    {   // cancel while the work ran but the completion is pending
        FakeReader reader; reader.next = okJar(goodRows());
        ManualWorker worker;
        Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0;
        CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        std::function<void()> fin = worker.runWork();
        source.cancel();
        fin();
        CHECK_EQ(calls, 0); CHECK_EQ(sink.stageCalls, 0);
    }
    {   // destruction mid-read (the reader outlives it, as the composition guarantees)
        FakeReader reader; reader.next = okJar(goodRows());
        ManualWorker worker; ParserSink sink; int calls = 0;
        {
            Source source(baseConfig(reader, &worker));
            CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
        }
        worker.runAll();
        CHECK_EQ(calls, 0); CHECK_EQ(sink.stageCalls, 0);
        std::function<void()> fin;
        {
            Source source(baseConfig(reader, &worker));
            CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; }).ok());
            fin = worker.runWork();
        }
        fin();
        CHECK_EQ(calls, 0); CHECK_EQ(sink.stageCalls, 0);
    }
    {   // done starts another import (inline worker)
        FakeReader reader; reader.next = okJar(goodRows());
        acc::ports::InlineWorker worker;
        Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0;
        Source::Completion again = [&](const acc::ports::SessionStageResult &) { if (++calls < 3) source.start(sink, again); };
        CHECK(source.start(sink, again).ok());
        CHECK_EQ(calls, 3); CHECK(!source.busy());
    }
    {   // a missing completion callback is fine
        FakeReader reader; reader.next = okJar(goodRows());
        acc::ports::InlineWorker worker;
        Source source(baseConfig(reader, &worker)); ParserSink sink;
        CHECK(source.start(sink, Source::Completion()).ok());
        CHECK_EQ(sink.stageCalls, 1);
    }
}

static void testSourceOnAWorkerThread() {
    FakeReader reader; reader.next = okJar(goodRows());
    ThreadWorker worker;
    Source source(baseConfig(reader, &worker)); ParserSink sink; int calls = 0; std::thread::id doneThread;
    CHECK(source.start(sink, [&](const acc::ports::SessionStageResult &) { ++calls; doneThread = std::this_thread::get_id(); }).ok());
    CHECK(source.busy());
    CHECK(worker.pumpOne());
    CHECK_EQ(calls, 1);
    CHECK(reader.readThread == worker.workerId());              // the blocking read ran on the worker
    CHECK(reader.readThread != std::this_thread::get_id());
    CHECK(doneThread == std::this_thread::get_id());            // the staging and the callback ran on the caller's thread
    CHECK(source.outcome() == src::domain::Outcome::Staged);
}

static void testNoValueInAnyMessage() {
    using src::domain::Outcome;
    const Outcome all[] = {Outcome::None, Outcome::Staged, Outcome::ConsentMissing, Outcome::NotFound, Outcome::NotReadable, Outcome::Unsafe, Outcome::TooLarge,
        Outcome::ChangedWhileReading, Outcome::CopyFailed, Outcome::NotADatabase, Outcome::UnexpectedSchema, Outcome::TooManyRows, Outcome::NoCookies,
        Outcome::NotSignedIn, Outcome::Rejected, Outcome::Cancelled};
    for (std::size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
        const std::string text = src::domain::describe(all[i]);
        CHECK(!containsSentinel(text));
        CHECK(text.find('=') == std::string::npos);                             // nothing shaped like name=value
        if (all[i] != Outcome::None) CHECK(!text.empty());
    }
    // every observable string of a full run (success and each failure) is free of values
    acc::ports::InlineWorker worker;
    std::vector<Rows> jars;
    jars.push_back(goodRows());
    { Rows r; r.push_back(row(".youtube.com", "__Secure-3PAPISID", kV3PAPISID)); jars.push_back(r); }                  // sink refuses
    { Rows r; r.push_back(row(".youtube.com", "SOCS", kVSOCS)); jars.push_back(r); }                                    // not signed in
    for (std::size_t i = 0; i < jars.size(); ++i) {
        FakeReader reader; reader.next = okJar(jars[i]);
        Source source(baseConfig(reader, &worker)); ParserSink sink; acc::ports::SessionStageResult got;
        source.start(sink, [&](const acc::ports::SessionStageResult &s) { got = s; });
        std::string seen = got.result.message + got.summary.fingerprint + got.summary.authUser + source.status().reason;
        for (std::size_t k = 0; k < got.summary.keptNames.size(); ++k) seen += got.summary.keptNames[k];
        const Source::Report rep = source.report();
        for (std::size_t k = 0; k < rep.names.size(); ++k) seen += rep.names[k];
        CHECK(!containsSentinel(seen));
        CHECK(!containsSentinel(sink.lastResult.result.message));
        // the fingerprint is the parser's 8-hex tag, not a prefix of the value
        CHECK(got.summary.fingerprint.size() == 0 || got.summary.fingerprint.size() == 8);
    }
}

// ---------------------------------------------------------------------------------------------
// the POSIX scratch helpers
// ---------------------------------------------------------------------------------------------
static std::string g_base;

static std::string slurp(const std::string &path) {
    std::string out; int fd = ::open(path.c_str(), O_RDONLY); if (fd < 0) return out;
    char buf[4096]; ssize_t n; while ((n = ::read(fd, buf, sizeof(buf))) > 0) out.append(buf, static_cast<std::size_t>(n));
    ::close(fd); return out;
}
static bool put(const std::string &path, const std::string &data, mode_t mode = 0644) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode); if (fd < 0) return false;
    const bool ok = ::write(fd, data.data(), data.size()) == static_cast<ssize_t>(data.size());
    ::fchmod(fd, mode);
    ::close(fd); return ok;
}
static int modeOf(const std::string &path) { struct stat st; return ::lstat(path.c_str(), &st) == 0 ? static_cast<int>(st.st_mode & 0777) : -1; }
static bool exists(const std::string &path) { struct stat st; return ::lstat(path.c_str(), &st) == 0; }
static void rmrf(const std::string &path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        ::chmod(path.c_str(), 0700);
        if (DIR *d = ::opendir(path.c_str())) {
            std::vector<std::string> names;
            while (struct dirent *e = ::readdir(d)) { if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, "..")) names.push_back(e->d_name); }
            ::closedir(d);
            for (std::size_t i = 0; i < names.size(); ++i) rmrf(path + "/" + names[i]);
        }
        ::rmdir(path.c_str());
    } else ::unlink(path.c_str());
}

static void testInspect() {
    const std::string dir = g_base + "/inspect"; ::mkdir(dir.c_str(), 0755);
    const std::string file = dir + "/f.db";
    put(file, std::string(1000, 'x'));
    sc::Stamp st;
    CHECK(sc::inspect(file, 4096, &st) == sc::FileState::Ok);
    CHECK(st.exists && st.size == 1000);
    CHECK(sc::inspect(dir + "/nope", 4096, &st) == sc::FileState::Absent);
    CHECK(!st.exists);
    CHECK(sc::inspect(dir + "/nope/deeper", 4096, nullptr) == sc::FileState::Absent);
    CHECK(sc::inspect("", 4096, nullptr) == sc::FileState::Absent);
    CHECK(sc::inspect(file, 999, &st) == sc::FileState::TooLarge);
    CHECK(sc::inspect(file, 1000, nullptr) == sc::FileState::Ok);
    // symlink (to a good file and to nothing), directory, fifo
    ::symlink(file.c_str(), (dir + "/link").c_str());
    ::symlink("/does/not/exist", (dir + "/dangling").c_str());
    CHECK(sc::inspect(dir + "/link", 4096, nullptr) == sc::FileState::Unsafe);
    CHECK(sc::inspect(dir + "/dangling", 4096, nullptr) == sc::FileState::Unsafe);
    CHECK(sc::inspect(dir, 4096, nullptr) == sc::FileState::Unsafe);
    ::mkfifo((dir + "/fifo").c_str(), 0600);
    CHECK(sc::inspect(dir + "/fifo", 4096, nullptr) == sc::FileState::Unsafe);
    // a file in a symlinked DIRECTORY is still an ordinary file of ours (only the leaf is judged)
    ::symlink(dir.c_str(), (g_base + "/dirlink").c_str());
    CHECK(sc::inspect(g_base + "/dirlink/f.db", 4096, nullptr) == sc::FileState::Ok);
    // not readable
    if (::geteuid() != 0) {
        const std::string locked = dir + "/locked.db"; put(locked, "data", 0600); ::chmod(locked.c_str(), 0);
        CHECK(sc::inspect(locked, 4096, nullptr) == sc::FileState::NotReadable);
        ::chmod(locked.c_str(), 0600);
        ::mkdir((dir + "/closed").c_str(), 0700); put(dir + "/closed/x.db", "x"); ::chmod((dir + "/closed").c_str(), 0);
        CHECK(sc::inspect(dir + "/closed/x.db", 4096, nullptr) == sc::FileState::NotReadable);
        ::chmod((dir + "/closed").c_str(), 0700);
    } else NOTE_SKIPPED("running as root: permission-denied cases");
    // a stamp changes when the file does
    sc::Stamp a, b; sc::inspect(file, 4096, &a);
    put(file, std::string(1001, 'y'));
    sc::inspect(file, 4096, &b);
    CHECK(a != b);
    sc::inspect(file, 4096, &a);
    CHECK(a == b);
}

static void testMakeDirectories() {
    const std::string root = g_base + "/mk/a/b/c";
    CHECK(sc::makePrivateDirectory(root));
    CHECK_EQ(modeOf(root), 0700);
    CHECK(sc::makePrivateDirectory(root));                                  // idempotent
    ::chmod(root.c_str(), 0755);
    CHECK(sc::makePrivateDirectory(root));
    CHECK_EQ(modeOf(root), 0700);                                           // tightened
    CHECK(!sc::makePrivateDirectory("relative/path"));
    CHECK(!sc::makePrivateDirectory(""));
    put(g_base + "/mk/plain", "x");
    CHECK(!sc::makePrivateDirectory(g_base + "/mk/plain"));                 // a file
    CHECK(!sc::makePrivateDirectory(g_base + "/mk/plain/sub"));
    ::symlink((g_base + "/mk/a").c_str(), (g_base + "/mk/linkroot").c_str());
    ::chmod((g_base + "/mk/a").c_str(), 0755);
    CHECK(!sc::makePrivateDirectory(g_base + "/mk/linkroot"));              // the private root itself must not be a link
    CHECK_EQ(modeOf(g_base + "/mk/a"), 0755);                               // ... and its target is not touched (no chmod through the link)
    ::chmod((g_base + "/mk/a").c_str(), 0700);
    CHECK(sc::makePrivateDirectory(g_base + "/mk/linkroot/b/c"));           // a link in the middle of the path is followed
    std::string d1, d2;
    CHECK(sc::makeScratchDirectory(root, &d1));
    CHECK(sc::makeScratchDirectory(root, &d2));
    CHECK(d1 != d2);
    CHECK_EQ(modeOf(d1), 0700); CHECK_EQ(modeOf(d2), 0700);
    CHECK(d1.compare(0, root.size() + 8, root + "/import-") == 0);
    CHECK(!sc::makeScratchDirectory(g_base + "/mk/linkroot", &d1) && d1.empty());
    CHECK(!sc::makeScratchDirectory(g_base + "/mk/plain", nullptr));
}

static void testCopyPrivate() {
    const std::string dir = g_base + "/copy"; sc::makePrivateDirectory(dir);
    const std::string from = dir + "/src.db";
    std::string payload; for (int i = 0; i < 300000; ++i) payload += char('a' + (i * 7) % 26);   // spans several chunks
    const mode_t sourceModes[] = {0666, 0644, 0444, 0600};
    const mode_t umasks[] = {0000, 0022, 0077, 0277};      // 0277 strips even the owner's write bit from the open() mode: only fchmod restores 0600
    int n = 0;
    for (std::size_t m = 0; m < sizeof(sourceModes) / sizeof(sourceModes[0]); ++m) {
        for (std::size_t u = 0; u < sizeof(umasks) / sizeof(umasks[0]); ++u) {
            ::chmod(from.c_str(), 0600); put(from, payload, sourceModes[m]);
            const mode_t old = ::umask(umasks[u]);
            const std::string to = dir + "/copy" + std::to_string(++n);
            const sc::FileState s = sc::copyPrivate(from, to, 1u << 20);
            ::umask(old);
            CHECK(s == sc::FileState::Ok);
            CHECK_EQ(modeOf(to), 0600);                                    // neither the source's mode nor the umask matters
            CHECK(slurp(to) == payload);
        }
    }
    const std::string to = dir + "/exists"; put(to, "already");
    CHECK(sc::copyPrivate(from, to, 1u << 20) == sc::FileState::IoError);   // O_EXCL: never overwrites
    CHECK_EQ(slurp(to), std::string("already"));
    const std::string big = dir + "/big";
    CHECK(sc::copyPrivate(from, big, 1000) == sc::FileState::TooLarge);
    CHECK(!exists(big));
    CHECK(sc::copyPrivate(dir + "/missing", dir + "/m", 1000) == sc::FileState::Absent);
    CHECK(!exists(dir + "/m"));
    ::symlink(from.c_str(), (dir + "/lnk").c_str());
    CHECK(sc::copyPrivate(dir + "/lnk", dir + "/viaLink", 1u << 20) == sc::FileState::Unsafe);
    CHECK(!exists(dir + "/viaLink"));
    CHECK(sc::copyPrivate(dir, dir + "/dirCopy", 1u << 20) == sc::FileState::Unsafe);
    // the destination being a symlink is refused (O_EXCL | O_NOFOLLOW) and its target is untouched
    put(dir + "/victim", "keep");
    ::symlink((dir + "/victim").c_str(), (dir + "/trap").c_str());
    CHECK(sc::copyPrivate(from, dir + "/trap", 1u << 20) == sc::FileState::IoError);
    CHECK_EQ(slurp(dir + "/victim"), std::string("keep"));
    // the original is not modified
    CHECK(slurp(from) == payload);
}

static void testZeroAndRemove() {
    const std::string dir = g_base + "/zero"; sc::makePrivateDirectory(dir);
    const std::string f = dir + "/secret.db";
    put(f, std::string(200000, 'S'), 0600);
    ::link(f.c_str(), (dir + "/second-name").c_str());                      // keep the inode reachable to prove it was overwritten
    CHECK(sc::zeroAndRemove(f));
    CHECK(!exists(f));
    const std::string left = slurp(dir + "/second-name");
    CHECK_EQ(left.size(), static_cast<std::size_t>(200000));
    CHECK(left.find_first_not_of('\0') == std::string::npos);               // every byte was zeroed before the unlink
    CHECK(sc::zeroAndRemove(dir + "/never-existed"));
    ::symlink((dir + "/second-name").c_str(), (dir + "/lnk").c_str());
    CHECK(sc::zeroAndRemove(dir + "/lnk"));                                 // the link goes ...
    CHECK(exists(dir + "/second-name"));                                    // ... its target stays (never followed)
    CHECK_EQ(slurp(dir + "/second-name").size(), static_cast<std::size_t>(200000));
}

static void testRemoveTreeAndSweep() {
    const std::string dir = g_base + "/tree"; sc::makePrivateDirectory(dir);
    const std::string outside = g_base + "/outside.txt"; put(outside, "do-not-touch");
    {
        const std::string t = dir + "/import-AAAAAA"; ::mkdir(t.c_str(), 0700);
        put(t + "/a", "1"); put(t + "/a-wal", "2"); ::mkdir((t + "/sub").c_str(), 0700); put(t + "/sub/b", "3");
        ::symlink(outside.c_str(), (t + "/escape").c_str());
        ::symlink(g_base.c_str(), (t + "/dirlink").c_str());
        CHECK(sc::removeTree(t));
        CHECK(!exists(t));
        CHECK_EQ(slurp(outside), std::string("do-not-touch"));             // links were removed, never followed
        CHECK(exists(g_base));
    }
    CHECK(sc::removeTree(dir + "/does-not-exist"));
    {   // the directory itself being a link: only the link goes
        ::symlink(g_base.c_str(), (dir + "/import-LINKED").c_str());
        CHECK(sc::removeTree(dir + "/import-LINKED"));
        CHECK(!exists(dir + "/import-LINKED"));
        CHECK(exists(g_base + "/outside.txt"));
    }
    {   // sweep: only import-* directories; a link named import-* and ordinary names stay
        const std::string r = dir + "/root"; sc::makePrivateDirectory(r);
        for (int i = 0; i < 3; ++i) { const std::string t = r + "/import-OLD" + std::to_string(i); ::mkdir(t.c_str(), 0700); put(t + "/cookies-copy.sqlite", "stale"); }
        ::mkdir((r + "/keep-me").c_str(), 0700); put(r + "/keep-me/f", "k"); put(r + "/import-file", "not a dir");
        ::symlink(g_base.c_str(), (r + "/import-link").c_str());
        CHECK_EQ(sc::sweepStale(r), 3);
        CHECK(exists(r + "/keep-me/f") && exists(r + "/import-file") && exists(r + "/import-link"));
        CHECK(exists(g_base + "/outside.txt"));
        CHECK_EQ(sc::sweepStale(r), 0);
        CHECK_EQ(sc::sweepStale(r + "/nope"), 0);
        // a root that is a link is not followed
        const std::string target = dir + "/linktarget"; sc::makePrivateDirectory(target); ::mkdir((target + "/import-INSIDE").c_str(), 0700);
        ::symlink(target.c_str(), (dir + "/rootlink").c_str());
        CHECK_EQ(sc::sweepStale(dir + "/rootlink"), 0);
        CHECK(exists(target + "/import-INSIDE"));
    }
    {   // the guard
        std::string d; CHECK(sc::makeScratchDirectory(dir + "/root", &d));
        { sc::ScratchGuard guard(d); put(d + "/x", "1"); CHECK(exists(d)); }
        CHECK(!exists(d));
        CHECK(sc::makeScratchDirectory(dir + "/root", &d));
        sc::ScratchGuard g2(d); put(d + "/y", "1"); g2.release(); CHECK(!exists(d));
        g2.release();                                                      // idempotent
    }
}

int main() {
    const char *tmp = std::getenv("TMPDIR");
    std::string pattern = std::string(tmp && *tmp ? tmp : "/tmp") + "/sailtune-sources-pure.XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end()); buf.push_back('\0');
    if (!::mkdtemp(buf.data())) { std::fprintf(stderr, "FAIL: no temp directory\n"); return 1; }
    g_base = buf.data();

    testOrderTable();
    testHostRank();
    testComposeExactText();
    testGateAndStatus();
    testRowFilters();
    testDuplicatePreference();
    testCapAndSelection();
    testWipe();
    testSelectionFuzz();
    testEnumerationPolicy();
    testEnumerationDedupAndBounds();
    testEnumerationAsyncAndLifetime();
    testEnumerationFuzz();
    testSourceHappyPath();
    testSourceConsentAndAvailability();
    testSourceOutcomes();
    testSourceLifecycle();
    testSourceOnAWorkerThread();
    testNoValueInAnyMessage();
    testInspect();
    testMakeDirectories();
    testCopyPrivate();
    testZeroAndRemove();
    testRemoveTreeAndSweep();

    rmrf(g_base);                       // leave nothing behind
    CHECK(!exists(g_base));
    return finish("ytm_sources test_pure");
}
