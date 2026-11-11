// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Compiles QML files (imports, types, property declarations) without creating
// them. Catches what moc and the C++ build cannot: e.g. a property clashing
// with a FINAL Item property. Build: tests/library_qml/run.sh
#include <QGuiApplication>
#include <QQmlEngine>
#include <QQmlComponent>
#include <QUrl>
#include <cstdio>
int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    QQmlEngine engine;
    int bad = 0;
    for (int i = 1; i < argc; ++i) {
        QQmlComponent c(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[i])));
        while (c.isLoading()) app.processEvents();
        if (c.isError()) { ++bad; for (const QQmlError &e : c.errors()) std::printf("%s\n", qPrintable(e.toString())); }
        else std::printf("OK %s\n", argv[i]);
    }
    return bad;
}
