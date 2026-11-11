// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "../Domain/Account.h"
#include <functional>

namespace nordtile { namespace music { namespace accounts { namespace ports {

using OperationId = std::uint64_t;
using AuthorizationCompletion = std::function<void(OperationId, const domain::Grant &, const domain::Result &)>;

struct LogoutResult {
    domain::Result result;
    // A remote revocation failure does not imply local deletion failed.
    bool localCredentialsRemoved = false;
    bool remoteGrantRevoked = false;
};
using LogoutCompletion = std::function<void(OperationId, const LogoutResult &)>;

class AuthorizationPort {
public:
    virtual ~AuthorizationPort() = default;
    virtual bool available() const = 0;
    virtual std::string unavailabilityReason() const = 0;
    // All operations and callbacks run on the caller's event-loop thread.
    // IDs are supplied by the service, so even synchronous failures are safe.
    // A production adapter must securely persist credentials BEFORE success;
    // verify state, PKCE, identity and granted scopes; redact all diagnostics.
    virtual void authorize(OperationId, AuthorizationCompletion) = 0;
    virtual void refresh(OperationId, const domain::Grant &, AuthorizationCompletion) = 0;
    // Must remove pending exchange material and stop network work. A cancelled
    // authorize may never persist new credentials after cancel() returns.
    // Cancelling logout must still complete local credential removal.
    virtual void cancel(OperationId) = 0;
    // Always attempt local removal, even if network revocation fails. Never
    // claim remoteGrantRevoked on a local-only/offline logout.
    virtual void logout(OperationId, const domain::Grant &, LogoutCompletion) = 0;
    // "Forget account": like logout(), but also wipes every local residue this adapter may
    // own even when no grant is known (a crash, a lost or corrupt grant record). Idempotent;
    // must work while available() is false (a kill switch must never block local deletion).
    // The default keeps older adapters unchanged: with a grant it is logout(); without one
    // there is nothing this adapter can be sure to own, so it reports local removal as done.
    virtual void forget(OperationId id, const domain::Grant &grant, LogoutCompletion completion) {
        if (grant.account.valid()) { logout(id, grant, completion); return; }
        LogoutResult result;
        result.localCredentialsRemoved = true;
        if (completion) completion(id, result);
    }
};

class ClockPort {
public:
    virtual ~ClockPort() = default;
    virtual std::int64_t nowUtcSeconds() const = 0;
};

}}}}
