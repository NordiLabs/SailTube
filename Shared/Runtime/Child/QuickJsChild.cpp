// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "QuickJsChild.h"
#include "NordTileRuntimeLayout.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLibrary>
#include <cstdio>
#include <cstdlib>
#include <cstddef>
#include <cstring>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace nordtile { namespace music { namespace sailfish { namespace infrastructure {
namespace {
QElapsedTimer childClock;
bool helpRequested = false;
QByteArray timingSocket;
struct timespec cpuStarted = {};
bool cpuClockValid = false;
qint64 libraryLoadMs = -1;

qint64 elapsedCpuMilliseconds()
{
    struct timespec finished = {};
    if (!cpuClockValid || ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &finished) != 0 ||
        finished.tv_sec < cpuStarted.tv_sec) return -1;
    const auto seconds = finished.tv_sec - cpuStarted.tv_sec;
    if (seconds > 120) return 120000;
    const qint64 nanoseconds = static_cast<qint64>(seconds) * 1000000000 +
        finished.tv_nsec - cpuStarted.tv_nsec;
    return nanoseconds < 0 ? -1 : qMin(qint64(120000), nanoseconds / 1000000);
}

void reportChildTiming()
{
    if (!childClock.isValid() || timingSocket.isEmpty()) return;
    // Snapshot before reporting so socket work is outside both measurements.
    const qint64 elapsedMs = childClock.elapsed();
    const qint64 cpuMs = elapsedCpuMilliseconds();
    // The provider treats any QuickJS stderr as an execution failure. Send
    // diagnostics to the resolver's private abstract socket instead.
    const int fd = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return;
    struct sockaddr_un address = {};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path + 1, timingSocket.constData(), timingSocket.size());
    const auto sendRecord = [fd, &address](const char *stage, qint64 milliseconds) {
        char record[96];
        const int size = std::snprintf(record, sizeof(record), "SAILTUNE_PHASE_V1 %s %lld\n",
            stage, static_cast<long long>(qBound(qint64(0), milliseconds, qint64(120000))));
        if (size > 0 && size < static_cast<int>(sizeof(record)))
            ::sendto(fd, record, static_cast<std::size_t>(size), 0,
                     reinterpret_cast<const struct sockaddr *>(&address),
                     static_cast<socklen_t>(offsetof(struct sockaddr_un, sun_path) + 1 + timingSocket.size()));
    };
    sendRecord(helpRequested ? "quickjs_help" : "quickjs_script", elapsedMs);
    if (!helpRequested) {
        // Library loading is part of script wall time. Process CPU also covers
        // loading and execution; neither is an additional wall-clock phase.
        if (libraryLoadMs >= 0) sendRecord("quickjs_library", libraryLoadMs);
        if (cpuMs >= 0) sendRecord("quickjs_script_cpu", cpuMs);
    }
    ::close(fd);
}
}

bool quickJsChildRequested()
{
    return qgetenv("SAILTUNE_QUICKJS_CHILD") == QByteArrayLiteral("1");
}

int runQuickJsChild(int argc, char **argv)
{
    // Only the resolver child sets this marker. Clear it before running any
    // JavaScript so a descendant cannot accidentally open another JS child.
    qunsetenv("SAILTUNE_QUICKJS_CHILD");
    qunsetenv("QJS_DUMP_FLAGS");
    bool accepted = argc == 2 && std::strcmp(argv[1], "--help") == 0;
    if (argc == 3 && std::strcmp(argv[1], "--script") == 0) {
        const QFileInfo file(QString::fromLocal8Bit(argv[2]));
        const QByteArray path = QFile::encodeName(file.absoluteFilePath());
        struct stat info;
        accepted = file.isAbsolute() && path.size() <= 4096 &&
            ::lstat(path.constData(), &info) == 0 && S_ISREG(info.st_mode) &&
            info.st_uid == ::geteuid() && info.st_size > 0 && info.st_size <= 32 * 1024 * 1024;
    }
    if (!accepted) {
        std::fputs("ERROR: unsupported JavaScript runtime request.\n", stderr);
        return 2;
    }
    if (qgetenv("SAILTUNE_STREAM_TIMING") == QByteArrayLiteral("1")) {
        // The upstream CLI can call exit(), including its --help path.
        // atexit covers those exits as well as a normal return from main.
        const QByteArray token = qgetenv("SAILTUNE_TIMING_SOCKET");
        bool valid = token.size() == 32;
        for (char byte : token)
            if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f'))) valid = false;
        if (valid) {
            timingSocket = QByteArrayLiteral("sailtune-phase-") + token;
            helpRequested = argc == 2;
            childClock.start();
            // The process loader and argument validation precede this point.
            // A failed CPU clock stays unobserved, rather than becoming zero.
            cpuClockValid = !helpRequested &&
                ::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpuStarted) == 0;
            std::atexit(reportChildTiming);
        }
    }
    // Shared-runtime patch: the consuming application supplies its install root.
    QLibrary library(QString::fromLatin1(NORDTILE_RUNTIME_ROOT "/lib/libsailtune-quickjs.so.1"));
    typedef int (*EntryPoint)(int, char **);
    QElapsedTimer libraryClock;
    if (childClock.isValid() && !helpRequested) libraryClock.start();
    const auto entry = reinterpret_cast<EntryPoint>(library.resolve("sailtune_qjs_main"));
    if (libraryClock.isValid()) libraryLoadMs = libraryClock.elapsed();
    if (!entry) {
        std::fputs("ERROR: the packaged JavaScript runtime is unavailable.\n", stderr);
        return 126;
    }
    // This is a dedicated process: no Qt application, UI, database, multimedia
    // object or playback state has been initialized. The parent owns its timeout.
    return entry(argc, argv);
}

}}}}
