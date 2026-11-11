// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountEnumeration.h"
#include <set>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace domain {

struct AccountEnumerator::State {
    ports::LoginProbePort *port = nullptr;
    bool running = false;
    bool waiting = false;       // a probe is in flight
    bool pumping = false;       // the issue loop is on the stack (an answer may arrive re-entrantly)
    bool needProbe = false;
    std::uint64_t ticket = 0;   // one number per request and per abandonment: only the answer to the current request is accepted
    int slot = 0;
    Done done;
    EnumerationResult result;
    std::set<std::string> seen;
};

namespace {

bool printableId(const std::string &s) {
    if (s.empty() || s.size() > static_cast<std::size_t>(EnumerationLimits::kMaxIdBytes)) return false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x21 || c > 0x7e) return false;
    }
    return true;
}

// Display name: control characters become blanks, the length is bounded on a UTF-8 boundary.
std::string cleanName(const std::string &in) {
    const std::size_t cap = static_cast<std::size_t>(EnumerationLimits::kMaxNameBytes);
    std::string out;
    bool cut = false;
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (out.size() >= cap) { cut = true; break; }
        const unsigned char c = static_cast<unsigned char>(in[i]);
        out += (c < 0x20 || c == 0x7f) ? ' ' : static_cast<char>(c);
    }
    if (cut) {
        // never leave half a multi-byte sequence at the end: drop trailing continuation bytes and an orphaned lead byte
        while (!out.empty() && (static_cast<unsigned char>(out[out.size() - 1]) & 0xC0) == 0x80) out.erase(out.size() - 1);
        if (!out.empty() && static_cast<unsigned char>(out[out.size() - 1]) >= 0xC0) out.erase(out.size() - 1);
    }
    std::size_t b = 0, e = out.size();
    while (b < e && out[b] == ' ') ++b;
    while (e > b && out[e - 1] == ' ') --e;
    return out.substr(b, e - b);
}

}  // namespace

std::string AccountEnumerator::dedupKey(const ports::LoginEntry &entry) {
    std::string base;
    if (!entry.gaiaId.empty()) base = "g:" + entry.gaiaId;
    else if (!entry.datasyncId.empty()) base = "d:" + entry.datasyncId;
    else base = "n:" + entry.name;
    return base + "|" + entry.pageId;
}

bool AccountEnumerator::usablePageId(const std::string &pageId) {
    if (pageId.empty() || pageId.size() > 32) return false;
    for (std::size_t i = 0; i < pageId.size(); ++i) if (pageId[i] < '0' || pageId[i] > '9') return false;
    return true;
}

AccountEnumerator::AccountEnumerator(ports::LoginProbePort &port) : port_(port), state_(new State) {
    state_->port = &port_;
}

AccountEnumerator::~AccountEnumerator() {
    const std::shared_ptr<State> s = state_;
    const bool wasWaiting = s->waiting;
    s->running = false; s->waiting = false; s->needProbe = false; ++s->ticket;
    s->done = Done();
    // The port is told only while it can still be reached; a probe completion that arrives later
    // finds a different ticket and does nothing.
    if (wasWaiting) port_.cancel();
    s->port = nullptr;
}

bool AccountEnumerator::busy() const { return state_->running; }

bool AccountEnumerator::start(Done done) {
    State &s = *state_;
    if (s.running) return false;
    s.running = true; s.waiting = false; s.needProbe = true;
    s.slot = 0;
    s.done = std::move(done);
    s.result = EnumerationResult();
    s.seen.clear();
    pump(state_);
    return true;
}

void AccountEnumerator::cancel() {
    State &s = *state_;
    if (!s.running) return;
    const bool wasWaiting = s.waiting;
    s.running = false; s.waiting = false; s.needProbe = false; ++s.ticket;
    s.done = Done();
    if (wasWaiting && s.port) s.port->cancel();
}

