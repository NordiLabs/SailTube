// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Test doubles for the ytm-session slice. No Qt, no network. Every secret in the tests is a
// SENTINEL (see kSentinel*): synthetic, recognisable, scanned for in every output.
#pragma once

#include "Core/Accounts/Ports/AuthorizationPort.h"
#include "Core/Accounts/Ports/CredentialVaultPort.h"
#include "Core/Accounts/Ports/IdentityProbePort.h"
#include "Core/Accounts/Ports/WorkerPort.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

using namespace nordtile::music::accounts;

// Recognisable synthetic secrets. They must NEVER appear in a message, a log line, the grant
// file, the consent file or anywhere after logout (the vault excepted while signed in).
static const char *const kSentinelSapisid = "SNTL_SAPISID_4f1c9a77e2b84d30";
static const char *const kSentinelPsid1 = "SNTL_PSID1_0a8d5be1c6f3497a";
static const char *const kSentinelPsid3 = "SNTL_PSID3_b27e91d4a05c68f3";
static const char *const kSentinelP1 = "SNTL_P1APISID_9c3e7a15d8b2460f";
static const char *const kSentinelP3 = "SNTL_P3APISID_61d0b4f9e7a3258c";
static const char *const kSentinelTs = "SNTL_PSIDTS_e5a2c8071b9d34f6";
static const char *const kSentinelTsNew = "SNTL_PSIDTS_NEW_3c6d91f0a7e24b85";
static const char *const kSentinelPsidOther = "SNTL_OTHERACCT_5e8a0d2f4b7c9163";
static const char *const kSentinelAuthz = "SNTL_AUTHZ_d41f7c0be98a2536";
static const char *const kSentinelOther = "SNTL_UNNEEDED_77aa11bb22cc33dd";
static const char *const kSentinelStableId = "SNTL_STABLEID_UCx9f3k2m7q1w8e5r";

inline const char *const *allSentinels(std::size_t *count) {
    static const char *const all[] = {kSentinelSapisid, kSentinelPsid1, kSentinelPsid3, kSentinelP1, kSentinelP3,
                                      kSentinelTs, kSentinelTsNew, kSentinelPsidOther, kSentinelAuthz, kSentinelOther};
    *count = sizeof(all) / sizeof(all[0]);
    return all;
}

// A Cookie-header paste that satisfies the parser; the sentinels are the cookie VALUES.
inline std::string sentinelCookieHeader(const char *psidts = kSentinelTs) {
    return std::string("Cookie: SAPISID=") + kSentinelSapisid + "; __Secure-1PAPISID=" + kSentinelP1 +
           "; __Secure-3PAPISID=" + kSentinelP3 + "; __Secure-1PSID=" + kSentinelPsid1 +
           "; __Secure-3PSID=" + kSentinelPsid3 + "; __Secure-1PSIDTS=" + psidts +
           "; UNNEEDED_COOKIE=" + kSentinelOther + "; VISITOR_INFO1_LIVE=visitor_value_000111";
}
inline ports::SecretBytes secretOf(const std::string &text) {
    ports::SecretBytes bytes;
    bytes.assign(reinterpret_cast<const unsigned char *>(text.data()), text.size());
    return bytes;
}

class ManualClock final : public ports::ClockPort {
public:
    explicit ManualClock(std::int64_t start = 1700000000) : now_(start) {}
    std::int64_t nowUtcSeconds() const override { return now_; }
    void advance(std::int64_t seconds) { now_ += seconds; }
    void set(std::int64_t value) { now_ = value; }
private:
    std::int64_t now_;
};

// Same validation as SailfishCredentialVault so a wrong handle or size fails here too.
class InMemoryCredentialVault final : public ports::CredentialVaultPort {
public:
    bool unavailable = false;                       // every call: SecureStorageUnavailable
    bool failStore = false, failLoad = false, failRemove = false;
    domain::ErrorCode failCode = domain::ErrorCode::SecureStorageFailure;
    bool removeMissingFails = false;                // a daemon that errors when deleting a missing secret
    int failStoreAfter = -1;                        // >= 0: the (n+1)-th store fails
    int storeCalls = 0, loadCalls = 0, removeCalls = 0;
    std::vector<std::thread::id> callerThreads;
    std::vector<std::string> log;                   // "store:<handle>", ... (no values)

