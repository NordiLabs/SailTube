// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Small, owner-only, atomic file helpers for the NON-secret records of the session slice
// (grant metadata, consent). Never use them for cookie material: that goes to the vault only.
#pragma once
#include "../Domain/Account.h"
#include <string>

namespace nordtile { namespace music { namespace accounts { namespace infrastructure {

enum class FileRead { Absent, Ok, TooLarge, Unsafe, IoError };

// Reads at most `maxBytes` (TooLarge beyond that). Refuses symlinks and non-regular files.
FileRead readPrivateFile(const std::string &path, std::size_t maxBytes, std::string *out);
// Creates the directory (owner-only, best effort) and writes `bytes` atomically with mode 0600.
domain::Result writePrivateFile(const std::string &path, const std::string &bytes);
// Removes the file; a missing file is a success. A directory or a symlink to elsewhere is not touched.
domain::Result removePrivateFile(const std::string &path);

}}}}
