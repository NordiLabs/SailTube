// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Pure C++11 tests (no Qt): SHA-1, SessionSigner golden vectors, SessionImportParser (formats,
// bounds, fuzz, secret-leak scans), SessionExpiryDetector matrix, SessionBundleCodec,
// SessionHolder (rotation merge with generation counter, snapshot/persist bookkeeping).
#include "check.h"
#include "Fakes.h"
#include "golden_vectors.h"
#include "Core/Accounts/Infrastructure/SessionBundleCodec.h"
#include "Core/Accounts/Infrastructure/SessionExpiryDetector.h"
#include "Core/Accounts/Infrastructure/SessionHolder.h"
#include "Core/Accounts/Infrastructure/SessionImportParser.h"
#include "Core/Accounts/Infrastructure/SessionSigner.h"
#include "Core/Accounts/Infrastructure/Sha1.h"
#include <algorithm>
#include <atomic>
#include <cstring>

using namespace nordtile::music::accounts::infrastructure;
using nordtile::music::accounts::ports::ImportError;
using nordtile::music::accounts::ports::ImportFormat;
using nordtile::music::accounts::domain::SessionBundle;
using nordtile::music::accounts::domain::SessionVerdict;

namespace {
bool hasName(const std::vector<std::string> &names, const std::string &n) {
    return std::find(names.begin(), names.end(), n) != names.end();
}
std::string valueOf(const SessionBundle &b, const char *name) {
    const domain::SessionCookie *c = b.find(name);
    return c ? c->value : std::string("<absent>");
}
bool containsAnySentinel(const std::string &text) {
    std::size_t n = 0; const char *const *all = allSentinels(&n);
    for (std::size_t i = 0; i < n; ++i) if (text.find(all[i]) != std::string::npos) return true;
    return text.find(kSentinelStableId) != std::string::npos;
}
// Everything a parse result exposes except the bundle itself (which legitimately holds values).
std::string publicSurface(const ImportResult &r) {
    std::string s = SessionImportParser::describe(r.error);
    for (std::size_t i = 0; i < r.summary.keptNames.size(); ++i) s += "|" + r.summary.keptNames[i];
    s += "|" + r.summary.authUser + "|" + r.summary.fingerprint;
    return s;
}
}

// ------------------------------------------------------------------------------------------
static void testSha1() {
    for (std::size_t i = 0; i < sizeof(kGoldenSha1) / sizeof(kGoldenSha1[0]); ++i) {
        const GoldenSha1 &g = kGoldenSha1[i];
        Sha1 sha;
        const std::size_t len = std::strlen(g.prefix);
        for (unsigned long r = 0; r < g.repeat; ++r) sha.update(g.prefix, len);
        unsigned char out[20]; sha.digest(out);
        const std::string got = Sha1::toHex(out, 20);
        if (got != g.hex) std::fprintf(stderr, "sha1 vector %s: got %s\n", g.name, got.c_str());
        CHECK_EQ(got, std::string(g.hex));
    }
    // Streaming in odd chunk sizes must equal one-shot hashing.
    std::string data; for (int i = 0; i < 1000; ++i) data.push_back(char('a' + i % 26));
    const std::string whole = Sha1::hex(data);
    for (std::size_t chunk = 1; chunk <= 70; chunk += 3) {
        Sha1 sha;
        for (std::size_t at = 0; at < data.size(); at += chunk) sha.update(data.data() + at, std::min(chunk, data.size() - at));
        unsigned char out[20]; sha.digest(out);
        CHECK_EQ(Sha1::toHex(out, 20), whole);
    }
}

// ------------------------------------------------------------------------------------------
static void testSignerGolden() {
    for (std::size_t i = 0; i < sizeof(kGoldenSigning) / sizeof(kGoldenSigning[0]); ++i) {
        const GoldenSigning &g = kGoldenSigning[i];
        SigningInput in;
        in.options.version = g.version == 1 ? SignerVersion::V1 : (g.version == 2 ? SignerVersion::V2 : SignerVersion::V3);
        in.options.unit = g.millis ? TimestampUnit::Milliseconds : TimestampUnit::Seconds;
        in.origin = g.origin; in.nowUtcSeconds = g.seconds; in.extraMillis = g.extraMillis;
        in.sapisid = g.sapisid; in.secure1Papisid = g.p1; in.secure3Papisid = g.p3; in.userSessionId = g.session;
        const SigningResult r = SessionSigner::sign(in);
        if (!r.ok || r.value != g.expected) std::fprintf(stderr, "signer vector %s: ok=%d error=%s\n", g.name, int(r.ok), r.error);
        CHECK(r.ok);
        CHECK_EQ(r.value, std::string(g.expected));
    }
}

static void testSignerRules() {
    SigningInput base;
    base.origin = "https://music.youtube.com"; base.nowUtcSeconds = 1700000000; base.secure3Papisid = "COOKIE_VALUE_1";
    CHECK(SessionSigner::sign(base).ok);
    // Seconds timestamp is the default (the milliseconds dispute is opt-in only).
    CHECK(SessionSigner::sign(base).value.compare(0, 23, "SAPISIDHASH 1700000000_") == 0);
    CHECK(SignerOptions().unit == TimestampUnit::Seconds);
    CHECK(SignerOptions().version == SignerVersion::V1);

    // Same input, different second: different value. Same input: identical value (pure).
    SigningInput later = base; later.nowUtcSeconds += 1;
    CHECK(SessionSigner::sign(base).value != SessionSigner::sign(later).value);
    CHECK_EQ(SessionSigner::sign(base).value, SessionSigner::sign(base).value);

    // Refusals use fixed texts and never echo input.
    SigningInput bad = base; bad.origin = "http://music.youtube.com";            CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.origin = "https://music.youtube.com/path";                    CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.origin = "https://music.youtube.com\r\nX-Evil: 1";            CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.origin = "https://";                                          CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.origin = "";                                                  CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.nowUtcSeconds = 0;                                            CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.secure3Papisid.clear();                                       CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.secure3Papisid = "has space";                                 CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.secure3Papisid = "has;semicolon";                             CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.options.version = SignerVersion::V3;                          CHECK(!SessionSigner::sign(bad).ok);   // needs a session id
    bad.userSessionId = "bad id!";                                                CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.options.version = SignerVersion::V2; bad.secure3Papisid.clear(); CHECK(!SessionSigner::sign(bad).ok);
    bad = base; bad.options.unit = TimestampUnit::Milliseconds; bad.extraMillis = 1000; CHECK(!SessionSigner::sign(bad).ok);
    CHECK(!SessionSigner::validOrigin("https://a b"));
    CHECK(SessionSigner::validOrigin("https://music.youtube.com:8443"));
    for (std::size_t i = 0; i < 8; ++i) {
        bad = base; bad.origin = "https://x.example"; bad.secure3Papisid = kSentinelP3; bad.nowUtcSeconds = 0;
        const SigningResult r = SessionSigner::sign(bad);
        CHECK(!r.ok); CHECK_NOT_CONTAINS(std::string(r.error), kSentinelP3); CHECK(r.value.empty());
    }
}

// ------------------------------------------------------------------------------------------
static ImportResult parse(const std::string &text, std::int64_t now = 0) {
    ImportOptions o; o.nowUtcSeconds = now; return SessionImportParser::parse(text, o);
}

