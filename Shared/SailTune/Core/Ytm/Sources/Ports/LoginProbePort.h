// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The small port the multi-login / brand-account enumeration consumes. Pure C++11, generic: it
// knows nothing about YouTube's wire format. The ytm-remote slice's facade (or any other class
// that can send one authenticated "list the identities of this login" request) implements it.
//
// Contract for an implementer (what AccountEnumerator and its tests rely on):
//  * probeLogin(authUser, done) sends ONE request for the browser login slot `authUser`
//    (0..7), WITHOUT a brand-account page id, and calls `done` exactly once, on the caller's
//    event-loop thread. It may call `done` before probeLogin returns.
//  * Status::LoggedOut means the answer says "no signed-in user in this slot". Status::Network
//    means the request failed before an answer (DNS, TLS, timeout, cancelled by the
//    transport): it must NOT be reported as LoggedOut. Status::Failed is anything else
//    (unexpected shape, blocked, rate limited, protocol drift).
//  * `entries` holds the identities the answer lists (the primary channel and the brand
//    channels of that login). The port filters nothing; the enumerator bounds and de-duplicates.
//  * cancel() abandons a probe in flight; a late `done` after cancel() is harmless (ignored).
//  * No header, cookie or body content in any field; entries carry only ids and display names.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace ports {

struct LoginEntry {
    std::string gaiaId;         // may be empty
    std::string datasyncId;     // may be empty
    std::string name;           // display name; memory only, may be empty
    std::string pageId;         // brand-channel page id; empty = the primary channel of the login
};

struct LoginProbeResult {
    enum class Status { Ok, LoggedOut, Network, Failed };
    Status status = Status::Failed;
    std::vector<LoginEntry> entries;
};

class LoginProbePort {
public:
    typedef std::function<void(const LoginProbeResult &)> Done;
    virtual ~LoginProbePort() = default;
    virtual void probeLogin(int authUser, Done done) = 0;
    virtual void cancel() = 0;
};

}}}}}
