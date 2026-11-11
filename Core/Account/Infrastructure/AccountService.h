// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The optional signed-in session (ACC-01, 06, 07, 08, 09, IMP-02 transport).
// Nothing else waits on it: every call is asynchronous, failures only change
// `state()`, and no other component is stopped or blocked by it (ACC-01).
//
//  * sign(): ACC-06 headers for one request, fresh SAPISIDHASH each time.
//  * post(): an authenticated InnerTube WEB POST (`?key=` kept, prettyPrint=false);
//    Set-Cookie answers are folded back (ACC-08).
//  * Verification (ACC-07): account/account_menu at start, after import, after a
//    switch, on demand and every 15 min while signed in. Inconclusive never
//    changes the state.
//  * Logins and brand channels (ACC-09): account/accounts_list with
//    X-Goog-AuthUser 0..7; the selection is only ever the stored one; a switch
//    emits cachesInvalidated().
//  * IMP-02: implements library::AccountBrowseTransport.

#include "Core/Account/Infrastructure/AccountStore.h"
#include "Core/Account/Infrastructure/AuthTransport.h"
#include "Core/Library/Infrastructure/AccountImport.h"
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <functional>
#include <vector>

namespace nordtile { namespace video {
namespace account {

class AccountService final : public QObject, public library::AccountBrowseTransport {
    Q_OBJECT
public:
    typedef std::function<qint64()> Clock; // ms since epoch

    // `transport` carries only authenticated requests (its own instance, never
    // shared with anonymous discovery). Both must outlive the service.
    AccountService(AccountStore *store, AuthTransport *transport, QObject *parent = nullptr);

    void setClockForTests(Clock clock);
    // The WEB client version for the request context (catalog's ClientIdentity
    // when wired; a built-in fallback otherwise).
    void setClientVersionProvider(std::function<QString()> provider);
    void setRequestLocale(const QString &hl, const QString &gl);
    // Runs a periodic verification when due (the service calls it every minute;
    // tests call it after moving the fake clock).
    void pollSchedule();
    int verificationsSent() const { return verificationsSent_; } // tests
    // Starts the schedule and verifies a stored session (ACC-07 "at start").
    void start();

    SessionState state() const { return state_; }
    QString accountName() const { return accountName_; }
    std::vector<AccountEntry> accounts() const { return accounts_; }
    AccountSelection selection() const { return store_->selection(); }

    // ACC-06. Empty when signed out.
    std::vector<std::pair<QByteArray, QByteArray>> sign() const;
    // POST https://www.youtube.com/youtubei/v1/<endpoint>?prettyPrint=false
    // with the WEB context added to `body`. done(ok, httpStatus, json).
    void post(const QString &endpoint, const QJsonObject &body,
              std::function<void(bool ok, int status, const QJsonObject &json)> done);

    // ACC-11: a signed request built elsewhere (e.g. the catalog's InnerTube client
    // through SignedCatalogTransport). Credentials are added here, fresh; any
    // Cookie/Authorization/X-Goog-* header in `request` is replaced. Rotations are
    // folded back. Signed out (or expired): `done` gets status 0 and nothing is sent.
    void sendSigned(const AuthRequest &request, QObject *context, std::function<void(const AuthReply &)> done);
    // True while signed-in features may be used (SignedIn, or Unreachable with a session).
    bool canSign() const;

    // ACC-10 / RES-08 cookie rung: one extractor (yt-dlp) call. begin writes a fresh
    // 0600 Netscape jar in the private dir (empty path when signed features are off);
    // end folds rotations the extractor wrote into the jar back (ACC-08 SHOULD) and
    // deletes the jar. Always call end, also on failure.
    struct ExtractorCall { QString jarPath; std::uint64_t generation = 0; };
    ExtractorCall beginExtractorCall() const;
    void endExtractorCall(const ExtractorCall &call);
    static std::vector<SetCookieValue> parseNetscapeJar(const QByteArray &text, qint64 nowSec);

    // After a successful import (ACC-03/04): verify, then probe the logins.
    void sessionImported();
    void verifyNow();
    void probeAccounts();
    // ACC-09: by index into accounts(). Stored, then verified; emits cachesInvalidated.
    bool selectAccount(int index);
    // ACC-10: deletes every secret and clears caches. Never touches the browser.
    void signOut();

    // library::AccountBrowseTransport (IMP-02)
    void browse(const QJsonObject &body, const QMap<QByteArray, QByteArray> &headers, Done done) override;

    // Parsers (tests).
    static Verification parseAccountMenu(int status, const QJsonObject &json, QString *accountName);
    static std::vector<AccountEntry> parseAccountsList(const QJsonObject &json, int authUser, bool *loggedOut);
    static std::vector<SetCookieValue> parseSetCookies(const std::vector<QByteArray> &headerValues, qint64 nowSec);

signals:
    void stateChanged();
    void accountsChanged();
    // Resolve and personalised caches must be dropped (switch, sign-out).
    void cachesInvalidated();

private:
    void setState(SessionState s);
    void onVerified(Verification v, const QString &name, std::uint64_t generation);
    void scheduleNext();
    void probeIndex(int index, std::vector<AccountEntry> found, std::vector<std::string> keys);
    void sendJson(const QString &endpoint, const QJsonObject &body,
                  const std::vector<std::pair<QByteArray, QByteArray>> &headers,
                  std::function<void(const AuthReply &)> done);
    std::vector<std::pair<QByteArray, QByteArray>> headersFor(const AccountSelection &selection, bool withPageId) const;

    AccountStore *store_;
    AuthTransport *transport_;
    std::function<QString()> clientVersion_;
    QString hl_ = QStringLiteral("en");
    QString gl_ = QStringLiteral("US");
    int verificationsSent_ = 0;
    Clock clock_;
    QTimer timer_;
    SessionState state_ = SessionState::SignedOut;
    QString accountName_;
    std::vector<AccountEntry> accounts_;
    qint64 lastCheckMs_ = -1;
    bool verifying_ = false;
    bool probing_ = false;
};

}}}
