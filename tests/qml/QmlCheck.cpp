// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Compiles every QML file as a component (syntax, imports, type resolution)
// without a window. Application types are stubbed. Build: tests/qml/run.sh
#include <QDirIterator>
#include <QGuiApplication>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlPropertyMap>
#include <QQuickItem>
#include <cstdio>

class PlaybackSessionStub : public QObject {
    Q_OBJECT
};

// Catalog types created from QML (CatalogComposition::expose). Only the members the pages set
// in their object declarations are stubbed; method calls are resolved at run time.
class ChannelModelStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hideShorts MEMBER hideShorts_)
    Q_PROPERTY(bool hideMembersOnly MEMBER hideMembersOnly_)
    bool hideShorts_ = false;
    bool hideMembersOnly_ = false;
};

class PlaylistModelStub : public QObject {
    Q_OBJECT
};

class WatchInfoStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString videoId MEMBER videoId_)
    QString videoId_;
signals:
    void openUrlRequested(const QString &url);
};

// Playback component types (main.cpp registers the real ones).
class VideoSurfaceItemStub : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(QObject *source MEMBER source_)
    Q_PROPERTY(int fillMode MEMBER fillMode_)
    Q_PROPERTY(bool hasFrame MEMBER hasFrame_ NOTIFY frameChanged)
    // Present frames (true) or keep the last picture (false): the Shorts pager and the cover.
    Q_PROPERTY(bool active MEMBER active_)
    // Width / height of the picture, 0 while unknown (the Shorts pager fills the screen with a vertical one).
    Q_PROPERTY(double videoAspect MEMBER videoAspect_ NOTIFY frameChanged)
    QObject *source_ = nullptr;
    int fillMode_ = 0;
    bool hasFrame_ = false;
    bool active_ = false;
    double videoAspect_ = 0;
public:
    enum FillMode { PreserveAspectFit = 0, PreserveAspectCrop = 1, Stretch = 2 };
    Q_ENUM(FillMode)
    Q_INVOKABLE void repaint() {}
signals:
    void frameChanged();
};

// The cover's live picture (cover/CoverVideo.qml): a VideoSurfaceItem that draws while active.
class CoverVideoItemStub : public VideoSurfaceItemStub {
    Q_OBJECT
    Q_PROPERTY(int framesDrawn MEMBER framesDrawn_)
    Q_PROPERTY(QSize videoSize MEMBER videoSize_)
    int framesDrawn_ = 0;
    QSize videoSize_;
};

class OverlayControllerStub : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool controlsVisible MEMBER controlsVisible_ NOTIFY controlsChanged)
    Q_PROPERTY(bool menuOpen MEMBER menuOpen_ NOTIFY controlsChanged)
    Q_PROPERTY(bool gestureActive MEMBER gestureActive_ NOTIFY gestureChanged)
    Q_PROPERTY(int gestureSide MEMBER gestureSide_ NOTIFY gestureChanged)
    Q_PROPERTY(int gestureSeconds MEMBER gestureSeconds_ NOTIFY gestureChanged)
    Q_PROPERTY(int stepSeconds MEMBER stepSeconds_ NOTIFY gestureChanged)
    bool controlsVisible_ = true, menuOpen_ = false, gestureActive_ = false;
    int gestureSide_ = 1, gestureSeconds_ = 0, stepSeconds_ = 10;
public:
    Q_INVOKABLE void tap(double, double, double, double) {}
    Q_INVOKABLE void interact() {}
    Q_INVOKABLE void setPlaying(bool) {}
    Q_INVOKABLE void setEnded() {}
signals:
    void controlsChanged();
    void gestureChanged();
    void seekRequested(double positionMs);
};

class VideoPlayerStub : public QObject {
    Q_OBJECT
};

