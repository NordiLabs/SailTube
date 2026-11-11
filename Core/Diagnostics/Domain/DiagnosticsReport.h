// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DIA-02: the copyable diagnostics report. Qt-free; the app fills ReportInput from its services
// and shows or copies the text. It is never uploaded (no network code here; NFR-SEC-01 inventory).
//
// Every free-text value passes through redact() before it is written, and the finished text is
// redacted again, so a careless caller cannot leak a signed stream address, a PO token, a
// signature or a session cookie value into it.

#include "ErrorCounters.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace diagnostics {

// One resolve, tap to first frame. -1 = phase did not happen or was not measured.
struct ResolveTimeline {
    std::string path;            // fixed label: cache, fast, extractor, extractor-js, failed
    std::int64_t queueWaitMs = -1;
    std::int64_t postMs = -1;    // player request (fast path) or extractor run
    std::int64_t probeMs = -1;
    std::int64_t prerollMs = -1;
    std::int64_t firstFrameMs = -1;
};

struct ReportInput {
    std::vector<std::pair<std::string, std::string>> versions;  // app, extractor, client definition, helpers
    std::vector<std::string> versionSkew;                        // human-readable skew warnings
    std::string breakerState;                                    // e.g. "closed", "open until +12 min"
    std::string poProviderState;                                 // "not configured" on STORE / when off
    ErrorCounters counters;
    std::vector<ResolveTimeline> timelines;                      // oldest first; the last 10 are shown
    std::string hardwareDecodeProbe;                             // the probe line
    std::string networkFamily;                                   // "ipv4", "ipv6", "dual", "unknown"
    std::vector<std::string> secrets;                            // values that must never appear (cookies, tokens)
};

namespace detail {
inline bool startsWithAt(const std::string &s, std::size_t i, const char *prefix) {
    for (std::size_t k = 0; prefix[k]; ++k)
        if (i + k >= s.size() || s[i + k] != prefix[k]) return false;
    return true;
}
inline bool isUrlChar(char c) {
    return !(c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '"' || c == '\'' || c == '<' || c == '>');
}
}

// Removes: any http(s) URL whose host part contains "googlevideo" (the whole URL), the values of
// sig=, signature=, lsig=, pot=, n= query parameters elsewhere, and every listed secret value.
inline std::string redact(std::string text, const std::vector<std::string> &secrets = {}) {
    for (const std::string &secret : secrets) {
        if (secret.size() < 4) continue;   // never blank out short common substrings
        std::size_t at;
        while ((at = text.find(secret)) != std::string::npos) text.replace(at, secret.size(), "[redacted]");
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        if (detail::startsWithAt(text, i, "http://") || detail::startsWithAt(text, i, "https://")) {
            std::size_t end = i;
            while (end < text.size() && detail::isUrlChar(text[end])) ++end;
            const std::string url = text.substr(i, end - i);
            if (url.find("googlevideo") != std::string::npos) { out += "[media-url]"; i = end; continue; }
        }
        static const char *const keys[] = {"sig=", "signature=", "lsig=", "pot=", "n="};
        bool replaced = false;
        for (const char *key : keys) {
            const bool boundary = i == 0 || text[i - 1] == '&' || text[i - 1] == '?' || text[i - 1] == ' ';
            if (boundary && detail::startsWithAt(text, i, key)) {
                std::size_t end = i + std::char_traits<char>::length(key);
                while (end < text.size() && detail::isUrlChar(text[end]) && text[end] != '&') ++end;
                out += key;
                out += "[redacted]";
                i = end;
                replaced = true;
                break;
            }
        }
        if (!replaced) out += text[i++];
    }
    return out;
}

inline std::string formatMs(std::int64_t ms) {
    if (ms < 0) return "-";
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(ms));
    return buf;
}

// Section headers are fixed so tools and tests can find them.
inline std::vector<std::string> reportSections() {
    return {"[versions]", "[breaker]", "[po-provider]", "[counters]", "[resolves]", "[hardware-decode]", "[network]"};
}

inline std::string buildReport(const ReportInput &in) {
    const std::vector<std::string> &s = in.secrets;
    std::string r = "SailTube diagnostics (local only, never uploaded)\n";
    r += "\n[versions]\n";
    for (const auto &v : in.versions) r += redact(v.first, s) + ": " + redact(v.second, s) + "\n";
    for (const auto &w : in.versionSkew) r += "skew: " + redact(w, s) + "\n";
    if (in.versionSkew.empty()) r += "skew: none\n";
    r += "\n[breaker]\n" + redact(in.breakerState.empty() ? "unknown" : in.breakerState, s) + "\n";
    r += "\n[po-provider]\n" + redact(in.poProviderState.empty() ? "not configured" : in.poProviderState, s) + "\n";
    r += "\n[counters]\n";
    bool any = false;
    for (int a = 0; a < static_cast<int>(Area::Count); ++a) {
        for (int c = 1; c < static_cast<int>(FailureClass::Count); ++c) {
            const std::uint32_t n = in.counters.count(static_cast<Area>(a), static_cast<FailureClass>(c));
            if (!n) continue;
            any = true;
            r += std::string(areaLabel(static_cast<Area>(a))) + " " + classLabel(static_cast<FailureClass>(c)) + ": " +
                 std::to_string(n) + "\n";
        }
    }
    if (!any) r += "no failures\n";
    r += std::string("parser update hint: ") + (in.counters.anyParserUpdateHint() ? "yes" : "no") + "\n";
    r += "\n[resolves]\n";
    r += "path queue_ms post_ms probe_ms preroll_ms first_frame_ms\n";
    const std::size_t first = in.timelines.size() > 10 ? in.timelines.size() - 10 : 0;
    for (std::size_t i = first; i < in.timelines.size(); ++i) {
        const ResolveTimeline &t = in.timelines[i];
        r += redact(t.path.empty() ? "unknown" : t.path, s) + " " + formatMs(t.queueWaitMs) + " " + formatMs(t.postMs) +
             " " + formatMs(t.probeMs) + " " + formatMs(t.prerollMs) + " " + formatMs(t.firstFrameMs) + "\n";
    }
    if (in.timelines.empty()) r += "none yet\n";
    r += "\n[hardware-decode]\n" + redact(in.hardwareDecodeProbe.empty() ? "not probed" : in.hardwareDecodeProbe, s) + "\n";
    r += "\n[network]\n" + redact(in.networkFamily.empty() ? "unknown" : in.networkFamily, s) + "\n";
    return redact(r, s);
}

}}
