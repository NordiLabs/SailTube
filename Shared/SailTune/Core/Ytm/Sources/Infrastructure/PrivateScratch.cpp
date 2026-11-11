// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PrivateScratch.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace nordtile { namespace music { namespace ytm { namespace sources { namespace infrastructure { namespace scratch {

namespace {

#ifdef __APPLE__
inline std::int64_t mtimeSec(const struct stat &st) { return static_cast<std::int64_t>(st.st_mtimespec.tv_sec); }
inline std::int64_t mtimeNsec(const struct stat &st) { return static_cast<std::int64_t>(st.st_mtimespec.tv_nsec); }
#else
inline std::int64_t mtimeSec(const struct stat &st) { return static_cast<std::int64_t>(st.st_mtim.tv_sec); }
inline std::int64_t mtimeNsec(const struct stat &st) { return static_cast<std::int64_t>(st.st_mtim.tv_nsec); }
#endif

void fillStamp(const struct stat &st, Stamp *stamp) {
    if (!stamp) return;
    stamp->exists = true;
    stamp->size = static_cast<std::int64_t>(st.st_size);
    stamp->mtimeSec = mtimeSec(st);
    stamp->mtimeNsec = mtimeNsec(st);
}

FileState classify(const struct stat &st, std::size_t maxBytes) {
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return FileState::Unsafe;
    if (st.st_uid != geteuid()) return FileState::Unsafe;
    if (st.st_size < 0 || static_cast<std::uint64_t>(st.st_size) > static_cast<std::uint64_t>(maxBytes)) return FileState::TooLarge;
    return FileState::Ok;
}

FileState fromErrno(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return FileState::Absent;
    case EACCES: case EPERM: return FileState::NotReadable;
    case ELOOP: return FileState::Unsafe;
    default: return FileState::IoError;
    }
}

void wipeBuffer(std::vector<char> &buf) {
    volatile char *p = buf.data();
    for (std::size_t i = 0; i < buf.size(); ++i) p[i] = 0;
}

bool writeAll(int fd, const char *data, std::size_t size) {
    while (size > 0) {
        const ssize_t n = ::write(fd, data, size);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (n == 0) return false;
        data += n; size -= static_cast<std::size_t>(n);
    }
    return true;
}

bool removeTreeAt(const std::string &dir, int depth) {
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0) return errno == ENOENT;
    if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode)) { ::unlink(dir.c_str()); return ::lstat(dir.c_str(), &st) != 0; }
    std::vector<std::string> names;
    if (DIR *d = ::opendir(dir.c_str())) {
        while (struct dirent *e = ::readdir(d)) {
            const std::string name(e->d_name);
            if (name == "." || name == "..") continue;
            if (names.size() < 1024) names.push_back(name);
        }
        ::closedir(d);
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const std::string full = dir + "/" + names[i];
        struct stat cs;
        if (::lstat(full.c_str(), &cs) != 0) continue;
        if (S_ISDIR(cs.st_mode) && !S_ISLNK(cs.st_mode)) { if (depth < 2) removeTreeAt(full, depth + 1); }
        else if (S_ISREG(cs.st_mode)) zeroAndRemove(full);
        else ::unlink(full.c_str());
    }
    ::rmdir(dir.c_str());
    return ::lstat(dir.c_str(), &st) != 0;
}

}  // namespace

FileState inspect(const std::string &path, std::size_t maxBytes, Stamp *stamp) {
    if (stamp) *stamp = Stamp();
    struct stat st;
    if (path.empty() || ::lstat(path.c_str(), &st) != 0) return path.empty() ? FileState::Absent : fromErrno(errno);
    fillStamp(st, stamp);
    const FileState state = classify(st, maxBytes);
    if (state != FileState::Ok) return state;
    if (::access(path.c_str(), R_OK) != 0) return FileState::NotReadable;
    return FileState::Ok;
}

bool makePrivateDirectory(const std::string &path) {
    if (path.empty() || path[0] != '/') return false;
    std::size_t pos = 1;
    std::string current;
    while (pos <= path.size()) {
        std::size_t next = path.find('/', pos);
        if (next == std::string::npos) next = path.size();
        if (next > pos) {
            current = path.substr(0, next);
            const bool last = (next >= path.size());
            struct stat st;
            if (::lstat(current.c_str(), &st) != 0) {
                if (errno != ENOENT) return false;
                if (::mkdir(current.c_str(), 0700) != 0 && errno != EEXIST) return false;
            } else if (last) {
                if (S_ISLNK(st.st_mode) || !S_ISDIR(st.st_mode) || st.st_uid != geteuid()) return false;
                if ((st.st_mode & 0777) != 0700 && ::chmod(current.c_str(), 0700) != 0) return false;
            } else {
                // intermediate components may be links (a relocated home); they must lead to a directory
                struct stat target;
                if (::stat(current.c_str(), &target) != 0 || !S_ISDIR(target.st_mode)) return false;
            }
        }
        pos = next + 1;
    }
    struct stat fin;
    return ::lstat(path.c_str(), &fin) == 0 && S_ISDIR(fin.st_mode) && !S_ISLNK(fin.st_mode);
}

