// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Checks of the account policy (ACC-02, 03, 06, 07, 08, 09). All cookie values
// are obviously fake. Build: tests/account_domain/run.sh
#include "Core/Account/Domain/AccountPolicy.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

using namespace nordtile::video::account;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static std::string qtSha1Hex(const std::string &in) {
    const QByteArray h = QCryptographicHash::hash(QByteArray(in.data(), int(in.size())), QCryptographicHash::Sha1).toHex();
    return std::string(h.constData(), std::size_t(h.size()));
}
static const std::function<std::string(const std::string &)> sha1 = qtSha1Hex;

static Cookie cookie(const std::string &name, const std::string &value, const std::string &domain = ".youtube.com") {
    Cookie c;
    c.name = name;
    c.value = value;
    c.domain = domain;
    return c;
}

static SetCookieValue setCookie(const std::string &name, const std::string &value, bool expired = false) {
    SetCookieValue s;
    s.name = name;
    s.value = value;
    s.domain = ".youtube.com";
    s.expired = expired;
    return s;
}

static std::string header(const RequestHeaders &h, const std::string &name) {
    for (std::size_t i = 0; i < h.headers.size(); ++i)
        if (h.headers[i].first == name) return h.headers[i].second;
    return "<absent>";
}

// ACC-02: rank order per variant; never an OAuth device flow (the enum has none).
static void signInOrder() {
    std::vector<SignInMethod> m = signInMethods(BuildVariant::Full, false);
    CHECK(m.size() == 2);
    CHECK(m.size() == 2 && m[0] == SignInMethod::BrowserImport && m[1] == SignInMethod::PasteOrFile);
    m = signInMethods(BuildVariant::Full, true);
    CHECK(m.size() == 3 && m[0] == SignInMethod::BrowserImport && m[1] == SignInMethod::PasteOrFile
          && m[2] == SignInMethod::WebView);
    m = signInMethods(BuildVariant::Store, false);
    CHECK(m.size() == 1 && m[0] == SignInMethod::PasteOrFile);
    m = signInMethods(BuildVariant::Store, true);
    CHECK(m.size() == 2 && m[0] == SignInMethod::PasteOrFile && m[1] == SignInMethod::WebView);
}

