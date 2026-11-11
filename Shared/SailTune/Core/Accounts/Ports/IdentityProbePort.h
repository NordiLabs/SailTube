// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
#include "SessionCredentialsPort.h"
#include <functional>

namespace nordtile { namespace music { namespace accounts { namespace ports {

using ProbeId = std::uint64_t;

struct IdentityProbeResult {
    // Classified by the transport with SessionExpiryDetector (design 3.6).
    domain::SessionVerdict verdict = domain::SessionVerdict::Transient;
    // Valid only when verdict == Ok. A provider-stable identifier of the signed-in identity
    // (channel id, or data-sync id for a brand account): NEVER an e-mail address and never the
    // display name. The adapter hashes it before it becomes AccountKey::subject.
    std::string stableId;
    // Optional label for the UI ("Signed in as ..."). Kept in memory only, never persisted.
    std::string displayName;
    // Diagnostic only (never shown with provider text): HTTP status or 0.
    int httpStatus = 0;
    // Additive (ytm-remote slice; docs/ytm_hybrid/impl/parsers.md 2.2 asks the session slice to pin
    // these with the grant). Both are optional and default to "nothing known".
    //  identityKind  fixed vocabulary ("channel", "datasync", "handle") naming what stableId was
    //                derived from. Stored with the grant record; never shown, never logged with a value.
    //  alternateIds  provider-stable ids derived from the OTHER identifiers present in the same answer
    //                (same rules as stableId: opaque, printable, <= 256 characters, never an e-mail or a
    //                name). The adapter hashes each one exactly like stableId and stores the hashes, so
    //                a later answer that is only identifiable through another identifier is recognised as
    //                the SAME account instead of being refused as a different one.
    std::string identityKind;
    std::vector<std::string> alternateIds;
};
using IdentityProbeCompletion = std::function<void(ProbeId, const IdentityProbeResult &)>;

// Implemented by the TRANSPORT slice (the only class that touches the network). The adapter
// calls probe() at sign-in, at every refresh() (app start and before sync runs) and expects:
//  - one lightweight authenticated request to music.youtube.com (account identity), built from
//    credentials.requestHeaders(); host pinned, redirects off, no cookie jar;
//  - Set-Cookie updates of a successful answer handed to credentials.applyRotation(...);
//  - completion exactly once, on the caller's event-loop thread, also after cancel() is NOT
//    required (the adapter ignores late completions);
//  - no header, cookie or body content in any error, log line or exception text (invariant I6).
// `credentials` outlives the probe (it lives inside the adapter).
class IdentityProbePort {
public:
    virtual ~IdentityProbePort() = default;
    virtual void probe(ProbeId id, SessionCredentialsPort &credentials, IdentityProbeCompletion done) = 0;
    virtual void cancel(ProbeId id) = 0;
};

}}}}
