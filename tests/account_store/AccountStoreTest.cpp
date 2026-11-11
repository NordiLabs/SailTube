// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// QtCore checks of the account session store (ACC-08, ACC-10, NFR-SEC-03).
// Build: tests/account_store/run.sh
#include "Core/Account/Infrastructure/AccountStore.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStringList>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

using namespace nordtile::video::account;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Every message the code under test logs, scanned for cookie values at the end.
static QMutex logMutex;
static QStringList logLines;
static void captureMessage(QtMsgType, const QMessageLogContext &, const QString &msg) {
    QMutexLocker lock(&logMutex);
    logLines << msg;
}

class FakeVault : public SecretVault {
public:
    bool avail = true;
    bool failStore = false;
    bool has = false;
    QByteArray data;
    int stores = 0;
    int erases = 0;
    bool available() const override { return avail; }
    bool store(const QByteArray &secret) override {
        ++stores;
        if (!avail || failStore) return false;
        data = secret;
        has = true;
        return true;
    }
    bool load(QByteArray *secret) override {
        if (!avail || !has) return false;
        *secret = data;
        return true;
    }
    bool erase() override {
        ++erases;
        data.clear();
        has = false;
        return true;
    }
};

static QString base;
static int dirCounter = 0;

static QString freshDir(const char *name) {
    const QString d = QDir(base).filePath(QString::fromLatin1(name) + QString::number(++dirCounter));
    QDir(d).removeRecursively();
    QDir().mkpath(d);
    return QDir(d).filePath(QStringLiteral("account")); // the store creates this one
}

static int modeOf(const QString &path) {
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) return -1;
    return int(st.st_mode & 0777);
}

static unsigned long long inodeOf(const QString &path) {
    struct stat st;
    if (::stat(QFile::encodeName(path).constData(), &st) != 0) return 0;
    return (unsigned long long)st.st_ino;
}

static QStringList filesUnder(const QString &dir) {
    QStringList out;
    QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (it.hasNext()) out << QDir(dir).relativeFilePath(it.next());
    out.sort();
    return out;
}

static QByteArray readAll(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QByteArray();
    return f.readAll();
}

static Cookie cookie(const char *name, const char *value, const char *domain = ".youtube.com",
                     long long expires = 1900000000LL) {
    Cookie c;
    c.name = name;
    c.value = value;
    c.domain = domain;
    c.expiresSec = expires;
    return c;
}

static SessionCookies fixture() {
    SessionCookies s;
    s.cookies.push_back(cookie("__Secure-3PAPISID", "fake-3papisid"));
    s.cookies.push_back(cookie("SAPISID", "fake-sapisid"));
    s.cookies.push_back(cookie("__Secure-3PSIDCC", "fake-3psidcc"));
    s.cookies.push_back(cookie("SID", "fake-sid", ".google.com"));
    s.cookies.push_back(cookie("PREF", "fake-pref", "www.youtube.com", 0)); // untracked, imported anyway
    return s;
}

static std::string valueOf(const SessionCookies &s, const char *name) {
    const Cookie *c = s.find(name);
    return c ? c->value : std::string("<absent>");
}

static AccountStoreOptions opts(const QString &dir, BuildVariant v = BuildVariant::Full, SecretVault *vault = nullptr) {
    AccountStoreOptions o;
    o.variant = v;
    o.privateDir = dir;
    o.vault = vault;
    return o;
}

static SetCookieValue rot(const char *name, const char *value, bool expired = false) {
    SetCookieValue r;
    r.name = name;
    r.value = value;
    r.domain = ".youtube.com";
    r.expired = expired;
    return r;
}

