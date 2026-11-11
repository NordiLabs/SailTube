// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Turns pasted/imported text into a SessionBundle. Pure C++11 (no Qt), bounded, never echoes
// a value: results carry enums, counts, cookie NAMES and fixed English texts only.
//
// Accepted inputs (docs/ytm_hybrid/02 section 3.1 and 04 section 5):
//  - a Cookie header value or line:      Cookie: SID=..; __Secure-3PAPISID=..   (or just the value)
//  - a request-header block copied from the browser's network tab (Firefox "Copy Request
//    Headers", Chrome "Copy request headers"): `Name: value` lines. Only Cookie,
//    X-Goog-AuthUser and X-Goog-PageId are read; a pasted Authorization header is DISCARDED
//    (it is recomputed per request) and its presence is only counted;
//  - a cURL command ("Copy as cURL (bash)"): -H/--header and -b/--cookie arguments;
//  - a Netscape cookies.txt (yt-dlp style export), including "#HttpOnly_" lines. Only cookies
//    whose domain is sent to music.youtube.com (.youtube.com, youtube.com with the
//    include-subdomains flag, music.youtube.com) are taken; expired ones are skipped when a
//    clock is supplied.
// Not accepted: Windows "cURL (cmd)" with ^ quoting, "Copy as fetch" JavaScript. They are
// rejected as Unrecognized rather than guessed.
#pragma once

#include "../Domain/SessionTypes.h"
#include "../Ports/SessionMaterialSinkPort.h"
#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

struct ImportOptions {
    std::int64_t nowUtcSeconds = 0;        // > 0 enables expiry filtering of cookies.txt entries
    std::size_t maxBytes = 1u << 20;       // 1 MiB: a whole-browser cookies.txt export still fits
    std::size_t maxLineBytes = 32768;
    std::size_t maxLines = 20000;
};

struct ImportResult {
    ports::ImportError error = ports::ImportError::None;
    int errorLine = 0;                     // 1-based, 0 when not tied to a line
    domain::SessionBundle bundle;          // SECRET, valid only when ok()
    ports::SessionStageSummary summary;    // non-secret
    bool ok() const { return error == ports::ImportError::None; }
    void wipe() { bundle.wipe(); }
};

class SessionImportParser {
public:
    // Never throws, never reads beyond `size`. NUL bytes are rejected.
    static ImportResult parse(const char *data, std::size_t size, const ImportOptions &options = ImportOptions());
    static ImportResult parse(const std::string &text, const ImportOptions &options = ImportOptions()) {
        return parse(text.data(), text.size(), options);
    }
    // Fixed English text for an error (never contains input). The UI uses YtmSessionMessages.
    static const char *describe(ports::ImportError error);
    // First 8 hex digits of SHA-1("ytm-session-fp\n" + value): a recognisable, non-reversible tag.
    static std::string fingerprintOf(const std::string &signingCookieValue);
};

}}}}
