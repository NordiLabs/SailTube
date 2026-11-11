// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SessionImportParser.h"
#include "Sha1.h"
#include <vector>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {
namespace {
using ports::ImportError;
using ports::ImportFormat;

// A non-owning view; keeps secrets out of temporary strings.
struct Span {
    const char *b; const char *e;
    Span() : b(nullptr), e(nullptr) {}
    Span(const char *begin, const char *end) : b(begin), e(end) {}
    std::size_t size() const { return std::size_t(e - b); }
    bool empty() const { return b == e; }
};
bool isBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
Span trim(Span s) {
    while (s.b < s.e && isBlank(*s.b)) ++s.b;
    while (s.e > s.b && isBlank(*(s.e - 1))) --s.e;
    return s;
}
std::string str(Span s) { return std::string(s.b, s.e); }
bool equalsLower(Span s, const char *lowerName) {
    std::size_t i = 0;
    for (const char *p = s.b; p < s.e; ++p, ++i) {
        char c = *p; if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (!lowerName[i] || c != lowerName[i]) return false;
    }
    return lowerName[i] == 0;
}
bool startsWithLower(Span s, const char *lowerPrefix) {
    std::size_t i = 0;
    for (const char *p = s.b; p < s.e && lowerPrefix[i]; ++p, ++i) {
        char c = *p; if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        if (c != lowerPrefix[i]) return false;
    }
    return lowerPrefix[i] == 0;
}
bool tokenChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-';
}
bool spanDigits(Span s, std::size_t minLen, std::size_t maxLen) {
    if (s.size() < minLen || s.size() > maxLen) return false;
    for (const char *p = s.b; p < s.e; ++p) if (*p < '0' || *p > '9') return false;
    return true;
}
bool cookieValueOk(Span s) {
    if (s.empty() || s.size() > 4096) return false;
    for (const char *p = s.b; p < s.e; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x21 || c > 0x7e || c == ';') return false;
    }
    return true;
}
bool cookieNameOk(Span s) {
    if (s.empty() || s.size() > 64) return false;
    for (const char *p = s.b; p < s.e; ++p) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

struct Collector {
    domain::SessionBundle bundle;
    ports::SessionStageSummary summary;
    ImportError error = ImportError::None;
    int errorLine = 0;
    int expired = 0;       // needed-name cookies skipped because they were expired
    int otherSites = 0;    // needed-name cookies skipped because of their domain
    bool failed() const { return error != ImportError::None; }
    void fail(ImportError e, int line) { if (error == ImportError::None) { error = e; errorLine = line; } }

    // One name=value pair (already split). Unneeded names are dropped without validation.
    void addCookie(Span name, Span value, int line) {
        if (failed()) return;
        const std::string n = str(name);   // names are not secret
        if (!cookieNameOk(name) || !domain::isNeededCookie(n)) { ++summary.cookiesSkipped; return; }
        if (!cookieValueOk(value)) { fail(ImportError::BadCookieValue, line); return; }
        std::string v = str(value);
        const bool known = bundle.find(n) != nullptr;
        if (!known && bundle.cookies.size() >= std::size_t(domain::SessionBundle::kMaxCookies)) { domain::wipeString(v); fail(ImportError::TooManyCookies, line); return; }
        bundle.set(n, v);
        domain::wipeString(v);
    }
    void addCookieString(Span value, int line) {
        std::size_t pieces = 0;
        const char *p = value.b;
        while (p <= value.e && !failed()) {
            const char *q = p;
            while (q < value.e && *q != ';') ++q;
            Span piece = trim(Span(p, q));
            if (!piece.empty()) {
                if (++pieces > 1024) { fail(ImportError::TooManyCookies, line); return; }
                const char *eq = piece.b;
                while (eq < piece.e && *eq != '=') ++eq;
                if (eq < piece.e) addCookie(trim(Span(piece.b, eq)), trim(Span(eq + 1, piece.e)), line);
                else ++summary.cookiesSkipped;
            }
            if (q >= value.e) break;
            p = q + 1;
        }
    }
    // A `Name: value` header (value may be empty). Returns true when the header was one we read.
    void addHeader(Span name, Span value, int line) {
        if (failed()) return;
        if (equalsLower(name, "cookie")) addCookieString(value, line);
        else if (equalsLower(name, "x-goog-authuser")) {
            if (!spanDigits(value, 1, 2)) fail(ImportError::BadAuthUser, line);
            else bundle.authUser = str(value);
        } else if (equalsLower(name, "x-goog-pageid")) {
            if (!spanDigits(value, 1, 32)) fail(ImportError::BadPageId, line);
            else bundle.pageId = str(value);
        } else if (equalsLower(name, "authorization")) summary.sawAuthorizationHeader = true;
    }
};

// `name: value` where name is token characters directly followed by ':' (so cookie strings
// such as "SID=..:" and request lines such as "POST /x HTTP/2" are not headers).
bool splitHeader(Span line, Span *name, Span *value) {
    const char *p = line.b;
    while (p < line.e && tokenChar(*p)) ++p;
    if (p == line.b || p >= line.e || *p != ':') return false;
    *name = Span(line.b, p);
    *value = trim(Span(p + 1, line.e));
    return true;
}

struct NetscapeLine { Span domain, name, value; bool includeSub; std::int64_t expiry; };
bool parseNetscape(Span line, NetscapeLine *out) {
    if (line.empty()) return false;
    if (startsWithLower(line, "#httponly_")) line.b += 10;
    else if (*line.b == '#') return false;
    Span fields[7]; int count = 0;
    const char *p = line.b;
    while (count < 7) {
        const char *q = p;
        while (q < line.e && *q != '\t') ++q;
        fields[count++] = Span(p, q);
        if (q >= line.e) break;
        p = q + 1;
    }
    // Exactly seven tab-separated fields: a stray tab-containing value would make eight.
    if (count != 7 || (fields[6].e < line.e)) return false;
    if (!(equalsLower(fields[1], "true") || equalsLower(fields[1], "false"))) return false;
    if (!(equalsLower(fields[3], "true") || equalsLower(fields[3], "false"))) return false;
    Span exp = trim(fields[4]);
    if (!exp.empty() && !spanDigits(exp, 1, 18)) return false;
    std::int64_t expiry = 0;
    for (const char *c = exp.b; c < exp.e; ++c) expiry = expiry * 10 + (*c - '0');
    out->domain = trim(fields[0]); out->name = trim(fields[5]); out->value = Span(fields[6].b, fields[6].e);
    // The value runs to the end of the line; strip trailing CR/blanks only.
    while (out->value.e > out->value.b && isBlank(*(out->value.e - 1))) --out->value.e;
    out->includeSub = equalsLower(fields[1], "true");
    out->expiry = expiry;
    return true;
}
// Domains whose cookies a request to music.youtube.com carries.
bool domainSentToMusic(Span domainField, bool includeSub) {
    std::string d = str(domainField);
    for (std::size_t i = 0; i < d.size(); ++i) if (d[i] >= 'A' && d[i] <= 'Z') d[i] = char(d[i] - 'A' + 'a');
    if (d == ".youtube.com" || d == ".music.youtube.com" || d == "music.youtube.com") return true;
    if (d == "youtube.com") return includeSub;
    return false;
}

// Reads one argument after a cURL flag; returns the span and advances `pos`.
bool curlArg(const char *&pos, const char *end, Span *arg) {
    while (pos < end && isBlank(*pos)) ++pos;
    if (pos >= end) return false;
    if (*pos == '$' && pos + 1 < end && pos[1] == '\'') ++pos;     // bash $'...' quoting
    if (*pos == '\'' || *pos == '"') {
        const char quote = *pos++;
        const char *start = pos;
        while (pos < end && *pos != quote) { if (quote == '"' && *pos == '\\' && pos + 1 < end) ++pos; ++pos; }
        if (pos >= end) return false;       // unterminated
        *arg = Span(start, pos); ++pos; return true;
    }
    const char *start = pos;
    while (pos < end && !isBlank(*pos)) ++pos;
    *arg = Span(start, pos); return pos > start;
}
}

