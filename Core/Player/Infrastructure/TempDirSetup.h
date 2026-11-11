// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLT-09 start-up step of TempDirPolicy (Core/Player/Domain/TempDirPolicy.h): the flash-backed
// temporary directory in the app cache, emptied, created owner-only and exported as TMPDIR.
// Qt Core only. Call it before the QGuiApplication / SailfishApp::application(), gst_init or
// anything else that lets GLib cache g_get_tmp_dir().

#include <QString>

namespace nordtile { namespace player {

// PLT-09: <cacheDir>/tmp (flashTempDir), emptied when kEmptyTempDirAtStartup, created with mode
// 0700 and set as TMPDIR. Returns the directory, or "" (TMPDIR untouched) when the cache
// directory is in a RAM-backed tree or cannot be created. *removed: entries deleted (stale spools).
QString prepareFlashTempDir(const QString &cacheDir, int *removed = nullptr);

} } // namespace nordtile::player
