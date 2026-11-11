// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// ACC-04 paste and file import through SailTune's pinned parser (used as is):
// a Cookie header, copied request headers, "Copy as cURL" and a Netscape
// cookies.txt all import; a pasted Authorization header is discarded; input
// without __Secure-3PAPISID or SAPISID is refused with a message and nothing
// is stored; reasons never echo the input. SailTune's own corpus for the
// parser runs in tests/shared_accounts. All values are synthetic.
#include "Clients/Sailfish/Presentation/AccountController.h"
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/AccountStore.h"
#include "Core/Account/Infrastructure/PinnedSessionParser.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTimer>
#include <cstdio>

using namespace nordtile::video;
using namespace nordtile::video::account;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const qint64 now = 1800000000; // 2027-01-15

static QString value(const SessionCookies &s, const char *name) {
    const Cookie *c = s.find(name);
    return c ? QString::fromStdString(c->value) : QString();
}

static void formats() {
    PinnedSessionParser parser([] { return now; });
    QString reason;

    SessionCookies header;
    CHECK(parser.parse(QStringLiteral("Cookie: SID=fake-sid; __Secure-3PAPISID=fake-3p; PREF=f6=8; NOTNEEDED=x"), &header, &reason));
    CHECK(value(header, "__Secure-3PAPISID") == QStringLiteral("fake-3p") && value(header, "SID") == QStringLiteral("fake-sid"));
    CHECK(!header.find("NOTNEEDED")); // only allow-listed names are kept
    CHECK(header.find("SID")->domain == ".youtube.com");

    SessionCookies block;
    CHECK(parser.parse(QStringLiteral("GET /youtubei/v1/browse HTTP/2\nHost: www.youtube.com\n"
                                      "Authorization: SAPISIDHASH 1_deadbeef\nCookie: SAPISID=fake-sapisid; SSID=fake-ssid\n"
                                      "X-Goog-AuthUser: 1\n"), &block, &reason));
    CHECK(value(block, "SAPISID") == QStringLiteral("fake-sapisid") && value(block, "SSID") == QStringLiteral("fake-ssid"));
    for (const Cookie &c : block.cookies) CHECK(c.name != "Authorization" && QString::fromStdString(c.value).indexOf(QStringLiteral("deadbeef")) < 0);

    SessionCookies curl;
    CHECK(parser.parse(QStringLiteral("curl 'https://www.youtube.com/youtubei/v1/browse' -H 'accept: */*' "
                                      "-H 'cookie: SAPISID=fake-curl; HSID=fake-hsid' --compressed"), &curl, &reason));
    CHECK(value(curl, "SAPISID") == QStringLiteral("fake-curl") && value(curl, "HSID") == QStringLiteral("fake-hsid"));

    SessionCookies netscape;
    CHECK(parser.parse(QStringLiteral("# Netscape HTTP Cookie File\n"
                                      ".youtube.com\tTRUE\t/\tTRUE\t1900000000\tSAPISID\tfake-ns\n"
                                      "#HttpOnly_.youtube.com\tTRUE\t/\tTRUE\t1900000000\tSID\tfake-ns-sid\n"
                                      ".youtube.com\tTRUE\t/\tTRUE\t1000000000\tHSID\tfake-expired\n"
                                      ".example.org\tTRUE\t/\tTRUE\t1900000000\tSID\tother-site\n"), &netscape, &reason));
    CHECK(value(netscape, "SAPISID") == QStringLiteral("fake-ns"));
    CHECK(value(netscape, "SID") == QStringLiteral("fake-ns-sid")); // the #HttpOnly_ line, not the other site
    CHECK(!netscape.find("HSID"));                                   // expired

    // refusals: fixed reasons, never the input
    SessionCookies none;
    CHECK(!parser.parse(QStringLiteral("Cookie: PREF=f6=8; SID=fake-sid"), &none, &reason));
    CHECK(reason == QStringLiteral("no-sign-in") && none.empty());
    // a signing cookie alone is not a session either (the parser also needs a session cookie)
    CHECK(!parser.parse(QStringLiteral("Cookie: SAPISID=fake-only"), &none, &reason));
    CHECK(reason == QStringLiteral("no-sign-in") && none.empty());
    CHECK(!parser.parse(QStringLiteral("hello, this is not a cookie export"), &none, &reason));
    CHECK(reason == QStringLiteral("unrecognized") && !reason.contains(QStringLiteral("hello")));
    CHECK(!parser.parse(QString(), &none, &reason) && none.empty());
}

class NullTransport final : public AuthTransport {
public:
    int sent = 0;
    void send(const AuthRequest &, QObject *context, Done done) override {
        ++sent;
        QTimer::singleShot(0, context, [done] { done(AuthReply()); });
    }
};

static void throughTheApp(const QString &dir) {
    for (int variant = 0; variant < 2; ++variant) { // ACC-04 on both variants (STORE with a vault)
        class Vault final : public SecretVault {
        public:
            bool available() const override { return true; }
            bool store(const QByteArray &s) override { blob = s; return true; }
            bool load(QByteArray *s) override { if (blob.isEmpty()) return false; *s = blob; return true; }
            bool erase() override { blob.clear(); return true; }
            QByteArray blob;
        } vault;
        AccountStoreOptions o;
        o.variant = variant ? BuildVariant::Store : BuildVariant::Full;
        o.privateDir = dir + QStringLiteral("/v%1").arg(variant);
        o.vault = &vault;
        AccountStore store(o);
        store.open();
        NullTransport net;
        AccountService service(&store, &net);
        PinnedSessionParser parser([] { return now; });
        sailfish::AccountController::Options ui;
        ui.variant = o.variant;
        ui.home = dir + QStringLiteral("/home");
        ui.scratchDir = o.privateDir + QStringLiteral("/scratch");
        sailfish::AccountController controller(&store, &service, &parser, ui);
        CHECK(controller.methods().contains(QStringLiteral("paste")));

        // no signing cookie: refused with a message, nothing stored
        CHECK(!controller.importText(QStringLiteral("Cookie: SID=fake-sid; HSID=fake-hsid")));
        CHECK(!controller.lastError().isEmpty() && !controller.lastError().contains(QStringLiteral("fake")));
        CHECK(!store.signedIn() && vault.blob.isEmpty());
        // a cookies.txt file
        const QString file = dir + QStringLiteral("/cookies-%1.txt").arg(variant);
        {
            QFile f(file);
            f.open(QIODevice::WriteOnly);
            f.write("# Netscape HTTP Cookie File\n.youtube.com\tTRUE\t/\tTRUE\t1900000000\t__Secure-3PAPISID\tfake-file\n"
                    ".youtube.com\tTRUE\t/\tTRUE\t1900000000\t__Secure-3PSID\tfake-file-sid\n");
        }
        CHECK(controller.importFile(file));
        CHECK(store.signedIn() && !vault.blob.isEmpty());
        const SessionCookies s = store.session();
        CHECK(value(s, "__Secure-3PAPISID") == QStringLiteral("fake-file"));
        controller.signOut();
        CHECK(!store.signedIn() && vault.blob.isEmpty());
    }
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QString::fromLocal8Bit(qgetenv("TEST_DIR"));
    formats();
    throughTheApp(dir);
    std::printf("account paste tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
