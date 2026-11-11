/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * LD_PRELOAD disk-full simulator for tests/offline_engine (NFR-TST-02 "space is low" mid-transfer).
 * While OFFLINE_ENOSPC_SUFFIX is set, a write()/pwrite()/writev() to a regular file whose path ends with
 * that suffix fails with ENOSPC as soon as it would end past OFFLINE_ENOSPC_AFTER bytes; writes below the
 * limit and to every other file go through unchanged. The test switches it on and off with setenv(). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

static int over(int fd, off_t start, size_t len) {
    const char *suffix = getenv("OFFLINE_ENOSPC_SUFFIX");
    if (!suffix || !*suffix || len == 0) return 0;
    char link[64], path[PATH_MAX];
    snprintf(link, sizeof link, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(link, path, sizeof path - 1);
    if (n <= 0) return 0;
    path[n] = 0;
    size_t sl = strlen(suffix);
    if ((size_t)n < sl || strcmp(path + n - sl, suffix) != 0) return 0;
    const char *after = getenv("OFFLINE_ENOSPC_AFTER");
    long long limit = after ? atoll(after) : 0;
    if (start < 0) start = lseek(fd, 0, SEEK_CUR);
    return start >= 0 && (long long)start + (long long)len > limit;
}

ssize_t write(int fd, const void *buf, size_t len) {
    static ssize_t (*real)(int, const void *, size_t);
    if (!real) real = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    if (over(fd, -1, len)) { errno = ENOSPC; return -1; }
    return real(fd, buf, len);
}

ssize_t pwrite(int fd, const void *buf, size_t len, off_t off) {
    static ssize_t (*real)(int, const void *, size_t, off_t);
    if (!real) real = (ssize_t (*)(int, const void *, size_t, off_t))dlsym(RTLD_NEXT, "pwrite");
    if (over(fd, off, len)) { errno = ENOSPC; return -1; }
    return real(fd, buf, len, off);
}

ssize_t writev(int fd, const struct iovec *iov, int cnt) {
    static ssize_t (*real)(int, const struct iovec *, int);
    if (!real) real = (ssize_t (*)(int, const struct iovec *, int))dlsym(RTLD_NEXT, "writev");
    size_t len = 0;
    for (int i = 0; i < cnt; ++i) len += iov[i].iov_len;
    if (over(fd, -1, len)) { errno = ENOSPC; return -1; }
    return real(fd, iov, cnt);
}
