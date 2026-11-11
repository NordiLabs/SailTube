// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The landing page setting (DSC-04): Subscriptions (default), Recommended (offered only when
// signed in) or Search. Stored as "catalog/home/landingPage" in the app's SailTube.conf. A
// stored Recommended choice falls back to Subscriptions while signed out.

#include <QObject>
#include <QScopedPointer>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace nordtile { namespace video { namespace sailfish {

class HomeSettings final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString landingPage READ landingPage WRITE setLandingPage NOTIFY changed)
    Q_PROPERTY(bool signedIn READ signedIn WRITE setSignedIn NOTIFY changed)
    Q_PROPERTY(QString effectiveLandingPage READ effectiveLandingPage NOTIFY changed)
    Q_PROPERTY(QString initialPage READ initialPage NOTIFY changed)
    Q_PROPERTY(QVariantList landingChoices READ landingChoices NOTIFY changed)
public:
    static const char *const kKey;            // "catalog/home/landingPage"
    static const char *const kSubscriptions;  // "subscriptions"
    static const char *const kRecommended;    // "recommended"
    static const char *const kSearch;         // "search"

    // AppConfigLocation/SailTube.conf.
    explicit HomeSettings(QObject *parent = nullptr);
    // A specific INI file (tests).
    HomeSettings(const QString &settingsFile, QObject *parent);

    QString landingPage() const;
    void setLandingPage(const QString &page);
    bool signedIn() const { return signedIn_; }
    void setSignedIn(bool signedIn);
    QString effectiveLandingPage() const;
    // The page file below qml/ for ApplicationWindow.initialPage: "pages/FeedPage.qml", ...
    QString initialPage() const;
    // [{value, label}] offered in the settings UI.
    QVariantList landingChoices() const;

    static QStringList landingPages(bool signedIn);

signals:
    void changed();

private:
    QScopedPointer<QSettings> settings_;
    bool signedIn_ = false;
};

}}}