// ACC-03: probe order, host filter, collision rule, gate.
static void browserImport() {
    const std::vector<std::string> dbs = browserCookieDatabases();
    CHECK(dbs.size() == 3);
    CHECK(dbs.size() == 3 && dbs[0] == ".local/share/org.sailfishos/browser/.mozilla/cookies.sqlite");
    CHECK(dbs.size() == 3 && dbs[1] == ".mozilla/mozembed/cookies.sqlite");
    CHECK(dbs.size() == 3 && dbs[2] == ".local/share/org.sailfishos/sailfish-browser/.mozilla/cookies.sqlite");

    CHECK(importableCookieHost("youtube.com"));
    CHECK(importableCookieHost(".youtube.com"));
    CHECK(importableCookieHost("www.youtube.com"));
    CHECK(importableCookieHost("m.youtube.com"));
    CHECK(importableCookieHost(".google.com"));
    CHECK(importableCookieHost("accounts.google.com"));
    CHECK(importableCookieHost("google.com"));
    CHECK(importableCookieHost(".YouTube.COM"));
    CHECK(!importableCookieHost("notyoutube.com"));
    CHECK(!importableCookieHost(".notgoogle.com"));
    CHECK(!importableCookieHost("youtube.com.evil.example"));
    CHECK(!importableCookieHost("google.co.uk"));
    CHECK(!importableCookieHost(""));
    CHECK(!importableCookieHost("."));

    // .youtube.com wins whether it comes first or last; slot of first appearance kept.
    std::vector<Cookie> rows;
    rows.push_back(cookie("SAPISID", "fake-sapisid-google", ".google.com"));
    rows.push_back(cookie("PREF", "fake-pref-1"));
    rows.push_back(cookie("SAPISID", "fake-sapisid-youtube", ".youtube.com"));
    rows.push_back(cookie("SAPISID", "fake-sapisid-www", "www.youtube.com"));
    SessionCookies merged = mergeImported(rows);
    CHECK(merged.cookies.size() == 2);
    CHECK(merged.cookies.size() == 2 && merged.cookies[0].name == "SAPISID" && merged.cookies[1].name == "PREF");
    CHECK(merged.find("SAPISID") && merged.find("SAPISID")->value == "fake-sapisid-youtube");

    std::vector<Cookie> reversed;
    reversed.push_back(cookie("SAPISID", "fake-sapisid-youtube", ".youtube.com"));
    reversed.push_back(cookie("SAPISID", "fake-sapisid-google", ".google.com"));
    merged = mergeImported(reversed);
    CHECK(merged.cookies.size() == 1 && merged.find("SAPISID")->value == "fake-sapisid-youtube");

    // No .youtube.com row: the first seen stays.
    std::vector<Cookie> none;
    none.push_back(cookie("SID", "fake-sid-google", ".google.com"));
    none.push_back(cookie("SID", "fake-sid-www", "www.youtube.com"));
    merged = mergeImported(none);
    CHECK(merged.cookies.size() == 1 && merged.find("SID")->value == "fake-sid-google");
    CHECK(!merged.find("SAPISID"));

    // Gate.
    CHECK(!hasSigningSecret(merged));
    CHECK(!hasSigningSecret(SessionCookies()));
    SessionCookies s;
    s.cookies.push_back(cookie("SAPISID", "fake-sapisid-1"));
    CHECK(hasSigningSecret(s));
    SessionCookies s3;
    s3.cookies.push_back(cookie("__Secure-3PAPISID", "fake-3papisid-1"));
    CHECK(hasSigningSecret(s3));
}

static SessionCookies fullSession() {
    SessionCookies s;
    // Deliberately out of header order, with an untracked name and some tracked names missing.
    s.cookies.push_back(cookie("PREF", "fake-pref-1"));
    s.cookies.push_back(cookie("NID", "fake-nid-untracked", ".google.com"));
    s.cookies.push_back(cookie("SID", "fake-sid-1"));
    s.cookies.push_back(cookie("__Secure-3PSIDCC", "fake-3psidcc-1"));
    s.cookies.push_back(cookie("SAPISID", "fake-sapisid-1"));
    s.cookies.push_back(cookie("__Secure-1PSIDTS", "fake-1psidts-1"));
    s.cookies.push_back(cookie("__Secure-3PAPISID", "fake-3papisid-1"));
    s.cookies.push_back(cookie("HSID", "fake-hsid-1"));
    return s;
}