static void testParserFormats() {
    {   // bare cookie string
        const std::string text = std::string("SAPISID=") + kSentinelSapisid + "; __Secure-3PAPISID=" + kSentinelP3 +
            "; __Secure-3PSID=" + kSentinelPsid3 + "; OTHER=" + kSentinelOther;
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK(r.summary.format == ImportFormat::CookieHeader);
        CHECK_EQ(r.summary.cookiesKept, 3); CHECK_EQ(r.summary.cookiesSkipped, 1);
        CHECK(r.summary.hasSigningCookie); CHECK(r.summary.hasSessionCookie);
        CHECK(!r.summary.hasPageId); CHECK_EQ(r.summary.fingerprint.size(), std::size_t(8));
        CHECK_EQ(valueOf(r.bundle, "__Secure-3PAPISID"), std::string(kSentinelP3));
        CHECK(!r.bundle.find("OTHER"));          // unneeded cookie dropped at once
        CHECK(r.bundle.authUser.empty());
        std::vector<std::string> names; for (size_t i = 0; i < r.bundle.cookies.size(); ++i) names.push_back(r.bundle.cookies[i].name);
        CHECK(std::is_sorted(names.begin(), names.end()));
    }
    {   // Cookie: header line
        ImportResult r = parse(sentinelCookieHeader());
        CHECK(r.ok()); CHECK(r.summary.format == ImportFormat::HeaderBlock);
        CHECK_EQ(r.summary.cookiesKept, 7);
        CHECK(hasName(r.summary.keptNames, "__Secure-1PSIDTS"));
        CHECK(!hasName(r.summary.keptNames, "UNNEEDED_COOKIE"));
        CHECK_EQ(valueOf(r.bundle, "__Secure-1PSIDTS"), std::string(kSentinelTs));
    }
    {   // Firefox "Copy Request Headers" block with a request line and a stray TE header
        const std::string text = std::string("POST /youtubei/v1/browse?prettyPrint=false&key=PUBLIC_KEY_X HTTP/2\r\n"
            "Host: music.youtube.com\r\nUser-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:109.0) Gecko/20100101 Firefox/115.0\r\n"
            "Accept: */*\r\nAuthorization: SAPISIDHASH 1700000000_") + kSentinelAuthz + "\r\n"
            "Content-Type: application/json\r\nX-Goog-AuthUser: 2\r\nx-origin: https://music.youtube.com\r\n" +
            sentinelCookieHeader() + "\r\nTE: trailers\r\n";
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK(r.summary.format == ImportFormat::HeaderBlock);
        CHECK_EQ(r.bundle.authUser, std::string("2")); CHECK_EQ(r.summary.authUser, std::string("2"));
        CHECK(r.summary.sawAuthorizationHeader);
        CHECK(containsAnySentinel(kSentinelAuthz));                          // sanity: the sentinel is registered
        for (size_t i = 0; i < r.bundle.cookies.size(); ++i) CHECK_NOT_CONTAINS(r.bundle.cookies[i].value, kSentinelAuthz);
        CHECK_NOT_CONTAINS(publicSurface(r), kSentinelAuthz);
    }
    {   // Chrome HTTP/2 block: pseudo-headers, lowercase names, page id (brand account)
        const std::string text = ":authority: music.youtube.com\n:method: POST\n:path: /youtubei/v1/browse\n:scheme: https\n"
            "accept: */*\nauthorization: SAPISIDHASH 1_x\nx-goog-authuser: 0\nx-goog-pageid: 118234567890123456789\n"
            "cookie: " + std::string("SAPISID=") + kSentinelSapisid + "; __Secure-3PSID=" + kSentinelPsid3 + "\n";
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK_EQ(r.bundle.authUser, std::string("0")); CHECK_EQ(r.bundle.pageId, std::string("118234567890123456789"));
        CHECK(r.summary.hasPageId);
    }
    {   // cURL (bash): -H, -b, a data body whose text contains "-b" and a header-looking word
        const std::string text = "curl 'https://music.youtube.com/youtubei/v1/browse?prettyPrint=false' \\\n"
            "  -H 'accept: */*' \\\n  -H 'authorization: SAPISIDHASH 1700000000_" + std::string(kSentinelAuthz) + "' \\\n"
            "  -H 'x-goog-authuser: 1' \\\n  -b 'SAPISID=" + kSentinelSapisid + "; __Secure-3PSID=" + kSentinelPsid3 + "; JUNK=" + kSentinelOther + "' \\\n"
            "  --data-raw '{\"context\":{\"client\":{\"clientName\":\"WEB_REMIX\"}},\"note\":\"x -b SID=INJECTED y\"}'\n";
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK(r.summary.format == ImportFormat::Curl);
        CHECK_EQ(r.bundle.authUser, std::string("1")); CHECK_EQ(r.summary.cookiesKept, 2);
        CHECK(!r.bundle.find("SID"));            // the "-b" inside the quoted body was not a flag
        CHECK(r.summary.sawAuthorizationHeader);
    }
    {   // cURL with --header and $'...' quoting
        const std::string text = std::string("curl 'https://music.youtube.com/x' --header \"Cookie: SAPISID=") + kSentinelSapisid +
            "; __Secure-3PSID=" + kSentinelPsid3 + "\" -H $'x-goog-authuser: 3'\n";
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK_EQ(r.bundle.authUser, std::string("3"));
    }
    {   // Netscape cookies.txt
        const std::string text = std::string("# Netscape HTTP Cookie File\n# This is a generated file!  Do not edit.\n\n") +
            ".youtube.com\tTRUE\t/\tTRUE\t1800000000\t__Secure-3PAPISID\t" + kSentinelP3 + "\n"
            "#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t1800000000\t__Secure-3PSID\t" + kSentinelPsid3 + "\n"
            ".youtube.com\tTRUE\t/\tFALSE\t0\tVISITOR_INFO1_LIVE\tvisitor_value_000111\n"
            ".google.com\tTRUE\t/\tTRUE\t1800000000\tSID\t" + kSentinelPsid1 + "\n"
            "youtube.com\tFALSE\t/\tTRUE\t1800000000\tLOGIN_INFO\thostonly_value_0001\n"
            ".youtube.com\tTRUE\t/\tTRUE\t1800000000\tUNNEEDED\t" + kSentinelOther + "\n";
        ImportResult r = parse(text, 1700000000);
        CHECK(r.ok()); CHECK(r.summary.format == ImportFormat::NetscapeFile);
        CHECK_EQ(r.summary.cookiesKept, 3);
        CHECK(!r.bundle.find("SID"));            // google.com is not sent to music.youtube.com
        CHECK(!r.bundle.find("LOGIN_INFO"));     // host-only youtube.com is not sent to subdomains
    }
    {   // multiple Cookie headers merge, last value wins
        const std::string text = std::string("Cookie: SAPISID=old_value_000111; __Secure-3PSID=") + kSentinelPsid3 +
            "\nCookie: SAPISID=" + kSentinelSapisid + "\n";
        ImportResult r = parse(text);
        CHECK(r.ok()); CHECK_EQ(valueOf(r.bundle, "SAPISID"), std::string(kSentinelSapisid));
    }
    {   // quoted cookie value is kept verbatim; whitespace around pairs is trimmed
        ImportResult r = parse(std::string("  SAPISID=\"quoted_value_0001\" ;   __Secure-3PSID=") + kSentinelPsid3 + "  ");
        CHECK(r.ok()); CHECK_EQ(valueOf(r.bundle, "SAPISID"), std::string("\"quoted_value_0001\""));
    }
    {   // BOM, CRLF and trailing blank lines
        ImportResult r = parse(std::string("\xEF\xBB\xBF") + "Cookie: SAPISID=" + kSentinelSapisid + "; __Secure-3PSID=" + kSentinelPsid3 + "\r\n\r\n\r\n");
        CHECK(r.ok());
    }
    {   // an Authorization-only paste is not a session (and is never stored)
        ImportResult r = parse(std::string("Authorization: SAPISIDHASH 1700000000_") + kSentinelAuthz + "\n");
        CHECK(!r.ok());
        CHECK_NOT_CONTAINS(publicSurface(r), kSentinelAuthz);
    }
}

