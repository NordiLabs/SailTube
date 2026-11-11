// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Per-request authorization value of the web client ("SAPISIDHASH"), pure C++11.
//
// What is documented (docs/ytm_hybrid/02-innertube-auth.md section 3, all public docs):
//  - VERIFIED (ytmusicapi_dart API docs, Nudge PR #138): the value is
//      SAPISIDHASH <unix-seconds>_<sha1hex("<unix-seconds> <cookie> <origin>")>
//    recomputed for every request; the origin is the one the request claims (x-origin).
//  - VERIFIED (yt-dlp PR #395 / ytmusicapi issue #376): the cookie value is SAPISID, with
//    __Secure-3PAPISID as the fallback / the one ytmusicapi uses; both looked identical.
//  - WEAK (two 2026 community PRs): a multi-token form "SAPISIDHASH .. SAPISID1PHASH ..
//    SAPISID3PHASH .." and a "_u" suffix when a user-session id is folded into the hash input
//    (order "<session-id> <ts> <cookie> <origin>"). Behind the version flag below because the
//    scheme is still moving (02 section 9).
//
// TIMESTAMP UNIT DISPUTE: ytmusicapi_dart and Nudge use SECONDS; a widely linked gist for
// photos.google.com uses MILLISECONDS (Date.now()). INFERENCE: Google endpoints may accept
// either or differ per service. For music.youtube.com SECONDS is the default and the
// MILLISECONDS switch exists only so a device test can try it; it is not otherwise used.
//
// INFERENCE (not testable without a live account): which cookie Google really requires per
// token, and the clock-skew tolerance. Nothing here is a security boundary of this app.
#pragma once

#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

enum class SignerVersion {
    V1 = 1,   // single token: SAPISIDHASH <ts>_<hash>                      (default)
    V2 = 2,   // up to three tokens: SAPISIDHASH, SAPISID1PHASH, SAPISID3PHASH
    V3 = 3    // V1 with a user-session id in the hash input and a "_u" suffix
};
enum class TimestampUnit { Seconds, Milliseconds };

struct SignerOptions {
    SignerVersion version = SignerVersion::V1;
    TimestampUnit unit = TimestampUnit::Seconds;
};

struct SigningInput {
    SignerOptions options;
    std::string origin;                 // e.g. "https://music.youtube.com"
    std::int64_t nowUtcSeconds = 0;
    int extraMillis = 0;                // 0..999, only used with TimestampUnit::Milliseconds
    // Cookie values; empty = not present. SECRET.
    std::string sapisid;
    std::string secure1Papisid;
    std::string secure3Papisid;
    std::string userSessionId;          // V3 only; [A-Za-z0-9_-]{1,64}
    void wipe();                        // zeroes the cookie values after use
    ~SigningInput() { wipe(); }
    SigningInput() = default;
    SigningInput(const SigningInput &) = default;
    SigningInput &operator=(const SigningInput &) = default;
};

struct SigningResult {
    bool ok = false;
    std::string value;                  // SECRET-derived: never log
    const char *error = "";             // fixed text, never input
    ~SigningResult();
    SigningResult() = default;
    SigningResult(const SigningResult &) = default;
    SigningResult &operator=(const SigningResult &) = default;
};

class SessionSigner {
public:
    // The whole Authorization header value, or ok=false with a fixed reason.
    static SigningResult sign(const SigningInput &input);
    // https://host[:port], printable ASCII, no path/query/space (guards header injection).
    static bool validOrigin(const std::string &origin);
    // sha1hex over "<ts> <cookie> <origin>" (or with the session id in front when non-empty).
    // Exposed so the vectors can be checked piecewise.
    static std::string hash(const std::string &timestamp, const std::string &cookie,
                            const std::string &origin, const std::string &userSessionId = std::string());
    static const char *versionName(SignerVersion version);
};

}}}}