// ACC-06: Cookie header, SAPISIDHASH, request headers.
static void authenticatedRequests() {
    const std::vector<std::string> &names = trackedCookieNames();
    CHECK(names.size() == 17);
    CHECK(names.size() == 17 && names[0] == "__Secure-3PAPISID" && names[1] == "SAPISID"
          && names[3] == "__Secure-3PSID" && names[5] == "SID" && names[9] == "SIDCC"
          && names[12] == "LOGIN_INFO" && names[14] == "VISITOR_INFO1_LIVE" && names[16] == "__Secure-1PSIDTS");
    CHECK(isTrackedCookie("__Secure-3PSIDCC"));
    CHECK(!isTrackedCookie("NID"));
    CHECK(!isTrackedCookie("sapisid"));

    const SessionCookies s = fullSession();
    CHECK(cookieHeader(s) == "__Secure-3PAPISID=fake-3papisid-1; SAPISID=fake-sapisid-1; SID=fake-sid-1; "
                             "HSID=fake-hsid-1; __Secure-3PSIDCC=fake-3psidcc-1; PREF=fake-pref-1; "
                             "__Secure-1PSIDTS=fake-1psidts-1");
    CHECK(cookieHeader(SessionCookies()).empty());

    // The hasher itself against the FIPS 180 "abc" vector.
    CHECK(sha1("abc") == "a9993e364706816aba3e25717850c26c9cd0d89d");
    // Precomputed independently: sha1("1700000000 fake-3papisid-1 https://www.youtube.com").
    const std::int64_t ts = 1700000000;
    CHECK(signingSecret(s) == "fake-3papisid-1");
    CHECK(sapisidHash(ts, "fake-3papisid-1", limits::origin, sha1)
          == "SAPISIDHASH 1700000000_e0f50fd6e431d1ac37b90ccc152feef3f7bcee3e");

    // Fall-back to SAPISID; precomputed sha1("1700000000 fake-sapisid-1 https://www.youtube.com").
    SessionCookies fallback;
    fallback.cookies.push_back(cookie("SAPISID", "fake-sapisid-1"));
    fallback.cookies.push_back(cookie("SID", "fake-sid-1"));
    CHECK(signingSecret(fallback) == "fake-sapisid-1");
    AccountSelection own;
    RequestHeaders h = authenticatedHeaders(fallback, own, ts, sha1);
    CHECK(header(h, "Authorization") == "SAPISIDHASH 1700000000_c4565a5fc01295c5a9faaf58388cb62fe8e62a5e");
    // An empty 3PAPISID does not shadow SAPISID.
    fallback.cookies.push_back(cookie("__Secure-3PAPISID", ""));
    CHECK(signingSecret(fallback) == "fake-sapisid-1");

    // Header set and order for the login's own channel: AuthUser always, no PageId.
    h = authenticatedHeaders(s, own, ts, sha1);
    CHECK(h.headers.size() == 5);
    CHECK(h.headers.size() == 5 && h.headers[0].first == "Cookie" && h.headers[1].first == "Authorization"
          && h.headers[2].first == "Origin" && h.headers[3].first == "Referer"
          && h.headers[4].first == "X-Goog-AuthUser");
    CHECK(header(h, "Cookie") == cookieHeader(s));
    CHECK(header(h, "Authorization") == "SAPISIDHASH 1700000000_e0f50fd6e431d1ac37b90ccc152feef3f7bcee3e");
    CHECK(header(h, "Origin") == "https://www.youtube.com");
    CHECK(header(h, "Referer") == "https://www.youtube.com/");
    CHECK(header(h, "X-Goog-AuthUser") == "0");
    CHECK(header(h, "X-Goog-PageId") == "<absent>");

    // Fresh per request: 1 s later gives a different hash.
    const RequestHeaders later = authenticatedHeaders(s, own, ts + 1, sha1);
    CHECK(header(later, "Authorization") != header(h, "Authorization"));
    CHECK(header(later, "Authorization").compare(0, 23, "SAPISIDHASH 1700000001_") == 0);
    CHECK(header(later, "X-Goog-AuthUser") == "0");

    // Brand channel on login 3: AuthUser and PageId.
    AccountSelection brand;
    brand.authUser = 3;
    brand.pageId = "fake-page-id-1";
    h = authenticatedHeaders(s, brand, ts, sha1);
    CHECK(h.headers.size() == 6);
    CHECK(header(h, "X-Goog-AuthUser") == "3");
    CHECK(header(h, "X-Goog-PageId") == "fake-page-id-1");
    CHECK(h.headers.size() == 6 && h.headers[5].first == "X-Goog-PageId");
    brand.authUser = 12;
    CHECK(header(authenticatedHeaders(s, brand, ts, sha1), "X-Goog-AuthUser") == "12");

    // No secret: no authenticated headers at all.
    SessionCookies noSecret;
    noSecret.cookies.push_back(cookie("SID", "fake-sid-1"));
    CHECK(authenticatedHeaders(noSecret, own, ts, sha1).headers.empty());

    // "Keep ?key=": these are headers only; the request URL (with its ?key=) is built
    // by the caller and is never touched here, so nothing in this API can drop it.
}

