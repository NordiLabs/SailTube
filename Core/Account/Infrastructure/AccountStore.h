// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// ACC-10 storage of the imported session and the account selection.
//  * The Sailfish Secrets vault when available (SecretVault port; the adapter
//    comes from SailTune's pinned vault).
//  * [FULL] otherwise a 0600 file in the private data dir, written through a
//    private temp file and rename, with a one-time disclosure (persisted).
//  * [STORE] without the vault: fail closed, nothing is written to disk.
// Thread-safe: one mutex guards every read-modify-write (ACC-08), and
// signOut() deletes everything under it and bumps a generation, so a rotation
// computed before the sign-out can never write the session back.
// Never logs a cookie value.

#include "Core/Account/Domain/AccountPolicy.h"
#include <QByteArray>
#include <QMutex>
#include <QString>
#include <memory>

namespace nordtile { namespace video { namespace account {

class SecretVault {
public:
    virtual ~SecretVault() {}
    virtual bool available() const = 0;
    virtual bool store(const QByteArray &secret) = 0;
    virtual bool load(QByteArray *secret) = 0;   // false when absent or failing
    virtual bool erase() = 0;
};

struct AccountStoreOptions {
    BuildVariant variant = BuildVariant::Full;
    QString privateDir;            // AppDataLocation/account (created 0700)
    SecretVault *vault = nullptr;  // may be null = unavailable
};

enum class ImportRefusal { None, NoSigningSecret, NoSecureStorage, WriteFailed };

class AccountStore {
public:
    explicit AccountStore(const AccountStoreOptions &options);
    AccountStore(const AccountStore &) = delete;
    AccountStore &operator=(const AccountStore &) = delete;

    // Loads a stored session (vault first, then the FULL file).
    bool open();

    bool signedIn() const;
    SessionCookies session() const;          // a copy
    AccountSelection selection() const;
    std::uint64_t generation() const;        // bumped by every import and sign-out

    // ACC-03/04: gate, then store. STORE without the vault: NoSecureStorage, nothing written.
    ImportRefusal importSession(const SessionCookies &cookies);
    bool setSelection(const AccountSelection &selection);

    // ACC-08: fold Set-Cookie values. `generation` is the value read when the
    // request was sent; a stale generation (sign-out/import since) is ignored.
    // Writes only on change. Returns true when stored values changed.
    bool applyRotation(const std::vector<SetCookieValue> &rotated, std::uint64_t generation);

    // ACC-10: deletes the vault entry, the file and the selection, under the lock.
    void signOut();

    // FULL file fallback in use (vault unavailable) and the disclosure not yet shown.
    bool disclosurePending() const;
    void markDisclosed();
    bool usingFileFallback() const;
    QString sessionFilePath() const;          // tests

    // ACC-10: a fresh 0600 Netscape cookie jar for one extractor call, in a
    // private 0700 directory; the caller removes it with removeExtractorJar.
    // Empty when signed out.
    QString writeExtractorJar() const;
    static void removeExtractorJar(const QString &path);

private:
    bool persistLocked(const SessionCookies &cookies, const AccountSelection &selection);
    bool vaultUsable() const;
    QString disclosureFilePath() const;
    QString jarsRootPath() const;
    AccountStoreOptions options_;
    mutable QMutex mutex_;
    SessionCookies session_;
    AccountSelection selection_;
    std::uint64_t generation_ = 1;
    bool fileFallback_ = false;
};

}}}
