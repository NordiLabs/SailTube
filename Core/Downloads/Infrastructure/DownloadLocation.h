// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-06 / PLT-03 / PLT-09: where downloads go. Default: the private data dir
// "<AppDataLocation>/downloads" (Sailjail layout ~/.local/share/<Org>/<App>).
// Optional: a user folder (Videos, Music, Downloads) or a mounted SD card
// (/run/media/<user>/*), write-probed when chosen and again at download time.
// Never a RAM file system such as /tmp. Roots are overridable for tests.

#include "Core/Downloads/Domain/DownloadPolicy.h"
#include <QString>
#include <QStringList>

namespace nordtile { namespace video { namespace downloads {

struct LocationRoots {
    QString appData;          // AppDataLocation
    QStringList userFolders;  // Videos, Music, Downloads
    QString removableRoot;    // /run/media/<user>
    bool refuseTemporary = true; // PLT-09; off only in tests whose TMPDIR is under /tmp
};

struct FolderCheck {
    bool ok = false;
    QString path;             // cleaned absolute path when ok
    QString errorKey;         // downloads_error_folder_* (DownloadPolicy.h validateChosenFolder)
};

class DownloadLocation {
public:
    explicit DownloadLocation(const LocationRoots &roots = defaultRoots());
    static LocationRoots defaultRoots();

    QString defaultDirectory() const;            // created 0700 on demand
    bool ensureDefaultDirectory() const;
    QStringList candidateFolders() const;        // existing user folders and SD mounts
    FolderCheck checkChosen(const QString &path) const; // DL-06 "checked when chosen"
    // At download time: the preferred folder if still usable, else the default
    // dir and noticeKeys::folderUnavailable.
    LocationDecision resolve(const QString &preferred) const;

    // Creates and deletes a probe file in dir.
    static bool writable(const QString &dir);

private:
    LocationRoots roots_;
};

}}}
