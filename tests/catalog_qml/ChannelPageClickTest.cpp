// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Runtime QML test of the channel page (device run 2 regression): the real ChannelPage.qml over a
// fake ChannelModel whose rows carry a `channel` role (the name that shadowed the model id),
// a recording `playback` and `pageStack`, and the real tapIntent / catalogText. Clicking a video
// row must open it with the row's hints, after tapIntent.opened; a playlist row pushes the
// preview page. Silica from the SDK container, minimal platform.
#include "Clients/Sailfish/Presentation/CatalogText.h"
#include "Clients/Sailfish/Presentation/TapIntent.h"

#include <QAbstractListModel>
#include <QDir>
#include <QGuiApplication>
#include <QMetaMethod>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>
#include <cstdio>
#include <functional>

using nordtile::video::sailfish::CatalogText;
using nordtile::video::sailfish::TapIntent;

static int failures = 0, checks = 0;
#define CHECK(c) do { ++checks; if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QStringList &events() { static QStringList e; return e; }

class FakeChannelModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString reference READ reference WRITE open NOTIFY changed)
    Q_PROPERTY(QString channelId READ channelId NOTIFY changed)
    Q_PROPERTY(QString title READ title NOTIFY changed)
    Q_PROPERTY(QString handle READ handle NOTIFY changed)
    Q_PROPERTY(QString avatarUrl READ empty NOTIFY changed)
    Q_PROPERTY(QString subscriberText READ subscriberText NOTIFY changed)
    Q_PROPERTY(double subscriberCount READ subscriberCount NOTIFY changed)
    Q_PROPERTY(QString videoCountText READ empty NOTIFY changed)
    Q_PROPERTY(double videoCount READ videoCount NOTIFY changed)
    Q_PROPERTY(QString description READ empty NOTIFY changed)
    Q_PROPERTY(QString tab READ tab WRITE setTab NOTIFY changed)
    Q_PROPERTY(QStringList tabs READ tabs NOTIFY changed)
    Q_PROPERTY(bool fallbackTab READ no NOTIFY changed)
    Q_PROPERTY(bool busy READ no NOTIFY changed)
    Q_PROPERTY(bool canLoadMore READ no NOTIFY changed)
    Q_PROPERTY(QString errorText READ empty NOTIFY changed)
    Q_PROPERTY(QString outcome READ empty NOTIFY changed)
    Q_PROPERTY(bool empty READ no NOTIFY changed)
    Q_PROPERTY(bool rssPainted READ no NOTIFY changed)
    Q_PROPERTY(int count READ count NOTIFY changed)
    Q_PROPERTY(int hiddenShorts READ zero NOTIFY changed)
    Q_PROPERTY(int hiddenMembersOnly READ zero NOTIFY changed)
    Q_PROPERTY(int hiddenRestricted READ zero NOTIFY changed)
    Q_PROPERTY(int hiddenWatched READ zero NOTIFY changed)
    Q_PROPERTY(int hiddenCount READ zero NOTIFY changed)
    Q_PROPERTY(bool hideShorts READ no WRITE setFlag NOTIFY changed)
    Q_PROPERTY(bool hideMembersOnly READ no WRITE setFlag NOTIFY changed)
    Q_PROPERTY(bool hideRestricted READ no WRITE setFlag NOTIFY changed)