    domain::Result store(const std::string &handle, const ports::SecretBytes &bytes) override {
        std::lock_guard<std::mutex> lock(m_);
        ++storeCalls; note("store", handle);
        if (unavailable) return err(domain::ErrorCode::SecureStorageUnavailable);
        if (!validHandle(handle) || bytes.bytes().empty() || bytes.bytes().size() > 65536) return err(domain::ErrorCode::InvalidArgument);
        if (failStore || (failStoreAfter >= 0 && storeCalls > failStoreAfter)) return err(failCode);
        items_[handle] = bytes.bytes();
        return domain::Result();
    }
    domain::Result load(const std::string &handle, ports::SecretBytes *out) override {
        std::lock_guard<std::mutex> lock(m_);
        ++loadCalls; note("load", handle);
        if (out) out->clear();
        if (unavailable) return err(domain::ErrorCode::SecureStorageUnavailable);
        if (!validHandle(handle) || !out) return err(domain::ErrorCode::InvalidArgument);
        if (failLoad) return err(failCode);
        std::map<std::string, std::vector<unsigned char> >::const_iterator it = items_.find(handle);
        if (it == items_.end()) return err(domain::ErrorCode::SecureStorageFailure);   // the real vault cannot tell "missing" apart
        out->assign(it->second.data(), it->second.size());
        return domain::Result();
    }
    domain::Result remove(const std::string &handle) override {
        std::lock_guard<std::mutex> lock(m_);
        ++removeCalls; note("remove", handle);
        if (unavailable) return err(domain::ErrorCode::SecureStorageUnavailable);
        if (!validHandle(handle)) return err(domain::ErrorCode::InvalidArgument);
        if (failRemove) return err(failCode);
        std::map<std::string, std::vector<unsigned char> >::iterator it = items_.find(handle);
        if (it == items_.end()) return removeMissingFails ? err(domain::ErrorCode::SecureStorageFailure) : domain::Result();
        for (std::size_t i = 0; i < it->second.size(); ++i) it->second[i] = 0;
        items_.erase(it);
        return domain::Result();
    }

    std::size_t count() const { std::lock_guard<std::mutex> lock(m_); return items_.size(); }
    bool contains(const std::string &handle) const { std::lock_guard<std::mutex> lock(m_); return items_.count(handle) != 0; }
    std::string value(const std::string &handle) const {
        std::lock_guard<std::mutex> lock(m_);
        std::map<std::string, std::vector<unsigned char> >::const_iterator it = items_.find(handle);
        return it == items_.end() ? std::string() : std::string(it->second.begin(), it->second.end());
    }
    bool anyContains(const std::string &needle) const {
        std::lock_guard<std::mutex> lock(m_);
        for (std::map<std::string, std::vector<unsigned char> >::const_iterator it = items_.begin(); it != items_.end(); ++it) {
            const std::string v(it->second.begin(), it->second.end());
            if (v.find(needle) != std::string::npos) return true;
        }
        return false;
    }
    void putRaw(const std::string &handle, const std::string &bytes) {
        std::lock_guard<std::mutex> lock(m_); items_[handle] = std::vector<unsigned char>(bytes.begin(), bytes.end());
    }
    std::string allLog() const { std::lock_guard<std::mutex> lock(m_); std::string s; for (std::size_t i = 0; i < log.size(); ++i) s += log[i] + "\n"; return s; }
private:
    static bool validHandle(const std::string &h) {
        if (h.size() < 16 || h.size() > 128) return false;
        for (std::size_t i = 0; i < h.size(); ++i) {
            const char c = h[i];
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
        }
        return true;
    }
    static domain::Result err(domain::ErrorCode code) { domain::Result r; r.code = code; r.message = "vault failure"; return r; }
    void note(const char *op, const std::string &handle) { callerThreads.push_back(std::this_thread::get_id()); log.push_back(std::string(op) + ":" + handle); }
    mutable std::mutex m_;
    std::map<std::string, std::vector<unsigned char> > items_;
};

// Queues jobs; drain() runs them in FIFO order (work, then done) on the calling thread.
class DeferredWorker final : public ports::WorkerPort {
public:
    void run(std::function<void()> work, std::function<void()> done) override { q_.push_back(std::make_pair(work, done)); }
    std::size_t pending() const { return q_.size(); }
    bool runNext() {
        if (q_.empty()) return false;
        std::pair<std::function<void()>, std::function<void()> > job = q_.front(); q_.pop_front();
        if (job.first) job.first();
        if (job.second) job.second();
        return true;
    }
    void drain() { int guard = 0; while (runNext() && ++guard < 1000) {} }
    // Work of the next job only, `done` stays queued as a pending delivery (worker finished,
    // event loop has not run yet).
    bool runNextWorkOnly() {
        if (q_.empty()) return false;
        std::pair<std::function<void()>, std::function<void()> > job = q_.front(); q_.pop_front();
        if (job.first) job.first();
        deliveries_.push_back(job.second);
        return true;
    }
    void deliverAll() { while (!deliveries_.empty()) { std::function<void()> d = deliveries_.front(); deliveries_.pop_front(); if (d) d(); } }
private:
    std::deque<std::pair<std::function<void()>, std::function<void()> > > q_;
    std::deque<std::function<void()> > deliveries_;
};

