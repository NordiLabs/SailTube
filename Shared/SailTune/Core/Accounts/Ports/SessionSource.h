// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "SessionMaterialSinkPort.h"
#include <functional>
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace ports {

// One account-session abstraction with interchangeable sources (decision 3 in IMPL_RULES):
// paste/import (fully implemented), embedded sign-in on Google's own page (device-pending
// spike, implemented by the UI slice), a system-browser source (possible later). A source only
// acquires text and hands it to the sink; it never parses, stores or logs it.
class SessionSource {
public:
    struct Status {
        bool available = false;
        // Honest, fixed English reason when unavailable (UI maps it through YtmSessionMessages).
        std::string reason;
    };
    using Completion = std::function<void(const SessionStageResult &)>;

    virtual ~SessionSource() = default;
    virtual domain::SessionSourceKind kind() const = 0;
    virtual Status status() const = 0;
    // Begins acquiring a session. `done` is invoked once with the sink's answer (or a source
    // failure) unless cancel() intervenes. `sink` outlives the acquisition.
    virtual domain::Result start(SessionMaterialSinkPort &sink, Completion done) = 0;
    // Abandons the acquisition and wipes anything the source still holds. Idempotent.
    virtual void cancel() = 0;
};

}}}}
