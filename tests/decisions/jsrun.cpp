// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Runs harness.js in Qt 5.6's QJSEngine, the JavaScript engine QML uses on the device.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJSEngine>
#include <QJSValue>
#include <QObject>
#include <QTextStream>
#include <cstdio>

class Host : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString root READ root CONSTANT)
public:
    Host(QJSEngine *engine, const QString &root) : engine_(engine), root_(root) {}
    QString root() const { return root_; }
    Q_INVOKABLE QString read(const QString &path) const {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return QString();
        return QString::fromUtf8(f.readAll());
    }
    Q_INVOKABLE QStringList list(const QString &dir) const {
        return QDir(dir).entryList(QDir::Files, QDir::Name);
    }
    Q_INVOKABLE void print(const QString &text) const {
        std::fprintf(stdout, "%s\n", text.toUtf8().constData());
        std::fflush(stdout);
    }
    Q_INVOKABLE QJSValue evaluate(const QString &code, const QString &file) {
        QJSValue out = engine_->newObject();
        const QJSValue value = engine_->evaluate(code, file);
        out.setProperty(QStringLiteral("ok"), !value.isError());
        out.setProperty(QStringLiteral("value"), value);
        if (value.isError())
            out.setProperty(QStringLiteral("error"), value.toString() + QStringLiteral(" (line ")
                            + value.property(QStringLiteral("lineNumber")).toString() + QStringLiteral(")"));
        return out;
    }
private:
    QJSEngine *engine_;
    QString root_;
};

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::fprintf(stderr, "usage: jsrun <repo-root>\n"); return 2; }
    const QString root = QString::fromLocal8Bit(argv[1]);
    QJSEngine engine;
    Host host(&engine, root);
    engine.globalObject().setProperty(QStringLiteral("__host"), engine.newQObject(&host));
    QFile f(root + QStringLiteral("/tests/decisions/harness.js"));
    if (!f.open(QIODevice::ReadOnly)) { std::fprintf(stderr, "harness.js missing\n"); return 2; }
    QJSValue loaded = engine.evaluate(QString::fromUtf8(f.readAll()), QStringLiteral("harness.js"));
    if (loaded.isError()) { std::fprintf(stderr, "harness: %s\n", qPrintable(loaded.toString())); return 2; }
    const QJSValue result = engine.globalObject().property(QStringLiteral("__run")).call();
    if (result.isError()) { std::fprintf(stderr, "harness: %s\n", qPrintable(result.toString())); return 2; }
    std::fprintf(stdout, "engine: QJSEngine (Qt %s)\n", qVersion());
    return result.toBool() ? 0 : 1;
}

#include "jsrun.moc"
