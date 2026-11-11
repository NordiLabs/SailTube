// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "FileNaming.h"

#include <cstdint>
#include <vector>

namespace nordtile { namespace video { namespace downloads {

namespace {

// Strict UTF-8 decoding (no overlongs, no surrogates, <= U+10FFFF). Invalid
// bytes are skipped one at a time.
std::vector<std::uint32_t> decode(const std::string &s) {
    std::vector<std::uint32_t> out;
    out.reserve(s.size());
    std::size_t i = 0;
    const std::size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        std::uint32_t cp = 0;
        std::size_t len = 0;
        std::uint32_t min = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; min = 0x80; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; min = 0x800; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; min = 0x10000; }
        else { ++i; continue; }
        if (i + len > n) { ++i; continue; }
        bool ok = true;
        for (std::size_t k = 1; k < len; ++k) {
            const unsigned char cc = static_cast<unsigned char>(s[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) { ++i; continue; }
        out.push_back(cp);
        i += len;
    }
    return out;
}

void encode(std::uint32_t cp, std::string *out) {
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

// Invisible formatting that can disguise a name (bidi overrides, zero-width
// space, BOM) or that some file systems treat oddly. ZWJ stays for emoji.
bool dropped(std::uint32_t cp) {
    return cp == 0x061C || cp == 0x200B || cp == 0x200E || cp == 0x200F
        || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069)
        || cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || cp == 0xFFFE || cp == 0xFFFF;
}

bool asSpace(std::uint32_t cp) {
    if (cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) return true; // C0, DEL, C1
    switch (cp) {
    case ' ': case '/': case '\\': case '<': case '>': case ':': case '"': case '|': case '?': case '*':
    case 0x00A0: case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000:
        return true;
    default:
        break;
    }
    return cp >= 0x2000 && cp <= 0x200A;
}

bool trimLeading(char c) { return c == ' ' || c == '.' || c == '-'; }
bool trimTrailing(char c) { return c == ' ' || c == '.'; }

void trim(std::string *s) {
    std::size_t b = 0;
    while (b < s->size() && trimLeading((*s)[b])) ++b;
    s->erase(0, b);
    for (;;) {
        while (!s->empty() && trimTrailing((*s)[s->size() - 1])) s->erase(s->size() - 1);
        // A dangling zero-width joiner (U+200D) after a cut.
        if (s->size() >= 3 && s->compare(s->size() - 3, 3, "\xE2\x80\x8D") == 0) { s->erase(s->size() - 3); continue; }
        break;
    }
}

bool fatReserved(char c) {
    return c == '<' || c == '>' || c == ':' || c == '"' || c == '|' || c == '?' || c == '*';
}

} // namespace

std::string sanitizeVideoId(const std::string &id) {
    std::string out;
    for (char c : id) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (ok) out.push_back(c);
        if (out.size() == maxIdBytes) break;
    }
    return out.empty() ? std::string("unknown") : out;
}

std::size_t utf8Prefix(const std::string &s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) return s.size();
    std::size_t i = maxBytes;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    return i;
}

std::string sanitizeTitle(const std::string &title, std::size_t maxBytes) {
    std::string out;
    out.reserve(title.size());
    bool pendingSpace = false;
    for (std::uint32_t cp : decode(title)) {
        if (dropped(cp)) continue;
        if (asSpace(cp)) { pendingSpace = true; continue; }
        if (pendingSpace && !out.empty()) out.push_back(' ');
        pendingSpace = false;
        encode(cp, &out);
    }
    trim(&out);
    if (out.size() > maxBytes) {
        out.resize(utf8Prefix(out, maxBytes));
        trim(&out);
    }
    return out.empty() ? std::string("video") : out;
}

bool allowedExtension(const std::string &ext) {
    return ext == "m4a" || ext == "webm" || ext == "mp4" || ext == "mkv" || ext == "opus" || ext == "jpg";
}

std::string baseName(const std::string &title, const std::string &videoId) {
    const std::string id = sanitizeVideoId(videoId);
    const std::size_t budget = maxNameBytes - maxSuffixBytes - (id.size() + 3);
    const std::string t = sanitizeTitle(title, budget < maxTitleBytes ? budget : maxTitleBytes);
    return t + " [" + id + "]";
}

bool safeFileName(const std::string &name) {
    if (name.empty() || name.size() > maxNameBytes || name == "." || name == "..") return false;
    if (name[0] == '.' || name[0] == '-') return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F || c == '/' || c == '\\' || fatReserved(c)) return false;
    }
    return true;
}

std::string fileName(const std::string &base, const std::string &ext) {
    if (!allowedExtension(ext)) return std::string();
    const std::string name = base + "." + ext;
    return safeFileName(name) && safeFileName(partFileName(name)) ? name : std::string();
}

std::string trackFileName(const std::string &base, Track track, const std::string &ext) {
    if (!allowedExtension(ext)) return std::string();
    const std::string name = base + (track == Track::Video ? ".video." : ".audio.") + ext;
    return safeFileName(name) && safeFileName(partFileName(name)) ? name : std::string();
}

std::string partFileName(const std::string &name) { return name + ".part"; }

std::string thumbnailFileName(const std::string &base) { return base + ".jpg"; }

}}}
