// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// End-to-end resolver cost, offline (container): the shipped one-shot child (pinned PythonResolverChild:
// app binary + QML engine + PyOtherSide + yt-dlp import + run, one process per resolve) against the warm
// worker (PythonWorkerChild + WarmResolver, import once). Both run the same local-file extraction with the
// real yt-dlp archive, so no network is involved. Fails if the warm path is not at least 3x faster.
#include "Shared/Runtime/Child/PythonResolverChild.h"
#include "Shared/Runtime/Child/QuickJsChild.h"
#include "Core/Worker/Infrastructure/PythonWorkerChild.h"
#include "Core/Worker/Infrastructure/WarmResolver.h"
#include "NordTileRuntimeLayout.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <algorithm>
#include <cstdio>
#include <vector>

using namespace nordtile::video;

int main(int argc, char **argv) {
    if (nordtile::music::sailfish::infrastructure::quickJsChildRequested())
        return nordtile::music::sailfish::infrastructure::runQuickJsChild(argc, argv);
    if (nordtile::music::sailfish::infrastructure::pythonResolverChildRequested())
        return nordtile::music::sailfish::infrastructure::runPythonResolverChild(argc, argv);
    if (worker::pythonWorkerRequested())
        return worker::runPythonWorkerChild(argc, argv, NORDTILE_RUNTIME_ROOT "/runtime");

    QCoreApplication app(argc, argv);
    const int runs = argc > 1 ? std::max(3, atoi(argv[1])) : 8;
    QTemporaryDir tmp;
    QFile clip(tmp.path() + QStringLiteral("/clip.mp4"));
    clip.open(QIODevice::WriteOnly);
    clip.write(QByteArray("\x00\x00\x00\x18" "ftypmp42", 12) + QByteArray(4096, '\0'));
    clip.close();
    const QStringList args = QStringList() << QStringLiteral("--ignore-config") << QStringLiteral("--no-plugin-dirs")
        << QStringLiteral("--no-cache-dir") << QStringLiteral("--no-warnings") << QStringLiteral("--enable-file-urls")
        << QStringLiteral("--simulate") << QStringLiteral("--print") << QStringLiteral("%(id)s %(ext)s")
        << QStringLiteral("file://") + clip.fileName();
    const QString self = QCoreApplication::applicationFilePath();
    auto median = [](std::vector<qint64> v) { std::sort(v.begin(), v.end()); return v[v.size() / 2]; };

    std::vector<qint64> cold;
    for (int i = 0; i < runs; ++i) {
        QTemporaryDir t(tmp.path() + QStringLiteral("/one-XXXXXX"));
        QFile::setPermissions(t.path(), QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("SAILTUNE_RESOLVER_CHILD"), QStringLiteral("1"));
        env.insert(QStringLiteral("SAILTUNE_RESOLVER_TMPDIR"), QDir(t.path()).canonicalPath());
        p.setProcessEnvironment(env);
        QElapsedTimer c; c.start();
        p.start(self, args);
        p.waitForFinished(60000);
        cold.push_back(c.elapsed());
        const QByteArray out = p.readAllStandardOutput();
        if (p.exitCode() != 0 || !out.contains("clip mp4")) {
            std::printf("resolver_bench: one-shot child failed (exit %d): %s\n", p.exitCode(), p.readAllStandardError().constData());
            return 1;
        }
    }

    worker::WorkerOptions o;
    o.workerProgram = self;
    o.archivePath = QStringLiteral(NORDTILE_RUNTIME_ROOT "/runtime/yt-dlp.pyz");
    o.workDirectory = tmp.path() + QStringLiteral("/worker");
    o.cacheDirectory = tmp.path() + QStringLiteral("/cache");
    worker::WarmResolver w(o);
    QElapsedTimer total; total.start();
    std::vector<qint64> warm;
    qint64 first = -1;
    for (int i = 0; i <= runs; ++i) {
        bool done = false; resolver::RunResult r;
        QElapsedTimer c; c.start();
        w.run(worker::Lane::Latency, args, 60000, 1 << 20, [&](const resolver::RunResult &res) { r = res; done = true; });
        while (!done) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (r.exitCode != 0 || !r.output.contains("clip mp4")) {
            std::printf("resolver_bench: warm call failed (exit %d, %s)\n", r.exitCode, r.diagnostics.constData());
            return 1;
        }
        if (i == 0) first = c.elapsed(); else warm.push_back(c.elapsed());
    }
    const QVariantMap d = w.diagnostics();
    std::printf("resolver_bench: one-shot child (current path) median %lld ms, min %lld ms over %d runs\n",
                static_cast<long long>(median(cold)), static_cast<long long>(*std::min_element(cold.begin(), cold.end())), runs);
    std::printf("resolver_bench: warm worker first call %lld ms (worker start + import %lld ms), then median %lld ms, max %lld ms\n",
                static_cast<long long>(first), d.value(QStringLiteral("latency.importMs")).toLongLong(),
                static_cast<long long>(median(warm)), static_cast<long long>(*std::max_element(warm.begin(), warm.end())));
    if (median(warm) * 3 > median(cold)) { std::printf("resolver_bench: FAIL (warm path not 3x faster)\n"); return 1; }
    std::printf("resolver_bench: warm saves %lld ms per resolve (median)\n", static_cast<long long>(median(cold) - median(warm)));
    return 0;
}
