// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The door between the import source and whatever holds the browser's cookies. Pure C++11.
// The real implementation reads a Gecko cookies.sqlite (infrastructure::GeckoCookieJarReader);
// the tests use fakes. A reader returns raw rows of ALLOW-LISTED cookie names only (their
// values are SECRET); choosing among them and composing the staged text is the job of
// domain::CookieJarSelection, which is pure and tested without a database.
#pragma once

#include "../../../Accounts/Domain/SessionTypes.h"
#include "../Domain/BrowserImportTypes.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace ports {

// One row of Gecko's public moz_cookies table (only the columns the selection uses).
struct RawCookieRow {
    std::string host;               // e.g. ".youtube.com"
    std::string name;
    std::string value;              // SECRET
    std::string path;               // "/" for the sign-in cookies
    std::string originAttributes;   // "" = ordinary context; "^userContextId=..." / "^firstPartyDomain=..." otherwise
    std::int64_t expiry = 0;        // seconds (a value above 1e11 is read as milliseconds); 0 = unknown
    bool hasExpiry = false;
    std::int64_t lastAccessed = 0;  // tie-break only; unit does not matter
    void wipe() { accounts::domain::wipeString(value); }
};

struct JarRead {
    domain::ReadStatus status = domain::ReadStatus::NotFound;
    int candidate = -1;             // index into domain::candidateLocation(), -1 = none
    int matchingRows = 0;           // rows of ANY name on YouTube/Google hosts in that file (a count; no row is kept)
    std::vector<RawCookieRow> rows; // allow-listed names only, SECRET values
    JarRead() = default;
    JarRead(const JarRead &) = default;
    JarRead(JarRead &&) = default;
    JarRead &operator=(const JarRead &) = default;
    JarRead &operator=(JarRead &&) = default;
    ~JarRead() { wipe(); }
    void wipe() {
        for (std::size_t i = 0; i < rows.size(); ++i) rows[i].wipe();
        rows.clear();
    }
};

class CookieJarReaderPort {
public:
    virtual ~CookieJarReaderPort() = default;
    // Cheap and free of side effects: stat-level checks only, no file is opened for reading, no
    // byte of any file is read. Safe to call from any thread, also while read() runs elsewhere.
    virtual domain::Availability probe() const = 0;
    // Blocking. Reads the first candidate that holds YouTube/Google cookies. May run on a worker
    // thread; one call at a time per reader. Never throws; never logs.
    virtual JarRead read() = 0;
};

}}}}}