// ACC-10 [FULL], vault unavailable: 0600 file in a 0700 private dir, temp file + rename.
static void testFullFileFallback() {
    for (int withVault = 0; withVault < 2; ++withVault) {
        const QString dir = freshDir("full");
        FakeVault unavailable;
        unavailable.avail = false;
        AccountStore store(opts(dir, BuildVariant::Full, withVault ? &unavailable : nullptr));
        CHECK(!store.open());
        CHECK(store.usingFileFallback());
        CHECK(store.importSession(fixture()) == ImportRefusal::None);
        CHECK(store.signedIn());
        CHECK(unavailable.stores == 0);
        CHECK(store.sessionFilePath() == QDir(dir).filePath(QStringLiteral("session.json")));
        CHECK(modeOf(dir) == 0700);
        CHECK(modeOf(store.sessionFilePath()) == 0600); // umask is 0 here: the chmod is explicit
        CHECK(filesUnder(dir) == QStringList() << QStringLiteral("session.json")); // no temp left
        const unsigned long long first = inodeOf(store.sessionFilePath());

        AccountSelection sel;
        sel.authUser = 1;
        CHECK(store.setSelection(sel));
        // A rewrite replaces the file (rename of a new temp), never edits it in place.
        CHECK(inodeOf(store.sessionFilePath()) != first);
        CHECK(modeOf(store.sessionFilePath()) == 0600);
        CHECK(filesUnder(dir) == QStringList() << QStringLiteral("session.json"));

        AccountStore reopened(opts(dir, BuildVariant::Full, withVault ? &unavailable : nullptr));
        CHECK(reopened.open());
        CHECK(reopened.signedIn());
        CHECK(reopened.session().cookies.size() == 5);
        CHECK(valueOf(reopened.session(), "__Secure-3PAPISID") == "fake-3papisid");
        CHECK(valueOf(reopened.session(), "PREF") == "fake-pref");
        const Cookie *pref = reopened.session().find("PREF");
        CHECK(pref && pref->domain == "www.youtube.com" && pref->expiresSec == 0);
        const Cookie *sid = reopened.session().find("SID");
        CHECK(sid && sid->domain == ".google.com" && sid->expiresSec == 1900000000LL && sid->secure);
        CHECK(reopened.selection().authUser == 1);
    }
}

// ACC-10 [FULL]: "the disclosure shows once across two restarts".
static void testDisclosureOnce() {
    const QString dir = freshDir("disclosure");
    {
        AccountStore s(opts(dir));
        CHECK(!s.open());
        CHECK(!s.disclosurePending()); // nothing stored yet
        CHECK(s.importSession(fixture()) == ImportRefusal::None);
        CHECK(s.disclosurePending());
        s.markDisclosed();
        CHECK(!s.disclosurePending());
    }
    for (int restart = 0; restart < 2; ++restart) {
        AccountStore s(opts(dir));
        CHECK(s.open());
        CHECK(s.signedIn());
        CHECK(!s.disclosurePending());
    }
    CHECK(!readAll(QDir(dir).filePath(QStringLiteral("disclosure-shown"))).contains("fake-"));
    CHECK(modeOf(QDir(dir).filePath(QStringLiteral("disclosure-shown"))) == 0600);

    // Not marked: still pending after a restart.
    const QString dir2 = freshDir("disclosure-unmarked");
    {
        AccountStore s(opts(dir2));
        CHECK(s.importSession(fixture()) == ImportRefusal::None);
        CHECK(s.disclosurePending());
    }
    {
        AccountStore s(opts(dir2));
        CHECK(s.open());
        CHECK(s.disclosurePending());
    }
    // With the vault there is nothing to disclose.
    FakeVault vault;
    AccountStore v(opts(freshDir("disclosure-vault"), BuildVariant::Full, &vault));
    CHECK(v.importSession(fixture()) == ImportRefusal::None);
    CHECK(!v.usingFileFallback());
    CHECK(!v.disclosurePending());
}

