// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Vocabulary of the "Import from the Sailfish Browser" session source (slice "ytm-sources",
// docs/ytm_hybrid/impl/sources.md). Pure C++11, no Qt. Nothing here is secret: the enums, the
// limits and the fixed English sentences carry no cookie value, no cookie row and no path of
// the user's data. The UI maps the fixed sentences through infrastructure::BrowserImportTexts.
//
// Evidence tags: SPEC = behaviour described by the clean-room specification of the reference
// app (FINTUNE_SPEC.md section 4.1, no code was read); INFERENCE = our own reasoning, to be
// confirmed on a device.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace domain {

// ---- where the stock Sailfish Browser keeps its Gecko cookie database (SPEC 4.1) -----------
// Relative to the user's home folder, in probing order; there is no profile globbing.
enum { kCandidateCount = 3 };
inline const char *candidateLocation(int index) {
    static const char *const table[kCandidateCount] = {
        ".local/share/org.sailfishos/browser/.mozilla/cookies.sqlite",              // current browser
        ".mozilla/mozembed/cookies.sqlite",                                         // legacy
        ".local/share/org.sailfishos/sailfish-browser/.mozilla/cookies.sqlite",     // alternative layout
    };
    return (index >= 0 && index < kCandidateCount) ? table[index] : "";
}

// ---- bounds. Every number is a cap against an odd or hostile file, not a tuned value -------
struct Limits {
    enum {
        kMaxDbBytes = 64 * 1024 * 1024,     // a real cookies.sqlite is a few hundred KiB
        kMaxWalBytes = 64 * 1024 * 1024,    // Gecko checkpoints at ~4 MiB
        kMaxRows = 512,                     // matching rows of allow-listed names (a real jar: < 150)
        kMaxValueBytes = 4096,              // same cap as accounts::domain::validCookieValue
        kCopyAttempts = 3,                  // a copy is repeated when the source changed while copying
        kMaxComposedBytes = 24 * 1024       // the composed cookie line stays below the parser's 32 KiB line cap
    };
};

// ---- can the source be used right now? (a probe that reads no data) ------------------------
enum class Availability {
    Available,      // a candidate exists, is a regular file of ours and the app may read it
    NotFound,       // no candidate exists: never used, or hidden from this app by the sandbox
    NotReadable,    // a candidate exists but this app may not read it (the Sailjail case)
    Unsafe,         // a candidate is a symbolic link / not a regular file / not owned by the user
    NoHome          // the home folder is unknown
};

// ---- what the reader found ------------------------------------------------------------------
enum class ReadStatus {
    Ok,                     // rows were read from a candidate that holds YouTube/Google cookies
    NotFound,
    NotReadable,
    Unsafe,
    TooLarge,               // database or write-ahead log beyond the cap: not copied
    ChangedWhileReading,    // the browser kept writing during every copy attempt
    CopyFailed,             // the private working copy could not be made
    NotADatabase,           // not SQLite, or corrupt
    UnexpectedSchema,       // no moz_cookies table / no name, value, host column
    TooManyRows,            // more matching rows than the cap
    NoCookies               // readable, but not one YouTube/Google cookie in it
};

// ---- what the whole import ended with (source.outcome()) ------------------------------------
enum class Outcome {
    None,                   // nothing has run yet
    Staged,                 // the sink accepted the session; AccountService::authorize() is next
    ConsentMissing,         // the notice was not accepted: nothing was read
    NotFound, NotReadable, Unsafe, TooLarge, ChangedWhileReading, CopyFailed, NotADatabase,
    UnexpectedSchema, TooManyRows,
    NoCookies,              // the browser holds no YouTube cookies (never visited, or a private tab)
    NotSignedIn,            // cookies, but neither of the two sign-in cookies: "cookies but not a session"
    Rejected,               // the sink refused (SessionStageResult::error says why)
    Cancelled
};

inline Outcome outcomeFor(ReadStatus status) {
    switch (status) {
    case ReadStatus::Ok: return Outcome::Staged;   // caller refines (Staged is decided by the sink)
    case ReadStatus::NotFound: return Outcome::NotFound;
    case ReadStatus::NotReadable: return Outcome::NotReadable;
    case ReadStatus::Unsafe: return Outcome::Unsafe;
    case ReadStatus::TooLarge: return Outcome::TooLarge;
    case ReadStatus::ChangedWhileReading: return Outcome::ChangedWhileReading;
    case ReadStatus::CopyFailed: return Outcome::CopyFailed;
    case ReadStatus::NotADatabase: return Outcome::NotADatabase;
    case ReadStatus::UnexpectedSchema: return Outcome::UnexpectedSchema;
    case ReadStatus::TooManyRows: return Outcome::TooManyRows;
    case ReadStatus::NoCookies: return Outcome::NoCookies;
    }
    return Outcome::CopyFailed;
}

// Fixed English reasons for SessionSource::Status::reason (the UI translates them with
// BrowserImportTexts::sourceReason). They are the only strings that tell the user why the source
// is unavailable, and they say precisely what is missing. A reading of the browser's folder is
// never requested through a permission: the source is simply unavailable under Sailjail.
inline const char *reasonFor(Availability availability) {
    switch (availability) {
    case Availability::Available: return "";
    case Availability::NotFound:
        return "Not available: this needs an unsandboxed build (OpenRepos profile). The Sailfish Browser's cookie database was not found.";
    case Availability::NotReadable:
        return "Not available: this needs an unsandboxed build (OpenRepos profile). The Sailfish Browser's cookie database exists, but this app is not allowed to read it.";
    case Availability::Unsafe:
        return "Not available: the Sailfish Browser's cookie database is a link or not an ordinary file of yours, so it is not read.";
    case Availability::NoHome:
        return "Not available: the home folder could not be determined.";
    }
    return "Importing from the Sailfish Browser is not available.";
}

// Fixed English sentence for an outcome (diagnostics and tests; the UI uses BrowserImportTexts).
inline const char *describe(Outcome outcome) {
    switch (outcome) {
    case Outcome::None: return "";
    case Outcome::Staged: return "The sign-in was read from the Sailfish Browser.";
    case Outcome::ConsentMissing: return "The notice about the unofficial connection has not been accepted. Nothing was read.";
    case Outcome::NotFound: return reasonFor(Availability::NotFound);
    case Outcome::NotReadable: return reasonFor(Availability::NotReadable);
    case Outcome::Unsafe: return reasonFor(Availability::Unsafe);
    case Outcome::TooLarge: return "The browser's cookie database is unusually large, so it was not read.";
    case Outcome::ChangedWhileReading: return "The browser changed its cookie database while it was being read.";
    case Outcome::CopyFailed: return "A private working copy of the cookie database could not be made.";
    case Outcome::NotADatabase: return "The browser's cookie file is not a readable database.";
    case Outcome::UnexpectedSchema: return "The browser's cookie database has an unexpected layout.";
    case Outcome::TooManyRows: return "The browser's cookie database holds an unexpected number of YouTube cookies, so it was not read.";
    case Outcome::NoCookies: return "The browser holds no YouTube cookies.";
    case Outcome::NotSignedIn: return "The browser holds YouTube cookies, but it is not signed in.";
    case Outcome::Rejected: return "The sign-in read from the browser was not accepted.";
    case Outcome::Cancelled: return "The import was cancelled.";
    }
    return "";
}

}}}}}
