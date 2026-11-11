// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// QML face of the time-limited beta (docs/BETA.md), context property `beta` (only in beta builds).
// Read by the shell banner, the About page and the "beta has ended" page.

#include "Core/Beta/Infrastructure/BetaClock.h"

#include <QObject>
#include <QTimer>

namespace nordtile { namespace video { namespace sailfish {

class BetaStatus : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool expired READ expired NOTIFY changed)
    Q_PROPERTY(bool bannerVisible READ bannerVisible NOTIFY changed)
    Q_PROPERTY(int daysLeft READ daysLeft NOTIFY changed)
    Q_PROPERTY(QString expiryText READ expiryText CONSTANT)     // "30 Dec 2026" in the device locale
    Q_PROPERTY(QString buildText READ buildText CONSTANT)       // "0.1.0-0.beta1"
    Q_PROPERTY(QString downloadUrl READ downloadUrl CONSTANT)   // where testers get the next build
public:
    BetaStatus(beta::BetaClock *clock, const QDate &expiryDate, const QString &buildText, QObject *parent = nullptr);
    bool expired() const { return evaluation_.phase == beta::Phase::Expired; }
    bool bannerVisible() const { return evaluation_.showBanner && !dismissed_; }
    int daysLeft() const { return evaluation_.daysLeft; }
    QString expiryText() const { return expiryText_; }
    QString buildText() const { return buildText_; }
    QString downloadUrl() const { return QStringLiteral("https://github.com/NordiLabs/SailTube/releases"); }
    Q_INVOKABLE void dismissBanner();
    void refresh();

signals:
    void changed();

private:
    beta::BetaClock *clock_;
    beta::Evaluation evaluation_;
    QString expiryText_, buildText_;
    bool dismissed_ = false;
    QTimer hourly_;
};

}}}