// ACC-10 [STORE], vault unavailable: import refused, nothing written to disk.
static void testStoreFailsClosed() {
    for (int withVault = 0; withVault < 2; ++withVault) {
        const QString dir = freshDir("store");
        FakeVault unavailable;
        unavailable.avail = false;
        AccountStore store(opts(dir, BuildVariant::Store, withVault ? &unavailable : nullptr));
        CHECK(!store.open());
        CHECK(!store.usingFileFallback());
        CHECK(store.importSession(fixture()) == ImportRefusal::NoSecureStorage);
        CHECK(!store.signedIn());
        store.markDisclosed();
        CHECK(!store.disclosurePending());
        CHECK(store.writeExtractorJar().isEmpty());
        AccountSelection sel;
        sel.authUser = 2;
        CHECK(!store.setSelection(sel));
        CHECK(unavailable.stores == 0);
        CHECK(!QFileInfo(dir).exists());
        CHECK(filesUnder(QFileInfo(dir).absolutePath()).isEmpty());
    }
    // STORE with the vault: stored there, still no session file.
    FakeVault vault;
    const QString dir = freshDir("store-vault");
    AccountStore store(opts(dir, BuildVariant::Store, &vault));
    CHECK(store.importSession(fixture()) == ImportRefusal::None);
    CHECK(vault.stores == 1 && vault.has);
    CHECK(!QFile::exists(store.sessionFilePath()));
    CHECK(filesUnder(QFileInfo(dir).absolutePath()).isEmpty());
}

// ACC-03/04 gate: no signing secret, nothing stored.
static void testNoSigningSecret() {
    SessionCookies s;
    s.cookies.push_back(cookie("SID", "fake-sid"));
    s.cookies.push_back(cookie("HSID", "fake-hsid"));
    FakeVault vault;
    const QString dir = freshDir("nosecret");
    AccountStore withVault(opts(dir, BuildVariant::Full, &vault));
    CHECK(withVault.importSession(s) == ImportRefusal::NoSigningSecret);
    CHECK(vault.stores == 0);
    AccountStore file(opts(dir));
    CHECK(file.importSession(s) == ImportRefusal::NoSigningSecret);
    CHECK(!file.signedIn());
    CHECK(!QFileInfo(dir).exists());

    SessionCookies sapisidOnly;
    sapisidOnly.cookies.push_back(cookie("SAPISID", "fake-sapisid"));
    CHECK(file.importSession(sapisidOnly) == ImportRefusal::None);
}

// ACC-10: the vault path, a failing vault, and the move from the file to a returning vault.
static void testVault() {
    FakeVault vault;
    const QString dir = freshDir("vault");
    {
        AccountStore s(opts(dir, BuildVariant::Full, &vault));
        CHECK(s.importSession(fixture()) == ImportRefusal::None);
        CHECK(vault.has);
        CHECK(!QFile::exists(s.sessionFilePath()));
        CHECK(!QFileInfo(dir).exists());
    }
    {
        AccountStore s(opts(dir, BuildVariant::Full, &vault));
        CHECK(s.open());
        CHECK(valueOf(s.session(), "SAPISID") == "fake-sapisid");
        CHECK(s.session().cookies.size() == 5);
    }

    FakeVault failing;
    failing.failStore = true;
    const QString dir2 = freshDir("vault-failing");
    AccountStore f(opts(dir2, BuildVariant::Full, &failing));
    CHECK(f.importSession(fixture()) == ImportRefusal::WriteFailed);
    CHECK(!f.signedIn());
    CHECK(!QFileInfo(dir2).exists()); // no silent fall back to the file

    // Session in the FULL file, then the vault becomes available: moved out of the file.
    const QString dir3 = freshDir("vault-migrate");
    {
        AccountStore s(opts(dir3));
        CHECK(s.importSession(fixture()) == ImportRefusal::None);
        CHECK(QFile::exists(s.sessionFilePath()));
    }
    FakeVault later;
    AccountStore m(opts(dir3, BuildVariant::Full, &later));
    CHECK(m.open());
    CHECK(later.has);
    CHECK(!QFile::exists(m.sessionFilePath()));
    CHECK(!m.usingFileFallback());
    CHECK(valueOf(m.session(), "__Secure-3PSIDCC") == "fake-3psidcc");
}

// Generation bumps and the selection reset on a new import; the selection persists.
static void testImportAndSelection() {
    const QString dir = freshDir("selection");
    AccountStore s(opts(dir));
    const std::uint64_t g0 = s.generation();
    CHECK(s.importSession(fixture()) == ImportRefusal::None);
    const std::uint64_t g1 = s.generation();
    CHECK(g1 > g0);
    AccountSelection sel;
    sel.authUser = 2;
    sel.pageId = "fake-page-id";
    CHECK(s.setSelection(sel));
    sel.authUser = 99;
    CHECK(!s.setSelection(sel)); // out of the 0..7 probe range
    {
        AccountStore r(opts(dir));
        CHECK(r.open());
        CHECK(r.selection().authUser == 2 && r.selection().pageId == "fake-page-id");
    }
    CHECK(s.importSession(fixture()) == ImportRefusal::None);
    CHECK(s.generation() > g1);
    CHECK(s.selection() == AccountSelection());
    AccountStore r(opts(dir));
    CHECK(r.open());
    CHECK(r.selection() == AccountSelection());
}

