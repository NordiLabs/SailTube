// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Test doubles for the ytm-sources slice. No Qt, no network. Every secret in these tests is a
// SENTINEL: synthetic, recognisable (common prefix kSentinelPrefix), scanned for in every
// observable string. The sentinels are valid cookie values (printable ASCII, no ';').
#pragma once

#include "Core/Accounts/Infrastructure/SessionImportParser.h"
#include "Core/Accounts/Ports/SessionMaterialSinkPort.h"
#include "Core/Accounts/Ports/WorkerPort.h"
#include "Core/Ytm/Sources/Domain/AccountEnumeration.h"
#include "Core/Ytm/Sources/Domain/CookieJarSelection.h"
#include "Core/Ytm/Sources/Ports/CookieJarReaderPort.h"
#include "Core/Ytm/Sources/Ports/LoginProbePort.h"
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace nordtile::music;
namespace src = nordtile::music::ytm::sources;
namespace acc = nordtile::music::accounts;

static const char *const kSentinelPrefix = "SNTLSRC_";
static const char *const kV3PAPISID = "SNTLSRC_3PAPISID_a1b2c3d4e5f60718";
static const char *const kVSAPISID = "SNTLSRC_SAPISID_YT_0f9e8d7c6b5a4938";
static const char *const kVSAPISIDGoogle = "SNTLSRC_SAPISID_GOOGLE_17263544f3e2d1c0";
static const char *const kV1PAPISID = "SNTLSRC_1PAPISID_55aa66bb77cc88dd";
static const char *const kV3PSID = "SNTLSRC_3PSID_9988776655443322";
static const char *const kV1PSID = "SNTLSRC_1PSID_1122334455667788";
static const char *const kVSID = "SNTLSRC_SID_0a8d5be1c6f3497a";
static const char *const kVHSID = "SNTLSRC_HSID_b27e91d4a05c68f3";
static const char *const kVSSID = "SNTLSRC_SSID_e5a2c8071b9d34f6";
static const char *const kVAPISID = "SNTLSRC_APISID_3c6d91f0a7e24b85";
static const char *const kVSIDCC = "SNTLSRC_SIDCC_c0ffee0123456789";
static const char *const kV3PSIDCC = "SNTLSRC_3PSIDCC_decafbad01234567";
static const char *const kV1PSIDCC = "SNTLSRC_1PSIDCC_feedface89abcdef";
static const char *const kVLOGIN = "SNTLSRC_LOGININFO_AFmmF2swRQIhAK-9z_xyz";
static const char *const kVVISITOR = "SNTLSRC_VISITOR_aabbccdd11223344";
static const char *const kV3PSIDTS = "SNTLSRC_3PSIDTS_0102030405060708";
static const char *const kV1PSIDTS = "SNTLSRC_1PSIDTS_090a0b0c0d0e0f10";
static const char *const kVSOCS = "SNTLSRC_SOCS_CAISNggDEitib3Fh";
static const char *const kVOther = "SNTLSRC_UNNEEDED_77aa11bb22cc33dd";    // a name the allow-list does not carry
static const char *const kVForeign = "SNTLSRC_OTHERSITE_5e8a0d2f4b7c9163";   // a cookie of an unrelated site
static const char *const kVContainer = "SNTLSRC_CONTAINER_4d3c2b1a0f9e8d7c"; // a row of a container / isolated context

inline acc::ports::SecretBytes secretBytes(const std::string &text) {
    acc::ports::SecretBytes bytes;
    bytes.assign(reinterpret_cast<const unsigned char *>(text.data()), text.size());
    return bytes;
}

inline src::ports::RawCookieRow row(const char *host, const char *name, const char *value, const char *path = "/",
                                    std::int64_t expiry = 4102444800LL /* 2100-01-01 */, const char *origin = "", std::int64_t accessed = 0) {
    src::ports::RawCookieRow r;
    r.host = host; r.name = name; r.value = value; r.path = path; r.originAttributes = origin;
    r.expiry = expiry; r.hasExpiry = true; r.lastAccessed = accessed;
    return r;
}