const char *SessionImportParser::describe(ImportError e) {
    switch (e) {
    case ImportError::None: return "";
    case ImportError::Empty: return "Nothing was pasted.";
    case ImportError::TooLarge: return "That text is too large to be a cookie or header copy.";
    case ImportError::LineTooLong: return "A line in that text is too long.";
    case ImportError::TooManyLines: return "That text has too many lines.";
    case ImportError::BadEncoding: return "That text could not be read as plain text.";
    case ImportError::Unrecognized: return "That text is not a Cookie header, a request-header block, a cURL command or a cookies.txt file.";
    case ImportError::NoCookies: return "No YouTube cookies were found in that text.";
    case ImportError::NoSigningCookie: return "The text has no sign-in cookie.";
    case ImportError::NoSessionCookie: return "The text has no session cookie.";
    case ImportError::CookiesExpired: return "The cookies in that text have expired.";
    case ImportError::OtherSitesOnly: return "Only cookies for other sites were found.";
    case ImportError::TooManyCookies: return "That text has too many cookies.";
    case ImportError::BadCookieValue: return "A cookie value is not valid.";
    case ImportError::BadAuthUser: return "The account index in that text is not valid.";
    case ImportError::BadPageId: return "The page id in that text is not valid.";
    case ImportError::Disabled: return "The YouTube Music account connection is turned off.";
    case ImportError::ConsentRequired: return "The notice about the unofficial connection has not been accepted.";
    case ImportError::Busy: return "Another account operation is in progress.";
    }
    return "";
}