// ACC-08: rotation fold.
static void rotationFold() {
    SessionCookies s = fullSession();
    // Rotation of a stored tracked cookie.
    std::vector<SetCookieValue> r;
    r.push_back(setCookie("__Secure-3PSIDCC", "fake-3psidcc-2"));
    CHECK(foldRotation(&s, r));
    CHECK(s.find("__Secure-3PSIDCC")->value == "fake-3psidcc-2");
    CHECK(s.cookies.size() == 8);

    // Same value again: no change, the caller writes nothing.
    CHECK(!foldRotation(&s, r));
    CHECK(!foldRotation(&s, std::vector<SetCookieValue>()));
    CHECK(!foldRotation(nullptr, r));

    // Untracked new cookie ignored.
    r.clear();
    r.push_back(setCookie("YSC", "fake-ysc-1"));
    CHECK(!foldRotation(&s, r));
    CHECK(!s.find("YSC"));

    // Untracked but already stored: updated.
    r.clear();
    r.push_back(setCookie("NID", "fake-nid-2"));
    CHECK(foldRotation(&s, r));
    CHECK(s.find("NID")->value == "fake-nid-2");

    // Tracked new cookie added.
    r.clear();
    r.push_back(setCookie("SIDCC", "fake-sidcc-1"));
    CHECK(foldRotation(&s, r));
    CHECK(s.find("SIDCC") && s.find("SIDCC")->value == "fake-sidcc-1");

    // Expiry and domain updates count as a change.
    r.clear();
    SetCookieValue e = setCookie("SIDCC", "fake-sidcc-1");
    e.expiresSec = 1800000000;
    r.push_back(e);
    CHECK(foldRotation(&s, r));
    CHECK(s.find("SIDCC")->expiresSec == 1800000000);
    CHECK(!foldRotation(&s, r));
    r[0].domain = ".google.com";
    CHECK(foldRotation(&s, r));
    CHECK(s.find("SIDCC")->domain == ".google.com");

    // Expired Set-Cookie removes a stored name; for an unknown name it is ignored.
    r.clear();
    r.push_back(setCookie("SIDCC", "", true));
    CHECK(foldRotation(&s, r));
    CHECK(!s.find("SIDCC"));
    CHECK(!foldRotation(&s, r));
    r.clear();
    r.push_back(setCookie("HSID", "fake-hsid-1", true)); // expired even with a value
    CHECK(foldRotation(&s, r));
    CHECK(!s.find("HSID"));
    // A deletion of an untracked unknown name or a tracked unknown one: nothing.
    r.clear();
    r.push_back(setCookie("__Secure-1PSID", "", true));
    CHECK(!foldRotation(&s, r));
    CHECK(!s.find("__Secure-1PSID"));

    // 20 responses each rotating a different cookie: all 20 applied. (The single
    // read-modify-write lock is the store's job; here each response is one fold.)
    SessionCookies twenty;
    const std::vector<std::string> &names = trackedCookieNames();
    for (std::size_t i = 0; i < names.size(); ++i) twenty.cookies.push_back(cookie(names[i], "fake-old-" + names[i]));
    twenty.cookies.push_back(cookie("NID", "fake-old-NID", ".google.com"));
    twenty.cookies.push_back(cookie("AEC", "fake-old-AEC", ".google.com"));
    twenty.cookies.push_back(cookie("SOCS", "fake-old-SOCS"));
    CHECK(twenty.cookies.size() == 20);
    int changes = 0;
    for (std::size_t i = 0; i < twenty.cookies.size(); ++i) {
        std::vector<SetCookieValue> one;
        const std::string n = twenty.cookies[i].name;
        one.push_back(setCookie(n, "fake-new-" + n));
        if (foldRotation(&twenty, one)) ++changes;
    }
    CHECK(changes == 20);
    bool allNew = twenty.cookies.size() == 20;
    for (std::size_t i = 0; i < twenty.cookies.size(); ++i)
        allNew = allNew && twenty.cookies[i].value == "fake-new-" + twenty.cookies[i].name;
    CHECK(allNew);
    // One response carrying several rotations folds them all at once.
    std::vector<SetCookieValue> batch;
    batch.push_back(setCookie("SID", "fake-batch-sid"));
    batch.push_back(setCookie("SIDCC", "fake-batch-sidcc"));
    batch.push_back(setCookie("YSC", "fake-ysc-untracked"));
    CHECK(foldRotation(&twenty, batch));
    CHECK(twenty.find("SID")->value == "fake-batch-sid" && twenty.find("SIDCC")->value == "fake-batch-sidcc");
    CHECK(!twenty.find("YSC"));
}