// A complete signed-in jar of the YouTube side (every allow-listed name once).
inline std::vector<src::ports::RawCookieRow> goodRows() {
    std::vector<src::ports::RawCookieRow> r;
    r.push_back(row(".youtube.com", "__Secure-3PAPISID", kV3PAPISID));
    r.push_back(row(".youtube.com", "SAPISID", kVSAPISID));
    r.push_back(row(".youtube.com", "__Secure-1PAPISID", kV1PAPISID));
    r.push_back(row(".youtube.com", "__Secure-3PSID", kV3PSID));
    r.push_back(row(".youtube.com", "__Secure-1PSID", kV1PSID));
    r.push_back(row(".youtube.com", "SID", kVSID));
    r.push_back(row(".youtube.com", "HSID", kVHSID));
    r.push_back(row(".youtube.com", "SSID", kVSSID));
    r.push_back(row(".youtube.com", "APISID", kVAPISID));
    r.push_back(row(".youtube.com", "SIDCC", kVSIDCC));
    r.push_back(row(".youtube.com", "__Secure-3PSIDCC", kV3PSIDCC));
    r.push_back(row(".youtube.com", "__Secure-1PSIDCC", kV1PSIDCC));
    r.push_back(row(".youtube.com", "LOGIN_INFO", kVLOGIN));
    r.push_back(row(".youtube.com", "VISITOR_INFO1_LIVE", kVVISITOR));
    r.push_back(row(".youtube.com", "__Secure-3PSIDTS", kV3PSIDTS));
    r.push_back(row(".youtube.com", "__Secure-1PSIDTS", kV1PSIDTS));
    r.push_back(row(".youtube.com", "SOCS", kVSOCS));
    return r;
}

inline bool containsSentinel(const std::string &text) { return text.find(kSentinelPrefix) != std::string::npos; }

// The session slice's real parser is the sink: what it accepts is what the real adapter accepts
// (InnerTubeSessionAuthorization::stage runs the same parse). Keeps the parsed bundle and the text.
class ParserSink final : public acc::ports::SessionMaterialSinkPort {
public:
    bool consent = true;
    int stageCalls = 0;
    acc::domain::SessionSourceKind lastKind = acc::domain::SessionSourceKind::Unknown;
    std::string lastText;                           // SECRET (test memory only)
    acc::infrastructure::ImportResult last;         // SECRET
    acc::ports::SessionStageResult lastResult;
    std::int64_t now = 1700000000;

    acc::ports::SessionStageResult stage(acc::ports::SecretBytes &&raw, acc::domain::SessionSourceKind source) override {
        ++stageCalls; lastKind = source;
        lastText.assign(reinterpret_cast<const char *>(raw.bytes().data()), raw.bytes().size());
        raw.clear();
        acc::ports::SessionStageResult out;
        if (!consent) {
            out.error = acc::ports::ImportError::ConsentRequired;
            out.result.code = acc::domain::ErrorCode::PermissionDenied;
            out.result.message = acc::infrastructure::SessionImportParser::describe(out.error);
            lastResult = out; return out;
        }
        acc::infrastructure::ImportOptions options; options.nowUtcSeconds = now;
        last = acc::infrastructure::SessionImportParser::parse(lastText, options);
        out.summary = last.summary;
        if (!last.ok()) {
            out.error = last.error; out.errorLine = last.errorLine;
            out.result.code = acc::domain::ErrorCode::InvalidArgument;
            out.result.message = acc::infrastructure::SessionImportParser::describe(last.error);
        } else staged_ = true;
        lastResult = out;
        return out;
    }
    bool hasStaged() const override { return staged_; }
    void discardStaged() override { staged_ = false; }
private:
    bool staged_ = false;
};

// A reader that answers from a script and counts its calls.
class FakeReader final : public src::ports::CookieJarReaderPort {
public:
    src::domain::Availability availability = src::domain::Availability::Available;
    src::ports::JarRead next;
    mutable int probes = 0;
    int reads = 0;
    std::thread::id readThread;
    src::domain::Availability probe() const override { ++probes; return availability; }
    src::ports::JarRead read() override { ++reads; readThread = std::this_thread::get_id(); return next; }
};

inline src::ports::JarRead okJar(const std::vector<src::ports::RawCookieRow> &rows, int candidate = 0) {
    src::ports::JarRead j;
    j.status = src::domain::ReadStatus::Ok; j.candidate = candidate;
    j.matchingRows = static_cast<int>(rows.size()); j.rows = rows;
    return j;
}

