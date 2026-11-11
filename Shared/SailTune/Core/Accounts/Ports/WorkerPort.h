// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include <functional>

namespace nordtile { namespace music { namespace accounts { namespace ports {

// Runs blocking work (the Sailfish Secrets vault is synchronous and "worker-only", see
// CredentialVaultPort) off the event-loop thread. Contract for implementers:
//  - ONE serial worker, strict FIFO. The session adapter relies on this ordering so a queued
//    vault write can never run after a later logout/forget removal (docs/ytm_hybrid/06 DD-2).
//  - `work` runs on the worker; `done` is then invoked exactly once on the thread that called
//    run() (the event-loop thread). Neither may throw.
class WorkerPort {
public:
    virtual ~WorkerPort() = default;
    virtual void run(std::function<void()> work, std::function<void()> done) = 0;
};

// Runs both steps synchronously on the caller's thread. Acceptable for unit tests and for the
// first device test only; production composition supplies a real worker thread so a vault
// call that waits for system interaction cannot block the UI.
class InlineWorker final : public WorkerPort {
public:
    void run(std::function<void()> work, std::function<void()> done) override {
        if (work) work();
        if (done) done();
    }
};

}}}}
