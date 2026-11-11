// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Stand-in for the Deno executable in tests/potoken (no Deno here). It records its arguments and
// environment in $HOME/<mode>-args.txt / <mode>-env.txt, then:
//  install: creates node_modules in the working directory; flag files in $HOME change it:
//           fake-install-fail -> exit 3, fake-touch-lock -> appends to deno.lock;
//  run:     binds a TCP listener to --host:--port (fake-bind-any: 0.0.0.0 instead, the
//           all-interfaces case) and waits for SIGTERM; fake-exit -> exits at once.
#define _POSIX_C_SOURCE 200809L
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static const char *home(void) { const char *h = getenv("HOME"); return h ? h : "."; }

static int flag(const char *name) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", home(), name);
    return access(path, F_OK) == 0;
}

static void record(const char *mode, int argc, char **argv) {
    char path[4096];
    snprintf(path, sizeof path, "%s/%s-args.txt", home(), mode);
    FILE *f = fopen(path, "w");
    if (f) { for (int i = 1; i < argc; ++i) fprintf(f, "%s\n", argv[i]); fclose(f); }
    snprintf(path, sizeof path, "%s/%s-env.txt", home(), mode);
    f = fopen(path, "w");
    if (f) { for (char **e = environ; *e; ++e) fprintf(f, "%s\n", *e); fclose(f); }
}

static volatile sig_atomic_t stop = 0;
static void onTerm(int s) { (void)s; stop = 1; }

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    if (strcmp(argv[1], "install") == 0) {
        record("install", argc, argv);
        if (flag("fake-install-fail")) return 3;
        mkdir("node_modules", 0700);
        if (flag("fake-touch-lock")) {
            FILE *f = fopen("deno.lock", "a");
            if (f) { fputs("changed\n", f); fclose(f); }
        }
        return 0;
    }
    if (strcmp(argv[1], "run") != 0) return 2;
    record("run", argc, argv);
    if (flag("fake-exit")) return 1;
    const char *host = "127.0.0.1";
    int port = 0;
    for (int i = 2; i + 1 < argc; ++i) {
        if (strcmp(argv[i], "--port") == 0) port = atoi(argv[i + 1]);
        if (strcmp(argv[i], "--host") == 0) host = argv[i + 1];
    }
    if (flag("fake-bind-any")) host = "0.0.0.0";
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    if (fd < 0 || inet_pton(AF_INET, host, &a.sin_addr) != 1 || bind(fd, (struct sockaddr *)&a, sizeof a) != 0
        || listen(fd, 4) != 0) return 4;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = onTerm;
    sigaction(SIGTERM, &sa, NULL);
    while (!stop) pause();
    close(fd);
    return 0;
}
