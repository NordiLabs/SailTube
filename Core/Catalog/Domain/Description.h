// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The video description on the watch page (DSC-14). Qt-free, UTF-8.
//
//  * A description is split into runs: plain text, links (tappable), timestamps (seek to
//    seconds) and hashtags (MAY open search). The watch-next answer already marks most of them
//    (attributedDescription.commandRuns); the infrastructure maps those and calls
//    descriptionRuns() for the unmarked text in between, and for plain-text descriptions.
//  * The page shows the description collapsed to 5 lines; collapseLines() cuts the logical
//    lines, the view limits the wrapped ones.
//  * Share and copy-link URLs: https://youtu.be/<id>, with ?t=<s> at the current time.

#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

enum class RunKind { Text, Link, Timestamp, Hashtag };

struct DescriptionRun {
    DescriptionRun() {}
    DescriptionRun(RunKind k, const std::string &t) : kind(k), text(t) {}
    RunKind kind = RunKind::Text;
    std::string text;               // what is shown
    std::string url;                // Link: absolute target
    std::int64_t seekSeconds = -1;  // Timestamp: seek target
    std::string hashtag;            // Hashtag: the tag without '#'
};

const int kCollapsedDescriptionLines = 5;

// Plain text -> runs (URLs with http(s):// or www., `[h:]mm:ss` timestamps, #hashtags).
std::vector<DescriptionRun> descriptionRuns(const std::string &text);

// Joins adjacent text runs; drops empty ones.
std::vector<DescriptionRun> mergeTextRuns(const std::vector<DescriptionRun> &runs);

// Plain text of the runs.
std::string runsText(const std::vector<DescriptionRun> &runs);

// The first `lines` logical lines of the runs (cut inside a run when needed); `truncated` tells
// whether anything was left out.
std::vector<DescriptionRun> collapseLines(const std::vector<DescriptionRun> &runs, int lines, bool *truncated = nullptr);

// Qt StyledText for a Label: escaped text, <br> for newlines, <a href> for the tappable runs:
// links keep their URL, timestamps use "seek:<seconds>", hashtags "hashtag:<tag>".
std::string runsToStyledText(const std::vector<DescriptionRun> &runs);

// What an anchor of runsToStyledText means.
struct LinkAction {
    RunKind kind = RunKind::Text;   // Text = not one of ours / invalid
    std::string url;
    std::int64_t seekSeconds = -1;
    std::string hashtag;
};
LinkAction parseLinkAnchor(const std::string &href);

// youtube.com/redirect?...&q=<target> -> <target>; other URLs unchanged. Protocol-relative and
// site-relative ("/watch?v=") URLs are made absolute on www.youtube.com.
std::string unwrapRedirect(const std::string &url);

// 11-character YouTube video id ([A-Za-z0-9_-]).
bool isVideoId(const std::string &id);

// https://youtu.be/<id>, plus ?t=<s> when `atSeconds` > 0. Empty for an invalid id.
std::string shareUrl(const std::string &videoId, std::int64_t atSeconds = 0);
// https://www.youtube.com/watch?v=<id>, plus &t=<s>s when `atSeconds` > 0 (open in browser).
std::string watchUrl(const std::string &videoId, std::int64_t atSeconds = 0);

// Text helpers shared with the chapter rebuild.
namespace text {
// Decodes the code point at `pos` (invalid bytes decode as themselves); `len` gets its size.
std::uint32_t decodeAt(const std::string &s, std::size_t pos, std::size_t *len);
// A "word" character: ASCII letters/digits/underscore, and non-ASCII letters (any code point
// outside the punctuation, symbol, emoji, space and combining-mark blocks).
bool isWordChar(std::uint32_t cp);
bool isSpace(std::uint32_t cp);
// Length in bytes of an emoji keycap at `pos` ([0-9#*] U+FE0F? U+20E3), else 0.
std::size_t keycapAt(const std::string &s, std::size_t pos);
std::string percentEncode(const std::string &s);
std::string percentDecode(const std::string &s);
}

}}}
