// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Chapters.h"
#include "Description.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace nordtile { namespace video { namespace catalog {

namespace {

bool isDigit(char c) { return c >= '0' && c <= '9'; }

std::size_t previousStart(const std::string &s, std::size_t pos) {
    std::size_t p = pos - 1;
    while (p > 0 && (static_cast<unsigned char>(s[p]) & 0xC0) == 0x80) --p;
    return p;
}

// Separators trimmed from the title side that touched the timestamp (and both title edges).
bool isLeadingSeparator(std::uint32_t cp) {
    if (text::isSpace(cp)) return true;
    if (cp < 0x80) return std::strchr("-:|~)]}>.,;/\\*=", static_cast<int>(cp)) != nullptr && cp != 0;
    return cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || cp == 0x00B7 || cp == 0x2022 || cp == 0x30FB ||
           cp == 0x3011 || cp == 0x300D || cp == 0xFF09 || cp == 0x2192 || cp == 0x301C || cp == 0xFF5E;
}

bool isTrailingSeparator(std::uint32_t cp) {
    if (text::isSpace(cp)) return true;
    if (cp < 0x80) return std::strchr("-:|~([{<,;/\\*=", static_cast<int>(cp)) != nullptr && cp != 0;
    return cp == 0x2013 || cp == 0x2014 || cp == 0x2015 || cp == 0x00B7 || cp == 0x2022 || cp == 0x30FB ||
           cp == 0x3010 || cp == 0x300C || cp == 0xFF08 || cp == 0x2190 || cp == 0x301C || cp == 0xFF5E;
}

std::string trimTitle(const std::string &t) {
    std::size_t b = 0, e = t.size();
    while (b < e) {
        std::size_t len = 0;
        const std::uint32_t cp = text::decodeAt(t, b, &len);
        // VS16 / ZWJ left over from a decoration.
        if (!isLeadingSeparator(cp) && cp != 0xFE0F && cp != 0x200D && cp != 0x20E3) break;
        b += len;
    }
    while (e > b) {
        const std::size_t p = previousStart(t, e);
        const std::uint32_t cp = text::decodeAt(t, p, nullptr);
        if (!isTrailingSeparator(cp)) break;
        e = p;
    }
    return t.substr(b, e - b);
}

// The line without its own indentation and trailing whitespace (those are not decorations).
std::string trimSpaces(const std::string &line) {
    std::size_t b = 0, e = line.size();
    while (b < e) {
        std::size_t len = 0;
        if (!text::isSpace(text::decodeAt(line, b, &len))) break;
        b += len;
    }
    while (e > b) {
        const std::size_t p = previousStart(line, e);
        if (!text::isSpace(text::decodeAt(line, p, nullptr))) break;
        e = p;
    }
    return line.substr(b, e - b);
}

// A timestamp exactly spanning [pos, end): seconds, else -1.
std::int64_t timestampSpan(const std::string &s, std::size_t pos, std::size_t end) {
    if (end <= pos) return -1;
    for (std::size_t i = pos; i < end; ++i) if (!isDigit(s[i]) && s[i] != ':') return -1;
    return parseTimestamp(s.substr(pos, end - pos));
}

// Timestamp near the line start: skips up to 8 non-word code points (keycaps count by their
// code points), then needs a timestamp followed by a non-digit.
bool matchAtStart(const std::string &line, ChapterMark &out) {
    std::size_t p = 0;
    int decorations = 0;
    while (p < line.size()) {
        const std::size_t keycap = text::keycapAt(line, p);
        if (keycap) {
            std::size_t q = p;
            while (q < p + keycap) { std::size_t len = 0; text::decodeAt(line, q, &len); q += len; ++decorations; }
            p += keycap;
        } else {
            std::size_t len = 0;
            const std::uint32_t cp = text::decodeAt(line, p, &len);
            if (text::isWordChar(cp)) break;
            p += len;
            ++decorations;
        }
        if (decorations > kMaxChapterDecoration) return false;
    }
    if (p >= line.size() || !isDigit(line[p])) return false;
    std::size_t e = p;
    while (e < line.size() && (isDigit(line[e]) || line[e] == ':')) ++e;
    while (e > p && line[e - 1] == ':') --e;   // "0:00:" -> "0:00"
    const std::int64_t start = timestampSpan(line, p, e);
    if (start < 0) return false;
    if (e < line.size()) {
        std::size_t len = 0;
        const std::uint32_t next = text::decodeAt(line, e, &len);
        if (next < 0x80 && text::isWordChar(next)) return false;   // "0:00am", "1:23x"
    }
    out.startSeconds = start;
    out.title = trimTitle(line.substr(e));
    return true;
}

// Timestamp near the line end: skips up to 8 non-word code points backwards (keycaps included).
bool matchAtEnd(const std::string &line, ChapterMark &out) {
    std::size_t e = line.size();
    int decorations = 0;
    while (e > 0) {
        const std::size_t p = previousStart(line, e);
        const std::uint32_t cp = text::decodeAt(line, p, nullptr);
        if (cp == 0x20E3) {
            // Keycap: [0-9#*] VS16? U+20E3.
            std::size_t q = p;
            int n = 1;
            if (q > 0) {
                const std::size_t r = previousStart(line, q);
                if (text::decodeAt(line, r, nullptr) == 0xFE0F) { q = r; ++n; }
            }
            if (q > 0 && text::keycapAt(line, q - 1)) { q = q - 1; ++n; }
            decorations += n;
            e = q;
        } else {
            if (text::isWordChar(cp)) break;
            ++decorations;
            e = p;
        }
        if (decorations > kMaxChapterDecoration) return false;
    }
    if (e == 0 || !isDigit(line[e - 1])) return false;
    std::size_t b = e;
    while (b > 0 && (isDigit(line[b - 1]) || line[b - 1] == ':')) --b;
    while (b < e && line[b] == ':') ++b;
    const std::int64_t start = timestampSpan(line, b, e);
    if (start < 0) return false;
    if (b > 0) {
        const std::size_t p = previousStart(line, b);
        const std::uint32_t before = text::decodeAt(line, p, nullptr);
        if (before < 0x80 && text::isWordChar(before)) return false;   // "abc1:23"
    }
    out.startSeconds = start;
    out.title = trimTitle(line.substr(0, b));
    return true;
}

}  // namespace