// ACC-08: update, ignore untracked, add tracked, delete on expiry, write only on change, stale generation.
static void testRotation() {
    const QString dir = freshDir("rotation");
    FakeVault vault;
    AccountStore file(opts(dir));
    AccountStore vaulted(opts(freshDir("rotation-vault"), BuildVariant::Full, &vault));
    CHECK(file.importSession(fixture()) == ImportRefusal::None);
    CHECK(vaulted.importSession(fixture()) == ImportRefusal::None);

    std::vector<SetCookieValue> r1(1, rot("__Secure-3PSIDCC", "fake-3psidcc-rotated"));
    CHECK(file.applyRotation(r1, file.generation()));
    CHECK(valueOf(file.session(), "__Secure-3PSIDCC") == "fake-3psidcc-rotated");
    {
        AccountStore r(opts(dir));
        CHECK(r.open());
        CHECK(valueOf(r.session(), "__Secure-3PSIDCC") == "fake-3psidcc-rotated");
    }
    CHECK(modeOf(file.sessionFilePath()) == 0600);

    // Untracked new name: ignored, nothing written.
    const unsigned long long ino = inodeOf(file.sessionFilePath());
    const QDateTime mtime = QFileInfo(file.sessionFilePath()).lastModified();
    const qint64 size = QFileInfo(file.sessionFilePath()).size();
    std::vector<SetCookieValue> untracked(1, rot("FAKE_UNTRACKED", "fake-untracked"));
    CHECK(!file.applyRotation(untracked, file.generation()));
    CHECK(file.session().find("FAKE_UNTRACKED") == nullptr);
    // Unchanged response: nothing written (same inode, mtime and size).
    CHECK(!file.applyRotation(r1, file.generation()));
    CHECK(inodeOf(file.sessionFilePath()) == ino);
    CHECK(QFileInfo(file.sessionFilePath()).lastModified() == mtime);
    CHECK(QFileInfo(file.sessionFilePath()).size() == size);
    CHECK(filesUnder(dir) == QStringList() << QStringLiteral("session.json"));

    const int storesBefore = vault.stores;
    std::vector<SetCookieValue> same(1, rot("SAPISID", "fake-sapisid"));
    CHECK(!vaulted.applyRotation(same, vaulted.generation()));
    CHECK(!vaulted.applyRotation(untracked, vaulted.generation()));
    CHECK(vault.stores == storesBefore);

    // A tracked name not stored yet is added; an expired stored one is removed.
    std::vector<SetCookieValue> r2;
    r2.push_back(rot("__Secure-1PSIDTS", "fake-1psidts"));
    r2.push_back(rot("PREF", "", true));
    CHECK(vaulted.applyRotation(r2, vaulted.generation()));
    CHECK(vault.stores == storesBefore + 1);
    CHECK(valueOf(vaulted.session(), "__Secure-1PSIDTS") == "fake-1psidts");
    CHECK(vaulted.session().find("PREF") == nullptr);

    // Stale generation (an import since): ignored.
    const std::uint64_t old = file.generation();
    CHECK(file.importSession(fixture()) == ImportRefusal::None);
    std::vector<SetCookieValue> r3(1, rot("SAPISID", "fake-sapisid-stale"));
    CHECK(!file.applyRotation(r3, old));
    CHECK(valueOf(file.session(), "SAPISID") == "fake-sapisid");
}

