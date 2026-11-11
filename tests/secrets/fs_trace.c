/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * NFR-SEC-03 test helper (Linux, glibc): an LD_PRELOAD shim that records what
 * the test process does to files under one directory, so the secrets test can
 * prove properties a snapshot cannot see (a file that exists for a moment):
 *
 *   C <pid> <mode> 0 <path>      a file was created (its mode after the umask)
 *   D <pid> <mode> 0 <path>      a directory was created
 *   W <pid> <mode> <size> <path> the first write to an open file whose mode grants group/other bits
 *   M <pid> <mode> <size> <path> chmod/fchmod/fchmodat to <mode> (size = bytes in the file then)
 *   R <pid> 0 0 <from>\t<to>     rename
 *
 * Only paths under $SEC_TRACE_ROOT are recorded, into $SEC_TRACE (append).
 * Nothing is changed: every call goes to the real function. Not built or used
 * outside the Linux container (tests/secrets/run.sh).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#define MAX_FD 4096

static int traceFd = -1;
static char root[PATH_MAX];
static size_t rootLen = 0;
static volatile unsigned char watched[MAX_FD];   /* 1 = under root, 2 = already reported */

typedef int (*open_fn)(const char *, int, ...);
typedef int (*openat_fn)(int, const char *, int, ...);
typedef int (*mkdir_fn)(const char *, mode_t);
typedef int (*mkdirat_fn)(int, const char *, mode_t);
typedef int (*chmod_fn)(const char *, mode_t);
typedef int (*fchmod_fn)(int, mode_t);
typedef int (*fchmodat_fn)(int, const char *, mode_t, int);
typedef int (*rename_fn)(const char *, const char *);
typedef int (*renameat_fn)(int, const char *, int, const char *);
typedef int (*renameat2_fn)(int, const char *, int, const char *, unsigned int);
typedef ssize_t (*write_fn)(int, const void *, size_t);
typedef ssize_t (*pwrite_fn)(int, const void *, size_t, off_t);
typedef ssize_t (*writev_fn)(int, const struct iovec *, int);
typedef int (*close_fn)(int);

static void *real(const char *name) {
    return dlsym(RTLD_NEXT, name);
}

static void emit(const char *line, size_t n) {
    static write_fn w;
    if (!w) w = (write_fn)real("write");
    if (traceFd >= 0 && w) w(traceFd, line, n);
}

__attribute__((constructor)) static void init(void) {
    const char *trace = getenv("SEC_TRACE");
    const char *r = getenv("SEC_TRACE_ROOT");
    if (!trace || !r || !*trace || !*r) return;
    open_fn o = (open_fn)real("open");
    if (!o) return;
    traceFd = o(trace, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    strncpy(root, r, sizeof root - 1);
    rootLen = strlen(root);
}

/* Absolute path of `path` relative to `dirfd` (no symlink resolution beyond the directory). */
static int absolute(int dirfd, const char *path, char *out, size_t size) {
    if (!path) return 0;
    if (path[0] == '/') { snprintf(out, size, "%s", path); return 1; }
    char base[PATH_MAX];
    if (dirfd == AT_FDCWD) {
        if (!getcwd(base, sizeof base)) return 0;
    } else {
        char link[64];
        snprintf(link, sizeof link, "/proc/self/fd/%d", dirfd);
        ssize_t n = readlink(link, base, sizeof base - 1);
        if (n <= 0) return 0;
        base[n] = 0;
    }
    snprintf(out, size, "%s/%s", base, path);
    return 1;
}

static int underRoot(const char *abs) {
    return rootLen && strncmp(abs, root, rootLen) == 0 && (abs[rootLen] == '/' || abs[rootLen] == 0);
}

static int fdPath(int fd, char *out, size_t size) {
    char link[64];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, out, size - 1);
    if (n <= 0) return 0;
    out[n] = 0;
    return 1;
}