// Collects work and completions so a test decides when each runs (the WorkerPort contract: serial FIFO).
class ManualWorker final : public acc::ports::WorkerPort {
public:
    struct Job { std::function<void()> work, done; };
    std::deque<Job> jobs;
    void run(std::function<void()> work, std::function<void()> done) override { Job j; j.work = std::move(work); j.done = std::move(done); jobs.push_back(std::move(j)); }
    // Runs the oldest job's work only (as the worker thread would), leaving its completion pending.
    std::function<void()> runWork() {
        Job j = std::move(jobs.front()); jobs.pop_front();
        if (j.work) j.work();
        return j.done;
    }
    void runAll() { while (!jobs.empty()) { std::function<void()> d = runWork(); if (d) d(); } }
};

// A real worker thread; completions are posted back to a queue the test drains on its own thread.
class ThreadWorker final : public acc::ports::WorkerPort {
public:
    ThreadWorker() : stop_(false), thread_(&ThreadWorker::loop, this) {}
    ~ThreadWorker() override { { std::lock_guard<std::mutex> l(m_); stop_ = true; } cv_.notify_all(); thread_.join(); }
    void run(std::function<void()> work, std::function<void()> done) override {
        { std::lock_guard<std::mutex> l(m_); jobs_.push_back(std::make_pair(std::move(work), std::move(done))); }
        cv_.notify_all();
    }
    // Blocks until one completion is available, then runs it on the calling thread.
    bool pumpOne() {
        std::function<void()> d;
        {
            std::unique_lock<std::mutex> l(m_);
            cv_.wait(l, [this] { return !completions_.empty() || stop_; });
            if (completions_.empty()) return false;
            d = std::move(completions_.front());
            completions_.pop_front();
        }
        if (d) d();
        return true;
    }
    std::thread::id workerId() const { return thread_.get_id(); }
private:
    void loop() {
        for (;;) {
            std::pair<std::function<void()>, std::function<void()> > job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return !jobs_.empty() || stop_; });
                if (jobs_.empty()) return;
                job = std::move(jobs_.front());
                jobs_.pop_front();
            }
            if (job.first) job.first();
            { std::lock_guard<std::mutex> l(m_); completions_.push_back(std::move(job.second)); }
            cv_.notify_all();
        }
    }
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::pair<std::function<void()>, std::function<void()> > > jobs_;
    std::deque<std::function<void()> > completions_;
    bool stop_;
    std::thread thread_;
};

// A login-probe port that answers from a script, synchronously or on demand.
class ScriptedProbe final : public src::ports::LoginProbePort {
public:
    typedef src::ports::LoginProbeResult Result;
    std::map<int, Result> script;                   // authUser -> answer (missing = LoggedOut)
    Result fallback;                                // used when the slot is not in the script and useFallback
    bool useFallback = false;
    bool synchronous = true;                        // answer inside probeLogin()
    std::vector<int> asked;
    int cancels = 0;
    std::deque<std::pair<int, Done> > pending;      // when !synchronous
    void probeLogin(int authUser, Done done) override {
        asked.push_back(authUser);
        if (synchronous) { done(answerFor(authUser)); return; }
        pending.push_back(std::make_pair(authUser, std::move(done)));
    }
    void cancel() override { ++cancels; }
    Result answerFor(int authUser) const {
        std::map<int, Result>::const_iterator it = script.find(authUser);
        if (it != script.end()) return it->second;
        if (useFallback) return fallback;
        Result r; r.status = Result::Status::LoggedOut; return r;
    }
    // Answers the oldest pending probe.
    bool answerOne() {
        if (pending.empty()) return false;
        std::pair<int, Done> p = std::move(pending.front()); pending.pop_front();
        p.second(answerFor(p.first));
        return true;
    }
};

inline src::ports::LoginEntry entry(const char *gaia, const char *datasync, const char *name, const char *page = "") {
    src::ports::LoginEntry e; e.gaiaId = gaia; e.datasyncId = datasync; e.name = name; e.pageId = page; return e;
}
inline ScriptedProbe::Result okAnswer(const std::vector<src::ports::LoginEntry> &entries) {
    ScriptedProbe::Result r; r.status = ScriptedProbe::Result::Status::Ok; r.entries = entries; return r;
}
inline ScriptedProbe::Result statusAnswer(ScriptedProbe::Result::Status s) {
    ScriptedProbe::Result r; r.status = s; return r;
}