// ACC-10: a sign-out during a delayed in-flight rotation leaves no cookie on disk.
static void testSignOutDuringRotation() {
    for (int withVault = 0; withVault < 2; ++withVault) {
        const QString dir = freshDir("signout");
        FakeVault vault;
        AccountStore store(opts(dir, BuildVariant::Full, withVault ? &vault : nullptr));
        CHECK(store.importSession(fixture()) == ImportRefusal::None);
        const QString jar = store.writeExtractorJar();
        CHECK(!jar.isEmpty());
        const std::uint64_t sentWith = store.generation();
        std::atomic<bool> signedOut(false);
        std::atomic<int> applied(-1);
        std::thread inFlight([&]() {
            while (!signedOut.load()) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::vector<SetCookieValue> r(1, rot("__Secure-3PSIDCC", "fake-3psidcc-late"));
            r.push_back(rot("SAPISID", "fake-sapisid-late"));
            applied.store(store.applyRotation(r, sentWith) ? 1 : 0);
        });
        const int storesBefore = vault.stores;
        store.signOut();
        signedOut.store(true);
        inFlight.join();
        CHECK(applied.load() == 0);
        CHECK(!store.signedIn());
        CHECK(store.generation() > sentWith);
        CHECK(store.selection() == AccountSelection());
        CHECK(!QFile::exists(store.sessionFilePath()));
        CHECK(!QFile::exists(jar));
        CHECK(filesUnder(QFileInfo(dir).absolutePath()).isEmpty());
        if (withVault) {
            CHECK(vault.erases == 1 && !vault.has);
            CHECK(vault.stores == storesBefore);
        }
        AccountStore reopened(opts(dir, BuildVariant::Full, withVault ? &vault : nullptr));
        CHECK(!reopened.open());
        CHECK(!reopened.signedIn());
    }
}

// ACC-08: 20 concurrent responses each rotating a different cookie leave all 20 updates.
// ACC-09: selections and other calls run meanwhile without a re-entered lock.
static void testConcurrentRotations() {
    const QString dir = freshDir("concurrent");
    SessionCookies s;
    s.cookies.push_back(cookie("SAPISID", "fake-sapisid"));
    char name[32], value[32];
    for (int i = 0; i < 20; ++i) {
        std::snprintf(name, sizeof name, "FAKE_C%02d", i);
        std::snprintf(value, sizeof value, "fake-old-%02d", i);
        s.cookies.push_back(cookie(name, value));
    }
    AccountStore store(opts(dir));
    CHECK(store.importSession(s) == ImportRefusal::None);
    const std::uint64_t gen = store.generation();
    std::atomic<bool> go(false);
    std::atomic<int> changed(0);
    std::atomic<bool> stop(false);
    std::vector<std::thread> threads;
    for (int i = 0; i < 20; ++i) {
        threads.push_back(std::thread([&store, &go, &changed, gen, i]() {
            char n[32], v[32];
            std::snprintf(n, sizeof n, "FAKE_C%02d", i);
            std::snprintf(v, sizeof v, "fake-new-%02d", i);
            std::vector<SetCookieValue> r(1, rot(n, v));
            while (!go.load()) std::this_thread::yield();
            if (store.applyRotation(r, gen)) ++changed;
        }));
    }
    std::thread selector([&store, &go, &stop]() {
        while (!go.load()) std::this_thread::yield();
        int k = 0;
        while (!stop.load()) {
            AccountSelection sel;
            sel.authUser = (k++ % 3) + 1;
            store.setSelection(sel);
            (void)store.selection();
            (void)store.session();
            (void)store.disclosurePending();
            AccountStore::removeExtractorJar(store.writeExtractorJar());
        }
    });
    go.store(true);
    for (std::size_t i = 0; i < threads.size(); ++i) threads[i].join();
    stop.store(true);
    selector.join();
    CHECK(changed.load() == 20);
    AccountStore reopened(opts(dir));
    CHECK(reopened.open());
    const SessionCookies a = store.session();
    const SessionCookies b = reopened.session();
    int good = 0;
    for (int i = 0; i < 20; ++i) {
        std::snprintf(name, sizeof name, "FAKE_C%02d", i);
        std::snprintf(value, sizeof value, "fake-new-%02d", i);
        if (valueOf(a, name) == value && valueOf(b, name) == value) ++good;
    }
    CHECK(good == 20);
    CHECK(reopened.selection() == store.selection());
    CHECK(store.selection().authUser >= 1 && store.selection().authUser <= 3);
    CHECK(filesUnder(dir) == QStringList() << QStringLiteral("session.json"));
}

