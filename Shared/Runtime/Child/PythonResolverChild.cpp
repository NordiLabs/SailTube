// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PythonResolverChild.h"
#include "NordTileRuntimeLayout.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibraryInfo>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nordtile { namespace music { namespace sailfish { namespace infrastructure {
namespace {
// Shared-runtime patch: the consuming application supplies its install root.
const char runtimeDirectory[] = NORDTILE_RUNTIME_ROOT "/runtime";

void childMessage(QtMsgType type, const QMessageLogContext &, const QString &)
{
    // Qt/plugin diagnostics can include Python tracebacks. Only fixed markers
    // cross the child boundary; the parent owns redaction and classification.
    if (type == QtFatalMsg) {
        std::fputs("ERROR: packaged resolver runtime initialization failed.\n", stderr);
        std::fflush(stderr);
        std::abort();
    }
}
}

bool pythonResolverChildRequested()
{
    return qgetenv("SAILTUNE_RESOLVER_CHILD") == QByteArrayLiteral("1");
}

void PythonResolverHost::ready()
{
    if (ready_ || finished_) return;
    ready_ = true;
    if (timingEnabled_)
        std::fprintf(stderr, "SAILTUNE_PHASE_V1 host_ready %lld\n",
                     static_cast<long long>(qBound(qint64(0), clock_.elapsed(), qint64(120000))));
}

void PythonResolverHost::finish(int exitCode)
{
    if (finished_) return;
    finished_ = true;
    const int code = exitCode >= 0 && exitCode <= 125 ? exitCode : 1;
    QTimer::singleShot(0, QCoreApplication::instance(), [code]() { QCoreApplication::exit(code); });
}

void PythonResolverHost::fail()
{
    std::fputs("ERROR: packaged resolver runtime initialization failed.\n", stderr);
    finish(2);
}

int runPythonResolverChild(int argc, char **argv)
{
    QElapsedTimer clock;
    clock.start();
    const bool timingEnabled = qgetenv("SAILTUNE_STREAM_TIMING") == QByteArrayLiteral("1");
    qunsetenv("SAILTUNE_RESOLVER_CHILD");
    qunsetenv("SAILTUNE_QUICKJS_CHILD");
    QStringList arguments;
    std::size_t bytes = 0;
    if (argc < 2 || argc > 64) return 2;
    for (int i = 1; i < argc; ++i) {
        const std::size_t size = std::strlen(argv[i]);
        bytes += size;
        if (size > 8192 || bytes > 65536) return 2;
        arguments.append(QString::fromUtf8(argv[i]));
    }
    // Apply before PyOtherSide initializes CPython. This is a separate process,
    // not the UI engine. Do not load Python paths or user site hooks inherited
    // from a developer shell. System startup hooks remain part of platform trust.
    const QStringList keys = QProcessEnvironment::systemEnvironment().keys();
    for (const QString &key : keys) {
        if (key.startsWith(QStringLiteral("PYTHON")))
            qunsetenv(key.toLocal8Bit().constData());
    }
    qunsetenv("QML_IMPORT_PATH");
    qunsetenv("QML2_IMPORT_PATH");
    qunsetenv("QT_PLUGIN_PATH");
    qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
    qputenv("PYTHONNOUSERSITE", "1");
    qputenv("PYTHONDONTWRITEBYTECODE", "1");
    qputenv("PYTHONSAFEPATH", "1");
    if (!QDir::setCurrent(QString::fromLatin1(runtimeDirectory))) {
        std::fputs("ERROR: packaged resolver runtime is unavailable.\n", stderr);
        return 2;
    }
    qInstallMessageHandler(childMessage);
    // Qt must not interpret yt-dlp arguments as application switches.
    int qtArgc = 1;
    char *qtArgv[] = { argv[0], nullptr };
    QCoreApplication application(qtArgc, qtArgv);
    QCoreApplication::setLibraryPaths(QStringList() << QLibraryInfo::location(QLibraryInfo::PluginsPath));
    PythonResolverHost host(clock, timingEnabled);
    QQmlEngine engine;
    engine.setOutputWarningsToStandardError(false);
    engine.setImportPathList(QStringList() << QLibraryInfo::location(QLibraryInfo::Qml2ImportsPath));
    engine.rootContext()->setContextProperty(QStringLiteral("resolverHost"), &host);
    engine.rootContext()->setContextProperty(QStringLiteral("resolverArguments"), QVariant(arguments));
    const QString bridge = QString::fromLatin1(runtimeDirectory) + QStringLiteral("/Resolver.qml");
    QQmlComponent component(&engine, QUrl::fromLocalFile(bridge));
    QScopedPointer<QObject> worker(component.create());
    if (!worker || component.isError()) {
        std::fputs("ERROR: packaged resolver runtime initialization failed.\n", stderr);
        return 2;
    }
    // No window, media backend, user database or application settings are
    // initialized here. The parent retains its process-group kill deadline.
    return application.exec();
}

}}}}