std::string SessionImportParser::fingerprintOf(const std::string &value) {
    if (value.empty()) return std::string();
    Sha1 sha; sha.update("ytm-session-fp\n", 15); sha.update(value);
    unsigned char out[20]; sha.digest(out);
    return Sha1::toHex(out, 4);
}

ImportResult SessionImportParser::parse(const char *data, std::size_t size, const ImportOptions &options) {
    ImportResult result;
    if (!data || size == 0) { result.error = ImportError::Empty; return result; }
    if (size > options.maxBytes) { result.error = ImportError::TooLarge; return result; }
    const char *begin = data, *end = data + size;
    if (size >= 3 && static_cast<unsigned char>(begin[0]) == 0xEF && static_cast<unsigned char>(begin[1]) == 0xBB &&
        static_cast<unsigned char>(begin[2]) == 0xBF) begin += 3;
    for (const char *p = begin; p < end; ++p) if (*p == '\0') { result.error = ImportError::BadEncoding; return result; }

    std::vector<Span> lines;
    {
        const char *p = begin;
        while (p <= end) {
            const char *q = p;
            while (q < end && *q != '\n') ++q;
            Span line(p, q);
            if (line.size() > options.maxLineBytes) {
                result.error = ImportError::LineTooLong; result.errorLine = int(lines.size() + 1); return result;
            }
            if (lines.size() >= options.maxLines) { result.error = ImportError::TooManyLines; return result; }
            lines.push_back(line);
            if (q >= end) break;
            p = q + 1;
        }
    }

    Collector c;
    bool netscape = false;
    for (std::size_t i = 0; i < lines.size() && !netscape; ++i) {
        NetscapeLine probe;
        if (parseNetscape(lines[i], &probe)) netscape = true;
    }
    bool curl = false;
    if (!netscape) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            const Span t = trim(lines[i]);
            if (t.empty()) continue;
            curl = startsWithLower(t, "curl ") || startsWithLower(t, "curl\t") || equalsLower(t, "curl");
            break;
        }
    }

    ImportFormat format = ImportFormat::Unknown;
    bool recognised = false;
    if (netscape) {
        format = ImportFormat::NetscapeFile;
        for (std::size_t i = 0; i < lines.size() && !c.failed(); ++i) {
            NetscapeLine l;
            if (!parseNetscape(lines[i], &l)) continue;
            recognised = true;
            const std::string name = str(l.name);
            if (!cookieNameOk(l.name) || !domain::isNeededCookie(name)) { ++c.summary.cookiesSkipped; continue; }
            if (!domainSentToMusic(l.domain, l.includeSub)) { ++c.otherSites; ++c.summary.cookiesSkipped; continue; }
            if (options.nowUtcSeconds > 0 && l.expiry > 0 && l.expiry <= options.nowUtcSeconds) {
                ++c.expired; ++c.summary.cookiesSkipped; continue;
            }
            c.addCookie(l.name, l.value, int(i + 1));
        }
    } else if (curl) {
        format = ImportFormat::Curl;
        int args = 0;
        for (std::size_t i = 0; i < lines.size() && !c.failed(); ++i) {
            const char *pos = lines[i].b, *lend = lines[i].e;
            while (pos < lend && !c.failed()) {
                while (pos < lend && isBlank(*pos)) ++pos;
                if (pos >= lend) break;
                // A quoted token that is not an argument of a flag we read (URL, --data-raw
                // body, ...) is skipped whole so words inside it can never look like flags.
                if (*pos == '\'' || *pos == '"' || (*pos == '$' && pos + 1 < lend && pos[1] == '\'')) {
                    Span skipped;
                    if (!curlArg(pos, lend, &skipped)) break;
                    continue;
                }
                const char *start = pos;
                while (pos < lend && !isBlank(*pos)) ++pos;
                const Span word(start, pos);
                if (word.empty()) break;
                const bool header = equalsLower(word, "-h") || equalsLower(word, "--header");
                const bool cookie = equalsLower(word, "-b") || equalsLower(word, "--cookie");
                if (!header && !cookie) continue;
                Span arg;
                if (!curlArg(pos, lend, &arg)) continue;
                if (++args > 512) { c.fail(ImportError::TooManyLines, int(i + 1)); break; }
                recognised = true;
                if (cookie) { c.addCookieString(trim(arg), int(i + 1)); continue; }
                Span n, v;
                if (splitHeader(trim(arg), &n, &v)) c.addHeader(n, v, int(i + 1));
            }
        }
    } else {
        // Header block and/or bare cookie lines (a block with a request line also lands here).
        bool sawHeader = false, sawBare = false;
        for (std::size_t i = 0; i < lines.size() && !c.failed(); ++i) {
            const Span t = trim(lines[i]);
            if (t.empty() || *t.b == ':') continue;          // blank or HTTP/2 pseudo-header
            Span n, v;
            if (splitHeader(t, &n, &v)) { sawHeader = true; recognised = true; c.addHeader(n, v, int(i + 1)); continue; }
            // A bare cookie string starts with a cookie name directly followed by '='; a request
            // line such as "POST /x?a=b HTTP/2" is ignored.
            const char *eq = t.b;
            while (eq < t.e && *eq != '=') ++eq;
            if (eq < t.e && cookieNameOk(trim(Span(t.b, eq)))) { sawBare = true; recognised = true; c.addCookieString(t, int(i + 1)); }
        }
        format = sawHeader ? ImportFormat::HeaderBlock : (sawBare ? ImportFormat::CookieHeader : ImportFormat::Unknown);
    }

    if (c.failed()) { result.error = c.error; result.errorLine = c.errorLine; c.bundle.wipe(); return result; }
    if (!recognised) { result.error = ImportError::Unrecognized; return result; }

    ImportError verdict = ImportError::None;
    std::size_t encodedSize = 128;
    for (std::size_t i = 0; i < c.bundle.cookies.size(); ++i) encodedSize += c.bundle.cookies[i].name.size() + c.bundle.cookies[i].value.size() + 4;
    if (encodedSize > std::size_t(domain::SessionBundle::kMaxEncodedBytes)) {
        // Would not fit the vault payload: refuse now, before anything is sent anywhere.
        c.bundle.wipe(); result.error = ImportError::TooLarge; return result;
    }
    if (c.bundle.empty()) {
        verdict = c.expired > 0 ? ImportError::CookiesExpired : (c.otherSites > 0 ? ImportError::OtherSitesOnly : ImportError::NoCookies);
    } else if (!c.bundle.hasRole(domain::CookieRole::Signing)) {
        verdict = c.expired > 0 ? ImportError::CookiesExpired : ImportError::NoSigningCookie;
    } else if (!c.bundle.hasRole(domain::CookieRole::Identity)) {
        verdict = c.expired > 0 ? ImportError::CookiesExpired : ImportError::NoSessionCookie;
    }
    c.summary.format = format;
    c.summary.cookiesKept = int(c.bundle.cookies.size());
    c.summary.hasSigningCookie = c.bundle.hasRole(domain::CookieRole::Signing);
    c.summary.hasSessionCookie = c.bundle.hasRole(domain::CookieRole::Identity);
    c.summary.authUser = c.bundle.authUser;
    c.summary.hasPageId = !c.bundle.pageId.empty();
    for (std::size_t i = 0; i < c.bundle.cookies.size(); ++i) c.summary.keptNames.push_back(c.bundle.cookies[i].name);
    const domain::SessionCookie *signing = c.bundle.find("__Secure-3PAPISID");
    if (!signing) signing = c.bundle.find("SAPISID");
    if (!signing) signing = c.bundle.find("__Secure-1PAPISID");
    if (signing) c.summary.fingerprint = fingerprintOf(signing->value);

    result.summary = c.summary;
    if (verdict != ImportError::None) { result.error = verdict; c.bundle.wipe(); return result; }
    result.bundle = c.bundle;     // copy; the collector's copy wipes itself on destruction
    c.bundle.wipe();
    return result;
}

}}}}