// ACC-10 / NFR-SEC-03: per-call extractor jar, 0600, in a private 0700 dir under privateDir, gone after.
static void testExtractorJar() {
    for (int variant = 0; variant < 2; ++variant) {
        FakeVault vault;
        const QString dir = freshDir("jar");
        AccountStore store(variant ? opts(dir, BuildVariant::Store, &vault) : opts(dir));
        CHECK(store.writeExtractorJar().isEmpty()); // signed out
        CHECK(!QFileInfo(dir).exists());
        CHECK(store.importSession(fixture()) == ImportRefusal::None);
        const QString jar = store.writeExtractorJar();
        const QString jar2 = store.writeExtractorJar();
        CHECK(!jar.isEmpty() && !jar2.isEmpty());
        const QString root = QFileInfo(dir).canonicalFilePath() + QStringLiteral("/");
        CHECK(QFileInfo(jar).canonicalFilePath().startsWith(root + QStringLiteral("jars/")));
        CHECK(QFileInfo(jar).fileName() == QStringLiteral("cookies.txt"));
        CHECK(QFileInfo(jar).absolutePath() != QFileInfo(jar2).absolutePath()); // fresh per call
        CHECK(modeOf(jar) == 0600);
        CHECK(modeOf(QFileInfo(jar).absolutePath()) == 0700);
        CHECK(modeOf(QDir(dir).filePath(QStringLiteral("jars"))) == 0700);
        CHECK(modeOf(dir) == 0700);
        CHECK(QDir(QFileInfo(jar).absolutePath()).entryList(QDir::Files | QDir::Hidden).size() == 1);

        const QList<QByteArray> lines = readAll(jar).split('\n');
        CHECK(lines.size() == 7 && lines.last().isEmpty());
        CHECK(lines.value(0) == "# Netscape HTTP Cookie File");
        CHECK(lines.contains(".youtube.com\tTRUE\t/\tTRUE\t1900000000\tSAPISID\tfake-sapisid"));
        CHECK(lines.contains(".google.com\tTRUE\t/\tTRUE\t1900000000\tSID\tfake-sid"));
        CHECK(lines.contains("www.youtube.com\tFALSE\t/\tTRUE\t0\tPREF\tfake-pref"));

        AccountStore::removeExtractorJar(jar);
        CHECK(!QFile::exists(jar));
        CHECK(!QFileInfo(QFileInfo(jar).absolutePath()).exists());
        CHECK(QFile::exists(jar2));
        AccountStore::removeExtractorJar(jar2);
        CHECK(!QFileInfo(QFileInfo(jar2).absolutePath()).exists());
        CHECK(QDir(QDir(dir).filePath(QStringLiteral("jars"))).entryList(QDir::AllEntries | QDir::NoDotAndDotDot
                                                                         | QDir::Hidden).isEmpty());
        AccountStore::removeExtractorJar(QString()); // no-op
        store.signOut();
        CHECK(store.writeExtractorJar().isEmpty());
    }
}

// open(): a corrupt or foreign file means signed out, without a crash.
static void testCorruptFile() {
    const char *const bodies[] = {
        "not json{", "", "[]", "{\"v\":2,\"cookies\":[]}", "{\"v\":1,\"cookies\":[]}",
        "{\"v\":1,\"cookies\":[{\"n\":\"\",\"v\":\"fake-x\"}]}", "{\"v\":1,\"cookies\":[7]}",
        "{\"v\":1,\"cookies\":{\"n\":\"SAPISID\"}}",
    };
    for (std::size_t i = 0; i < sizeof bodies / sizeof bodies[0]; ++i) {
        const QString dir = freshDir("corrupt");
        QDir().mkpath(dir);
        QFile f(QDir(dir).filePath(QStringLiteral("session.json")));
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(bodies[i]);
        f.close();
        AccountStore s(opts(dir));
        CHECK(!s.open());
        CHECK(!s.signedIn());
        CHECK(s.session().empty());
        CHECK(s.writeExtractorJar().isEmpty());
        CHECK(!QFile::exists(s.sessionFilePath()));
        CHECK(s.importSession(fixture()) == ImportRefusal::None); // recovers on the next import
    }
}

