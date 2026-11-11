// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// The real MediaControls.qml on a private session bus, with a stand-in for `playback`.
// MPRIS 2 units: mpris:length and Position are microseconds; Seek takes an offset and SetPosition
// a position in microseconds. The session's own units are milliseconds.
#include <QCoreApplication>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusVariant>
#include <QElapsedTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QUrl>
#include <QVariantMap>
#include <cstdio>
#include <functional>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

class FakePlayback : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool canPlay MEMBER canPlay NOTIFY changed)
    Q_PROPERTY(bool canPause MEMBER canPause NOTIFY changed)
    Q_PROPERTY(bool canSeek MEMBER canSeek NOTIFY changed)
    Q_PROPERTY(bool canGoNext MEMBER canGoNext NOTIFY changed)
    Q_PROPERTY(bool canGoPrevious MEMBER canGoPrevious NOTIFY changed)
    Q_PROPERTY(bool playing MEMBER playing NOTIFY changed)
    Q_PROPERTY(bool live MEMBER live NOTIFY changed)
    Q_PROPERTY(double position MEMBER position NOTIFY changed)   // milliseconds
    Q_PROPERTY(double duration MEMBER duration NOTIFY changed)   // milliseconds
    Q_PROPERTY(QString videoId MEMBER videoId NOTIFY changed)
    Q_PROPERTY(QString title MEMBER title NOTIFY changed)
    Q_PROPERTY(QString channel MEMBER channel NOTIFY changed)
    Q_PROPERTY(QString artUrl MEMBER artUrl NOTIFY changed)
public:
    bool canPlay = true, canPause = true, canSeek = true, canGoNext = false, canGoPrevious = false;
    bool playing = true, live = false;
    double position = 0, duration = 0;
    QString videoId = QStringLiteral("FIXTURE0001"), title = QStringLiteral("A title"), channel = QStringLiteral("A channel");
    QString artUrl;
    int seeks = 0, stops = 0, pauses = 0;
    double lastSeekMs = -1;
    Q_INVOKABLE void play() {}
    Q_INVOKABLE void pause() { ++pauses; }
    Q_INVOKABLE void togglePlayback() {}
    Q_INVOKABLE void stop() { ++stops; }
    Q_INVOKABLE void next() {}
    Q_INVOKABLE void previous() {}
    Q_INVOKABLE void continueWatching() {}
    Q_INVOKABLE void seek(double ms) { ++seeks; lastSeekMs = ms; }
    void set(const std::function<void()> &change) { change(); emit changed(); }
signals:
    void changed();
};