// The issue loop. A port may answer synchronously: then answer() runs inside probeLogin(), sets
// needProbe and calls pump() again, which returns at once because the outer loop is still on the
// stack and picks the request up. That keeps the stack flat however many slots answer at once.
// `s` is taken BY VALUE: a completion callback may destroy the enumerator (and with it the member that
// `s` would otherwise refer to) while this function is still on the stack.
void AccountEnumerator::pump(std::shared_ptr<State> s) {
    if (s->pumping) return;
    s->pumping = true;
    while (s->running && s->needProbe && s->port) {
        s->needProbe = false;
        s->waiting = true;
        const std::uint64_t ticket = ++s->ticket;
        ports::LoginProbePort *port = s->port;
        const std::shared_ptr<State> keep = s;
        port->probeLogin(s->slot, [keep, ticket](const ports::LoginProbeResult &r) { AccountEnumerator::answer(keep, ticket, r); });
    }
    s->pumping = false;
}

void AccountEnumerator::answer(std::shared_ptr<State> s, std::uint64_t ticket, const ports::LoginProbeResult &r) {
    if (!s->running || s->ticket != ticket || !s->waiting) return;      // late, duplicate, or from an abandoned walk
    s->waiting = false;
    if (absorb(*s, r)) {
        ++s->slot;
        s->needProbe = true;
        pump(s);
        return;
    }
    // finished
    EnumerationResult result = s->result;
    if (!result.choices.empty()) result.status = EnumerationStatus::Ok;
    Done done = std::move(s->done);
    s->done = Done();
    s->running = false;
    if (done) done(result);     // may start a new walk or destroy the enumerator: nothing of `s` is used after this
}

// Absorbs one answer. Returns true when the next slot should be probed.
bool AccountEnumerator::absorb(State &s, const ports::LoginProbeResult &r) {
    EnumerationResult &res = s.result;
    ++res.probes;
    const bool first = (s.slot == 0);
    typedef ports::LoginProbeResult::Status St;
    switch (r.status) {
    case St::Network:
        res.stop = StopReason::Network;
        if (first) res.status = EnumerationStatus::Unreachable; else res.incomplete = true;
        return false;
    case St::Failed:
        res.stop = StopReason::Failed;
        if (first) res.status = EnumerationStatus::Failed; else res.incomplete = true;
        return false;
    case St::LoggedOut:
        res.stop = StopReason::LoggedOut;
        if (first) res.status = EnumerationStatus::SignedOut;
        return false;
    case St::Ok: break;
    }
    int added = 0, usable = 0;
    const std::size_t cap = static_cast<std::size_t>(EnumerationLimits::kEntriesPerProbe);
    const std::size_t considered = r.entries.size() < cap ? r.entries.size() : cap;
    if (r.entries.size() > considered) { res.droppedEntries += static_cast<int>(r.entries.size() - considered); res.incomplete = true; }
    for (std::size_t i = 0; i < considered; ++i) {
        ports::LoginEntry e = r.entries[i];
        e.gaiaId = printableId(e.gaiaId) ? e.gaiaId : std::string();
        e.datasyncId = printableId(e.datasyncId) ? e.datasyncId : std::string();
        e.name = cleanName(e.name);
        const bool pageOk = e.pageId.empty() || usablePageId(e.pageId);
        if (!pageOk || (e.gaiaId.empty() && e.datasyncId.empty() && e.name.empty())) { ++res.droppedEntries; continue; }
        ++usable;
        const std::string key = dedupKey(e);
        if (s.seen.count(key)) continue;
        if (res.choices.size() >= static_cast<std::size_t>(EnumerationLimits::kMaxChoices)) {
            ++res.droppedEntries; res.incomplete = true; res.stop = StopReason::Cap; continue;
        }
        s.seen.insert(key);
        AccountChoice c;
        c.authUser = s.slot; c.pageId = e.pageId; c.label = e.name; c.key = key; c.primary = e.pageId.empty();
        res.choices.push_back(c);
        ++added;
    }
    if (res.stop == StopReason::Cap) return false;
    if (added == 0) {
        if (usable == 0) {
            res.stop = StopReason::NoItems;
            if (first) res.status = EnumerationStatus::NoAccounts;
        } else {
            res.stop = StopReason::OnlyDuplicates;      // Google keeps answering for the last valid login
        }
        return false;
    }
    if (s.slot >= static_cast<int>(EnumerationLimits::kSlots) - 1) { res.stop = StopReason::SlotLimit; return false; }
    return true;
}

}}}}}
