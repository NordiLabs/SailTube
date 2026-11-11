// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Headless UI tests: loads the real application QML (Silica, QPA "minimal") with mock back
// ends from Mocks.qml, then runs one tst_*.qml file's steps with the event loop spinning in
// between. Any QML warning or error fails the test unless the test expects it.
//
//   harness <repo-root> <tst_file.qml> [--rtl] [--qm <file.qm>] [--locale <name>] [--screen <W>x<H>]
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QScreen>
#include <QStringList>
#include <QTranslator>
#include <QVariant>
#include <QtQml>
#include <cstdio>
#ifdef HARNESS_QPA
// The "minimal" platform has a fixed 240x320 screen; --screen sets a phone's size through the
// platform interface so Silica's Screen and Theme metrics match a device (540x960 at ratio 1).
#include <qpa/qwindowsysteminterface.h>
#endif

// Mirrors PlaybackSession's State enum; tst_contract.qml checks the header still matches.
class PlaybackSessionEnums : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Loading, Preparing, Ready, Failed };
    Q_ENUM(State)
};

static QStringList g_messages;
static const char *const kBenign[] = {
    "No pixel ratio defined", "Defaulting to pixel ratio", "Suspicious DPI value",
};

#ifdef HARNESS_FONTS
// The "minimal" platform has no font database: Qt lays text out with a box glyph one em wide,
// so its text widths say nothing about a device. textWidth() measures with FreeType instead,
// in the system's default sans (DejaVu Sans in the SDK target, wider than Sailfish Sans, so a
// text that fits here fits on a device) with fontconfig's fallback for other scripts. Advances
// are unshaped (no kerning, isolated Arabic forms), which also errs on the wide side.
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <QHash>
#include <QVector>

class FontMeasure {
public:
    FontMeasure() {
        if (FT_Init_FreeType(&library_)) library_ = nullptr;
        FcInit();
    }
    bool usable() { return library_ && face(primaryFile()); }
    // Width of `text` in pixels at `pixelSize`, or -1 when a character has no font.
    double width(const QString &text, int pixelSize) {
        if (!usable()) return -1;
        double total = 0;
        for (uint ucs : text.toUcs4()) {
            if (ucs < 0x20 || ucs == 0x200e || ucs == 0x200f || ucs == 0x00ad) continue;   // controls, marks, soft hyphen
            if (ucs == 0x00a0 || ucs == 0x202f) ucs = ' ';
            FT_Face f = face(primaryFile());
            FT_UInt glyph = FT_Get_Char_Index(f, ucs);
            if (!glyph) {
                f = face(fallbackFile(ucs));
                glyph = f ? FT_Get_Char_Index(f, ucs) : 0;
                if (!glyph) return -1;
            }
            if (FT_Set_Pixel_Sizes(f, 0, pixelSize)) return -1;
            if (FT_Load_Glyph(f, glyph, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP)) return -1;
            total += f->glyph->linearHoriAdvance / 65536.0;
        }
        return total;
    }
private:
    QString primaryFile() {
        if (primary_.isEmpty()) primary_ = match(nullptr);
        return primary_;
    }
    QString fallbackFile(uint ucs) {
        const uint block = ucs >> 8;
        if (!fallback_.contains(block)) {
            FcCharSet *set = FcCharSetCreate();
            FcCharSetAddChar(set, ucs);
            fallback_.insert(block, match(set));
            FcCharSetDestroy(set);
        }
        return fallback_.value(block);
    }
    QString match(FcCharSet *set) {
        FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>("sans"));
        if (set) FcPatternAddCharSet(pattern, FC_CHARSET, set);
        FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        FcPattern *found = FcFontMatch(nullptr, pattern, &result);
        QString file;
        FcChar8 *path = nullptr;
        if (found && FcPatternGetString(found, FC_FILE, 0, &path) == FcResultMatch)
            file = QString::fromUtf8(reinterpret_cast<const char *>(path));
        if (found) FcPatternDestroy(found);
        FcPatternDestroy(pattern);
        return file;
    }
    FT_Face face(const QString &file) {
        if (file.isEmpty()) return nullptr;
        if (!faces_.contains(file)) {
            FT_Face f = nullptr;
            if (FT_New_Face(library_, file.toLocal8Bit().constData(), 0, &f)) f = nullptr;
            faces_.insert(file, f);
        }
        return faces_.value(file);
    }
    FT_Library library_ = nullptr;
    QString primary_;
    QHash<uint, QString> fallback_;
    QHash<QString, FT_Face> faces_;
};
#endif

// A headless window is never exposed, so nothing runs the scene graph's polish step and
// positioners (Row, Column, Flow, Grid) never place their children. updatePolish() is
// protected; a pointer to member taken through a derived class may be called on the base.
struct PolishAccess : QQuickItem { using QQuickItem::updatePolish; };
static void polishTree(QQuickItem *item) {
    if (!item) return;
    // Children first: a positioner needs its children's sizes.
    for (QQuickItem *child : item->childItems()) polishTree(child);
    (item->*(&PolishAccess::updatePolish))();
}