public:
    explicit FakeChannelModel(QObject *parent = nullptr) : QAbstractListModel(parent) {
        rows_.append(row(QStringLiteral("video"), QStringLiteral("aaaaaaaaaaa"), QStringLiteral("First video")));
        rows_.append(row(QStringLiteral("playlist"), QString(), QStringLiteral("A playlist")));
        rows_.last().insert(QStringLiteral("playlistId"), QStringLiteral("PLxyz"));
    }
    static QVariantMap row(const QString &kind, const QString &videoId, const QString &title) {
        QVariantMap m;
        m.insert(QStringLiteral("itemId"), videoId);
        m.insert(QStringLiteral("videoId"), videoId);
        m.insert(QStringLiteral("kind"), kind);
        m.insert(QStringLiteral("title"), title);
        m.insert(QStringLiteral("channel"), QStringLiteral("Blender Studio"));
        m.insert(QStringLiteral("channelId"), QStringLiteral("UCSMOQeBJ2RAnuFungnQOxLg"));
        m.insert(QStringLiteral("duration"), QStringLiteral("2:50"));
        m.insert(QStringLiteral("durationSeconds"), 170.0);
        m.insert(QStringLiteral("viewCount"), 1234567.0);
        m.insert(QStringLiteral("viewText"), QString());
        m.insert(QStringLiteral("published"), QString());
        m.insert(QStringLiteral("thumbnail"), QString());
        m.insert(QStringLiteral("live"), false);
        m.insert(QStringLiteral("upcoming"), false);
        m.insert(QStringLiteral("membersOnly"), false);
        m.insert(QStringLiteral("restricted"), false);
        m.insert(QStringLiteral("isShort"), false);
        m.insert(QStringLiteral("shortCandidate"), false);
        m.insert(QStringLiteral("videoCount"), -1.0);
        m.insert(QStringLiteral("videoCountText"), QString());
        m.insert(QStringLiteral("playlistId"), QString());
        return m;
    }
    int rowCount(const QModelIndex &parent = QModelIndex()) const override { return parent.isValid() ? 0 : rows_.size(); }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() >= rows_.size()) return QVariant();
        return rows_.at(index.row()).value(QString::fromLatin1(roleNames().value(role)));
    }
    QHash<int, QByteArray> roleNames() const override {
        QHash<int, QByteArray> r;
        int i = Qt::UserRole + 1;
        for (const QString &k : rows_.first().keys()) r.insert(i++, k.toLatin1());
        return r;
    }
    Q_INVOKABLE QVariantMap get(int i) const { return i >= 0 && i < rows_.size() ? rows_.at(i) : QVariantMap(); }
    Q_INVOKABLE void open(const QString &reference) { reference_ = reference; emit changed(); }
    Q_INVOKABLE void refresh() {}
    Q_INVOKABLE void loadMore() {}
    Q_INVOKABLE void setTab(const QString &t) { tab_ = t; emit changed(); }
    QString reference() const { return reference_; }
    QString channelId() const { return QStringLiteral("UCSMOQeBJ2RAnuFungnQOxLg"); }
    QString title() const { return QStringLiteral("Blender Studio"); }
    QString handle() const { return QStringLiteral("@BlenderStudio"); }
    QString subscriberText() const { return QString(); }
    double subscriberCount() const { return 1020000; }
    double videoCount() const { return 6201; }
    QString tab() const { return tab_; }
    QStringList tabs() const { return QStringList() << QStringLiteral("videos") << QStringLiteral("playlists"); }
    QString empty() const { return QString(); }
    bool no() const { return false; }
    int zero() const { return 0; }
    int count() const { return rows_.size(); }
    void setFlag(bool) {}
signals:
    void changed();
private:
    QList<QVariantMap> rows_;
    QString reference_;
    QString tab_ = QStringLiteral("videos");
};

class FakePlayback : public QObject {
    Q_OBJECT
public:
    QString openedId;
    QVariantMap openedHints;
    Q_INVOKABLE void open(const QString &videoId, const QVariantMap &hints) {
        openedId = videoId;
        openedHints = hints;
        events().append(QStringLiteral("open ") + videoId);
    }
};

// The shell: every list opens a video through window.openVideo(videoId, hints).
class FakeWindow : public QObject {
    Q_OBJECT
public:
    QString openedId;
    QVariantMap openedHints;
    Q_INVOKABLE void openVideo(const QString &videoId, const QVariant &hints = QVariant()) {
        openedId = videoId;
        openedHints = hints.toMap();
        events().append(QStringLiteral("openVideo ") + videoId);
    }
};

class FakeStack : public QObject {
    Q_OBJECT
public:
    QStringList pushed;
    QVariantList props;
    Q_INVOKABLE QVariant push(const QVariant &page, const QVariant &properties = QVariant(), const QVariant & = QVariant()) {
        pushed.append(page.toString());
        props.append(properties);
        return QVariant();
    }
};

static QQuickItem *findListView(QQuickItem *root) {
    if (!root) return nullptr;
    if (QByteArray(root->metaObject()->className()).contains("ListView")) return root;
    for (QQuickItem *c : root->childItems())
        if (QQuickItem *f = findListView(c)) return f;
    return nullptr;
}

// Emits the item's `clicked` signal whatever its argument type (Silica's ListItem).
static bool emitClicked(QObject *item) {
    const QMetaObject *mo = item->metaObject();
    for (int i = mo->methodOffset() >= 0 ? 0 : 0; i < mo->methodCount(); ++i) {
        const QMetaMethod m = mo->method(i);
        if (m.methodType() != QMetaMethod::Signal || m.name() != "clicked") continue;
        if (m.parameterCount() == 0) return m.invoke(item, Qt::DirectConnection);
        const QByteArray type = m.parameterTypes().value(0);
        if (type == "QVariant") return m.invoke(item, Qt::DirectConnection, Q_ARG(QVariant, QVariant()));
        void *null = nullptr;
        return m.invoke(item, Qt::DirectConnection, QGenericArgument(type.constData(), &null));
    }
    return false;
}

