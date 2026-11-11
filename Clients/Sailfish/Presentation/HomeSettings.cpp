// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "HomeSettings.h"

#include <QCoreApplication>
#include <QStandardPaths>
#include <QVariantMap>

namespace nordtile { namespace video { namespace sailfish {

const char *const HomeSettings::kKey = "catalog/home/landingPage";
const char *const HomeSettings::kSubscriptions = "subscriptions";
const char *const HomeSettings::kRecommended = "recommended";
const char *const HomeSettings::kSearch = "search";

namespace {
QString defaultSettingsPath() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf");
}
}

HomeSettings::HomeSettings(QObject *parent) : HomeSettings(defaultSettingsPath(), parent) {}

HomeSettings::HomeSettings(const QString &settingsFile, QObject *parent)
    : QObject(parent), settings_(new QSettings(settingsFile, QSettings::IniFormat)) {
    settings_->setFallbacksEnabled(false);
}

QStringList HomeSettings::landingPages(bool signedIn) {
    QStringList pages;
    pages << QLatin1String(kSubscriptions);
    if (signedIn) pages << QLatin1String(kRecommended);
    pages << QLatin1String(kSearch);
    return pages;
}

QString HomeSettings::landingPage() const {
    const QString v = settings_->value(QLatin1String(kKey), QLatin1String(kSubscriptions)).toString();
    return landingPages(true).contains(v) ? v : QString::fromLatin1(kSubscriptions);
}

void HomeSettings::setLandingPage(const QString &page) {
    if (!landingPages(signedIn_).contains(page) || page == landingPage()) return;
    settings_->setValue(QLatin1String(kKey), page);
    settings_->sync();
    emit changed();
}

void HomeSettings::setSignedIn(bool signedIn) {
    if (signedIn == signedIn_) return;
    signedIn_ = signedIn;
    emit changed();
}

QString HomeSettings::effectiveLandingPage() const {
    const QString page = landingPage();
    return landingPages(signedIn_).contains(page) ? page : QString::fromLatin1(kSubscriptions);
}

QString HomeSettings::initialPage() const {
    const QString page = effectiveLandingPage();
    if (page == QLatin1String(kRecommended)) return QStringLiteral("pages/RecommendedPage.qml");
    if (page == QLatin1String(kSearch)) return QStringLiteral("pages/SearchPage.qml");
    return QStringLiteral("pages/FeedPage.qml");
}

QVariantList HomeSettings::landingChoices() const {
    QVariantList out;
    for (const QString &page : landingPages(signedIn_)) {
        QVariantMap m;
        m.insert(QStringLiteral("value"), page);
        QString label;
        //: Landing page choice: the subscriptions feed
        if (page == QLatin1String(kSubscriptions)) label = QCoreApplication::translate("HomeSettings", "Subscriptions");
        //: Landing page choice: recommendations of the signed-in account
        else if (page == QLatin1String(kRecommended)) label = QCoreApplication::translate("HomeSettings", "Recommended");
        //: Landing page choice: the search page
        else label = QCoreApplication::translate("HomeSettings", "Search");
        m.insert(QStringLiteral("label"), label);
        out.append(m);
    }
    return out;
}

}}}
