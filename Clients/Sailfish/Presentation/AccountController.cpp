// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "AccountController.h"

#include "Core/Account/Domain/AccountPolicy.h"
#include "Core/Account/Infrastructure/AccountService.h"
#include "Core/Account/Infrastructure/AccountStore.h"
#include "Core/Account/Infrastructure/BrowserCookieImport.h"
#include "Core/Account/Infrastructure/SessionTextImport.h"
#include <QFile>
#include <QUrl>

namespace nordtile { namespace video { namespace sailfish {

using account::SessionState;

const int AccountController::maxImportFileBytes;

AccountController::AccountController(account::AccountStore *store, account::AccountService *service,
                                     account::SessionTextParser *parser, const Options &options, QObject *parent)
    : QObject(parent), store_(store), service_(service), parser_(parser), options_(options) {
    for (account::SignInMethod m : account::signInMethods(options.variant, options.webViewBuiltIn)) {
        if (m == account::SignInMethod::BrowserImport) methods_ << QStringLiteral("browser");
        else if (m == account::SignInMethod::PasteOrFile) methods_ << QStringLiteral("paste");
        else methods_ << QStringLiteral("webview");
    }
    connect(service_, &account::AccountService::stateChanged, this, &AccountController::changed);
    connect(service_, &account::AccountService::accountsChanged, this, &AccountController::changed);
}

int AccountController::state() const { return static_cast<int>(service_->state()); }
bool AccountController::hasSession() const { return store_->signedIn(); }
bool AccountController::expired() const { return service_->state() == SessionState::Expired; }
bool AccountController::unreachable() const { return service_->state() == SessionState::Unreachable; }
QString AccountController::accountName() const { return service_->accountName(); }
bool AccountController::disclosurePending() const { return store_->disclosurePending(); }

QString AccountController::stateText() const {
    switch (service_->state()) {
    case SessionState::SignedOut:
        //: Account state: no session imported (the normal state; everything works without one)
        return tr("Not signed in");
    case SessionState::Checking:
        //: Account state while the session is being verified
        return tr("Checking the session…");
    case SessionState::SignedIn:
        if (service_->accountName().isEmpty()) {
            //: Account state when the account name is not known
            return tr("Signed in");
        }
        //: Account state, %1 is the account name
        return tr("Signed in as %1").arg(service_->accountName());
    case SessionState::Expired:
        //: Account state: YouTube no longer accepts the imported session
        return tr("The session has expired. Import it again.");
    case SessionState::Unreachable:
        //: Account state: the session could not be checked (offline or server error)
        return tr("Could not check the session. It will be checked again.");
    }
    return QString();
}

QVariantList AccountController::accounts() const {
    QVariantList out;
    const account::AccountSelection sel = service_->selection();
    for (const account::AccountEntry &e : service_->accounts()) {
        QVariantMap m;
        m.insert(QStringLiteral("name"), QString::fromStdString(e.name));
        m.insert(QStringLiteral("handle"), QString::fromStdString(e.handle));
        m.insert(QStringLiteral("photo"), QString::fromStdString(e.photoUrl));
        m.insert(QStringLiteral("brand"), !e.pageId.empty());
        m.insert(QStringLiteral("selected"), e.authUser == sel.authUser && e.pageId == sel.pageId);
        out << m;
    }
    return out;
}

void AccountController::fail(const QString &message) {
    error_ = message;
    emit changed();
}

bool AccountController::store(const account::SessionCookies &cookies) {
    switch (store_->importSession(cookies)) {
    case account::ImportRefusal::None:
        error_.clear();
        service_->sessionImported();   // ACC-07 "after import", then ACC-09 probe
        emit changed();
        emit imported();
        return true;
    case account::ImportRefusal::NoSigningSecret:
        //: Import error: the text or browser data holds no YouTube sign-in
        fail(tr("No YouTube sign-in was found. Sign in at youtube.com first."));
        return false;
    case account::ImportRefusal::NoSecureStorage:
        //: Import error (Harbour build): the Sailfish Secrets service is unavailable, so nothing is stored
        fail(tr("Secure storage is not available on this device, so the session cannot be kept."));
        return false;
    case account::ImportRefusal::WriteFailed:
        //: Import error: the session could not be saved
        fail(tr("The session could not be saved."));
        return false;
    }
    return false;
}

bool AccountController::importFromBrowser() {
    if (!methods_.contains(QStringLiteral("browser"))) return false;
    const account::BrowserImportResult r = account::BrowserCookieImport(options_.home, options_.scratchDir).run();
    switch (r.error) {
    case account::BrowserImportError::None:
        return store(r.cookies);
    case account::BrowserImportError::NoBrowserDatabase:
        //: Import error: the Sailfish Browser has no cookie database yet
        fail(tr("The Browser has no saved sign-in. Open youtube.com in the Browser and sign in."));
        return false;
    case account::BrowserImportError::Unreadable:
        //: Import error: the Browser's cookie database could not be read
        fail(tr("The Browser data could not be read. Close the Browser and try again."));
        return false;
    case account::BrowserImportError::NoSession:
        //: Import error: no YouTube sign-in in the Browser; private tabs keep none
        fail(tr("No YouTube sign-in was found in the Browser. Sign in at youtube.com in a normal tab, not a private one."));
        return false;
    }
    return false;
}

bool AccountController::importText(const QString &text) {
    if (!parser_) {
        //: Import error: pasting a session is not available in this build yet
        fail(tr("Importing pasted sessions is not available yet."));
        return false;
    }
    account::SessionCookies cookies;
    QString reason;
    if (!parser_->parse(text, &cookies, &reason)) {
        // Braced: Qt 5.6's lupdate drops a //: comment that directly follows a ')'.
        if (reason == QLatin1String("no-sign-in")) {
            //: Import error: the pasted cookies hold no YouTube sign-in
            fail(tr("These cookies hold no YouTube sign-in. Copy them while signed in at youtube.com."));
        } else if (reason == QLatin1String("expired")) {
            //: Import error: every cookie in the pasted cookies.txt has expired
            fail(tr("These cookies have expired. Sign in again and copy them anew."));
        } else if (reason == QLatin1String("too-large")) {
            //: Import error: the pasted text or file is too large
            fail(tr("This is too large to be a cookie export."));
        } else {
            //: Import error: the pasted text is not a cookie header, request headers, cURL command or cookies.txt
            fail(tr("This text holds no cookies. Paste a Cookie header, copied request headers, a cURL command or a cookies.txt."));
        }
        return false;
    }
    return store(cookies);
}

bool AccountController::importFile(const QString &path) {
    const QString local = path.startsWith(QLatin1String("file:")) ? QUrl(path).toLocalFile() : path;
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxImportFileBytes) {
        //: Import error: the chosen file could not be read or is too large
        fail(tr("The file could not be read."));
        return false;
    }
    return importText(QString::fromUtf8(file.readAll()));
}

void AccountController::verifyNow() { service_->verifyNow(); }
bool AccountController::select(int index) { return service_->selectAccount(index); }

void AccountController::signOut() {
    service_->signOut();
    error_.clear();
    emit changed();
}

void AccountController::acknowledgeDisclosure() {
    store_->markDisclosed();
    emit changed();
}

}}}
