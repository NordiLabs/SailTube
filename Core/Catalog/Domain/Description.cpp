// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Description.h"
#include "Chapters.h"

#include <cctype>
#include <cstdlib>

namespace nordtile { namespace video { namespace catalog {

namespace text {

std::uint32_t decodeAt(const std::string &s, std::size_t pos, std::size_t *len) {
    const unsigned char c = static_cast<unsigned char>(s[pos]);
    std::size_t n = 1;
    std::uint32_t cp = c;
    if (c >= 0xF8) { n = 1; }
    else if (c >= 0xF0) { n = 4; cp = c & 0x07; }
    else if (c >= 0xE0) { n = 3; cp = c & 0x0F; }
    else if (c >= 0xC0) { n = 2; cp = c & 0x1F; }
    if (n > 1) {
        if (pos + n > s.size()) n = 1;
        for (std::size_t i = 1; i < n; ++i) {
            const unsigned char d = static_cast<unsigned char>(s[pos + i]);
            if ((d & 0xC0) != 0x80) { n = 1; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (n == 1) cp = c;
    }
    if (len) *len = n;
    return cp;
}

bool isSpace(std::uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == '\r' || cp == '\n' || cp == '\f' || cp == '\v' || cp == 0xA0 ||
           (cp >= 0x2000 && cp <= 0x200B) || cp == 0x202F || cp == 0x205F || cp == 0x3000 || cp == 0xFEFF;
}

bool isWordChar(std::uint32_t cp) {
    if (cp < 0x80) return std::isalnum(static_cast<int>(cp)) != 0 || cp == '_';
    if (cp <= 0xBF) return cp == 0xAA || cp == 0xB5 || cp == 0xBA;
    if (cp == 0xD7 || cp == 0xF7) return false;
    if (cp >= 0x2000 && cp <= 0x2BFF) return false;    // punctuation, symbols, arrows, box drawing, dingbats
    if (cp >= 0x2E00 && cp <= 0x2E7F) return false;
    if (cp >= 0x3000 && cp <= 0x303F) return false;    // CJK punctuation
    if (cp == 0x30FB) return false;
    if (cp >= 0xFE00 && cp <= 0xFE6F) return false;    // variation selectors (VS16), small forms
    if (cp == 0xFEFF) return false;
    if ((cp >= 0xFF00 && cp <= 0xFF0F) || (cp >= 0xFF1A && cp <= 0xFF20) || (cp >= 0xFF3B && cp <= 0xFF40) ||
        (cp >= 0xFF5B && cp <= 0xFF65)) return false;  // fullwidth punctuation
    if (cp >= 0xFFF0 && cp <= 0xFFFF) return false;
    if (cp >= 0x1F000 && cp <= 0x1FAFF) return false;  // emoji and pictographs
    if (cp >= 0xE0000 && cp <= 0xE007F) return false;  // tag characters
    return true;
}

std::size_t keycapAt(const std::string &s, std::size_t pos) {
    if (pos >= s.size()) return 0;
    const char c = s[pos];
    if (!((c >= '0' && c <= '9') || c == '#' || c == '*')) return 0;
    std::size_t p = pos + 1, len = 0;
    if (p < s.size() && decodeAt(s, p, &len) == 0xFE0F) p += len;
    if (p < s.size() && decodeAt(s, p, &len) == 0x20E3) return p + len - pos;
    return 0;
}

std::string percentEncode(const std::string &s) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}

std::string percentDecode(const std::string &s) {
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

}  // namespace text

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

// The code point before byte `pos` (0 at the start).
std::uint32_t previousCodePoint(const std::string &s, std::size_t pos) {
    if (pos == 0) return 0;
    std::size_t p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return text::decodeAt(s, p, nullptr);
}

bool startsWithNoCase(const std::string &s, std::size_t pos, const char *prefix) {
    for (std::size_t i = 0; prefix[i]; ++i) {
        if (pos + i >= s.size()) return false;
        if (std::tolower(static_cast<unsigned char>(s[pos + i])) != prefix[i]) return false;
    }
    return true;
}

// A timestamp starting at `pos`: returns its end, or `pos` when none.
std::size_t matchTimestampAt(const std::string &s, std::size_t pos, std::int64_t *seconds) {
    std::size_t p = pos;
    std::vector<std::string> groups;
    while (true) {
        const std::size_t start = p;
        while (p < s.size() && isDigit(s[p]) && p - start < 4) ++p;
        if (p == start) return pos;
        groups.push_back(s.substr(start, p - start));
        if (groups.size() < 3 && p + 1 < s.size() && s[p] == ':' && isDigit(s[p + 1])) { ++p; continue; }
        break;
    }
    if (groups.size() < 2) return pos;
    if (p < s.size() && (isDigit(s[p]) || (s[p] == ':' && p + 1 < s.size() && isDigit(s[p + 1])))) return pos;
    // "10:30am" is a time of day, not a position.
    if (p < s.size() && std::isalpha(static_cast<unsigned char>(s[p]))) return pos;
    const std::int64_t value = parseTimestamp(s.substr(pos, p - pos));
    if (value < 0) return pos;
    if (seconds) *seconds = value;
    return p;
}

std::size_t matchUrlAt(const std::string &s, std::size_t pos, std::string *url) {
    bool www = false;
    if (startsWithNoCase(s, pos, "https://") || startsWithNoCase(s, pos, "http://")) {}
    else if (startsWithNoCase(s, pos, "www.")) www = true;
    else return pos;
    std::size_t p = pos;
    while (p < s.size()) {
        std::size_t len = 0;
        const std::uint32_t cp = text::decodeAt(s, p, &len);
        if (text::isSpace(cp) || cp == '<' || cp == '>' || cp == '"' || cp < 0x20) break;
        p += len;
    }
    // Trailing punctuation belongs to the sentence; closers only when unbalanced.
    while (p > pos) {
        const char c = s[p - 1];
        if (c == '.' || c == ',' || c == ';' || c == ':' || c == '!' || c == '?' || c == '\'') { --p; continue; }
        if (c == ')' || c == ']' || c == '}') {
            const char open = c == ')' ? '(' : c == ']' ? '[' : '{';
            int balance = 0;
            for (std::size_t i = pos; i < p; ++i) { if (s[i] == open) ++balance; else if (s[i] == c) --balance; }
            if (balance < 0) { --p; continue; }
        }
        break;
    }
    const std::size_t minimum = www ? 5 : 9;
    if (p - pos < minimum) return pos;
    if (url) *url = (www ? std::string("https://") : std::string()) + s.substr(pos, p - pos);
    return p;
}

std::size_t matchHashtagAt(const std::string &s, std::size_t pos, std::string *tag) {
    if (s[pos] != '#') return pos;
    std::size_t p = pos + 1;
    bool nonDigit = false;
    while (p < s.size()) {
        std::size_t len = 0;
        const std::uint32_t cp = text::decodeAt(s, p, &len);
        if (!text::isWordChar(cp)) break;
        if (!(cp >= '0' && cp <= '9')) nonDigit = true;
        p += len;
    }
    if (p == pos + 1 || !nonDigit) return pos;
    if (tag) *tag = s.substr(pos + 1, p - pos - 1);
    return p;
}

void appendText(std::vector<DescriptionRun> &runs, const std::string &t) {
    if (t.empty()) return;
    if (!runs.empty() && runs.back().kind == RunKind::Text) runs.back().text += t;
    else runs.push_back(DescriptionRun(RunKind::Text, t));
}

std::string escapeHtml(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\r': break;
        case '\n': out += "<br>"; break;
        default: out += c;
        }
    }
    return out;
}

std::string escapeHref(const std::string &s) {
    std::string out;
    for (char c : s) {
        switch (c) {
        case '"': out += "%22"; break;
        case '<': out += "%3C"; break;
        case '>': out += "%3E"; break;
        case ' ': out += "%20"; break;
        case '\'': out += "%27"; break;
        default: out += c;
        }
    }
    return out;
}

}  // namespace

std::vector<DescriptionRun> descriptionRuns(const std::string &s) {
    std::vector<DescriptionRun> runs;
    std::string pending;
    std::size_t i = 0;
    while (i < s.size()) {
        const std::uint32_t before = previousCodePoint(s, i);
        const bool boundary = i == 0 || !text::isWordChar(before);
        if (boundary) {
            std::string url, tag;
            std::int64_t seconds = -1;
            std::size_t end = matchUrlAt(s, i, &url);
            if (end > i) {
                appendText(runs, pending); pending.clear();
                DescriptionRun r(RunKind::Link, s.substr(i, end - i));
                r.url = url;
                runs.push_back(r);
                i = end;
                continue;
            }
            if (isDigit(s[i]) && before != ':') {
                end = matchTimestampAt(s, i, &seconds);
                if (end > i) {
                    appendText(runs, pending); pending.clear();
                    DescriptionRun r(RunKind::Timestamp, s.substr(i, end - i));
                    r.seekSeconds = seconds;
                    runs.push_back(r);
                    i = end;
                    continue;
                }
            }
            end = matchHashtagAt(s, i, &tag);
            if (end > i) {
                appendText(runs, pending); pending.clear();
                DescriptionRun r(RunKind::Hashtag, s.substr(i, end - i));
                r.hashtag = tag;
                runs.push_back(r);
                i = end;
                continue;
            }
        }
        std::size_t len = 0;
        text::decodeAt(s, i, &len);
        pending += s.substr(i, len);
        i += len;
    }
    appendText(runs, pending);
    return runs;
}

std::vector<DescriptionRun> mergeTextRuns(const std::vector<DescriptionRun> &runs) {
    std::vector<DescriptionRun> out;
    for (const DescriptionRun &r : runs) {
        if (r.text.empty()) continue;
        if (r.kind == RunKind::Text) appendText(out, r.text);
        else out.push_back(r);
    }
    return out;
}

std::string runsText(const std::vector<DescriptionRun> &runs) {
    std::string out;
    for (const DescriptionRun &r : runs) out += r.text;
    return out;
}

std::vector<DescriptionRun> collapseLines(const std::vector<DescriptionRun> &runs, int lines, bool *truncated) {
    std::vector<DescriptionRun> out;
    int newlines = 0;
    bool cut = false;
    std::size_t ri = 0;
    for (; ri < runs.size() && !cut; ++ri) {
        const DescriptionRun &r = runs[ri];
        if (r.kind != RunKind::Text) { out.push_back(r); continue; }
        for (std::size_t p = 0; p < r.text.size(); ++p) {
            if (r.text[p] != '\n') continue;
            if (++newlines >= lines) {
                appendText(out, r.text.substr(0, p));
                // Anything visible after the cut?
                bool more = false;
                for (std::size_t q = p; q < r.text.size() && !more; ++q)
                    more = !std::isspace(static_cast<unsigned char>(r.text[q]));
                for (std::size_t rj = ri + 1; rj < runs.size() && !more; ++rj)
                    for (char c : runs[rj].text) if (!std::isspace(static_cast<unsigned char>(c))) { more = true; break; }
                if (truncated) *truncated = more;
                cut = true;
                break;
            }
        }
        if (!cut) appendText(out, r.text);
    }
    if (!cut && truncated) *truncated = false;
    // Trailing blank lines are not worth showing.
    while (!out.empty() && out.back().kind == RunKind::Text) {
        std::string &t = out.back().text;
        while (!t.empty() && (t.back() == '\n' || t.back() == ' ' || t.back() == '\r')) t.pop_back();
        if (!t.empty()) break;
        out.pop_back();
    }
    return out;
}

std::string runsToStyledText(const std::vector<DescriptionRun> &runs) {
    std::string out;
    for (const DescriptionRun &r : runs) {
        switch (r.kind) {
        case RunKind::Text:
            out += escapeHtml(r.text);
            break;
        case RunKind::Link:
            out += "<a href=\"" + escapeHref(r.url) + "\">" + escapeHtml(r.text) + "</a>";
            break;
        case RunKind::Timestamp:
            out += "<a href=\"seek:" + std::to_string(r.seekSeconds) + "\">" + escapeHtml(r.text) + "</a>";
            break;
        case RunKind::Hashtag:
            out += "<a href=\"hashtag:" + text::percentEncode(r.hashtag) + "\">" + escapeHtml(r.text) + "</a>";
            break;
        }
    }
    return out;
}

LinkAction parseLinkAnchor(const std::string &href) {
    LinkAction a;
    if (href.compare(0, 5, "seek:") == 0) {
        const std::string n = href.substr(5);
        if (n.empty() || n.size() > 9) return a;
        for (char c : n) if (!isDigit(c)) return a;
        a.kind = RunKind::Timestamp;
        a.seekSeconds = std::atoll(n.c_str());
        return a;
    }
    if (href.compare(0, 8, "hashtag:") == 0) {
        const std::string tag = text::percentDecode(href.substr(8));
        if (tag.empty()) return a;
        a.kind = RunKind::Hashtag;
        a.hashtag = tag;
        return a;
    }
    if (startsWithNoCase(href, 0, "https://") || startsWithNoCase(href, 0, "http://")) {
        a.kind = RunKind::Link;
        a.url = href;
    }
    return a;
}

std::string unwrapRedirect(const std::string &url) {
    std::string u = url;
    if (u.compare(0, 2, "//") == 0) u = "https:" + u;
    else if (!u.empty() && u[0] == '/') u = "https://www.youtube.com" + u;
    const char *hosts[] = {"https://www.youtube.com/redirect?", "https://youtube.com/redirect?",
                           "https://m.youtube.com/redirect?"};
    for (const char *h : hosts) {
        if (u.compare(0, std::string(h).size(), h) != 0) continue;
        const std::string query = u.substr(std::string(h).size());
        std::size_t p = 0;
        while (p < query.size()) {
            std::size_t amp = query.find('&', p);
            if (amp == std::string::npos) amp = query.size();
            if (query.compare(p, 2, "q=") == 0) {
                const std::string target = text::percentDecode(query.substr(p + 2, amp - p - 2));
                if (startsWithNoCase(target, 0, "http://") || startsWithNoCase(target, 0, "https://")) return target;
                if (!target.empty()) return "https://" + target;
            }
            p = amp + 1;
        }
    }
    return u;
}

bool isVideoId(const std::string &id) {
    if (id.size() != 11) return false;
    for (char c : id)
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) return false;
    return true;
}

std::string shareUrl(const std::string &videoId, std::int64_t atSeconds) {
    if (!isVideoId(videoId)) return std::string();
    std::string u = "https://youtu.be/" + videoId;
    if (atSeconds > 0) u += "?t=" + std::to_string(atSeconds);
    return u;
}

std::string watchUrl(const std::string &videoId, std::int64_t atSeconds) {
    if (!isVideoId(videoId)) return std::string();
    std::string u = "https://www.youtube.com/watch?v=" + videoId;
    if (atSeconds > 0) u += "&t=" + std::to_string(atSeconds) + "s";
    return u;
}

}}}