static void handler(QtMsgType type, const QMessageLogContext &, const QString &message) {
    if (type == QtDebugMsg || type == QtInfoMsg) return;
    for (const char *b : kBenign) if (message.contains(QLatin1String(b))) return;
    g_messages << message;
    std::fprintf(stderr, "  qml: %s\n", qPrintable(message));
}

class Harness : public QObject {
    Q_OBJECT
public:
    // The installed app icon is not in the build container (the cover and About show it).
    QStringList expected { QStringLiteral("Cannot open: file:///usr/share/icons/hicolor/172x172/apps/harbour-sailtube.png") };
    Q_INVOKABLE void expectWarning(const QString &pattern) { expected << pattern; }
    Q_INVOKABLE QString readFile(const QString &relative) const {
        QFile f(root + QLatin1Char('/') + relative);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }
    Q_INVOKABLE void setLayoutDirection(bool rtl) {
        QGuiApplication::setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
    }
    // Lays out positioners and text below `root` the way a rendered frame would (three passes:
    // sizes settle from the leaves up, then bindings on them, then positions).
    Q_INVOKABLE void polish(QQuickItem *root) {
        for (int pass = 0; pass < 3; ++pass) {
            polishTree(root);
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    }
    // Width in pixels of `text` at `pixelSize` in the system's sans font (see FontMeasure), or
    // -1 when it cannot be measured here.
    Q_INVOKABLE double textWidth(const QString &text, int pixelSize) {
#ifdef HARNESS_FONTS
        static FontMeasure measure;
        return measure.width(text, pixelSize);
#else
        Q_UNUSED(text); Q_UNUSED(pixelSize);
        return -1;
#endif
    }
    // The catalog this run loaded ("" without --qm; "ar", "de", ... with --locale).
    Q_PROPERTY(QString locale MEMBER locale CONSTANT)
    QString locale;
    QString root;
};

static void spin(int ms) {
    QElapsedTimer t; t.start();
    do { QCoreApplication::processEvents(QEventLoop::AllEvents, 10); } while (t.elapsed() < ms);
}

// True while the application window's page stack is in a push or pop transition.
static bool pageStackBusy(QObject *window) {
    QObject *stack = window ? window->property("pageStack").value<QObject *>() : nullptr;
    return stack && stack->property("busy").toBool();
}

// The test's own word on whether the application is still loading something (UiTest.busy()).
static bool testBusy(QObject *test) {
    QVariant busy;
    return QMetaObject::invokeMethod(test, "busy", Q_RETURN_ARG(QVariant, busy)) && busy.toBool();
}

int main(int argc, char **argv) {
    qInstallMessageHandler(handler);
    QGuiApplication app(argc, argv);
    if (argc < 3) { std::fprintf(stderr, "usage: harness <root> <test.qml> [--rtl] [--qm file]\n"); return 2; }
    const QString root = QString::fromLocal8Bit(argv[1]);
    const QString testFile = QString::fromLocal8Bit(argv[2]);
    QTranslator translator;
    QString locale;
    for (int i = 3; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QLatin1String("--locale") && i + 1 < argc) locale = QString::fromLocal8Bit(argv[++i]);
        if (a == QLatin1String("--screen") && i + 1 < argc) {
            const QStringList wh = QString::fromLocal8Bit(argv[++i]).split(QLatin1Char('x'));
            const QRect geometry(0, 0, wh.value(0).toInt(), wh.value(1).toInt());
#ifdef HARNESS_QPA
            if (geometry.width() > 0 && geometry.height() > 0) {
                QWindowSystemInterface::handleScreenGeometryChange(QGuiApplication::primaryScreen(), geometry, geometry);
                QWindowSystemInterface::flushWindowSystemEvents();
            }
#endif
            if (QGuiApplication::primaryScreen()->size() != geometry.size()) {
                std::fprintf(stdout, "SKIP %s: no platform interface to set a %dx%d screen\n",
                             qPrintable(testFile.section('/', -1)), geometry.width(), geometry.height());
                return 0;
            }
        }
        if (a == QLatin1String("--rtl")) QGuiApplication::setLayoutDirection(Qt::RightToLeft);
        if (a == QLatin1String("--qm") && i + 1 < argc) {
            if (!translator.load(QString::fromLocal8Bit(argv[++i]))) { std::fprintf(stderr, "cannot load qm\n"); return 2; }
            app.installTranslator(&translator);
        }
    }

