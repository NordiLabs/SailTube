/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * NFR-TST-02: "networking disabled" without a network namespace (unshare is not permitted in the
 * build container). Preloaded with LD_PRELOAD, it wraps the libc entry points a process uses to reach
 * another host and refuses everything that is not loopback or a local socket:
 *   connect()            AF_INET outside 127.0.0.0/8, AF_INET6 other than ::1 / ::ffff:127.x.y.z
 *   sendto(), sendmsg()  the same rule for an explicit destination (UDP, DNS)
 *   getaddrinfo()        any non-numeric name other than localhost / *.localhost
 * A refused call fails with ENETUNREACH (getaddrinfo: EAI_SYSTEM with errno ENETUNREACH). AF_UNIX,
 * AF_NETLINK, AF_UNSPEC (a UDP disconnect) and calls without a destination pass through. Every refusal
 * is printed to stderr and counted; when NO_NETWORK_LOG names a file, one line per refusal is appended
 * to it so the caller can assert that a run never tried to leave the machine.
 * Build: cc -shared -fPIC -o no_network.so no_network.c -ldl */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

static volatile int blocked_count = 0;

/* Exported so a probe can tell that the guard is really loaded. */
int no_network_blocked_count(void) { return blocked_count; }

static void report(const char *call, const char *what) {
    char line[320];
    const int n = __sync_add_and_fetch(&blocked_count, 1);
    const int len = snprintf(line, sizeof line, "no_network: blocked %s to %s (pid %d, #%d)\n", call, what, (int)getpid(), n);
    if (len <= 0) return;
    const size_t size = (size_t)len < sizeof line ? (size_t)len : sizeof line - 1;
    ssize_t ignored = write(2, line, size);
    (void)ignored;
    const char *log = getenv("NO_NETWORK_LOG");
    if (log && *log) {
        FILE *f = fopen(log, "a");
        if (f) { fwrite(line, 1, size, f); fclose(f); }
    }
}

/* 1: may pass; 0: leaves the machine. `text` receives a printable form of the address. */
static int allowed(const struct sockaddr *sa, socklen_t len, char *text, size_t textSize) {
    snprintf(text, textSize, "?");
    if (!sa || len < (socklen_t)sizeof(sa_family_t)) return 1;
    if (sa->sa_family == AF_INET) {
        if (len < (socklen_t)sizeof(struct sockaddr_in)) return 1; /* the kernel rejects it anyway */
        const struct sockaddr_in *in = (const struct sockaddr_in *)sa;
        char ip[INET_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET, &in->sin_addr, ip, sizeof ip);
        snprintf(text, textSize, "%s:%u", ip, (unsigned)ntohs(in->sin_port));
        return (ntohl(in->sin_addr.s_addr) >> 24) == 127;
    }
    if (sa->sa_family == AF_INET6) {
        if (len < (socklen_t)sizeof(struct sockaddr_in6)) return 1;
        const struct sockaddr_in6 *in6 = (const struct sockaddr_in6 *)sa;
        char ip[INET6_ADDRSTRLEN] = "?";
        inet_ntop(AF_INET6, &in6->sin6_addr, ip, sizeof ip);
        snprintf(text, textSize, "[%s]:%u", ip, (unsigned)ntohs(in6->sin6_port));
        if (IN6_IS_ADDR_LOOPBACK(&in6->sin6_addr)) return 1;
        if (IN6_IS_ADDR_V4MAPPED(&in6->sin6_addr)) return in6->sin6_addr.s6_addr[12] == 127;
        return 0;
    }
    return 1; /* AF_UNIX, AF_NETLINK, AF_UNSPEC, ... */
}

static void *next(const char *name) {
    void *f = dlsym(RTLD_NEXT, name);
    if (!f) { fprintf(stderr, "no_network: cannot find %s\n", name); abort(); }
    return f;
}

int connect(int fd, const struct sockaddr *addr, socklen_t len) {
    static int (*real)(int, const struct sockaddr *, socklen_t);
    if (!real) real = (int (*)(int, const struct sockaddr *, socklen_t))next("connect");
    char text[96];
    if (!allowed(addr, len, text, sizeof text)) { report("connect", text); errno = ENETUNREACH; return -1; }
    return real(fd, addr, len);
}

ssize_t sendto(int fd, const void *buf, size_t n, int flags, const struct sockaddr *addr, socklen_t len) {
    static ssize_t (*real)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
    if (!real) real = (ssize_t (*)(int, const void *, size_t, int, const struct sockaddr *, socklen_t))next("sendto");
    char text[96];
    if (addr && !allowed(addr, len, text, sizeof text)) { report("sendto", text); errno = ENETUNREACH; return -1; }
    return real(fd, buf, n, flags, addr, len);
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags) {
    static ssize_t (*real)(int, const struct msghdr *, int);
    if (!real) real = (ssize_t (*)(int, const struct msghdr *, int))next("sendmsg");
    char text[96];
    if (msg && msg->msg_name && !allowed((const struct sockaddr *)msg->msg_name, msg->msg_namelen, text, sizeof text)) {
        report("sendmsg", text); errno = ENETUNREACH; return -1;
    }
    return real(fd, msg, flags);
}

static int localName(const char *node) {
    const size_t n = strlen(node);
    if (strcasecmp(node, "localhost") == 0 || strcasecmp(node, "localhost.") == 0) return 1;
    return n > 10 && strcasecmp(node + n - 10, ".localhost") == 0;
}

static int numericName(const char *node) {
    struct in_addr a4;
    struct in6_addr a6;
    return inet_pton(AF_INET, node, &a4) == 1 || inet_pton(AF_INET6, node, &a6) == 1;
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res) {
    static int (*real)(const char *, const char *, const struct addrinfo *, struct addrinfo **);
    if (!real) real = (int (*)(const char *, const char *, const struct addrinfo *, struct addrinfo **))next("getaddrinfo");
    if (node && *node && !numericName(node) && !localName(node)) {
        char text[96];
        snprintf(text, sizeof text, "name %.80s", node);
        report("getaddrinfo", text);
        errno = ENETUNREACH;
        return EAI_SYSTEM;
    }
    return real(node, service, hints, res);
}
