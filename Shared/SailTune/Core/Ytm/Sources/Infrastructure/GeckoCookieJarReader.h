// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Reads the stock Sailfish Browser's Gecko cookie database without ever touching the original.
// Qt Core + Qt Sql (QSQLITE), plus the POSIX helpers of PrivateScratch.h.
//
// SPEC (clean-room specification of the reference app, FINTUNE_SPEC.md 4.1) followed here:
//  * three candidate locations, in order, no profile globbing; the first file that holds
//    YouTube/Google cookies wins;
//  * the database is never opened in place: it is copied into a private working directory and
//    read there; the working copy is always removed afterwards;
//  * the public moz_cookies table, hosts of youtube.com / google.com.
// Our own choices on top (INFERENCE where marked):
//  * the "-wal" file is copied together with the database, so a login the browser has not yet
//    checkpointed is visible; the "-shm" file is deliberately NOT copied. It is only an index of
//    the log, SQLite rebuilds it from the copied log when it opens the copy, and a copy of it made
//    at another instant than the copy of the log could disagree with it (SQLite documents the
//    wal-index as transient). The specification text says "-wal/-shm"; skipping the index is
//    the safe reading of it (INFERENCE: not yet confirmed on a device with a running browser);
//  * the source files are stat()ed before and after the copy; if either changed (the browser
//    wrote or checkpointed in between) the copy is repeated, at most Limits::kCopyAttempts times;
//  * only rows of the session allow-list's cookie names are SELECTed (everything else in the
//    jar stays in the SQLite page cache of the private copy and is never turned into a string),
//    at most Limits::kMaxRows of them, values at most Limits::kMaxValueBytes long;
//  * read-only in spirit: PRAGMA query_only is set on the copy; the original is opened by nothing
//    but a read-only, no-follow descriptor.
//
// Values come out of Qt's SQL layer as QVariant/QString, which cannot be zeroed; this class wipes
// every buffer it owns (the std::string copies and the file buffers) and keeps the exposure short.
// Nothing is logged. No exception is thrown by this code.
#pragma once

#include "../Domain/BrowserImportTypes.h"
#include "../Ports/CookieJarReaderPort.h"
#include <QString>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace infrastructure {

class GeckoCookieJarReader final : public ports::CookieJarReaderPort {
public:
    enum class Stage { AfterCopy, BeforeCleanup };

    struct Config {
        QString homeDir;                            // empty = QDir::homePath()
        std::vector<std::string> candidates;        // relative to home; empty = domain::candidateLocation(0..2)
        QString scratchRoot;                        // private working area, e.g. <CacheLocation>/ytm-browser-import (created 0700)
        std::size_t maxDbBytes = static_cast<std::size_t>(domain::Limits::kMaxDbBytes);
        std::size_t maxWalBytes = static_cast<std::size_t>(domain::Limits::kMaxWalBytes);
        int maxRows = static_cast<int>(domain::Limits::kMaxRows);
        int copyAttempts = static_cast<int>(domain::Limits::kCopyAttempts);
        // Tests only: called with the working directory's path (no data) once the copy is complete
        // (before the "did the source change meanwhile" check) and once the query is finished, just
        // before the directory is removed.
        std::function<void(const std::string &dir, Stage stage)> inspectScratch;
    };

    // Also removes the working directories an earlier, interrupted run may have left behind.
    explicit GeckoCookieJarReader(const Config &config);

    domain::Availability probe() const override;
    ports::JarRead read() override;
    // Removes leftovers of an interrupted run (also done by the constructor and before each read()).
    int sweepStale();

private:
    Config cfg_;
    std::string home() const;
    std::string candidatePath(const std::string &home, int index) const;
    std::size_t candidateCount() const;
    ports::JarRead readCandidate(const std::string &path, int index);
};

}}}}}
