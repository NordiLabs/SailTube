// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// PLT-03: the catalog's settings live in the application's one file (AppConfigLocation /
// SailTube.conf, AppSettings' file), and the keys an older build wrote through a default
// QSettings are moved there once.
#include "tests/catalog_common/TestSupport.h"

#include "Clients/Sailfish/Presentation/SettingsFile.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTemporaryDir>

using namespace nordtile::video::sailfish;

static void paths() {
    QStandardPaths::setTestModeEnabled(true);
    const QString config = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    CHECK_EQ(appSettingsFile(), config + QStringLiteral("/SailTube.conf"));
    // The legacy file is the default QSettings one: <org>/<app>.conf, outside AppConfigLocation.
    const QString legacy = legacyDefaultSettingsFile();
    CHECK(legacy.endsWith(QStringLiteral("/org.nordilabs.sailtube/SailTube.conf")));
    CHECK(!legacy.startsWith(config + QStringLiteral("/")));
}

static void migration() {
    QTemporaryDir dir;
    const QString from = dir.path() + QStringLiteral("/old/SailTube.conf");
    const QString to = dir.path() + QStringLiteral("/new/SailTube/SailTube.conf");
    QDir().mkpath(dir.path() + QStringLiteral("/old"));
    {
        QSettings old(from, QSettings::IniFormat);
        old.setValue(QStringLiteral("catalog/search/history"), QStringList() << QStringLiteral("nasa") << QStringLiteral("blender"));
        old.setValue(QStringLiteral("catalog/captions/preferred"), QStringLiteral("es"));
        old.setValue(QStringLiteral("catalog/ryd/enabled"), true);
    }
    {
        QSettings target(to, QSettings::IniFormat);
        target.setValue(QStringLiteral("playback/maxHeight"), 720);
        target.setValue(QStringLiteral("catalog/captions/preferred"), QStringLiteral("fr"));   // newer, kept
    }
    CHECK_EQ(migrateSettingsFile(from, to), 2);
    CHECK(!QFile::exists(from));
    QSettings merged(to, QSettings::IniFormat);
    CHECK(merged.value(QStringLiteral("catalog/search/history")).toStringList() == QStringList() << QStringLiteral("nasa") << QStringLiteral("blender"));
    CHECK_EQ(merged.value(QStringLiteral("catalog/captions/preferred")).toString(), QStringLiteral("fr"));
    CHECK(merged.value(QStringLiteral("catalog/ryd/enabled")).toBool());
    CHECK_EQ(merged.value(QStringLiteral("playback/maxHeight")).toInt(), 720);
    // Once only: nothing left to move; the same file or a missing one is a no-op.
    CHECK_EQ(migrateSettingsFile(from, to), 0);
    CHECK_EQ(migrateSettingsFile(to, to), 0);
    CHECK(QFile::exists(to));
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("org.nordilabs.sailtube"));
    QCoreApplication::setApplicationName(QStringLiteral("SailTube"));
    paths();
    migration();
    return catalog_test::finish("catalog_settings");
}