static bool waitFor(const std::function<bool()> &done, int ms) {
    QElapsedTimer t;
    t.start();
    while (!done() && t.elapsed() < ms) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

static QString gService;
static const char kPath[] = "/org/mpris/MediaPlayer2";
static const char kPlayer[] = "org.mpris.MediaPlayer2.Player";

// Asynchronous call, event loop spinning: the adaptor answers on this thread.
static QDBusMessage call(const QDBusMessage &m) {
    QDBusPendingCall pending = QDBusConnection::sessionBus().asyncCall(m, 5000);
    QDBusPendingCallWatcher watcher(pending);
    waitFor([&]() { return watcher.isFinished(); }, 6000);
    return watcher.isFinished() ? QDBusPendingReply<>(watcher).reply() : QDBusMessage();
}

static QVariant property(const char *name) {
    QDBusMessage m = QDBusMessage::createMethodCall(gService, QLatin1String(kPath),
                                                    QStringLiteral("org.freedesktop.DBus.Properties"), QStringLiteral("Get"));
    m << QString::fromLatin1(kPlayer) << QString::fromLatin1(name);
    const QDBusMessage r = call(m);
    if (r.type() != QDBusMessage::ReplyMessage || r.arguments().isEmpty()) return QVariant();
    return r.arguments().at(0).value<QDBusVariant>().variant();
}

static QVariantMap metadata() {
    const QVariant v = property("Metadata");
    if (v.canConvert<QDBusArgument>()) {
        QVariantMap map;
        v.value<QDBusArgument>() >> map;
        return map;
    }
    return v.toMap();
}

// MPR-01: a video of 829 s, one second in (the figures of the device report of 2026-11-10).
static void mpr01_lengthAndPositionAreMicroseconds(FakePlayback &p) {
    p.set([&]() { p.duration = 828981.0; p.position = 998.0; p.playing = true; p.live = false; });
    CHECK(waitFor([&]() { return metadata().value(QStringLiteral("mpris:length")).toLongLong() > 0; }, 3000));
    const qlonglong length = metadata().value(QStringLiteral("mpris:length")).toLongLong();
    const qlonglong position = property("Position").toLongLong();
    std::printf("  mpris:length %lld, Position %lld (session: 828981 ms, 998 ms)\n", length, position);
    CHECK(length == 828981000LL);    // microseconds, not nanoseconds
    CHECK(position == 998000LL);
    CHECK(metadata().value(QStringLiteral("xesam:title")).toString() == QLatin1String("A title"));
    // A live stream and an unknown duration publish no length.
    p.set([&]() { p.live = true; });
    CHECK(waitFor([&]() { return metadata().value(QStringLiteral("mpris:length")).toLongLong() == 0; }, 3000));
    p.set([&]() { p.live = false; p.duration = 0; });
    CHECK(waitFor([&]() { return metadata().value(QStringLiteral("mpris:length")).toLongLong() == 0; }, 3000));
    p.set([&]() { p.duration = 828981.0; });
}

// MPR-01: Seek(offset) and SetPosition(track, position) arrive in microseconds; the session
// seeks in milliseconds (within 1 s of the request per the acceptance; exact here).
static void mpr01_seekAndSetPositionUnits(FakePlayback &p) {
    p.set([&]() { p.position = 30000.0; p.seeks = 0; p.lastSeekMs = -1; });
    waitFor([&]() { return property("Position").toLongLong() == 30000000LL; }, 3000);
    QDBusMessage seek = QDBusMessage::createMethodCall(gService, QLatin1String(kPath), QLatin1String(kPlayer), QStringLiteral("Seek"));
    seek << qlonglong(10000000);   // +10 s
    call(seek);
    CHECK(waitFor([&]() { return p.seeks == 1; }, 3000));
    std::printf("  Seek(+10 s) -> session seek(%.0f ms)\n", p.lastSeekMs);
    CHECK(p.lastSeekMs == 40000.0);
    QDBusMessage back = QDBusMessage::createMethodCall(gService, QLatin1String(kPath), QLatin1String(kPlayer), QStringLiteral("Seek"));
    back << qlonglong(-5000000);   // -5 s
    call(back);
    CHECK(waitFor([&]() { return p.seeks == 2; }, 3000));
    CHECK(p.lastSeekMs == 25000.0);
    const QString track = metadata().value(QStringLiteral("mpris:trackid")).value<QDBusObjectPath>().path();
    CHECK(!track.isEmpty());
    QDBusMessage set = QDBusMessage::createMethodCall(gService, QLatin1String(kPath), QLatin1String(kPlayer), QStringLiteral("SetPosition"));
    set << QVariant::fromValue(QDBusObjectPath(track)) << qlonglong(60000000);   // 60 s
    call(set);
    CHECK(waitFor([&]() { return p.seeks == 3; }, 3000));
    std::printf("  SetPosition(60 s) -> session seek(%.0f ms)\n", p.lastSeekMs);
    CHECK(p.lastSeekMs == 60000.0);
    // Not seekable (live edge): requests are ignored.
    p.set([&]() { p.canSeek = false; });
    QDBusMessage ignored = QDBusMessage::createMethodCall(gService, QLatin1String(kPath), QLatin1String(kPlayer), QStringLiteral("Seek"));
    ignored << qlonglong(10000000);
    call(ignored);
    waitFor([&]() { return false; }, 300);
    CHECK(p.seeks == 3);
    p.set([&]() { p.canSeek = true; });
}

int main(int argc, char **argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    QCoreApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: %s <MediaControls.qml>\n", argv[0]); return 2; }
    FakePlayback playback;
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("playback"), &playback);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QString::fromLocal8Bit(argv[1])));
    QObject *controls = component.create();
    if (!controls) {
        std::printf("FAIL MediaControls.qml did not load: %s\n", qPrintable(component.errorString()));
        return 1;
    }
    CHECK(waitFor([]() {
        for (const QString &name : QDBusConnection::sessionBus().interface()->registeredServiceNames().value())
            if (name.startsWith(QLatin1String("org.mpris.MediaPlayer2.sailtube"))) { gService = name; return true; }
        return false;
    }, 5000));
    if (gService.isEmpty()) {
        std::printf("FAIL no org.mpris.MediaPlayer2.sailtube* service on the session bus\n");
        return 1;
    }
    std::printf("  service %s\n", qPrintable(gService));
    mpr01_lengthAndPositionAreMicroseconds(playback);
    mpr01_seekAndSetPositionUnits(playback);
    delete controls;
    std::printf("mpris tests: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

#include "MprisTest.moc"
