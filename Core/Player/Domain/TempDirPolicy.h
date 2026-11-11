// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLT-09: /tmp (and /run, /dev/shm) are tmpfs, i.e. RAM. Header-only, Qt-free.
//
// Start-up rule for main(), in this order:
//   1. compute flashTempDir(<app cache dir>) (e.g. ~/.cache/<org>/<app>/tmp);
//   2. empty it (kEmptyTempDirAtStartup) and create it;
//   3. setenv("TMPDIR", dir, 1)
// all BEFORE GStreamer (gst_init) or GLib initialise, i.e. before the
// QGuiApplication / SailfishApp::application() is created: GLib caches the
// temp dir on first use (g_get_tmp_dir), and the Qt event dispatcher is GLib.
// Media spools, downloads, cookie copies and archives must pass
// isAllowedSpoolLocation(); the engine and the downloader check every path.

#include <string>
#include <vector>

namespace nordtile { namespace player {

const bool kEmptyTempDirAtStartup = true;

// Lexical normalisation: collapses "//", resolves "." and "..". Absolute
// paths only; returns "" for relative or empty input.
inline std::string normalizeAbsolutePath(const std::string &path) {
    if (path.empty() || path[0] != '/') return std::string();
    std::vector<std::string> parts;
    std::string::size_type i = 0;
    while (i <= path.size()) {
        std::string::size_type j = path.find('/', i);
        if (j == std::string::npos) j = path.size();
        std::string seg = path.substr(i, j - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        i = j + 1;
    }
    std::string out;
    for (std::size_t k = 0; k < parts.size(); ++k) out += "/" + parts[k];
    return out.empty() ? std::string("/") : out;
}

// PLT-09: false for any path in a RAM-backed tree (/tmp, /run, /dev/shm, and
// /var/run which links to /run), for relative paths and for "".
inline bool isAllowedSpoolLocation(const std::string &path) {
    std::string p = normalizeAbsolutePath(path);
    if (p.empty()) return false;
    static const char *const ramTrees[] = { "/tmp", "/run", "/dev/shm", "/var/run" };
    for (std::size_t k = 0; k < sizeof(ramTrees) / sizeof(ramTrees[0]); ++k) {
        std::string root = ramTrees[k];
        if (p == root || p.compare(0, root.size() + 1, root + "/") == 0) return false;
    }
    return true;
}

// PLT-09: the flash-backed TMPDIR inside the app cache directory, or "" when
// the cache dir is relative, the root, or itself in a RAM-backed tree.
inline std::string flashTempDir(const std::string &cacheDir) {
    std::string base = normalizeAbsolutePath(cacheDir);
    if (base.empty() || base == "/") return std::string();
    std::string dir = base + "/tmp";
    return isAllowedSpoolLocation(dir) ? dir : std::string();
}

} } // namespace nordtile::player
