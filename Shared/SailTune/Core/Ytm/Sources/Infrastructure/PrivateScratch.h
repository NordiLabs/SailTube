// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// File-system helpers for reading ANOTHER application's cookie database safely (POSIX, no Qt).
//
// What they guarantee, and what the tests pin:
//  * the original is never opened for writing, never locked and never handed to SQLite: it is only
//    lstat()ed and read through a descriptor opened with O_RDONLY | O_NOFOLLOW;
//  * a candidate must be an ordinary file owned by the current user; a symbolic link (also swapped
//    in between the check and the open), a directory, a socket or somebody else's file is refused;
//  * the working copy lives in a fresh directory created with mkdtemp (mode 0700) below a private
//    root, and every file in it is created with mode 0600 (open(..., 0600) + fchmod, so neither the
//    umask nor the source's mode matters);
//  * the copy is overwritten with zeros and deleted, then the directory is removed, on success and
//    on every failure path (ScratchGuard); leftovers of a crashed run are swept at the next start.
//    Zeroing is best effort: a journaling or copy-on-write file system may keep old blocks.
//
// Nothing here knows what a cookie is, and nothing logs.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace infrastructure { namespace scratch {

enum class FileState { Absent, Ok, NotReadable, Unsafe, TooLarge, IoError };

// Identity of a file at one instant, to notice that the browser changed it while we copied.
struct Stamp {
    bool exists = false;
    std::int64_t size = 0;
    std::int64_t mtimeSec = 0;
    std::int64_t mtimeNsec = 0;
    bool operator==(const Stamp &o) const {
        return exists == o.exists && size == o.size && mtimeSec == o.mtimeSec && mtimeNsec == o.mtimeNsec;
    }
    bool operator!=(const Stamp &o) const { return !(*this == o); }
};

// lstat-level classification, nothing is opened. Absent (also a missing parent), NotReadable
// (access denied on the path or no read permission), Unsafe (symlink, not a regular file, not
// owned by the current user), TooLarge (> maxBytes), Ok. `stamp` is filled when the file exists.
FileState inspect(const std::string &path, std::size_t maxBytes, Stamp *stamp);

// Creates `path` (and missing parents) with mode 0700. False if a component exists as a symbolic
// link or a non-directory, or cannot be created. An existing directory of ours is tightened to 0700.
bool makePrivateDirectory(const std::string &path);

// Makes a fresh 0700 directory "<root>/import-XXXXXX"; *dir receives its path.
bool makeScratchDirectory(const std::string &root, std::string *dir);

// Copies a regular file into a new 0600 file (O_EXCL: `to` must not exist). Reads at most maxBytes
// (TooLarge when the file is or grows beyond it). On any failure `to` is removed again.
FileState copyPrivate(const std::string &from, const std::string &to, std::size_t maxBytes);

// Overwrites a regular file with zeros and unlinks it. False if it still exists.
bool zeroAndRemove(const std::string &path);

// Removes everything below `dir` (files zeroed first; sub-directories to a small depth; symbolic
// links are unlinked, never followed) and then `dir`. False if `dir` is still there.
bool removeTree(const std::string &dir);

// Removes the "import-*" directories below `root` that an earlier run left behind. Returns how many.
int sweepStale(const std::string &root);

// Removes the directory when it goes out of scope, whatever the path out of the function.
class ScratchGuard {
public:
    explicit ScratchGuard(const std::string &dir) : dir_(dir) {}
    ~ScratchGuard() { release(); }
    ScratchGuard(const ScratchGuard &) = delete;
    ScratchGuard &operator=(const ScratchGuard &) = delete;
    void release() { if (!dir_.empty()) { removeTree(dir_); dir_.clear(); } }
    const std::string &dir() const { return dir_; }
private:
    std::string dir_;
};

}}}}}}
