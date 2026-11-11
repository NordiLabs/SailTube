// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLT-03: the one settings file of the application, inside the Sailjail config directory
// (AppConfigLocation + "/SailTube.conf", the file AppSettings uses). Every QSettings in the
// catalog component opens this path explicitly; a default-constructed QSettings would land one level
// up (<org>/<app>.conf), outside the application's own directory.

#include <QString>

namespace nordtile { namespace video { namespace sailfish {

// AppConfigLocation/SailTube.conf.
QString appSettingsFile();
// The file a default-constructed QSettings uses (an older catalog build wrote there).
QString legacyDefaultSettingsFile();

// Moves every key of `from` into `to` unless `to` already has it (the newer value wins), then
// removes `from`. Returns the number of keys copied; 0 when `from` does not exist or cannot be
// read (for example outside the sandbox), in which case nothing is touched.
int migrateSettingsFile(const QString &from, const QString &to);

}}}