static void testParserErrors() {
    CHECK(parse("").error == ImportError::Empty);
    CHECK(parse("   \n \t \n").error == ImportError::Unrecognized);
    CHECK(parse("hello world, this is not a cookie").error == ImportError::Unrecognized);
    CHECK(parse("Accept: */*\nUser-Agent: x\n").error == ImportError::NoCookies);
    CHECK(parse(std::string("Cookie: UNNEEDED=") + kSentinelOther).error == ImportError::NoCookies);
    CHECK(parse(std::string("Cookie: __Secure-3PSID=") + kSentinelPsid3).error == ImportError::NoSigningCookie);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid).error == ImportError::NoSessionCookie);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "; SID=bad value with spaces;").error == ImportError::NoSessionCookie ||
          parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "; SID=bad value with spaces;").error == ImportError::BadCookieValue);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "; SID=ctl\x01" "char").error == ImportError::BadCookieValue);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "; SID=\n").error != ImportError::None);
    CHECK(parse("Cookie: SAPISID=;").error != ImportError::None);                              // empty value of a needed cookie
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "\nX-Goog-AuthUser: abc\n").error == ImportError::BadAuthUser);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "\nX-Goog-AuthUser: 123\n").error == ImportError::BadAuthUser);
    CHECK(parse(std::string("Cookie: SAPISID=") + kSentinelSapisid + "\nX-Goog-PageId: 12ab\n").error == ImportError::BadPageId);
    CHECK(parse(std::string("a\0b", 3)).error == ImportError::BadEncoding);
    CHECK(parse(std::string("Cookie: SAPISID=x\0y", 19)).error == ImportError::BadEncoding);

    // Netscape specifics
    const std::string expired = std::string(".youtube.com\tTRUE\t/\tTRUE\t1600000000\t__Secure-3PAPISID\t") + kSentinelP3 + "\n"
        ".youtube.com\tTRUE\t/\tTRUE\t1600000000\t__Secure-3PSID\t" + kSentinelPsid3 + "\n";
    CHECK(parse(expired, 1700000000).error == ImportError::CookiesExpired);
    CHECK(parse(expired, 0).error == ImportError::None);      // no clock: expiry not judged
    const std::string other = std::string(".google.com\tTRUE\t/\tTRUE\t0\tSAPISID\t") + kSentinelSapisid + "\n"
        ".google.com\tTRUE\t/\tTRUE\t0\tSID\t" + kSentinelPsid3 + "\n";
    CHECK(parse(other, 1700000000).error == ImportError::OtherSitesOnly);
    const std::string halfExpired = std::string(".youtube.com\tTRUE\t/\tTRUE\t1600000000\t__Secure-3PAPISID\t") + kSentinelP3 + "\n"
        ".youtube.com\tTRUE\t/\tTRUE\t0\t__Secure-3PSID\t" + kSentinelPsid3 + "\n";
    CHECK(parse(halfExpired, 1700000000).error == ImportError::CookiesExpired);

    // Error line numbers point at the offending line and never carry text.
    ImportResult r = parse(std::string("Accept: */*\nCookie: SAPISID=") + kSentinelSapisid + "; SID=bad\x02" "value\n");
    CHECK(r.error == ImportError::BadCookieValue); CHECK_EQ(r.errorLine, 2);
    CHECK_NOT_CONTAINS(publicSurface(r), kSentinelSapisid);
}

static void testParserBounds() {
    ImportOptions o;
    CHECK(SessionImportParser::parse(std::string(2 * 1024 * 1024, 'a'), o).error == ImportError::TooLarge);
    CHECK(SessionImportParser::parse(std::string("Cookie: ") + std::string(40000, 'a'), o).error == ImportError::LineTooLong);
    std::string many; for (int i = 0; i < 30000; ++i) many += "x\n";
    CHECK(SessionImportParser::parse(many, o).error == ImportError::TooManyLines);
    std::string pieces = "Cookie: "; for (int i = 0; i < 2000; ++i) pieces += "a=b; ";
    CHECK(SessionImportParser::parse(pieces, o).error == ImportError::TooManyCookies);
    std::string huge = std::string("Cookie: SAPISID=") + std::string(5000, 'v');
    CHECK(SessionImportParser::parse(huge, o).error == ImportError::BadCookieValue);
    CHECK(SessionImportParser::parse(nullptr, 0, o).error == ImportError::Empty);
    ImportOptions tiny; tiny.maxBytes = 10;
    CHECK(SessionImportParser::parse(std::string("Cookie: SAPISID=abcdef"), tiny).error == ImportError::TooLarge);
    // A cURL command with thousands of -H flags is cut off, not looped over.
    std::string flags = "curl 'https://x.example'"; for (int i = 0; i < 3000; ++i) flags += " -H 'a: b'";
    CHECK(!SessionImportParser::parse(flags, o).ok());
}

static std::uint32_t lcg(std::uint32_t &s) { s = s * 1664525u + 1013904223u; return s >> 8; }