// ACC-07: periodic schedule on a fake clock and state transitions. The start,
// after-import, after-switch and on-demand checks are service triggers.
static void verificationSchedule() {
    const std::int64_t min = 60 * 1000;
    CHECK(limits::verifyIntervalMs == 15 * min);
    CHECK(nextVerificationMs(SessionState::SignedIn, -1) == 0);
    CHECK(nextVerificationMs(SessionState::Checking, -1) == 0);
    CHECK(nextVerificationMs(SessionState::SignedIn, 1000) == 1000 + 15 * min);
    CHECK(nextVerificationMs(SessionState::Checking, 1000) == 1000 + 15 * min);
    CHECK(nextVerificationMs(SessionState::SignedOut, -1) == -1);
    CHECK(nextVerificationMs(SessionState::SignedOut, 1000) == -1);
    CHECK(nextVerificationMs(SessionState::Expired, 1000) == -1);
    CHECK(nextVerificationMs(SessionState::Unreachable, 1000) == -1);

    // Fake clock, one tick per second for two hours: signed in for the first hour,
    // then the session is rejected at the first check after 60 min.
    SessionState state = SessionState::SignedIn;
    std::int64_t last = -1;
    std::vector<std::int64_t> checks;
    for (std::int64_t now = 0; now <= 120 * min; now += 1000) {
        const std::int64_t due = nextVerificationMs(state, last);
        if (due < 0 || now < due) continue;
        checks.push_back(now);
        last = now;
        state = stateAfter(state, now >= 60 * min ? Verification::Rejected : Verification::Accepted);
    }
    CHECK(checks.size() == 5);
    CHECK(checks.size() == 5 && checks[0] == 0 && checks[1] == 15 * min && checks[2] == 30 * min
          && checks[3] == 45 * min && checks[4] == 60 * min);
    CHECK(state == SessionState::Expired);

    // Never signed in: no checks at all.
    int signedOutChecks = 0;
    for (std::int64_t now = 0; now <= 120 * min; now += 1000) {
        const std::int64_t due = nextVerificationMs(SessionState::SignedOut, -1);
        if (due >= 0 && now >= due) ++signedOutChecks;
    }
    CHECK(signedOutChecks == 0);

    CHECK(stateAfter(SessionState::Checking, Verification::Accepted) == SessionState::SignedIn);
    CHECK(stateAfter(SessionState::Expired, Verification::Accepted) == SessionState::SignedIn);
    CHECK(stateAfter(SessionState::Unreachable, Verification::Accepted) == SessionState::SignedIn);
    CHECK(stateAfter(SessionState::SignedIn, Verification::Rejected) == SessionState::Expired);
    CHECK(stateAfter(SessionState::Checking, Verification::Rejected) == SessionState::Expired);
    // Inconclusive never changes a known state.
    CHECK(stateAfter(SessionState::SignedIn, Verification::Inconclusive) == SessionState::SignedIn);
    CHECK(stateAfter(SessionState::Expired, Verification::Inconclusive) == SessionState::Expired);
    CHECK(stateAfter(SessionState::SignedOut, Verification::Inconclusive) == SessionState::SignedOut);
    CHECK(stateAfter(SessionState::Unreachable, Verification::Inconclusive) == SessionState::Unreachable);
    // Checking has no known state yet: unreachable, never signed out.
    CHECK(stateAfter(SessionState::Checking, Verification::Inconclusive) == SessionState::Unreachable);

    // An inconclusive check while signed in keeps the 15 min cadence.
    CHECK(nextVerificationMs(stateAfter(SessionState::SignedIn, Verification::Inconclusive), 5 * min) == 20 * min);
}

