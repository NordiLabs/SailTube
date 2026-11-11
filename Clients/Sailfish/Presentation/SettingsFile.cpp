// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SettingsFile.h"

#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace nordtile { namespace video { namespace sailfish {

QString appSettingsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + QStringLiteral("/SailTube.conf");
}

QString legacyDefaultSettingsFile() {
    // QSettings() is (NativeFormat, UserScope, organizationName, applicationName); on Linux the
    // native format is an INI file at <GenericConfigLocation>/<organization>/<application>.conf.
    QSettings probe;
    return probe.fileName();
}

int migrateSettingsFile(const QString &from, const QString &to) {
    if (from.isEmpty() || to.isEmpty() || QFileInfo(from).absoluteFilePath() == QFileInfo(to).absoluteFilePath()) return 0;
    if (!QFileInfo(from).isFile() || !QFileInfo(from).isReadable()) return 0;
    int copied = 0;
    {
        QSettings old(from, QSettings::IniFormat);
        if (old.status() != QSettings::NoError) return 0;
        QSettings target(to, QSettings::IniFormat);
        target.setFallbacksEnabled(false);
        for (const QString &key : old.allKeys()) {
            if (target.contains(key)) continue;
            target.setValue(key, old.value(key));
            ++copied;
        }
        target.sync();
        if (target.status() != QSettings::NoError) return 0;   // keep the old file if the copy failed
    }
    QFile::remove(from);
    return copied;
}

}}}