static QQuickItem *delegateAt(QQuickItem *list, int index) {
    // The delegates are children of the list's contentItem; pick the one whose model index matches.
    QQuickItem *content = qvariant_cast<QQuickItem *>(list->property("contentItem"));
    if (!content) return nullptr;
    for (QQuickItem *c : content->childItems()) {
        QQmlContext *ctx = qmlContext(c);
        if (!ctx) continue;
        const QVariant i = ctx->contextProperty(QStringLiteral("index"));
        if (i.isValid() && i.toInt() == index && c->property("contentHeight").isValid()) return c;
    }
    return nullptr;
}

static void spin(int ms) {
    QTimer t; t.setSingleShot(true); t.start(ms);
    while (t.isActive()) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

int main(int argc, char **argv) {
    QGuiApplication app(argc, argv);
    if (argc < 2) { std::printf("usage: %s <qml root>\n", argv[0]); return 2; }
    qmlRegisterType<FakeChannelModel>("org.nordilabs.sailtube", 1, 0, "ChannelModel");
    QQmlEngine engine;
    FakePlayback playback;
    FakeStack stack;
    TapIntent intent;
    CatalogText text(QStringLiteral("es"));
    QObject appSettings;
    FakeWindow shell;
    QObject::connect(&intent, &TapIntent::opened, [](const QString &id, int) { events().append(QStringLiteral("opened ") + id); });
    engine.rootContext()->setContextProperty(QStringLiteral("playback"), &playback);
    engine.rootContext()->setContextProperty(QStringLiteral("pageStack"), &stack);
    engine.rootContext()->setContextProperty(QStringLiteral("tapIntent"), &intent);
    engine.rootContext()->setContextProperty(QStringLiteral("catalogText"), &text);
    engine.rootContext()->setContextProperty(QStringLiteral("appSettings"), &appSettings);
    engine.rootContext()->setContextProperty(QStringLiteral("window"), &shell);

    QQuickWindow window;
    window.resize(540, 960);
    QQmlComponent component(&engine, QUrl::fromLocalFile(QDir(QString::fromLocal8Bit(argv[1])).filePath(QStringLiteral("pages/ChannelPage.qml"))));
    QObject *object = component.beginCreate(engine.rootContext());
    if (!object) {
        for (const QQmlError &e : component.errors()) std::printf("ERROR %s\n", qPrintable(e.toString()));
        return 1;
    }
    object->setProperty("reference", QStringLiteral("@BlenderStudio"));
    component.completeCreate();
    QQuickItem *page = qobject_cast<QQuickItem *>(object);
    CHECK(page != nullptr);
    if (!page) return 1;
    page->setParentItem(window.contentItem());
    page->setWidth(540);
    page->setHeight(960);
    spin(300);

    QQuickItem *list = findListView(page);
    CHECK(list != nullptr);
    if (!list) return 1;
    list->setWidth(540);
    list->setHeight(960);
    spin(300);

    QQuickItem *video = delegateAt(list, 0);
    QQuickItem *playlist = delegateAt(list, 1);
    CHECK(video != nullptr);
    CHECK(playlist != nullptr);
    if (video) {
        CHECK(emitClicked(video));
        spin(50);
        // The row opens through the shell with its own hints (the device-run TypeError), and
        // tapIntent reported the tap first.
        CHECK(shell.openedId == QLatin1String("aaaaaaaaaaa"));
        CHECK(shell.openedHints.value(QStringLiteral("title")).toString() == QLatin1String("First video"));
        CHECK(shell.openedHints.value(QStringLiteral("channel")).toString() == QLatin1String("Blender Studio"));
        CHECK(events().indexOf(QStringLiteral("opened aaaaaaaaaaa")) >= 0);
        CHECK(events().indexOf(QStringLiteral("opened aaaaaaaaaaa")) < events().indexOf(QStringLiteral("openVideo aaaaaaaaaaa")));
    }
    if (playlist) {
        CHECK(emitClicked(playlist));
        spin(50);
        CHECK(!stack.pushed.isEmpty() && stack.pushed.last().endsWith(QLatin1String("PlaylistPreviewPage.qml")));
        CHECK(stack.props.last().toMap().value(QStringLiteral("playlistId")).toString() == QLatin1String("PLxyz"));
    }
    std::printf("catalog qml: %d checks, %d failed\n", checks, failures);
    delete object;
    return failures ? 1 : 0;
}

#include "ChannelPageClickTest.moc"