static AccountEntry entry(int authUser, const std::string &name, const std::string &gaia,
                          const std::string &datasync = std::string(), const std::string &page = std::string()) {
    AccountEntry e;
    e.authUser = authUser;
    e.name = name;
    e.gaiaId = gaia;
    e.datasyncId = datasync;
    e.pageId = page;
    return e;
}

// ACC-09: dedupe key and probe stop rule.
static void accountsList() {
    CHECK(limits::maxAuthUserProbe == 7);
    CHECK(accountKey(entry(0, "Fake Name", "fake-gaia-1", "fake-ds-1")) == "fake-gaia-1|");
    CHECK(accountKey(entry(0, "Fake Name", "", "fake-ds-1")) == "fake-ds-1|");
    CHECK(accountKey(entry(0, "Fake Name", "", "")) == "Fake Name|");
    CHECK(accountKey(entry(0, "Fake Brand", "fake-gaia-1", "", "fake-page-1")) == "fake-gaia-1|fake-page-1");
    // Same login seen at two AuthUser indexes: same key. Login vs its brand: different.
    CHECK(accountKey(entry(0, "A", "fake-gaia-1")) == accountKey(entry(1, "A renamed", "fake-gaia-1")));
    CHECK(accountKey(entry(0, "A", "fake-gaia-1")) != accountKey(entry(0, "A", "fake-gaia-1", "", "fake-page-1")));

    // Simulated probe over AuthUser 0..7: index 0 has a login and its brand, index 1 a
    // second login, index 2 repeats index 0 (all duplicates) -> stop there.
    std::vector<std::vector<AccountEntry> > answers(limits::maxAuthUserProbe + 1);
    answers[0].push_back(entry(0, "Fake One", "fake-gaia-1"));
    answers[0].push_back(entry(0, "Fake Brand", "fake-gaia-1", "", "fake-page-1"));
    answers[1].push_back(entry(1, "Fake Two", "", "fake-ds-2"));
    answers[2].push_back(entry(2, "Fake One", "fake-gaia-1"));
    answers[2].push_back(entry(2, "Fake Brand", "fake-gaia-1", "", "fake-page-1"));
    answers[3].push_back(entry(3, "Fake Three", "fake-gaia-3"));
    std::vector<std::string> known;
    int stoppedAt = -1;
    for (int i = 0; i <= limits::maxAuthUserProbe; ++i) {
        if (probeShouldStop(false, answers[i], known)) { stoppedAt = i; break; }
        for (std::size_t j = 0; j < answers[i].size(); ++j) {
            const std::string k = accountKey(answers[i][j]);
            if (std::find(known.begin(), known.end(), k) == known.end()) known.push_back(k);
        }
    }
    CHECK(stoppedAt == 2);
    CHECK(known.size() == 3);

    // Mixed new and duplicate entries: keep going.
    std::vector<AccountEntry> mixed;
    mixed.push_back(entry(2, "Fake One", "fake-gaia-1"));
    mixed.push_back(entry(2, "Fake Three", "fake-gaia-3"));
    CHECK(!probeShouldStop(false, mixed, known));
    // Logged out or empty: stop, even with new entries or nothing known yet.
    CHECK(probeShouldStop(true, mixed, known));
    CHECK(probeShouldStop(false, std::vector<AccountEntry>(), std::vector<std::string>()));
    CHECK(!probeShouldStop(false, answers[0], std::vector<std::string>()));
}

int main() {
    signInOrder();
    browserImport();
    authenticatedRequests();
    rotationFold();
    verificationSchedule();
    accountsList();
    std::printf("account domain tests: %s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