// MPR-02: the cover shows the three action states the session's cover state asks for.
static int coverStates(QQmlEngine &engine, const QString &qmlDir) {
    struct Case { const char *name; int mode; int right; const char *enabledList; };
    const Case cases[] = {
        { "queue: play/pause + next", 2, 2, "queueActions" },
        { "no queue: play/pause + stop", 2, 3, "sessionActions" },
        { "idle with a resumable video", 1, 0, "continueActions" },
        { "idle", 0, 0, "" },
    };
    const char *const lists[] = { "queueActions", "sessionActions", "continueActions" };
    int bad = 0;
    for (const Case &c : cases) {
        QQmlPropertyMap playback;
        playback.insert(QStringLiteral("coverMode"), c.mode);
        playback.insert(QStringLiteral("coverLeftAction"), c.mode == 2 ? 1 : c.mode == 1 ? 4 : 0);
        playback.insert(QStringLiteral("coverRightAction"), c.right);
        playback.insert(QStringLiteral("videoId"), c.mode ? QStringLiteral("AAAAAAAAAAA") : QString());
        playback.insert(QStringLiteral("title"), QStringLiteral("Title"));
        playback.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
        playback.insert(QStringLiteral("thumbnail"), QString());
        playback.insert(QStringLiteral("playing"), c.mode == 2);
        playback.insert(QStringLiteral("position"), 1000.0);
        playback.insert(QStringLiteral("duration"), 10000.0);
        playback.insert(QStringLiteral("live"), false);
        QQmlContext context(engine.rootContext());
        context.setContextProperty(QStringLiteral("playback"), &playback);
        QQmlComponent component(&engine, QUrl::fromLocalFile(qmlDir + QStringLiteral("/cover/CoverPage.qml")),
                                QQmlComponent::PreferSynchronous);
        QObject *cover = component.create(&context);
        if (!cover) {
            ++bad;
            for (const QQmlError &error : component.errors()) std::printf("ERROR cover: %s\n", qPrintable(error.toString()));
            continue;
        }
        for (const char *list : lists) {
            QObject *o = cover->findChild<QObject *>(QLatin1String(list));
            const bool enabled = o && o->property("enabled").toBool();
            const bool want = QByteArray(list) == c.enabledList;
            if (!o || enabled != want) {
                ++bad;
                std::printf("FAIL cover state \"%s\": %s enabled=%d, want %d\n", c.name, list, enabled, want);
            }
        }
        delete cover;
    }
    std::printf("qml check: cover states %s\n", bad ? "FAIL" : "ok (4 states)");
    return bad;
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    qmlRegisterUncreatableType<PlaybackSessionStub>("org.nordilabs.sailtube", 1, 0, "PlaybackSession", QStringLiteral("stub"));
    qmlRegisterType<ChannelModelStub>("org.nordilabs.sailtube", 1, 0, "ChannelModel");
    qmlRegisterType<PlaylistModelStub>("org.nordilabs.sailtube", 1, 0, "PlaylistModel");
    qmlRegisterType<WatchInfoStub>("org.nordilabs.sailtube", 1, 0, "WatchInfo");
    qmlRegisterType<VideoSurfaceItemStub>("org.nordilabs.sailtube", 1, 0, "VideoSurfaceItem");
    qmlRegisterType<CoverVideoItemStub>("org.nordilabs.sailtube", 1, 0, "CoverVideoItem");
    qmlRegisterType<OverlayControllerStub>("org.nordilabs.sailtube", 1, 0, "OverlayController");
    qmlRegisterUncreatableType<VideoPlayerStub>("org.nordilabs.sailtube", 1, 0, "VideoPlayer", QStringLiteral("stub"));
    QQmlEngine engine;
    int bad = 0, count = 0;
    QDirIterator it(QString::fromLocal8Bit(argv[1]), QStringList() << QStringLiteral("*.qml"), QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QQmlComponent component(&engine, QUrl::fromLocalFile(it.next()), QQmlComponent::PreferSynchronous);
        ++count;
        if (!component.isError()) continue;
        ++bad;
        for (const QQmlError &error : component.errors()) std::printf("ERROR %s\n", qPrintable(error.toString()));
    }
    std::printf("qml check: %d files, %d with errors\n", count, bad);
    const int coverBad = coverStates(engine, QString::fromLocal8Bit(argv[1]));
    return bad || coverBad || !count ? 1 : 0;
}
#include "QmlCheck.moc"
