/* SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 * Proof that the no_network.so guard works (NFR-TST-02). Run under LD_PRELOAD=no_network.so:
 *   - the guard is loaded (its counter symbol resolves),
 *   - TCP to a public address (1.1.1.1:443, also over IPv6) is refused with ENETUNREACH,
 *   - UDP to a public resolver (8.8.8.8:53) is refused, so is a lookup of a public name,
 *   - loopback TCP (a listener of this process on 127.0.0.1) and AF_UNIX still work.
 * The public connect is non-blocking: if the guard were missing the probe fails at once instead of
 * waiting on the network. Exit 0 when every expectation holds. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static int failures = 0;
static void expect(int ok, const char *what) {
    printf("probe: %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main(void) {
    int (*count)(void) = (int (*)(void))dlsym(RTLD_DEFAULT, "no_network_blocked_count");
    expect(count != NULL, "guard loaded (no_network_blocked_count resolves)");

    int s = socket(AF_INET, SOCK_STREAM, 0);
    fcntl(s, F_SETFL, O_NONBLOCK);
    struct sockaddr_in pub;
    memset(&pub, 0, sizeof pub);
    pub.sin_family = AF_INET;
    pub.sin_port = htons(443);
    inet_pton(AF_INET, "1.1.1.1", &pub.sin_addr);
    int r = connect(s, (struct sockaddr *)&pub, sizeof pub);
    int e = errno;
    printf("probe: connect(1.1.1.1:443) = %d, errno %d (%s)\n", r, r ? e : 0, r ? strerror(e) : "none");
    expect(r == -1 && e == ENETUNREACH, "TCP connect to 1.1.1.1:443 refused with ENETUNREACH");
    close(s);

    int expectedBlocks = 3;
    s = socket(AF_INET6, SOCK_STREAM, 0);
    if (s >= 0) {
        ++expectedBlocks;
        fcntl(s, F_SETFL, O_NONBLOCK);
        struct sockaddr_in6 pub6;
        memset(&pub6, 0, sizeof pub6);
        pub6.sin6_family = AF_INET6;
        pub6.sin6_port = htons(443);
        inet_pton(AF_INET6, "2606:4700:4700::1111", &pub6.sin6_addr);
        r = connect(s, (struct sockaddr *)&pub6, sizeof pub6);
        e = errno;
        expect(r == -1 && e == ENETUNREACH, "TCP connect to [2606:4700:4700::1111]:443 refused");
        close(s);
    }

    s = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in dns = pub;
    dns.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &dns.sin_addr);
    const ssize_t sent = sendto(s, "x", 1, 0, (struct sockaddr *)&dns, sizeof dns);
    e = errno;
    expect(sent == -1 && e == ENETUNREACH, "UDP sendto 8.8.8.8:53 refused");
    close(s);

    struct addrinfo *res = NULL;
    r = getaddrinfo("example.com", "443", NULL, &res);
    e = errno;
    expect(r == EAI_SYSTEM && e == ENETUNREACH, "getaddrinfo(example.com) refused");
    if (r == 0) freeaddrinfo(res);

    /* Loopback still works: listen, connect, exchange a byte. */
    int l = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in lo;
    memset(&lo, 0, sizeof lo);
    lo.sin_family = AF_INET;
    lo.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof lo;
    int ok = bind(l, (struct sockaddr *)&lo, sizeof lo) == 0 && listen(l, 1) == 0 && getsockname(l, (struct sockaddr *)&lo, &len) == 0;
    int c = socket(AF_INET, SOCK_STREAM, 0);
    ok = ok && connect(c, (struct sockaddr *)&lo, sizeof lo) == 0;
    int a = ok ? accept(l, NULL, NULL) : -1;
    char byte = 0;
    ok = ok && a >= 0 && write(c, "y", 1) == 1 && read(a, &byte, 1) == 1 && byte == 'y';
    expect(ok, "loopback TCP 127.0.0.1 still works");
    if (a >= 0) close(a);
    close(c);
    close(l);

    struct addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    res = NULL;
    r = getaddrinfo("localhost", "80", &hints, &res);
    expect(r == 0, "getaddrinfo(localhost) still works");
    if (r == 0) freeaddrinfo(res);

    int sv[2];
    ok = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0 && write(sv[0], "z", 1) == 1 && read(sv[1], &byte, 1) == 1 && byte == 'z';
    expect(ok, "AF_UNIX still works");

    if (count) printf("probe: blocked attempts counted by the guard: %d\n", count());
    expect(count && count() == expectedBlocks, "guard counted exactly the public attempts above");
    return failures ? 1 : 0;
}
