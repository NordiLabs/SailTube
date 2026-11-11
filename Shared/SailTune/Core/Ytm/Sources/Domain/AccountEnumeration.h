// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Multi-login and brand-account enumeration as a pure helper (SPEC FINTUNE_SPEC.md 4.3).
// Pure C++11, no Qt, no network of its own: it drives a ports::LoginProbePort.
//
// Policy (SPEC, VERIFIED in the reference app's behaviour, restated in our words):
//  * The browser keeps up to eight Google logins in slots 0..7 (X-Goog-AuthUser). The helper
//    probes the slots in order, one request at a time, each WITHOUT a brand-account page id.
//  * It stops at the first slot that is logged out, lists nothing, or lists only identities
//    already seen (Google keeps answering with the last valid login when the slot is beyond
//    the last one). It also stops after slot 7: a login outside 0..7 cannot be selected.
//  * Identities are de-duplicated by (Gaia id, else data-sync id, else display name) plus the
//    page id. The provider's own "selected" marker is never consulted: the caller keeps the
//    user's choice.
//  * A network failure on the FIRST probe is "unreachable", never "signed out". On a later probe
//    it ends the walk with what was found so far, flagged `incomplete`.
//  * Every collection and string is bounded; entries that cannot be used (a page id that is not
//    1-32 digits) are dropped.
#pragma once

#include "../Ports/LoginProbePort.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace domain {

struct EnumerationLimits {
    enum {
        kSlots = 8,             // authuser 0..7
        kEntriesPerProbe = 32,
        kMaxChoices = 64,
        kMaxIdBytes = 64,
        kMaxNameBytes = 120
    };
};

// One thing the user can choose: a login slot and, for a brand channel, its page id.
struct AccountChoice {
    int authUser = 0;           // value for X-Goog-AuthUser / SelectionOptions::authUser
    std::string pageId;         // empty = the primary channel
    std::string label;          // display name (memory only); may be empty
    std::string key;            // the de-duplication key (opaque, may contain the name: never log it)
    bool primary = true;        // pageId.empty()
};

enum class EnumerationStatus {
    Ok,             // at least one choice
    SignedOut,      // slot 0 says nobody is signed in
    Unreachable,    // the network failed on slot 0: this says nothing about the sign-in
    NoAccounts,     // slot 0 answered but listed nothing usable: use the defaults (login 0, primary)
    Failed          // slot 0 answered with something unexpected
};

enum class StopReason { None, LoggedOut, NoItems, OnlyDuplicates, SlotLimit, Network, Failed, Cap };

struct EnumerationResult {
    EnumerationStatus status = EnumerationStatus::Failed;
    std::vector<AccountChoice> choices;
    int probes = 0;                 // requests issued
    StopReason stop = StopReason::None;
    // True when the list may be shorter than the real one: a later probe failed or a cap was hit.
    // Reaching slot 7 is reported as stop == SlotLimit only (informational: eight logins is the maximum
    // the browser keeps and the only ones that can be selected).
    bool incomplete = false;
    int droppedEntries = 0;         // unusable or over-cap entries
};

class AccountEnumerator {
public:
    typedef std::function<void(const EnumerationResult &)> Done;
    explicit AccountEnumerator(ports::LoginProbePort &port);
    ~AccountEnumerator();
    AccountEnumerator(const AccountEnumerator &) = delete;
    AccountEnumerator &operator=(const AccountEnumerator &) = delete;

    // False when a walk is already running. `done` is called once, possibly before start returns
    // (a port may answer synchronously), never after cancel() or destruction.
    bool start(Done done);
    // Abandons the walk and the probe in flight; `done` is not called. Idempotent.
    void cancel();
    bool busy() const;

    // Exposed for the tests.
    static std::string dedupKey(const ports::LoginEntry &entry);
    static bool usablePageId(const std::string &pageId);

private:
    struct State;
    ports::LoginProbePort &port_;
    std::shared_ptr<State> state_;
    static bool absorb(State &state, const ports::LoginProbeResult &result);
    static void pump(std::shared_ptr<State> state);
    static void answer(std::shared_ptr<State> state, std::uint64_t ticket, const ports::LoginProbeResult &result);
};

}}}}}