// A real serial worker thread. `done` callbacks are queued for the "event loop" (the test
// thread) and delivered by pump(), exactly like a queued signal would be.
class ThreadWorker final : public ports::WorkerPort {
public:
    ThreadWorker() : stop_(false) { thread_ = std::thread([this] { loop(); }); }
    ~ThreadWorker() override {
        { std::lock_guard<std::mutex> lock(m_); stop_ = true; }
        cv_.notify_all();
        thread_.join();
    }
    void run(std::function<void()> work, std::function<void()> done) override {
        { std::lock_guard<std::mutex> lock(m_); jobs_.push_back(std::make_pair(work, done)); }
        cv_.notify_all();
    }
    std::thread::id workerThread() const { return thread_.get_id(); }
    // Delivers queued `done` callbacks on the calling thread; true while anything is in flight.
    bool pump() {
        std::deque<std::function<void()> > ready;
        { std::lock_guard<std::mutex> lock(m_); ready.swap(done_); }
        for (std::size_t i = 0; i < ready.size(); ++i) if (ready[i]) ready[i]();
        std::lock_guard<std::mutex> lock(m_);
        return !jobs_.empty() || running_ > 0 || !done_.empty();
    }
    void pumpUntilIdle() {
        // Waits for the worker thread. The deadline is wall-clock and generous (it only matters when the worker is
        // broken); an iteration count, as before, shrinks with the machine's speed (a loaded host sleeps longer).
        const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        while (std::chrono::steady_clock::now() < deadline) {
            if (!pump()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
private:
    void loop() {
        for (;;) {
            std::pair<std::function<void()>, std::function<void()> > job;
            {
                std::unique_lock<std::mutex> lock(m_);
                cv_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
                if (jobs_.empty()) return;          // stop requested and drained
                job = jobs_.front(); jobs_.pop_front(); ++running_;
            }
            if (job.first) job.first();
            { std::lock_guard<std::mutex> lock(m_); done_.push_back(job.second); --running_; }
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::function<void()>, std::function<void()> > > jobs_;
    std::deque<std::function<void()> > done_;
    int running_ = 0;
    bool stop_;
    std::thread thread_;
};

// The transport slice's port, faked. By default answers at once; `manual` holds the answer
// until complete(). It builds real request headers from the credentials it is given (so the
// signing path is exercised) and can apply a rotation like a successful response would.
class FakeIdentityProbe final : public ports::IdentityProbePort {
public:
    ports::IdentityProbeResult next;                // what the next probe answers
    bool manual = false;
    std::vector<ports::RotatedCookie> rotateOnProbe;
    int probes = 0, cancels = 0;
    std::string lastCookieHeader, lastAuthorization, lastAuthUser, lastPageId;
    std::uint64_t lastGeneration = 0;
    bool lastHeadersOk = false;
    std::int64_t headerClock = 1700000000;

    FakeIdentityProbe() { next.verdict = domain::SessionVerdict::Ok; next.stableId = kSentinelStableId; next.displayName = "Test Listener"; }

    void probe(ports::ProbeId id, ports::SessionCredentialsPort &credentials, ports::IdentityProbeCompletion done) override {
        ++probes;
        ports::SessionRequestHeaders h;
        lastHeadersOk = credentials.requestHeaders("https://music.youtube.com", headerClock, &h);
        lastCookieHeader = h.cookie; lastAuthorization = h.authorization; lastAuthUser = h.authUser; lastPageId = h.pageId;
        lastGeneration = h.generation;
        if (lastHeadersOk && !rotateOnProbe.empty()) credentials.applyRotation(rotateOnProbe, h.generation);
        pendingId_ = id; pendingDone_ = done;
        if (!manual) complete();
    }
    void cancel(ports::ProbeId id) override { ++cancels; if (id == pendingId_) { pendingDone_ = ports::IdentityProbeCompletion(); } }
    void complete() {
        const ports::IdentityProbeCompletion done = pendingDone_;
        const ports::ProbeId id = pendingId_;
        pendingDone_ = ports::IdentityProbeCompletion();
        if (done) done(id, next);
    }
    // Delivers an answer for an id the adapter no longer waits for (late/stale completion).
    void completeStale(ports::ProbeId id, const ports::IdentityProbeResult &r, const ports::IdentityProbeCompletion &done) { if (done) done(id, r); }
    ports::IdentityProbeCompletion takeCompletion() { ports::IdentityProbeCompletion d = pendingDone_; pendingDone_ = ports::IdentityProbeCompletion(); return d; }
    ports::ProbeId pendingId() const { return pendingId_; }
    bool hasPending() const { return bool(pendingDone_); }
private:
    ports::ProbeId pendingId_ = 0;
    ports::IdentityProbeCompletion pendingDone_;
};