bool makeScratchDirectory(const std::string &root, std::string *dir) {
    if (dir) dir->clear();
    if (!makePrivateDirectory(root)) return false;
    std::string pattern = root + "/import-XXXXXX";
    std::vector<char> buf(pattern.begin(), pattern.end());
    buf.push_back('\0');
    if (!::mkdtemp(buf.data())) return false;
    const std::string made(buf.data());
    struct stat st;
    if (::lstat(made.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode) || (st.st_mode & 0777) != 0700) {
        ::rmdir(made.c_str());
        return false;
    }
    if (dir) *dir = made;
    return true;
}

FileState copyPrivate(const std::string &from, const std::string &to, std::size_t maxBytes) {
    const int in = ::open(from.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (in < 0) return fromErrno(errno);
    struct stat st;
    if (::fstat(in, &st) != 0) { ::close(in); return FileState::IoError; }
    const FileState state = classify(st, maxBytes);
    if (state != FileState::Ok) { ::close(in); return state; }

    const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (out < 0) { ::close(in); return FileState::IoError; }
    FileState result = FileState::Ok;
    if (::fchmod(out, 0600) != 0) result = FileState::IoError;

    std::vector<char> buf(64 * 1024);
    std::uint64_t total = 0;
    while (result == FileState::Ok) {
        const ssize_t n = ::read(in, buf.data(), buf.size());
        if (n < 0) { if (errno == EINTR) continue; result = FileState::IoError; break; }
        if (n == 0) break;
        total += static_cast<std::uint64_t>(n);
        if (total > static_cast<std::uint64_t>(maxBytes)) { result = FileState::TooLarge; break; }    // grew while we copied
        if (!writeAll(out, buf.data(), static_cast<std::size_t>(n))) { result = FileState::IoError; break; }
    }
    wipeBuffer(buf);
    if (result == FileState::Ok) {
        struct stat os;
        if (::fstat(out, &os) != 0 || (os.st_mode & 0777) != 0600 || os.st_uid != geteuid()) result = FileState::IoError;
    }
    ::close(in);
    if (::close(out) != 0 && result == FileState::Ok) result = FileState::IoError;
    if (result != FileState::Ok) ::unlink(to.c_str());
    return result;
}

bool zeroAndRemove(const std::string &path) {
    const int fd = ::open(path.c_str(), O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd >= 0) {
        struct stat st;
        if (::fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            std::vector<char> zeros(64 * 1024, '\0');
            std::uint64_t left = static_cast<std::uint64_t>(st.st_size);
            while (left > 0) {
                const std::size_t n = left < zeros.size() ? static_cast<std::size_t>(left) : zeros.size();
                if (!writeAll(fd, zeros.data(), n)) break;
                left -= n;
            }
            ::fsync(fd);
        }
        ::close(fd);
    }
    ::unlink(path.c_str());
    struct stat gone;
    return ::lstat(path.c_str(), &gone) != 0;
}

bool removeTree(const std::string &dir) { return removeTreeAt(dir, 0); }

int sweepStale(const std::string &root) {
    int removed = 0;
    std::vector<std::string> stale;
    struct stat rootStat;
    // a root that is a link (or not a directory) is not ours to sweep: never follow it
    if (::lstat(root.c_str(), &rootStat) != 0 || S_ISLNK(rootStat.st_mode) || !S_ISDIR(rootStat.st_mode)) return 0;
    DIR *d = ::opendir(root.c_str());
    if (!d) return 0;
    while (struct dirent *e = ::readdir(d)) {
        const std::string name(e->d_name);
        if (name.compare(0, 7, "import-") != 0) continue;
        if (stale.size() < 64) stale.push_back(name);
    }
    ::closedir(d);
    for (std::size_t i = 0; i < stale.size(); ++i) {
        const std::string full = root + "/" + stale[i];
        struct stat st;
        if (::lstat(full.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) continue;
        if (removeTree(full)) ++removed;
    }
    return removed;
}

}}}}}}