    qmlRegisterUncreatableType<PlaybackSessionEnums>("org.nordilabs.sailtube", 1, 0, "PlaybackSession",
                                                     QStringLiteral("mock"));
    // The playback component's QML player types, as headless stand-ins.
    qmlRegisterType(QUrl::fromLocalFile(root + QStringLiteral("/tests/qml_ui/fakes/VideoSurfaceItem.qml")),
                    "org.nordilabs.sailtube", 1, 0, "VideoSurfaceItem");
    qmlRegisterType(QUrl::fromLocalFile(root + QStringLiteral("/tests/qml_ui/fakes/OverlayController.qml")),
                    "org.nordilabs.sailtube", 1, 0, "OverlayController");
    qmlRegisterType(QUrl::fromLocalFile(root + QStringLiteral("/tests/qml_ui/fakes/CoverVideoItem.qml")),
                    "org.nordilabs.sailtube", 1, 0, "CoverVideoItem");
    // The catalog component's channel model, so the channel page opens headless.
    qmlRegisterType(QUrl::fromLocalFile(root + QStringLiteral("/tests/qml_ui/fakes/ChannelModel.qml")),
                    "org.nordilabs.sailtube", 1, 0, "ChannelModel");
    QQmlApplicationEngine engine;
    Harness harness;
    harness.root = root;
    harness.locale = locale;
    engine.rootContext()->setContextProperty(QStringLiteral("harness"), &harness);

    QQmlComponent mocksComponent(&engine, QUrl::fromLocalFile(root + QStringLiteral("/tests/qml_ui/Mocks.qml")));
    QObject *mocks = mocksComponent.create();
    if (!mocks) { std::fprintf(stderr, "Mocks.qml: %s\n", qPrintable(mocksComponent.errorString())); return 2; }
    const char *const names[] = { "playback", "searchModel", "appSettings", "deviceCapabilities",
                                  "resolverAvailable", "javascriptRuntimeAvailable", "appVersion",
                                  "storeVariant", "subscriptionsModel", "playlistsModel", "historyModel",
                                  "watchIndex", "libraryPlayback", "importer", "homeSettings", "feedModel",
                                  "recommendedModel", "shortsModel", "suggestionModel", "commentsModel", "watchInfo",
                                  "startupTiming" };
    for (const char *n : names)
        engine.rootContext()->setContextProperty(QString::fromLatin1(n), mocks->property(n));
    engine.rootContext()->setContextProperty(QStringLiteral("mocks"), mocks);

    engine.load(QUrl::fromLocalFile(root + QStringLiteral("/Clients/Sailfish/qml/harbour-sailtube.qml")));
    if (engine.rootObjects().isEmpty()) { std::fprintf(stderr, "application QML failed to load\n"); return 1; }
    QObject *window = engine.rootObjects().first();
    spin(300);

    QQmlComponent testComponent(&engine, QUrl::fromLocalFile(testFile));
    QObject *test = testComponent.create();
    if (!test) { std::fprintf(stderr, "%s: %s\n", qPrintable(testFile), qPrintable(testComponent.errorString())); return 1; }
    test->setProperty("window", QVariant::fromValue(window));

    QVariant count;
    QMetaObject::invokeMethod(test, "count", Q_RETURN_ARG(QVariant, count));
    int failures = 0;
    for (int i = 0; i < count.toInt(); ++i) {
        // A page transition or a section that is still loading (a busy build machine): let it finish
        // before the next step looks at the page, for up to 10 s. Steps keep their own waits for
        // everything else.
        for (int waited = 0; waited < 10000 && (pageStackBusy(window) || testBusy(test)); waited += 100) spin(100);
        QVariant result;
        QMetaObject::invokeMethod(test, "run", Q_RETURN_ARG(QVariant, result), Q_ARG(QVariant, i));
        if (result.type() == QVariant::String) {
            std::fprintf(stdout, "FAIL %s step %d: %s\n", qPrintable(testFile.section('/', -1) + (locale.isEmpty() ? QString() : QStringLiteral(" [") + locale + QLatin1Char(']'))), i, qPrintable(result.toString()));
            ++failures;
            break;
        }
        spin(qMax(50, result.toInt()));
    }
    delete test;
    spin(100);
    int unexpected = 0;
    for (const QString &m : g_messages) {
        bool ok = false;
        for (const QString &p : harness.expected) ok = ok || QRegularExpression(p).match(m).hasMatch();
        if (!ok) { std::fprintf(stdout, "FAIL %s: unexpected QML message: %s\n", qPrintable(testFile.section('/', -1)), qPrintable(m)); ++unexpected; }
    }
    std::fprintf(stdout, "%s %s%s: %d steps\n", failures || unexpected ? "FAIL" : "ok  ", qPrintable(testFile.section('/', -1)),
                 qPrintable(locale.isEmpty() ? QString() : QStringLiteral(" [") + locale + QLatin1Char(']')), count.toInt());
    return failures || unexpected ? 1 : 0;
}

#include "harness.moc"