static void testParserFuzz() {
    static const char *const pool[] = {
        "Cookie: ", "cookie:", "SAPISID=", "__Secure-3PAPISID=", "__Secure-3PSID=", "SID=", "LOGIN_INFO=", "UNNEEDED=", ";", "; ", "=", "\t", "\n", "\r\n",
        " ", "TRUE", "FALSE", ".youtube.com", "youtube.com", "music.youtube.com", ".google.com", "#HttpOnly_", "# Netscape HTTP Cookie File\n",
        "curl ", "curl 'https://x'", "-H ", "--header ", "-b ", "--cookie ", "'", "\"", "$'", "\\", "x-goog-authuser: ", "X-Goog-PageId: ", "Authorization: ",
        "SAPISIDHASH ", ":authority: ", "POST /x HTTP/2\n", "0", "9", "12345678901234567890", "1800000000", "1600000000", "a", "zz",
        "SNTL_SAPISID_4f1c9a77e2b84d30", "SNTL_PSID3_b27e91d4a05c68f3", "SNTL_P3APISID_61d0b4f9e7a3258c", "SNTL_AUTHZ_d41f7c0be98a2536",
        "SNTL_UNNEEDED_77aa11bb22cc33dd", "\xEF\xBB\xBF", "\xC3\xA9", "\x01", "\x7f", "~", "{", "}", "[", "]", "<html>", "..", "%41"
    };
    const std::size_t poolSize = sizeof(pool) / sizeof(pool[0]);
    std::uint32_t seed = 0xC0FFEEu;
    int okCount = 0;
    for (int iter = 0; iter < 40000; ++iter) {
        std::string text;
        const int parts = 1 + int(lcg(seed) % 24);
        for (int p = 0; p < parts; ++p) {
            const std::uint32_t pick = lcg(seed) % (poolSize + 3);
            if (pick < poolSize) text += pool[pick];
            else if (pick == poolSize) text.push_back(char(lcg(seed) & 0xff));          // any byte, NUL included
            else if (pick == poolSize + 1) text += std::string(1 + lcg(seed) % 40, char('a' + lcg(seed) % 26));
            else text.push_back(char(0x20 + lcg(seed) % 95));
        }
        ImportOptions o; o.nowUtcSeconds = (iter % 2) ? 1700000000 : 0;
        o.maxBytes = 4096; o.maxLines = 64; o.maxLineBytes = 512;
        ImportResult r = SessionImportParser::parse(text, o);
        // The public surface can never carry a sentinel, whatever the input.
        CHECK(!containsAnySentinel(publicSurface(r)));
        if (r.ok()) {
            ++okCount;
            CHECK(r.bundle.usable());
            for (std::size_t i = 0; i < r.bundle.cookies.size(); ++i) {
                CHECK(domain::isNeededCookie(r.bundle.cookies[i].name));
                CHECK(domain::validCookieValue(r.bundle.cookies[i].value));
            }
            CHECK(r.bundle.authUser.empty() || domain::digitsOnly(r.bundle.authUser, 1, 2));
            CHECK(r.bundle.pageId.empty() || domain::digitsOnly(r.bundle.pageId, 1, 32));
            std::string encoded;
            CHECK(SessionBundleCodec::encode(r.bundle, &encoded));
        } else {
            CHECK(r.bundle.empty());              // a refused paste leaves nothing behind
        }
        if (g_failures > 20) break;
    }
    std::printf("parser fuzz (random tokens): 40000 inputs, %d accepted\n", okCount);

    // Near-valid inputs: mutate real-looking pastes so the accepting paths are exercised too.
    const std::string seeds[] = {
        sentinelCookieHeader(),
        std::string("POST /youtubei/v1/browse?x=1 HTTP/2\nHost: music.youtube.com\nAuthorization: SAPISIDHASH 1_SNTL_AUTHZ_d41f7c0be98a2536\nX-Goog-AuthUser: 0\n") + sentinelCookieHeader() + "\n",
        std::string("curl 'https://music.youtube.com/x' -H 'x-goog-authuser: 1' -b 'SAPISID=") + kSentinelSapisid + "; __Secure-3PSID=" + kSentinelPsid3 + "'\n",
        std::string("# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t1800000000\t__Secure-3PAPISID\t") + kSentinelP3 +
            "\n#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t1800000000\t__Secure-3PSID\t" + kSentinelPsid3 + "\n"
    };
    int nearOk = 0;
    for (int iter = 0; iter < 40000; ++iter) {
        std::string text = seeds[iter % 4];
        for (std::uint32_t n = 1 + lcg(seed) % 3; n; --n) {
            const std::size_t at = lcg(seed) % text.size();
            switch (lcg(seed) % 5) {
            case 0: text[at] = char(lcg(seed) & 0xff); break;
            case 1: text.erase(at, 1 + lcg(seed) % 8); break;
            case 2: text.insert(at, std::string(1, pool[lcg(seed) % poolSize][0])); break;
            case 3: text.insert(at, pool[lcg(seed) % poolSize]); break;
            default: text.resize(at + 1); break;
            }
            if (text.empty()) text = "x";
        }
        ImportOptions o; o.nowUtcSeconds = 1700000000;
        ImportResult r = SessionImportParser::parse(text, o);
        CHECK(!containsAnySentinel(publicSurface(r)));
        if (r.ok()) {
            ++nearOk;
            CHECK(r.bundle.usable());
            for (std::size_t i = 0; i < r.bundle.cookies.size(); ++i)
                CHECK(domain::isNeededCookie(r.bundle.cookies[i].name) && domain::validCookieValue(r.bundle.cookies[i].value));
        } else CHECK(r.bundle.empty());
        if (g_failures > 20) break;
    }
    CHECK(nearOk > 5000);     // the accepting paths really ran
    std::printf("parser fuzz (mutated pastes): 40000 inputs, %d accepted\n", nearOk);
}

// ------------------------------------------------------------------------------------------
namespace {
struct ExpiryRow {
    const char *name; int status; bool net; bool oversized; const char *host; const char *path; const char *type; const char *body; BodyShape shape;
    SessionVerdict verdict; ExpiryReason reason;
};
}

