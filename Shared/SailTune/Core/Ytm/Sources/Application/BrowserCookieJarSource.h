// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The third session source, "Import from the Sailfish Browser" (decision 3 in IMPL_RULES): the
// user signs in to YouTube Music in the stock Sailfish Browser (a NORMAL tab, private tabs keep no
// cookies) and, on an explicit tap after the consent screen, SailTune reads the browser's Gecko
// cookie database read-only, keeps only the allow-listed sign-in cookies and stages them through
// SessionMaterialSinkPort exactly as the paste and embedded sources do.
//
// Pure C++11, no Qt: the database work is behind ports::CookieJarReaderPort
// (infrastructure::GeckoCookieJarReader in production). Flow, as for PasteSource:
//
//   source.start(sink, done)         // after the consent screen; done(SessionStageResult) once
//   -> AccountService::authorize()   // by the UI, when done(...).ok()
//
// Honesty about availability: status().available is true only when a candidate cookie database
// exists, is an ordinary file of the user and may be read by this process. Under Sailjail the
// browser's profile is not reachable, so the source reports itself unavailable with the precise
// reason; it never asks for a wider permission and never turns the sandbox off.
//
// Threading: start()/cancel()/status() are called on the event-loop thread. With a WorkerPort the
// blocking read and the pure selection run on the worker and `done` runs on the caller's thread
// (WorkerPort contract); without one everything runs inline, so `done` may be called before
// start() returns. The reader and the worker must outlive any read in flight (the composition
// stops the worker last, as for the vault); destroying the source mid-read is safe: the result is
// wiped and nothing is staged.
#pragma once

#include "../../../Accounts/Ports/SessionSource.h"
#include "../../../Accounts/Ports/WorkerPort.h"
#include "../Domain/BrowserImportTypes.h"
#include "../Domain/CookieJarSelection.h"
#include "../Ports/CookieJarReaderPort.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace application {

class BrowserCookieJarSource final : public accounts::ports::SessionSource {
public:
    struct Config {
        ports::CookieJarReaderPort *reader = nullptr;       // required
        accounts::ports::WorkerPort *worker = nullptr;      // optional; nullptr = read on the caller's thread
        // The versioned consent of the unofficial connection (ConsentStore::accepted(...)). REQUIRED:
        // without it, or when it returns false, nothing in the browser's folder is touched. (The
        // sink checks consent too; this is the earlier, stricter gate for reading another app's data.)
        std::function<bool()> consentGranted;
        std::function<std::int64_t()> nowUtcSeconds;        // null = the system clock
        bool allowGoogleFallback = true;                    // see domain::SelectionOptions
    };

    // Non-secret facts about the last run, for the UI and diagnostics. Carries no value and no path.
    struct Report {
        domain::Outcome outcome = domain::Outcome::None;
        int candidate = -1;                 // which location (0..2) was used
        int cookiesUsed = 0;                // allow-listed cookies written to the staged text
        int fromGoogleHost = 0;             // of those, taken from a google.com row
        bool selectionApplied = false;      // the account-selection lines were written
        std::vector<std::string> names;     // cookie NAMES in the order written
    };

    explicit BrowserCookieJarSource(const Config &config);
    ~BrowserCookieJarSource() override;
    BrowserCookieJarSource(const BrowserCookieJarSource &) = delete;
    BrowserCookieJarSource &operator=(const BrowserCookieJarSource &) = delete;

    // ---- accounts::ports::SessionSource
    accounts::domain::SessionSourceKind kind() const override { return accounts::domain::SessionSourceKind::SystemBrowser; }
    // available == true only when a candidate database is readable (a stat-level probe, no data read).
    Status status() const override;
    // Synchronous refusals (done is NOT called): PermissionDenied (consent missing, or the database
    // may not be read / is not an ordinary file), Unsupported (no database), Busy (a read is running).
    // Otherwise returns success and calls `done` once, unless cancel() or the destructor intervenes.
    accounts::domain::Result start(accounts::ports::SessionMaterialSinkPort &sink, Completion done) override;
    // Abandons the read: its result is wiped, nothing is staged, `done` is not called. Idempotent.
    void cancel() override;

    // ---- extras
    bool busy() const;
    domain::Outcome outcome() const;                // of the last run; None before the first
    Report report() const;
    // Multi-login / brand account (domain::AccountEnumerator output). Applied to the NEXT start().
    // False (and nothing stored) when a value is not 1-2 digits (authUser) or 1-32 digits (pageId).
    bool setAccountSelection(const std::string &authUser, const std::string &pageId);
    void clearAccountSelection();

private:
    struct Shared;
    struct Payload;
    Config cfg_;
    std::shared_ptr<Shared> shared_;
    std::string authUser_, pageId_;

    static void finish(const std::shared_ptr<Shared> &shared, std::uint64_t generation, const std::shared_ptr<Payload> &payload,
                       accounts::ports::SessionMaterialSinkPort *sink, const Completion &done);
    static accounts::ports::SessionStageResult failureResult(domain::Outcome outcome);
};

}}}}}
