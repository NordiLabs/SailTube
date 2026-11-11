// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML view of the optional account (context property `account`). Signed out
// is a normal state; nothing in the app waits for this object (ACC-01).
// Methods are offered in the ACC-02 rank order for the build variant.

#include "Core/Account/Domain/AccountTypes.h"
#include <QObject>
#include <QStringList>
#include <QVariantList>

namespace nordtile { namespace video {
namespace account { class AccountService; class AccountStore; class SessionTextParser; }
namespace sailfish {

class AccountController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int state READ state NOTIFY changed)              // account::SessionState
    Q_PROPERTY(bool hasSession READ hasSession NOTIFY changed)
    Q_PROPERTY(bool expired READ expired NOTIFY changed)
    Q_PROPERTY(bool unreachable READ unreachable NOTIFY changed)
    Q_PROPERTY(QString accountName READ accountName NOTIFY changed)
    Q_PROPERTY(QString stateText READ stateText NOTIFY changed)
    Q_PROPERTY(QVariantList accounts READ accounts NOTIFY changed)
    Q_PROPERTY(QStringList methods READ methods CONSTANT)        // "browser", "paste", "webview" in rank order
    Q_PROPERTY(bool disclosurePending READ disclosurePending NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
    struct Options {
        account::BuildVariant variant = account::BuildVariant::Full;
        bool webViewBuiltIn = false;   // ACC-05, only after E1
        QString home;                  // for the browser jar probe (FULL)
        QString scratchDir;            // private, for the jar copy
    };
    AccountController(account::AccountStore *store, account::AccountService *service,
                      account::SessionTextParser *parser, const Options &options, QObject *parent = nullptr);

    int state() const;
    bool hasSession() const;
    bool expired() const;
    bool unreachable() const;
    QString accountName() const;
    QString stateText() const;
    QVariantList accounts() const;
    QStringList methods() const { return methods_; }
    bool disclosurePending() const;
    QString lastError() const { return error_; }

    // ACC-03 [FULL]. false with lastError set; nothing stored on failure.
    Q_INVOKABLE bool importFromBrowser();
    // ACC-04: pasted text (Cookie header, headers, cURL, cookies.txt).
    Q_INVOKABLE bool importText(const QString &text);
    // ACC-04: a picked file (at most 1 MiB), then as importText.
    Q_INVOKABLE bool importFile(const QString &path);
    Q_INVOKABLE void verifyNow();
    Q_INVOKABLE bool select(int index);
    Q_INVOKABLE void signOut();
    Q_INVOKABLE void acknowledgeDisclosure();

    static const int maxImportFileBytes = 1024 * 1024;

signals:
    void changed();
    void imported();

private:
    bool store(const account::SessionCookies &cookies);
    void fail(const QString &message);

    account::AccountStore *store_;
    account::AccountService *service_;
    account::SessionTextParser *parser_;
    Options options_;
    QStringList methods_;
    QString error_;
};

}}}