static void record(char op, unsigned mode, long long size, const char *path, const char *second) {
    char line[2 * PATH_MAX + 96];
    int n = second
        ? snprintf(line, sizeof line, "%c %d %04o %lld %s\t%s\n", op, (int)getpid(), mode & 07777, size, path, second)
        : snprintf(line, sizeof line, "%c %d %04o %lld %s\n", op, (int)getpid(), mode & 07777, size, path);
    if (n > 0) emit(line, (size_t)(n < (int)sizeof line ? n : (int)sizeof line - 1));
}

static void afterOpen(int fd, const char *abs, int existed) {
    if (fd < 0 || !underRoot(abs)) return;
    if (fd < MAX_FD) watched[fd] = 1;
    if (!existed) {
        struct stat st;
        if (fstat(fd, &st) == 0) record('C', st.st_mode, 0, abs, NULL);
    }
}

static void beforeWrite(int fd) {
    if (fd < 0 || fd >= MAX_FD || watched[fd] != 1) return;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) return;
    if (st.st_mode & 077) {
        char p[PATH_MAX];
        if (fdPath(fd, p, sizeof p)) record('W', st.st_mode, (long long)st.st_size, p, NULL);
        watched[fd] = 2;
    }
}

static int doOpen(int dirfd, const char *path, int flags, mode_t mode, const char *sym) {
    char abs[PATH_MAX];
    int known = absolute(dirfd, path, abs, sizeof abs);
    int existed = 1;
    if (known && (flags & O_CREAT)) existed = faccessat(AT_FDCWD, abs, F_OK, AT_EACCESS) == 0;
    int fd;
    if (dirfd == AT_FDCWD && strcmp(sym, "openat") != 0 && strcmp(sym, "openat64") != 0) {
        open_fn f = (open_fn)real(sym);
        fd = f(path, flags, mode);
    } else {
        openat_fn f = (openat_fn)real(sym);
        fd = f(dirfd, path, flags, mode);
    }
    if (known) {
        int saved = errno;
        afterOpen(fd, abs, existed);
        errno = saved;
    }
    return fd;
}

#define MODE_ARG(flags) \
    mode_t mode = 0; \
    if ((flags) & (O_CREAT | O_TMPFILE)) { va_list ap; va_start(ap, flags); mode = (mode_t)va_arg(ap, int); va_end(ap); }

int open(const char *path, int flags, ...) { MODE_ARG(flags); return doOpen(AT_FDCWD, path, flags, mode, "open"); }
int open64(const char *path, int flags, ...) { MODE_ARG(flags); return doOpen(AT_FDCWD, path, flags, mode, "open64"); }
int openat(int dirfd, const char *path, int flags, ...) { MODE_ARG(flags); return doOpen(dirfd, path, flags, mode, "openat"); }
int openat64(int dirfd, const char *path, int flags, ...) { MODE_ARG(flags); return doOpen(dirfd, path, flags, mode, "openat64"); }
int creat(const char *path, mode_t mode) { return doOpen(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode, "open"); }
int creat64(const char *path, mode_t mode) { return doOpen(AT_FDCWD, path, O_CREAT | O_WRONLY | O_TRUNC, mode, "open64"); }

static void afterMkdir(int rc, int dirfd, const char *path) {
    char abs[PATH_MAX];
    if (rc != 0 || !absolute(dirfd, path, abs, sizeof abs) || !underRoot(abs)) return;
    struct stat st;
    if (stat(abs, &st) == 0) record('D', st.st_mode, 0, abs, NULL);
}

int mkdir(const char *path, mode_t mode) {
    int rc = ((mkdir_fn)real("mkdir"))(path, mode);
    int saved = errno; afterMkdir(rc, AT_FDCWD, path); errno = saved;
    return rc;
}
int mkdirat(int dirfd, const char *path, mode_t mode) {
    int rc = ((mkdirat_fn)real("mkdirat"))(dirfd, path, mode);
    int saved = errno; afterMkdir(rc, dirfd, path); errno = saved;
    return rc;
}