std::int64_t parseTimestamp(const std::string &t) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= t.size(); ++i) {
        if (i == t.size() || t[i] == ':') { parts.push_back(t.substr(start, i - start)); start = i + 1; }
        else if (!isDigit(t[i])) return -1;
    }
    if (parts.size() < 2 || parts.size() > 3) return -1;
    for (const std::string &p : parts) if (p.empty() || p.size() > 4) return -1;
    const std::string &sec = parts.back();
    if (sec.size() != 2) return -1;
    const std::int64_t s = std::atoll(sec.c_str());
    if (s >= 60) return -1;
    if (parts.size() == 3) {
        const std::string &min = parts[1];
        if (min.size() != 2) return -1;
        const std::int64_t m = std::atoll(min.c_str());
        if (m >= 60 || parts[0].size() > 2) return -1;
        return std::atoll(parts[0].c_str()) * 3600 + m * 60 + s;
    }
    if (parts[0].size() > 3) return -1;
    return std::atoll(parts[0].c_str()) * 60 + s;
}

std::string formatTimestamp(std::int64_t seconds) {
    if (seconds < 0) seconds = 0;
    const std::int64_t h = seconds / 3600, m = (seconds % 3600) / 60, s = seconds % 60;
    auto two = [](std::int64_t v) { return (v < 10 ? std::string("0") : std::string()) + std::to_string(v); };
    if (h > 0) return std::to_string(h) + ":" + two(m) + ":" + two(s);
    return std::to_string(m) + ":" + two(s);
}

std::vector<ChapterMark> chapterCandidates(const std::string &description) {
    std::vector<ChapterMark> out;
    std::size_t start = 0;
    while (start <= description.size()) {
        std::size_t end = description.find('\n', start);
        if (end == std::string::npos) end = description.size();
        std::string line = trimSpaces(description.substr(start, end - start));
        ChapterMark c;
        if ((matchAtStart(line, c) || matchAtEnd(line, c)) && !c.title.empty()) out.push_back(c);
        start = end + 1;
    }
    return out;
}

ChapterRejection validateDescriptionChapters(const std::vector<ChapterMark> &chapters, std::int64_t durationSeconds) {
    if (static_cast<int>(chapters.size()) < kMinDescriptionChapters) return ChapterRejection::TooFew;
    if (chapters.front().startSeconds != 0) return ChapterRejection::FirstNotZero;
    for (std::size_t i = 1; i < chapters.size(); ++i) {
        const std::int64_t gap = chapters[i].startSeconds - chapters[i - 1].startSeconds;
        if (gap <= 0) return ChapterRejection::NotIncreasing;
        if (gap < kMinChapterGapSeconds) return ChapterRejection::GapTooSmall;
    }
    if (durationSeconds > 0 && chapters.back().startSeconds >= durationSeconds) return ChapterRejection::PastDuration;
    return ChapterRejection::None;
}

std::vector<ChapterMark> chaptersFromDescription(const std::string &description, std::int64_t durationSeconds,
                                             ChapterRejection *why) {
    const std::vector<ChapterMark> candidates = chapterCandidates(description);
    const ChapterRejection r = validateDescriptionChapters(candidates, durationSeconds);
    if (why) *why = r;
    if (r != ChapterRejection::None) return std::vector<ChapterMark>();
    return finalizeChapters(candidates, durationSeconds);
}

std::vector<ChapterMark> finalizeChapters(std::vector<ChapterMark> chapters, std::int64_t durationSeconds) {
    std::stable_sort(chapters.begin(), chapters.end(),
                     [](const ChapterMark &a, const ChapterMark &b) { return a.startSeconds < b.startSeconds; });
    std::vector<ChapterMark> out;
    for (const ChapterMark &c : chapters) {
        if (c.startSeconds < 0) continue;
        if (!out.empty() && out.back().startSeconds == c.startSeconds) continue;
        out.push_back(c);
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        if (i + 1 < out.size()) out[i].endSeconds = out[i + 1].startSeconds;
        else out[i].endSeconds = durationSeconds > out[i].startSeconds ? durationSeconds : -1;
    }
    return out;
}

std::vector<ChapterMark> chooseChapters(const std::vector<ChapterMark> &responseMarkers, const std::string &description,
                                    std::int64_t durationSeconds, bool *fromResponse) {
    if (!responseMarkers.empty()) {
        if (fromResponse) *fromResponse = true;
        return finalizeChapters(responseMarkers, durationSeconds);
    }
    if (fromResponse) *fromResponse = false;
    return chaptersFromDescription(description, durationSeconds);
}

int chapterAt(const std::vector<ChapterMark> &chapters, double positionSeconds) {
    if (chapters.empty()) return -1;
    int index = 0;
    for (std::size_t i = 0; i < chapters.size(); ++i)
        if (static_cast<double>(chapters[i].startSeconds) <= positionSeconds) index = static_cast<int>(i);
        else break;
    return index;
}

}}}