static void testExpiryMatrix() {
    const BodyShape NC = BodyShape::NotChecked;
    static const ExpiryRow rows[] = {
        {"network error", 0, true, false, "", "", "", "", NC, SessionVerdict::Transient, ExpiryReason::NetworkError},
        {"status 0 without flag", 0, false, false, "", "", "", "", NC, SessionVerdict::Transient, ExpiryReason::NetworkError},
        {"oversized body", 200, false, true, "", "", "application/json", "{", NC, SessionVerdict::ProtocolChanged, ExpiryReason::OversizedBody},
        {"401", 401, false, false, "", "", "application/json", "{\"error\":{\"code\":401}}", NC, SessionVerdict::SessionExpired, ExpiryReason::Http401},
        {"401 html", 401, false, false, "", "", "text/html", "<html>x</html>", NC, SessionVerdict::SessionExpired, ExpiryReason::Http401},
        {"429", 429, false, false, "", "", "text/html", "unusual traffic", NC, SessionVerdict::RateLimited, ExpiryReason::Http429},
        {"403 challenge", 403, false, false, "", "", "text/html", "<html>Our systems have detected unusual traffic</html>", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 html sign in to confirm", 403, false, false, "", "", "text/html", "<html><h1>Sign in to confirm you're not a bot</h1></html>", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 captcha path", 403, false, false, "", "", "text/html", "<a href=\"/sorry/index\">", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 plain", 403, false, false, "", "", "application/json", "{\"error\":{\"code\":403}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        // ---- the 403 split (session.md section 13): a permission error of ONE operation is not a block
        {"403 json permission denied", 403, false, false, "", "", "application/json; charset=UTF-8",
         "{\"error\":{\"code\":403,\"message\":\"The caller does not have permission\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json permission denied, pretty printed", 403, false, false, "", "", "application/json",
         "{\n  \"error\": {\n    \"code\": 403,\n    \"status\": \"PERMISSION_DENIED\"\n  }\n}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json upper-case marker", 403, false, false, "", "", "application/json", "{\"error\":{\"code\":403,\"status\":\"Permission_Denied\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json reason forbidden", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"errors\":[{\"domain\":\"global\",\"reason\":\"forbidden\"}]}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json reason forbidden, spaced", 403, false, false, "", "", "application/json",
         "{\"error\": {\"code\": 403, \"errors\": [{\"reason\" : \"forbidden\"}]}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json insufficient permissions", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"errors\":[{\"reason\":\"insufficientPermissions\"}]}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json insufficient scopes", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"message\":\"Request had insufficient authentication scopes.\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json scope insufficient reason", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"details\":[{\"reason\":\"ACCESS_TOKEN_SCOPE_INSUFFICIENT\"}]}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json forbidden message", 403, false, false, "", "", "application/json", "{\"error\":{\"code\":403,\"message\":\"Forbidden\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json permission, xssi prefix", 403, false, false, "", "", "", ")]}'\n{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        {"403 json permission, no content type", 403, false, false, "", "", "", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Forbidden, ExpiryReason::Http403Forbidden},
        // everything that is a block, or merely unexplained, stays Blocked
        {"403 json permission + unusual traffic", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"message\":\"Unusual traffic from your network\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 json permission + sign in to confirm", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"message\":\"Sign in to confirm you're not a bot\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 json permission + sign in to confirm (no bot word)", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"message\":\"Sign in to confirm\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 json permission + captcha", 403, false, false, "", "", "application/json", "{\"error\":{\"message\":\"captcha required\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 json permission + sorry path", 403, false, false, "", "", "application/json", "{\"error\":{\"url\":\"/sorry/index\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403Challenge},
        {"403 json permission + consent wording", 403, false, false, "", "", "application/json",
         "{\"error\":{\"message\":\"Before you continue\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 json permission + consent host", 403, false, false, "", "", "application/json",
         "{\"error\":{\"url\":\"https://consent.youtube.com/m\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 json permission + unauthenticated", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"status\":\"UNAUTHENTICATED\",\"message\":\"permission denied\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 json permission + login required", 403, false, false, "", "", "application/json",
         "{\"error\":{\"code\":403,\"message\":\"LOGIN_REQUIRED\",\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 html forbidden", 403, false, false, "", "", "text/html", "<html><body>403 Forbidden</body></html>", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 html with a json-looking permission body", 403, false, false, "", "", "text/html",
         "<html>{\"error\":{\"status\":\"PERMISSION_DENIED\"}}</html>", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 plain text forbidden", 403, false, false, "", "", "text/plain", "Forbidden", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 empty", 403, false, false, "", "", "application/json", "", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 json without an error document", 403, false, false, "", "", "application/json", "{\"status\":\"PERMISSION_DENIED\"}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        {"403 json error without a marker", 403, false, false, "", "", "application/json", "{\"error\":{\"code\":403,\"message\":\"x\"}}", NC, SessionVerdict::Blocked, ExpiryReason::Http403},
        // only an HTTP 403 is a permission error; the same document under another status keeps its own row
        {"401 json permission denied", 401, false, false, "", "", "application/json", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::SessionExpired, ExpiryReason::Http401},
        {"429 json forbidden", 429, false, false, "", "", "application/json", "{\"error\":{\"reason\":\"forbidden\"}}", NC, SessionVerdict::RateLimited, ExpiryReason::Http429},
        {"404 json permission denied", 404, false, false, "", "", "application/json", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedStatus},
        {"400 json permission denied", 400, false, false, "", "", "application/json", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::ProtocolChanged, ExpiryReason::BadRequest},
        {"500 json permission denied", 500, false, false, "", "", "application/json", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::Transient, ExpiryReason::Http5xx},
        {"302 json permission denied", 302, false, false, "elsewhere.example", "/", "application/json", "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedRedirect},
        {"408", 408, false, false, "", "", "", "", NC, SessionVerdict::Transient, ExpiryReason::Http408},
        {"500", 500, false, false, "", "", "", "", NC, SessionVerdict::Transient, ExpiryReason::Http5xx},
        {"503", 503, false, false, "", "", "text/html", "<html>busy", NC, SessionVerdict::Transient, ExpiryReason::Http5xx},
        {"599", 599, false, false, "", "", "", "", NC, SessionVerdict::Transient, ExpiryReason::Http5xx},
        {"302 sign-in", 302, false, false, "accounts.google.com", "/ServiceLogin", "", "", NC, SessionVerdict::SessionExpired, ExpiryReason::SignInRedirect},
        {"302 sign-in youtube", 303, false, false, "accounts.youtube.com", "/accounts/SetSID", "", "", NC, SessionVerdict::SessionExpired, ExpiryReason::SignInRedirect},
        {"302 sign-in upper-case host", 302, false, false, "ACCOUNTS.Google.com", "/", "", "", NC, SessionVerdict::SessionExpired, ExpiryReason::SignInRedirect},
        {"302 consent", 302, false, false, "consent.youtube.com", "/m", "", "", NC, SessionVerdict::Blocked, ExpiryReason::ConsentRedirect},
        {"302 consent google", 302, false, false, "consent.google.com", "/", "", "", NC, SessionVerdict::Blocked, ExpiryReason::ConsentRedirect},
        {"302 sorry", 302, false, false, "www.google.com", "/sorry/index", "", "", NC, SessionVerdict::Blocked, ExpiryReason::ChallengeRedirect},
        {"302 elsewhere", 302, false, false, "evil.example", "/", "", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedRedirect},
        {"302 look-alike host", 302, false, false, "notaccounts.google.com.evil.example", "/", "", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedRedirect},
        {"301 same host", 301, false, false, "music.youtube.com", "/x", "", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedRedirect},
        {"400", 400, false, false, "", "", "application/json", "{\"error\":{\"message\":\"Request contains an invalid argument.\"}}", NC, SessionVerdict::ProtocolChanged, ExpiryReason::BadRequest},
        {"404", 404, false, false, "", "", "", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedStatus},
        {"204", 204, false, false, "", "", "", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::UnexpectedStatus},
        {"200 html consent", 200, false, false, "", "", "text/html; charset=utf-8", "<!DOCTYPE html><html>Before you continue to YouTube</html>", NC, SessionVerdict::Blocked, ExpiryReason::HtmlConsent},
        {"200 html consent host", 200, false, false, "", "", "text/html", "<html><form action=\"https://consent.youtube.com/save\">", NC, SessionVerdict::Blocked, ExpiryReason::HtmlConsent},
        {"200 html challenge", 200, false, false, "", "", "text/html", "<html>Sign in to confirm you are not a bot. Unusual traffic from your network</html>", NC, SessionVerdict::Blocked, ExpiryReason::HtmlChallenge},
        {"200 html captcha", 200, false, false, "", "", "text/html", "<html><div class=\"g-recaptcha\"> CAPTCHA", NC, SessionVerdict::Blocked, ExpiryReason::HtmlChallenge},
        {"200 html sign-in", 200, false, false, "", "", "text/html", "<html><a href=\"https://accounts.google.com/ServiceLogin\">Sign in</a>", NC, SessionVerdict::SessionExpired, ExpiryReason::HtmlSignIn},
        {"200 html unknown", 200, false, false, "", "", "text/html", "<html><body>hello</body></html>", NC, SessionVerdict::SessionExpired, ExpiryReason::HtmlUnknown},
        {"200 html without content-type", 200, false, false, "", "", "", "\n  <!doctype html><title>x</title>", NC, SessionVerdict::SessionExpired, ExpiryReason::HtmlUnknown},
        {"200 json content-type but html body", 200, false, false, "", "", "application/json", "<html>sign in</html>", NC, SessionVerdict::SessionExpired, ExpiryReason::HtmlSignIn},
        {"200 json signed in", 200, false, false, "", "", "application/json; charset=UTF-8", "{\"contents\":{}}", BodyShape::SignedIn, SessionVerdict::Ok, ExpiryReason::Ok},
        {"200 json signed out", 200, false, false, "", "", "application/json", "{\"contents\":{}}", BodyShape::SignedOut, SessionVerdict::SessionExpired, ExpiryReason::SignedOutShape},
        {"200 json missing fields", 200, false, false, "", "", "application/json", "{\"contents\":{}}", BodyShape::MissingRequired, SessionVerdict::ProtocolChanged, ExpiryReason::MissingRequiredFields},
        {"200 json shape unchecked", 200, false, false, "", "", "application/json", "{}", NC, SessionVerdict::Ok, ExpiryReason::OkShapeUnchecked},
        {"200 json xssi prefix", 200, false, false, "", "", "", ")]}'\n{\"a\":1}", BodyShape::SignedIn, SessionVerdict::Ok, ExpiryReason::Ok},
        {"200 json array", 200, false, false, "", "", "text/plain", "[1,2]", BodyShape::SignedIn, SessionVerdict::Ok, ExpiryReason::Ok},
        {"200 empty body", 200, false, false, "", "", "application/json", "", NC, SessionVerdict::ProtocolChanged, ExpiryReason::EmptyBody},
        {"200 whitespace body", 200, false, false, "", "", "application/json", " \r\n ", NC, SessionVerdict::ProtocolChanged, ExpiryReason::EmptyBody},
        {"200 plain text", 200, false, false, "", "", "text/plain", "ok", NC, SessionVerdict::ProtocolChanged, ExpiryReason::NotJson},
        {"200 json-typed garbage", 200, false, false, "", "", "application/json", "garbage", BodyShape::SignedIn, SessionVerdict::Ok, ExpiryReason::Ok},
    };
    for (std::size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i) {
        const ExpiryRow &row = rows[i];
        ExpiryObservation o;
        o.httpStatus = row.status; o.networkError = row.net; o.oversized = row.oversized;
        o.redirectHost = row.host; o.redirectPath = row.path; o.contentType = row.type; o.bodyPrefix = row.body; o.shape = row.shape;
        const Classification c = SessionExpiryDetector::classify(o);
        if (c.verdict != row.verdict || c.reason != row.reason)
            std::fprintf(stderr, "expiry row '%s': got %s/%s\n", row.name, domain::verdictName(c.verdict), SessionExpiryDetector::reasonName(c.reason));
        CHECK(c.verdict == row.verdict);
        CHECK(c.reason == row.reason);
    }
    // commitSafe only for a verified signed-in document.
    ExpiryObservation ok; ok.httpStatus = 200; ok.contentType = "application/json"; ok.bodyPrefix = "{}"; ok.shape = BodyShape::SignedIn;
    CHECK(SessionExpiryDetector::classify(ok).commitSafe());
    ok.shape = BodyShape::NotChecked; CHECK(!SessionExpiryDetector::classify(ok).commitSafe());
    // Only the first 8 KiB of the body are examined (a late "captcha" word is ignored).
    ExpiryObservation late; late.httpStatus = 200; late.contentType = "text/html";
    late.bodyPrefix = "<html>" + std::string(9000, ' ') + "captcha</html>";
    CHECK(SessionExpiryDetector::classify(late).reason == ExpiryReason::HtmlUnknown);
    // Every verdict has a distinct, safe account-level mapping; Transient is "unreachable", not "expired".
    CHECK(domain::errorCodeFor(SessionVerdict::SessionExpired) == domain::ErrorCode::AuthorizationRequired);
    CHECK(domain::errorCodeFor(SessionVerdict::Transient) == domain::ErrorCode::Network);
    CHECK(domain::errorCodeFor(SessionVerdict::Transient) != domain::errorCodeFor(SessionVerdict::SessionExpired));
    CHECK(domain::errorCodeFor(SessionVerdict::Blocked) == domain::ErrorCode::Blocked);
    CHECK(domain::errorCodeFor(SessionVerdict::ProtocolChanged) == domain::ErrorCode::ProtocolChanged);
    CHECK(domain::errorCodeFor(SessionVerdict::RateLimited) == domain::ErrorCode::RateLimited);
    CHECK(domain::errorCodeFor(SessionVerdict::Ok) == domain::ErrorCode::None);
    // Forbidden is a class of its own: a permission error is neither "sign in again", nor "unreachable", nor a block.
    CHECK(domain::errorCodeFor(SessionVerdict::Forbidden) == domain::ErrorCode::PermissionDenied);
    CHECK(domain::errorCodeFor(SessionVerdict::Forbidden) != domain::errorCodeFor(SessionVerdict::Blocked));
    CHECK(domain::errorCodeFor(SessionVerdict::Forbidden) != domain::errorCodeFor(SessionVerdict::SessionExpired));
    CHECK(domain::errorCodeFor(SessionVerdict::Forbidden) != domain::errorCodeFor(SessionVerdict::Transient));
    CHECK_EQ(std::string(domain::verdictName(SessionVerdict::Forbidden)), std::string("Forbidden"));
    CHECK(std::string(domain::verdictMessage(SessionVerdict::Forbidden)) != std::string(domain::verdictMessage(SessionVerdict::Blocked)));
    CHECK(!std::string(domain::verdictMessage(SessionVerdict::Forbidden)).empty());
    CHECK_EQ(std::string(SessionExpiryDetector::reasonName(ExpiryReason::Http403Forbidden)), std::string("Http403Forbidden"));
    {   // A body that is only a permission error never becomes commit-safe or Ok
        ExpiryObservation f; f.httpStatus = 403; f.contentType = "application/json"; f.bodyPrefix = "{\"error\":{\"status\":\"PERMISSION_DENIED\"}}";
        const Classification c = SessionExpiryDetector::classify(f);
        CHECK(c.verdict == SessionVerdict::Forbidden && !c.commitSafe());
    }
    {   // Only the first 8 KiB are examined: a permission marker after that is not seen (the 403 stays unexplained)
        ExpiryObservation f; f.httpStatus = 403; f.contentType = "application/json";
        f.bodyPrefix = "{\"error\":{\"message\":\"" + std::string(9000, 'a') + "\",\"status\":\"PERMISSION_DENIED\"}}";
        CHECK(SessionExpiryDetector::classify(f).verdict == SessionVerdict::Blocked);
        // ... and neither is a late captcha word after an early marker: the early marker decides
        ExpiryObservation g; g.httpStatus = 403; g.contentType = "application/json";
        g.bodyPrefix = "{\"error\":{\"status\":\"PERMISSION_DENIED\",\"message\":\"" + std::string(9000, 'a') + " captcha\"}}";
        CHECK(SessionExpiryDetector::classify(g).verdict == SessionVerdict::Forbidden);
    }
    // Property: for an HTTP 403 JSON error document, Forbidden <=> a permission marker AND none of the block /
    // authentication signals. 6 000 generated documents, whitespace and case varied.
    {
        static const char *const markers[] = {"PERMISSION_DENIED", "permission denied", "The caller does not have permission", "insufficientPermissions",
                                              "Request had insufficient authentication scopes.", "ACCESS_TOKEN_SCOPE_INSUFFICIENT", "Forbidden"};
        static const char *const blockers[] = {"unusual traffic", "captcha", "not a robot", "not a bot", "Sign in to confirm", "/sorry/index",
                                               "Our systems have detected", "consent.youtube.com", "Before you continue", "UNAUTHENTICATED", "LOGIN_REQUIRED"};
        static const char *const fillers[] = {"x", "request failed", "try later", "id 42", "ok?"};
        std::uint32_t seed = 4242;
        int forbidden = 0, blocked = 0;
        for (int i = 0; i < 6000; ++i) {
            const bool withMarker = lcg(seed) % 4 != 0;
            const bool withBlocker = lcg(seed) % 3 == 0;
            const char *marker = markers[lcg(seed) % (sizeof(markers) / sizeof(markers[0]))];
            const char *blocker = blockers[lcg(seed) % (sizeof(blockers) / sizeof(blockers[0]))];
            const char *filler = fillers[lcg(seed) % (sizeof(fillers) / sizeof(fillers[0]))];
            const bool spaced = lcg(seed) % 2 == 0;
            std::string text = std::string("{\"error\":") + (spaced ? " " : "") + "{\"code\":403,\"message\":\"" + filler;
            if (withMarker) text += std::string(" ") + marker;
            if (withBlocker) text += std::string(" ") + blocker;
            text += "\"}}";
            ExpiryObservation o; o.httpStatus = 403; o.contentType = "application/json"; o.bodyPrefix = text;
            const Classification c = SessionExpiryDetector::classify(o);
            const bool expectForbidden = withMarker && !withBlocker;
            if ((c.verdict == SessionVerdict::Forbidden) != expectForbidden) std::fprintf(stderr, "403 property: %s -> %s\n", text.c_str(), domain::verdictName(c.verdict));
            CHECK((c.verdict == SessionVerdict::Forbidden) == expectForbidden);
            CHECK(c.verdict == SessionVerdict::Forbidden || c.verdict == SessionVerdict::Blocked);   // never anything else for a 403
            c.verdict == SessionVerdict::Forbidden ? ++forbidden : ++blocked;
            if (g_failures > 20) break;
        }
        CHECK(forbidden > 1500 && blocked > 1500);          // both branches really ran
    }
    // Fuzz: the detector never throws or loops on arbitrary observations.
    std::uint32_t seed = 12345;
    for (int i = 0; i < 20000; ++i) {
        ExpiryObservation o;
        o.httpStatus = int(lcg(seed) % 700); o.networkError = lcg(seed) % 11 == 0; o.oversized = lcg(seed) % 17 == 0;
        for (std::uint32_t n = lcg(seed) % 40; n; --n) o.bodyPrefix.push_back(char(lcg(seed) & 0xff));
        for (std::uint32_t n = lcg(seed) % 12; n; --n) o.contentType.push_back(char(lcg(seed) & 0x7f));
        for (std::uint32_t n = lcg(seed) % 12; n; --n) o.redirectHost.push_back(char('a' + lcg(seed) % 26));
        o.shape = BodyShape(lcg(seed) % 4);
        const Classification c = SessionExpiryDetector::classify(o);
        CHECK(int(c.verdict) >= 0 && int(c.verdict) <= int(SessionVerdict::Forbidden));
        if (g_failures > 20) break;
    }
}

// ------------------------------------------------------------------------------------------
static SessionBundle sampleBundle() {
    ImportResult r = parse(sentinelCookieHeader() + "\nX-Goog-AuthUser: 1\n");
    SessionBundle b = r.bundle;
    b.source = domain::SessionSourceKind::Paste; b.importedAtUtcSeconds = 1700000123;
    return b;
}

static void testBundleCodec() {
    const SessionBundle original = sampleBundle();
    CHECK(original.usable());
    std::string encoded;
    CHECK(SessionBundleCodec::encode(original, &encoded));
    CHECK(encoded.compare(0, 22, "ytm-session-bundle v1\n") == 0);
    SessionBundle decoded;
    CHECK(SessionBundleCodec::decode(encoded.data(), encoded.size(), &decoded) == BundleDecode::Ok);
    CHECK_EQ(decoded.cookies.size(), original.cookies.size());
    for (size_t i = 0; i < original.cookies.size(); ++i) { CHECK_EQ(decoded.cookies[i].name, original.cookies[i].name); CHECK_EQ(decoded.cookies[i].value, original.cookies[i].value); }
    CHECK_EQ(decoded.authUser, std::string("1")); CHECK(decoded.source == domain::SessionSourceKind::Paste);
    CHECK_EQ(decoded.importedAtUtcSeconds, std::int64_t(1700000123));

    // Strict reading.
    SessionBundle out;
    CHECK(SessionBundleCodec::decode("", 0, &out) == BundleDecode::Empty);
    CHECK(SessionBundleCodec::decode("ytm-session-bundle v2\n", 22, &out) == BundleDecode::UnsupportedVersion);
    CHECK(SessionBundleCodec::decode("garbage\n", 8, &out) == BundleDecode::Malformed);
    std::string noNewline = encoded.substr(0, encoded.size() - 1);
    CHECK(SessionBundleCodec::decode(noNewline.data(), noNewline.size(), &out) == BundleDecode::Malformed);
    const std::string unknownCookie = "ytm-session-bundle v1\nc.NOT_ALLOWED=abc\n";
    CHECK(SessionBundleCodec::decode(unknownCookie.data(), unknownCookie.size(), &out) == BundleDecode::Malformed);
    const std::string unusable = "ytm-session-bundle v1\nc.SAPISID=abcdef\n";
    CHECK(SessionBundleCodec::decode(unusable.data(), unusable.size(), &out) == BundleDecode::Unusable);
    const std::string future = encoded + "future_key=whatever\n";
    CHECK(SessionBundleCodec::decode(future.data(), future.size(), &out) == BundleDecode::Ok);     // forward compatible
    CHECK(SessionBundleCodec::decode(std::string(70000, 'x').data(), 70000, &out) == BundleDecode::TooLarge);
    SessionBundle empty; CHECK(!SessionBundleCodec::encode(empty, &encoded)); CHECK(encoded.empty());

    // Mutation fuzz: never crashes, and anything accepted is a usable allow-listed bundle.
    std::uint32_t seed = 99;
    const std::string good = [&] { std::string s; SessionBundleCodec::encode(original, &s); return s; }();
    int accepted = 0;
    for (int i = 0; i < 20000; ++i) {
        std::string m = good;
        for (std::uint32_t n = 1 + lcg(seed) % 4; n; --n) {
            const std::size_t at = lcg(seed) % m.size();
            switch (lcg(seed) % 4) {
            case 0: m[at] = char(lcg(seed) & 0xff); break;
            case 1: m.erase(at, 1 + lcg(seed) % 6); break;
            case 2: m.insert(at, std::string(1 + lcg(seed) % 6, char('=' + lcg(seed) % 5))); break;
            default: m.resize(at); break;
            }
            if (m.empty()) m = "x";
        }
        SessionBundle d;
        if (SessionBundleCodec::decode(m.data(), m.size(), &d) == BundleDecode::Ok) {
            ++accepted;
            CHECK(d.usable());
            for (size_t k = 0; k < d.cookies.size(); ++k) CHECK(domain::isNeededCookie(d.cookies[k].name) && domain::validCookieValue(d.cookies[k].value));
        } else CHECK(d.empty());
        if (g_failures > 20) break;
    }
    std::printf("bundle codec fuzz: 20000 mutations, %d still accepted\n", accepted);
}

static void testWipe() {
    std::string s(100, 'S');
    const char *p = s.data();
    domain::wipeString(s);
    CHECK(s.empty());
    bool zero = true; for (int i = 0; i < 100; ++i) if (p[i] != 0) zero = false;     // capacity is kept, bytes are zeroed
    CHECK(zero);
    // (A bundle's heap buffers are freed by wipe(), so their bytes cannot be inspected afterwards;
    // wipe() zeroes through wipeString() first. Here: the observable effect.)
    SessionBundle b = sampleBundle();
    CHECK(!b.cookies.empty());
    b.wipe();
    CHECK(b.cookies.empty()); CHECK(b.authUser.empty()); CHECK(b.pageId.empty()); CHECK(!b.usable());
    CHECK(b.source == domain::SessionSourceKind::Unknown);
}

// ------------------------------------------------------------------------------------------
static void testHolder() {
    SessionHolder holder;
    ports::SessionRequestHeaders h;
    CHECK(!holder.signedIn()); CHECK(!holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    const std::uint64_t g0 = holder.generation();
    const std::uint64_t g1 = holder.install(sampleBundle(), false);
    CHECK(g1 > g0); CHECK(holder.signedIn());
    CHECK(holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    CHECK_EQ(h.generation, g1); CHECK_EQ(h.authUser, std::string("1")); CHECK(h.pageId.empty());
    CHECK(h.authorization.compare(0, 23, "SAPISIDHASH 1700000000_") == 0);
    CHECK(h.cookie.find("SAPISID=") == 0 || h.cookie.find("; SAPISID=") != std::string::npos);
    CHECK_NOT_CONTAINS(h.cookie, kSentinelOther);                       // dropped at import
    CHECK_NOT_CONTAINS(h.authorization, kSentinelP3);                   // a hash, never the value
    CHECK(h.origin == "https://music.youtube.com");
    CHECK(!holder.requestHeaders("http://music.youtube.com", 1700000000, &h));
    CHECK(h.cookie.empty() && h.authorization.empty());                 // a refused call leaves nothing in the output
    {   // the default account index is "0"
        SessionBundle b = sampleBundle(); b.authUser.clear();
        holder.install(b, false);
        holder.requestHeaders("https://music.youtube.com", 1700000000, &h);
        CHECK_EQ(h.authUser, std::string("0"));
    }
    const std::uint64_t gen = holder.install(sampleBundle(), false);

    // Rotation merge rules.
    std::vector<ports::RotatedCookie> up(1);
    up[0].name = "__Secure-1PSIDTS"; up[0].value = "rotated_value_0001";
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::Applied);
    ports::SessionCredentialsPort &credentials = holder;
    SessionHolder::Snapshot snap; CHECK(holder.snapshot(&snap));
    CHECK(snap.dirty); CHECK_EQ(valueOf(snap.bundle, "__Secure-1PSIDTS"), std::string("rotated_value_0001"));
    CHECK_EQ(credentials.generation(), gen);                           // identity unchanged by a merge
    CHECK(holder.revision() > 0);
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);          // identical value
    up[0].name = "NOT_ALLOWED"; CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);
    up[0].name = "__Secure-3PAPISID"; up[0].value = ""; up[0].deleted = true;
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);          // signing cookie deletion refused
    CHECK(holder.snapshot(&snap)); CHECK(snap.bundle.find("__Secure-3PAPISID") != nullptr);
    up[0].name = "__Secure-3PSID"; CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);   // identity deletion refused
    up[0].name = "VISITOR_INFO1_LIVE"; CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::Applied); // client cookie may go
    CHECK(holder.snapshot(&snap)); CHECK(snap.bundle.find("VISITOR_INFO1_LIVE") == nullptr);
    up[0].name = "__Secure-1PSIDTS"; up[0].deleted = false; up[0].value = "bad value;with;separators";
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);          // invalid value ignored
    up[0].value = "x y"; CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::NoChange);
    // A batch applies atomically per call: one good update among bad ones still lands.
    std::vector<ports::RotatedCookie> batch(3);
    batch[0].name = "junk"; batch[0].value = "v";
    batch[1].name = "SIDCC"; batch[1].value = "sidcc_value_000111";
    batch[2].name = "__Secure-3PSIDCC"; batch[2].value = "bad;value";
    CHECK(holder.applyRotation(batch, gen) == ports::RotationOutcome::Applied);
    CHECK(holder.snapshot(&snap)); CHECK(snap.bundle.find("SIDCC") != nullptr); CHECK(snap.bundle.find("__Secure-3PSIDCC") == nullptr);

    // markPersisted only clears when nothing newer exists.
    CHECK(holder.snapshot(&snap));
    up[0].name = "SIDCC"; up[0].value = "newer_than_the_snapshot"; up[0].deleted = false;
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::Applied);
    CHECK(!holder.markPersisted(snap.generation, snap.revision));          // a newer revision exists
    SessionHolder::Snapshot fresh; CHECK(holder.snapshot(&fresh)); CHECK(fresh.dirty);
    CHECK(holder.markPersisted(fresh.generation, fresh.revision));
    CHECK(holder.snapshot(&fresh)); CHECK(!fresh.dirty);

    // Generation counter: a response of a replaced or wiped session can never change anything.
    const std::uint64_t replacedGen = holder.install(sampleBundle(), false);
    up[0].name = "__Secure-1PSIDTS"; up[0].value = "late_value_from_old_session";
    CHECK(holder.applyRotation(up, gen) == ports::RotationOutcome::StaleGeneration);
    CHECK(holder.snapshot(&snap)); CHECK_EQ(valueOf(snap.bundle, "__Secure-1PSIDTS"), std::string(kSentinelTs));
    const std::uint64_t wipedGen = holder.wipe();
    CHECK(wipedGen > replacedGen); CHECK(holder.empty()); CHECK(!holder.signedIn());
    CHECK(holder.applyRotation(up, replacedGen) == ports::RotationOutcome::NotSignedIn);
    CHECK(!holder.snapshot(&snap));
    CHECK(!holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    holder.install(sampleBundle(), false);                                  // sign in again: old generations stay dead
    CHECK(holder.applyRotation(up, replacedGen) == ports::RotationOutcome::StaleGeneration);
    CHECK(holder.applyRotation(up, wipedGen) == ports::RotationOutcome::StaleGeneration);

    // Dirty observer fires outside the lock (re-entering the holder from it must not deadlock).
    int fired = 0;
    const std::uint64_t cur = holder.generation();
    holder.setDirtyObserver([&] { ++fired; SessionHolder::Snapshot s; holder.snapshot(&s); });
    up[0].value = "observer_value_0001"; CHECK(holder.applyRotation(up, cur) == ports::RotationOutcome::Applied);
    CHECK_EQ(fired, 1);
    CHECK(holder.applyRotation(up, cur) == ports::RotationOutcome::NoChange); CHECK_EQ(fired, 1);
    holder.setDirtyObserver(std::function<void()>());

    // Signer options come from the holder (V2 multi-token form).
    SignerOptions v2; v2.version = SignerVersion::V2; holder.setSignerOptions(v2);
    CHECK(holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    CHECK(h.authorization.find("SAPISID1PHASH") != std::string::npos && h.authorization.find("SAPISID3PHASH") != std::string::npos);
    // V3 needs a user-session id that the holder keeps in memory only.
    SignerOptions v3; v3.version = SignerVersion::V3; holder.setSignerOptions(v3);
    CHECK(!holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    holder.setUserSessionId("sess_0001"); CHECK(holder.requestHeaders("https://music.youtube.com", 1700000000, &h));
    CHECK(h.authorization.size() > 2 && h.authorization.substr(h.authorization.size() - 2) == "_u");
    h.wipe(); CHECK(h.cookie.empty() && h.authorization.empty() && h.generation == 0);
}

static void testHolderThreads() {
    SessionHolder holder;
    const std::uint64_t gen = holder.install(sampleBundle(), false);
    std::atomic<int> applied(0), headers(0);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) threads.push_back(std::thread([&, t] {
        for (int i = 0; i < 400; ++i) {
            if (t % 2) {
                ports::SessionRequestHeaders h;
                if (holder.requestHeaders("https://music.youtube.com", 1700000000 + i, &h)) ++headers;
            } else {
                std::vector<ports::RotatedCookie> up(1);
                up[0].name = "SIDCC"; up[0].value = "v" + std::to_string(t) + "_" + std::to_string(i);
                if (holder.applyRotation(up, gen) == ports::RotationOutcome::Applied) ++applied;
                SessionHolder::Snapshot s; holder.snapshot(&s);
                holder.markPersisted(s.generation, s.revision);
            }
        }
    }));
    for (size_t i = 0; i < threads.size(); ++i) threads[i].join();
    CHECK(applied > 0); CHECK_EQ(headers.load(), 800);
    CHECK(holder.signedIn());
}

int main() {
    testSha1();
    testSignerGolden();
    testSignerRules();
    testParserFormats();
    testParserErrors();
    testParserBounds();
    testParserFuzz();
    testExpiryMatrix();
    testBundleCodec();
    testWipe();
    testHolder();
    testHolderThreads();
    return finish("ytm_session pure tests");
}