static void afterChmod(int rc, const char *abs, mode_t mode) {
    if (rc != 0 || !underRoot(abs)) return;
    struct stat st;
    long long size = stat(abs, &st) == 0 && S_ISREG(st.st_mode) ? (long long)st.st_size : -1;
    record('M', mode, size, abs, NULL);
}

int chmod(const char *path, mode_t mode) {
    int rc = ((chmod_fn)real("chmod"))(path, mode);
    int saved = errno;
    char abs[PATH_MAX];
    if (absolute(AT_FDCWD, path, abs, sizeof abs)) afterChmod(rc, abs, mode);
    errno = saved;
    return rc;
}
int fchmod(int fd, mode_t mode) {
    int rc = ((fchmod_fn)real("fchmod"))(fd, mode);
    int saved = errno;
    char abs[PATH_MAX];
    if (fdPath(fd, abs, sizeof abs)) afterChmod(rc, abs, mode);
    if (rc == 0 && fd >= 0 && fd < MAX_FD && watched[fd] == 2 && !(mode & 077)) watched[fd] = 1;
    errno = saved;
    return rc;
}
int fchmodat(int dirfd, const char *path, mode_t mode, int flags) {
    int rc = ((fchmodat_fn)real("fchmodat"))(dirfd, path, mode, flags);
    int saved = errno;
    char abs[PATH_MAX];
    if (absolute(dirfd, path, abs, sizeof abs)) afterChmod(rc, abs, mode);
    errno = saved;
    return rc;
}

static void afterRename(int rc, int fromfd, const char *from, int tofd, const char *to) {
    char a[PATH_MAX], b[PATH_MAX];
    if (rc != 0 || !absolute(fromfd, from, a, sizeof a) || !absolute(tofd, to, b, sizeof b)) return;
    if (underRoot(a) || underRoot(b)) record('R', 0, 0, a, b);
}

int rename(const char *from, const char *to) {
    int rc = ((rename_fn)real("rename"))(from, to);
    int saved = errno; afterRename(rc, AT_FDCWD, from, AT_FDCWD, to); errno = saved;
    return rc;
}
int renameat(int fromfd, const char *from, int tofd, const char *to) {
    int rc = ((renameat_fn)real("renameat"))(fromfd, from, tofd, to);
    int saved = errno; afterRename(rc, fromfd, from, tofd, to); errno = saved;
    return rc;
}
int renameat2(int fromfd, const char *from, int tofd, const char *to, unsigned int flags) {
    renameat2_fn f = (renameat2_fn)real("renameat2");
    if (!f) { errno = ENOSYS; return -1; }
    int rc = f(fromfd, from, tofd, to, flags);
    int saved = errno; afterRename(rc, fromfd, from, tofd, to); errno = saved;
    return rc;
}

ssize_t write(int fd, const void *buf, size_t n) {
    static write_fn f;
    if (!f) f = (write_fn)real("write");
    if (fd != traceFd) beforeWrite(fd);
    return f(fd, buf, n);
}
ssize_t pwrite(int fd, const void *buf, size_t n, off_t off) {
    static pwrite_fn f;
    if (!f) f = (pwrite_fn)real("pwrite");
    beforeWrite(fd);
    return f(fd, buf, n, off);
}
ssize_t pwrite64(int fd, const void *buf, size_t n, off_t off) {
    static pwrite_fn f;
    if (!f) f = (pwrite_fn)real("pwrite64");
    beforeWrite(fd);
    return f(fd, buf, n, off);
}
ssize_t writev(int fd, const struct iovec *iov, int count) {
    static writev_fn f;
    if (!f) f = (writev_fn)real("writev");
    beforeWrite(fd);
    return f(fd, iov, count);
}
int close(int fd) {
    static close_fn f;
    if (!f) f = (close_fn)real("close");
    if (fd >= 0 && fd < MAX_FD) watched[fd] = 0;
    return f(fd);
}
