// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "../Domain/SessionTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace accounts { namespace ports {

// Per-request header values for music.youtube.com. EVERY field here except `generation` is
// secret or derived from a secret: never log, persist or copy it beyond the request.
struct SessionRequestHeaders {
    std::string cookie;          // "Cookie:" value, "name=value; name=value" (allow-listed names only)
    std::string authorization;   // "Authorization:" value, e.g. "SAPISIDHASH <ts>_<sha1hex>"
    std::string origin;          // value for both "Origin" and "x-origin"
    std::string authUser;        // "X-Goog-AuthUser" (never empty, "0" by default)
    std::string pageId;          // "X-Goog-PageId" for brand accounts; empty = do not send
    // Identity of the session these headers were derived from (see applyRotation).
    std::uint64_t generation = 0;
    void wipe() {
        domain::wipeString(cookie); domain::wipeString(authorization); domain::wipeString(origin);
        domain::wipeString(authUser); domain::wipeString(pageId); generation = 0;
    }
    ~SessionRequestHeaders() { wipe(); }
    SessionRequestHeaders() = default;
    SessionRequestHeaders(const SessionRequestHeaders &) = default;
    SessionRequestHeaders &operator=(const SessionRequestHeaders &) = default;
};

// One cookie update seen in a Set-Cookie header of a response. The transport applies the
// domain/path rules (only cookies for .youtube.com / music.youtube.com) before calling.
struct RotatedCookie {
    std::string name;
    std::string value;           // SECRET
    bool deleted = false;        // Max-Age=0 / Expires in the past / empty value
};

enum class RotationOutcome {
    Applied,           // at least one allow-listed cookie changed; persistence has been scheduled
    NoChange,          // nothing relevant (unknown names, identical values, refused deletions)
    StaleGeneration,   // the session was replaced or wiped since the request was built: dropped
    NotSignedIn        // nothing installed: dropped
};

// The ONLY door through which the authenticated transport touches the session. The adapter
// (InnerTubeSessionAuthorization) owns the implementation; the transport never sees the
// vault, the grant store or the raw bundle. Thread-safe: it may be called from the event-loop
// thread while a worker persists.
class SessionCredentialsPort {
public:
    virtual ~SessionCredentialsPort() = default;
    virtual bool signedIn() const = 0;
    // Current identity of the installed session; changes on install, wipe and logout.
    virtual std::uint64_t generation() const = 0;
    // Builds fresh headers (the signature depends on `nowUtcSeconds`) for an `origin` that
    // must be https and a plain host, normally "https://music.youtube.com". False when no
    // usable session is installed or the origin is not acceptable.
    virtual bool requestHeaders(const std::string &origin, std::int64_t nowUtcSeconds,
                                SessionRequestHeaders *out) const = 0;
    // Merges Set-Cookie updates from a SUCCESSFUL authenticated response. `requestGeneration`
    // is SessionRequestHeaders::generation of the request that produced the response, so a
    // late response cannot resurrect or modify a session that was logged out or replaced.
    // Only allow-listed names are merged; deleting a signing cookie is refused (the next probe
    // classifies the session; a rotation response is not a sign-out signal we act on).
    virtual RotationOutcome applyRotation(const std::vector<RotatedCookie> &updates,
                                          std::uint64_t requestGeneration) = 0;
};

}}}}
