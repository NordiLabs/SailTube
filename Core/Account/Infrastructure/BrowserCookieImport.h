// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-03 [FULL]: import the session from the Sailfish Browser's cookie jar.
// Probes the three documented databases in order, copies the first present
// one together with its -wal and -shm files into a fresh 0700 private
// directory (a login that exists only in the WAL is read too), reads the
// youtube.com / google.com rows of moz_cookies from the copy through a
// read-only connection, and deletes the copy. The browser's own files are
// never opened for writing and the browser is never signed out.

#include "Core/Account/Domain/AccountTypes.h"
#include <QString>

namespace nordtile { namespace video { namespace account {

enum class BrowserImportError { None, NoBrowserDatabase, Unreadable, NoSession };

struct BrowserImportResult {
    BrowserImportError error = BrowserImportError::None;
    SessionCookies cookies;      // merged (ACC-03 collision rule), gate checked
    QString databasePath;        // which database was used (no secrets)
};

class BrowserCookieImport {
public:
    // `home`: the user's home directory; `scratchRoot`: a private directory of
    // the app (the copy goes into a fresh 0700 subdirectory and is removed).
    BrowserCookieImport(const QString &home, const QString &scratchRoot);
    BrowserImportResult run() const;
    // The first existing database of the probe list, or empty.
    QString findDatabase() const;

private:
    QString home_;
    QString scratchRoot_;
};

}}}
