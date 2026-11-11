// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// NFR-ROB-02: at start, remove orphaned temp, spool and lock files that a crash or a SIGKILL left
// in the app's own directories (config, data, cache; top level only, never recursive).
//
// Removed:
// - "*.tmp" and "*.spool";
// - "*.lock" whose owner process is gone (QLockFile format: first line is the pid);
// - atomic-write leftovers "<name>.XXXXXX" (QSaveFile / QSettings temp files: six letters or
//   digits after a dot, not a plain lowercase word such as ".backup") when <name> is a store-like file (.conf .ini .json .db .sqlite .mpd .txt)
//   or exists next to it.
// Never removed: download partials (".part"), anything in subdirectories (each store cleans its
// own directory, e.g. LibraryStore::open), and files the patterns do not name.

#include <QStringList>

namespace nordtile { namespace storage {

struct CleanupReport {
    QStringList removed;   // file names, for the debug log; no paths outside the app directories
    int kept = 0;          // live locks
};

CleanupReport removeOrphans(const QStringList &directories);
bool isAtomicWriteLeftover(const QString &fileName, const QStringList &siblings);

}}
