// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "SessionSigner.h"
#include "../Domain/SessionTypes.h"
#include "../Ports/SessionCredentialsPort.h"
#include <functional>
#include <mutex>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

// The decrypted session, in memory only (docs/ytm_hybrid/06 DD-11): loaded once by a worker,
// read by the transport through SessionCredentialsPort, wiped on logout/forget/destruction.
//
// Two counters:
//  - generation(): identity of the installed session. Bumped by install() and wipe() only. A
//    response that belongs to an older generation cannot change the session (DD-2: a late
//    write must not resurrect a signed-out session).
//  - revision(): bumped by every content change (install and each applied rotation); used to
//    persist exactly what was merged without losing a concurrent update.
// Thread-safe: every member takes the internal mutex; the dirty observer is called after the
// mutex is released, from the thread that merged.
class SessionHolder final : public ports::SessionCredentialsPort {
public:
    explicit SessionHolder(const SignerOptions &signer = SignerOptions());
    ~SessionHolder() override;
    SessionHolder(const SessionHolder &) = delete;
    SessionHolder &operator=(const SessionHolder &) = delete;

    void setSignerOptions(const SignerOptions &options);
    // V3 signing only; memory only. Empty clears it.
    void setUserSessionId(const std::string &id);

    // Replaces the session (the previous content is wiped) and returns the new generation.
    // `dirty` marks the content as not yet persisted.
    std::uint64_t install(const domain::SessionBundle &bundle, bool dirty);
    // Wipes everything and returns the new generation; later rotations for older generations
    // are dropped.
    std::uint64_t wipe();
    bool empty() const;
    std::uint64_t revision() const;

    struct Snapshot {
        domain::SessionBundle bundle;      // SECRET copy
        std::uint64_t generation = 0;
        std::uint64_t revision = 0;
        bool dirty = false;
    };
    bool snapshot(Snapshot *out) const;    // false when nothing is installed
    // Clears the dirty flag iff the generation AND revision still match (nothing newer exists).
    bool markPersisted(std::uint64_t generation, std::uint64_t revision);
    // Called (outside the lock) after a rotation changed the content.
    void setDirtyObserver(std::function<void()> observer);

    void setDisplayName(const std::string &name);   // UI label, memory only
    std::string displayName() const;

    // SessionCredentialsPort
    bool signedIn() const override;
    std::uint64_t generation() const override;
    bool requestHeaders(const std::string &origin, std::int64_t nowUtcSeconds, ports::SessionRequestHeaders *out) const override;
    ports::RotationOutcome applyRotation(const std::vector<ports::RotatedCookie> &updates, std::uint64_t requestGeneration) override;

private:
    mutable std::mutex mutex_;
    SignerOptions signer_;
    std::string userSessionId_;
    std::string displayName_;
    domain::SessionBundle bundle_;
    std::uint64_t generation_;
    std::uint64_t revision_;
    bool dirty_;
    std::function<void()> observer_;
};

}}}}