// FULL write failure (private dir unusable): WriteFailed, nothing leaked in the log line.
static void testWriteFailure() {
    const QString parent = freshDir("blocked");
    QDir().mkpath(QFileInfo(parent).absolutePath());
    QFile blocker(parent); // a regular file where the private dir should be
    CHECK(blocker.open(QIODevice::WriteOnly));
    blocker.close();
    AccountStore s(opts(QDir(parent).filePath(QStringLiteral("sub"))));
    CHECK(s.importSession(fixture()) == ImportRefusal::WriteFailed);
    CHECK(!s.signedIn());
}

// NFR-SEC-03: a secret file is never visible wider than 0600, not even as a temp (the umask is 0
// here, so a temp created at the default mode would show 0666), and a link planted at its name is
// replaced, never written through.
static void testCreatedPrivate() {
    const QString dir = freshDir("private-create");
    FakeVault unavailable;
    unavailable.avail = false;
    AccountStore store(opts(dir, BuildVariant::Full, &unavailable));
    store.open();
    CHECK(store.importSession(fixture()) == ImportRefusal::None);

    std::atomic<bool> stop(false);
    std::atomic<int> wide(0), seen(0);
    std::thread watcher([&]() {
        while (!stop) {
            QDirIterator it(dir, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                struct stat st;
                if (::lstat(QFile::encodeName(it.next()).constData(), &st) != 0) continue;
                ++seen;
                if ((st.st_mode & 0777) != 0600) ++wide;
            }
        }
    });
    for (int i = 0; i < 500; ++i) {
        AccountSelection sel;
        sel.authUser = i % 3;
        store.setSelection(sel);
        if (i % 25 == 0) AccountStore::removeExtractorJar(store.writeExtractorJar());
    }
    stop = true;
    watcher.join();
    CHECK(seen > 0);
    CHECK(wide == 0);
    CHECK(filesUnder(dir) == QStringList() << QStringLiteral("session.json")); // no temp left

    // A link planted at the session file: replaced by the new file, its target untouched.
    const QString victim = QDir(base).filePath(QStringLiteral("victim-session"));
    { QFile f(victim); f.open(QIODevice::WriteOnly); f.write("keep"); }
    QFile::remove(store.sessionFilePath());
    CHECK(QFile::link(victim, store.sessionFilePath()));
    AccountSelection sel;
    sel.authUser = 2;
    CHECK(store.setSelection(sel));
    CHECK(!QFileInfo(store.sessionFilePath()).isSymLink());
    CHECK(modeOf(store.sessionFilePath()) == 0600);
    CHECK(readAll(victim) == "keep");
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    ::umask(0); // proves every 0600/0700 mode is set explicitly
    qInstallMessageHandler(captureMessage);
    const QByteArray tmp = qgetenv("ACCOUNT_STORE_TMP");
    base = tmp.isEmpty() ? QDir::temp().filePath(QStringLiteral("sailtube-account-store")) : QString::fromLocal8Bit(tmp);
    QDir(base).removeRecursively();
    QDir().mkpath(base);

    testFullFileFallback();
    testDisclosureOnce();
    testStoreFailsClosed();
    testNoSigningSecret();
    testVault();
    testImportAndSelection();
    testRotation();
    testSignOutDuringRotation();
    testConcurrentRotations();
    testExtractorJar();
    testCorruptFile();
    testWriteFailure();
    testCreatedPrivate();

    // NFR-SEC-03: no cookie value in any log line.
    qInstallMessageHandler(0);
    int leaked = 0;
    for (int i = 0; i < logLines.size(); ++i) {
        if (logLines.at(i).contains(QStringLiteral("fake-"))) {
            ++leaked;
            std::printf("leaked log line #%d\n", i);
        }
    }
    CHECK(leaked == 0);
    CHECK(!logLines.isEmpty()); // the failure paths above did log (without values)

    QDir(base).removeRecursively();
    std::printf("account store tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
