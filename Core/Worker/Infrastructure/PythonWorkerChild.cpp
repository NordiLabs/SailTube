// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "PythonWorkerChild.h"

#include <QCoreApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QScopedPointer>
#include <QTimer>
#include <QUrl>
#include <cstdio>

namespace nordtile { namespace video { namespace worker {

namespace {
void quietMessages(QtMsgType type, const QMessageLogContext &, const QString &) {
    if (type == QtFatalMsg) { std::fputs("ERROR: resolver worker initialization failed.\n", stderr); std::fflush(stderr); std::abort(); }
}
}

bool pythonWorkerRequested() { return qgetenv("NORDTILE_RESOLVER_WORKER") == QByteArrayLiteral("1"); }

void PythonWorkerHost::finish(int exitCode) {
    if (finished_) return;
    finished_ = true;
    const int code = exitCode >= 0 && exitCode <= 125 ? exitCode : 1;
    QTimer::singleShot(0, QCoreApplication::instance(), [code]() { QCoreApplication::exit(code); });
}

void PythonWorkerHost::fail() {
    std::fputs("ERROR: resolver worker initialization failed.\n", stderr);
    finish(2);
}

int runPythonWorkerChild(int argc, char **argv, const char *runtimeDirectory) {
    Q_UNUSED(argc);
    qunsetenv("NORDTILE_RESOLVER_WORKER");
    qunsetenv("SAILTUNE_RESOLVER_CHILD");
    qunsetenv("SAILTUNE_QUICKJS_CHILD");
    for (const QString &key : QProcessEnvironment::systemEnvironment().keys())
        if (key.startsWith(QStringLiteral("PYTHON"))) qunsetenv(key.toLocal8Bit().constData());
    qunsetenv("QML_IMPORT_PATH");
    qunsetenv("QML2_IMPORT_PATH");
    qunsetenv("QT_PLUGIN_PATH");
    qunsetenv("QT_QPA_PLATFORM_PLUGIN_PATH");
    qputenv("PYTHONNOUSERSITE", "1");
    qputenv("PYTHONDONTWRITEBYTECODE", "1");
    qputenv("PYTHONSAFEPATH", "1");
    const QString runtime = QString::fromLocal8Bit(runtimeDirectory);
    if (!QDir::setCurrent(runtime)) { std::fputs("ERROR: resolver worker runtime is unavailable.\n", stderr); return 2; }
    qInstallMessageHandler(quietMessages);
    int qtArgc = 1;
    char *qtArgv[] = {argv[0], nullptr};
    QCoreApplication application(qtArgc, qtArgv);
    QCoreApplication::setLibraryPaths(QStringList() << QLibraryInfo::location(QLibraryInfo::PluginsPath));
    PythonWorkerHost host;
    QQmlEngine engine;
    engine.setOutputWarningsToStandardError(false);
    engine.setImportPathList(QStringList() << QLibraryInfo::location(QLibraryInfo::Qml2ImportsPath));
    engine.rootContext()->setContextProperty(QStringLiteral("workerHost"), &host);
    QQmlComponent component(&engine, QUrl::fromLocalFile(runtime + QStringLiteral("/Worker.qml")));
    QScopedPointer<QObject> bridge(component.create());
    if (!bridge || component.isError()) { std::fputs("ERROR: resolver worker initialization failed.\n", stderr); return 2; }
    return application.exec();
}

}}}
