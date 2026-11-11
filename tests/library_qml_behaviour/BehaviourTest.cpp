// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// QML behaviour of the library component's Silica components and pages under the
// minimal platform: windows are never shown, input is sent as QMouseEvents and
// time passes in a real event loop. Build: tests/library_qml_behaviour/run.sh
#include "tests/library_qml_behaviour/Fakes.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickRenderControl>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <functional>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static QString sceneDir;

// One scene: a QML file in its own engine and window. The window is driven by a
// QQuickRenderControl and never rendered (no GL under the minimal platform), so the
// per-frame polish (list and positioner layout) is run by hand in wait().
struct Scene {
    QQmlEngine engine;
    QQuickRenderControl control;
    QQuickWindow window;
    QQuickItem *root = nullptr;
    Scene() : window(&control) {}
    ~Scene() { delete root; }
};
static QList<Scene *> scenes;

// ---- event loop and item helpers ---------------------------------------------

static void collect(QQuickItem *it, QList<QQuickItem *> &out) {
    out.append(it);
    for (QQuickItem *c : it->childItems()) collect(c, out);
}

static void wait(int ms) {
    QElapsedTimer t;
    t.start();
    do {
        QEventLoop loop;
        QTimer::singleShot(qMin<qint64>(10, qMax<qint64>(0, ms - t.elapsed())), &loop, SLOT(quit()));
        loop.exec();
        for (Scene *s : scenes) s->control.polishItems();
    } while (t.elapsed() < ms);
}

static bool waitFor(const std::function<bool()> &pred, int ms) {
    QElapsedTimer t;
    t.start();
    while (!pred()) {
        if (t.elapsed() > ms) return false;
        wait(10);
    }
    return true;
}

// QML types show up as "<File>_QMLTYPE_<n>" (optionally "_QML_<m>" for inline subclasses).
// "Is a": a component built on the type (VideoMenuItems is a ContextMenu) matches too.
static bool isType(const QObject *o, const char *type) {
    const QByteArray t(type);
    for (const QMetaObject *m = o->metaObject(); m; m = m->superClass()) {
        const QByteArray cls = m->className();
        if (cls == t || cls.startsWith(t + "_QMLTYPE_") || cls.startsWith(t + "_QML_")) return true;
    }
    return false;
}

static QList<QQuickItem *> find(QQuickItem *root, const char *type) {
    QList<QQuickItem *> all, out;
    if (root) collect(root, all);
    for (QQuickItem *it : all) if (isType(it, type)) out.append(it);
    return out;
}

static QQuickItem *named(QQuickItem *root, const QString &name) {
    QList<QQuickItem *> all;
    if (root) collect(root, all);
    for (QQuickItem *it : all) if (it->objectName() == name) return it;
    return nullptr;
}

static QVariant prop(QObject *o, const char *name) { return o ? QQmlProperty::read(o, QLatin1String(name)) : QVariant(); }
static QString text(QObject *o) { return prop(o, "text").toString(); }

// The VideoListItem rows of a view, by video id.
static QQuickItem *videoRow(QQuickItem *root, const QString &videoId) {
    for (QQuickItem *r : find(root, "VideoListItem")) if (prop(r, "videoId").toString() == videoId) return r;
    return nullptr;
}

// A ListItem delegate holding a label with this text.
static QQuickItem *rowWithLabel(QQuickItem *root, const QString &label) {
    for (QQuickItem *r : find(root, "ListItem"))
        for (QQuickItem *l : find(r, "Label")) if (text(l) == label) return r;
    return nullptr;
}

static QQuickItem *childOfType(QQuickItem *root, const char *type) {
    const QList<QQuickItem *> l = find(root, type);
    return l.isEmpty() ? nullptr : l.first();
}

// ---- input --------------------------------------------------------------------

static QPointF centre(QQuickItem *it) { return it->mapToScene(QPointF(it->width() / 2, it->height() / 2)); }

static void mouse(QQuickWindow *w, QEvent::Type type, const QPointF &p) {
    const Qt::MouseButtons buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent e(type, p, p, QPointF(w->mapToGlobal(p.toPoint())), Qt::LeftButton, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(w, &e);
}

static void tap(QQuickWindow *w, const QPointF &p) {
    mouse(w, QEvent::MouseButtonPress, p);
    wait(60);
    mouse(w, QEvent::MouseButtonRelease, p);
    wait(60);
}

static QQuickItem *openMenu(QQuickItem *root) {
    for (QQuickItem *m : find(root, "ContextMenu")) if (prop(m, "active").toBool()) return m;
    return nullptr;
}

// Long press on the row, then tap the context menu entry with this text, as a user does.
static bool chooseMenuEntry(QQuickWindow *w, QQuickItem *row, const QString &entry) {
    const QPointF p = centre(row);
    mouse(w, QEvent::MouseButtonPress, p);
    const bool opened = waitFor([&]() { return openMenu(w->contentItem()) != nullptr; }, 2500);
    mouse(w, QEvent::MouseButtonRelease, p);
    if (!opened) { std::printf("  context menu did not open\n"); return false; }
    QQuickItem *menu = openMenu(w->contentItem());
    QQuickItem *item = nullptr;
    for (QQuickItem *mi : find(menu, "MenuItem")) if (mi->isVisible() && text(mi) == entry) item = mi;
    if (!item) { std::printf("  no menu entry '%s'\n", qPrintable(entry)); return false; }
    // The menu expands and the list scrolls it into view: tap once the entry stands still.
    QPointF last(-1, -1);
    waitFor([&]() { wait(100); const QPointF p = centre(item); const bool still = p == last; last = p; return still; }, 3000);
    tap(w, centre(item));
    const bool closed = waitFor([&]() { return openMenu(w->contentItem()) == nullptr; }, 2000);
    if (!closed) std::printf("  context menu did not close\n");
    return closed;
}

// The remorse item Silica puts over a ListItem (ListItem::remorseAction), while counting down.
static QQuickItem *pendingRemorse(QQuickItem *row) {
    for (QQuickItem *r : find(row, "RemorseItem")) if (prop(r, "pending").toBool()) return r;
    return nullptr;
}

static Scene *open(const QString &file, const QList<QPair<QString, QObject *> > &context) {
    Scene *s = new Scene;
    for (const QPair<QString, QObject *> &c : context) s->engine.rootContext()->setContextProperty(c.first, c.second);
    QQmlComponent component(&s->engine, QUrl::fromLocalFile(sceneDir + QLatin1Char('/') + file));
    s->root = qobject_cast<QQuickItem *>(component.create());
    for (const QQmlError &e : component.errors()) std::printf("  %s\n", qPrintable(e.toString()));
    CHECK(s->root != nullptr);
    s->window.resize(540, 960);
    if (s->root) {
        s->root->setSize(QSizeF(540, 960));
        s->root->setParentItem(s->window.contentItem());
    }
    scenes.append(s);
    wait(300); // page push and delegate creation
    return s;
}

static void close(Scene *s) {
    scenes.removeAll(s);
    delete s;
}

// ---- LIB-06: progress bar, watched badge, incremental updates, no input --------

static void testWatchOverlay() {
    FakeWatchIndex watch;
    FakeDownloads downloads;
    downloads.add(1, QStringLiteral("w1"), 2, 0.3);
    Scene *v = open(QStringLiteral("OverlayScene.qml"),
                         { qMakePair(QStringLiteral("watchIndex"), (QObject *)&watch),
                           qMakePair(QStringLiteral("downloads"), (QObject *)&downloads) });
    QQuickItem *root = v->root;
    QQuickItem *o1 = named(root, QStringLiteral("overlayW1"));
    QQuickItem *o2 = named(root, QStringLiteral("overlayW2"));
    CHECK(o1 && o2);
    if (!o1 || !o2) { close(v); return; }
    QQuickItem *bar1 = named(o1, QStringLiteral("progressBar")), *fill1 = named(o1, QStringLiteral("progressFill"));
    QQuickItem *badge1 = named(o1, QStringLiteral("watchedBadge"));
    QQuickItem *bar2 = named(o2, QStringLiteral("progressBar")), *badge2 = named(o2, QStringLiteral("watchedBadge"));
    CHECK(bar1 && fill1 && badge1 && bar2 && badge2);
    if (!bar1 || !fill1 || !badge1 || !bar2 || !badge2) { close(v); return; }
    CHECK(!bar1->isVisible() && !badge1->isVisible());
    CHECK(!o1->isEnabled()); // takes no input

    // Every overlay of w1 (the bare one and the row's) asks again once per w1 event.
    int overlaysW1 = 0;
    for (QQuickItem *o : find(root, "WatchOverlay")) if (prop(o, "videoId").toString() == QLatin1String("w1")) ++overlaysW1;
    CHECK(overlaysW1 == 2);

    int calls = watch.stateCalls.value(QStringLiteral("w1"));
    watch.set(QStringLiteral("w1"), 0.006, false);
    wait(20);
    CHECK(bar1->isVisible());                              // 0.6 % shows the bar
    CHECK(qAbs(fill1->width() - o1->width() * 0.006) < 0.01);
    CHECK(!badge1->isVisible());
    CHECK(watch.stateCalls.value(QStringLiteral("w1")) == calls + overlaysW1);

    watch.set(QStringLiteral("w1"), 0.004, false);
    wait(20);
    CHECK(!bar1->isVisible());                             // 0.4 % hides it

    calls = watch.stateCalls.value(QStringLiteral("w1"));
    watch.set(QStringLiteral("w2"), 0.5, true);            // another video: w1 is not asked again
    wait(20);
    CHECK(watch.stateCalls.value(QStringLiteral("w1")) == calls);
    CHECK(!bar1->isVisible() && !badge1->isVisible());
    CHECK(bar2->isVisible() && badge2->isVisible());

    watch.set(QStringLiteral("w1"), 1.0, true);
    wait(20);
    CHECK(watch.stateCalls.value(QStringLiteral("w1")) == calls + overlaysW1);
    CHECK(badge1->isVisible() && bar1->isVisible());

    // A tap on the watched badge, the progress bar or the download badge still reaches the row.
    QQuickItem *row = videoRow(named(root, QStringLiteral("rows")), QStringLiteral("w1"));
    CHECK(row != nullptr);
    if (row) {
        QQuickItem *overlay = childOfType(row, "WatchOverlay");
        QQuickItem *dlBadge = childOfType(row, "DownloadBadge");
        QQuickItem *rowBadge = named(overlay, QStringLiteral("watchedBadge"));
        QQuickItem *rowBar = named(overlay, QStringLiteral("progressBar"));
        CHECK(overlay && dlBadge && rowBadge && rowBar);
        if (overlay && dlBadge && rowBadge && rowBar) {
            CHECK(rowBadge->isVisible() && rowBar->isVisible() && dlBadge->isVisible());
            CHECK(!overlay->isEnabled() && !dlBadge->isEnabled());
            const QPointF points[] = { centre(rowBadge), centre(rowBar), centre(dlBadge), centre(overlay) };
            int expected = 0;
            for (const QPointF &p : points) {
                tap(&v->window, p);
                ++expected;
                CHECK(waitFor([&]() { return prop(root, "clicks").toInt() == expected; }, 1000));
                wait(400); // keep taps apart
            }
        }
    }
    close(v);
}

// ---- DL-04: row badges in three lists and the downloads page, once a second -----

static QString pageStatus(QQuickItem *pageRow) {
    for (QQuickItem *l : find(pageRow, "Label")) if (text(l).contains(QStringLiteral(" · "))) return text(l);
    return QString();
}

static void testDownloadProgress() {
    FakeWatchIndex watch;
    FakeDownloads downloads;
    downloads.add(1, QStringLiteral("dl1"), 2, 0.0);
    downloads.add(2, QStringLiteral("dl2"), 2, 0.5);
    const QList<QPair<QString, QObject *> > ctx = { qMakePair(QStringLiteral("watchIndex"), (QObject *)&watch),
                                                    qMakePair(QStringLiteral("downloads"), (QObject *)&downloads) };
    Scene *lists = open(QStringLiteral("ListsScene.qml"), ctx);
    Scene *page = open(QStringLiteral("DownloadsPageScene.qml"), ctx);

    QList<QQuickItem *> badges1, badges2;
    const char *contexts[] = { "feed", "search", "channel" };
    for (const char *c : contexts) {
        QQuickItem *list = named(lists->root, QLatin1String(c));
        QQuickItem *r1 = videoRow(list, QStringLiteral("dl1")), *r2 = videoRow(list, QStringLiteral("dl2"));
        CHECK(r1 && r2);
        if (r1) badges1.append(named(childOfType(r1, "DownloadBadge"), QStringLiteral("downloadBadgeText")));
        if (r2) badges2.append(named(childOfType(r2, "DownloadBadge"), QStringLiteral("downloadBadgeText")));
    }
    CHECK(badges1.size() == 3 && badges2.size() == 3 && !badges1.contains(nullptr) && !badges2.contains(nullptr));
    if (badges1.size() != 3 || badges2.size() != 3 || badges1.contains(nullptr) || badges2.contains(nullptr)) {
        close(lists); close(page); return;
    }
    QQuickItem *pageRow1 = rowWithLabel(page->window.contentItem(), QStringLiteral("dl1"));
    QQuickItem *pageRow2 = rowWithLabel(page->window.contentItem(), QStringLiteral("dl2"));
    CHECK(pageRow1 && pageRow2);
    QQuickItem *pageBar1 = pageRow1 ? childOfType(pageRow1, "ProgressBar") : nullptr;
    CHECK(pageBar1 != nullptr);

    for (QQuickItem *b : badges1) CHECK(b->isVisible() && text(b) == QLatin1String("0 %"));
    for (QQuickItem *b : badges2) CHECK(b->isVisible() && text(b) == QLatin1String("50 %"));
    const QString status2 = pageStatus(pageRow2);
    CHECK(status2.endsWith(QStringLiteral("Downloading 50 %")));
    const int dl2Calls = downloads.stateForCalls.value(QStringLiteral("dl2"));

    // The engine's progress tick: every 1000 ms, then done.
    int ticks = 0;
    QElapsedTimer sinceTick;
    QTimer timer;
    timer.setInterval(1000);
    QObject::connect(&timer, &QTimer::timeout, [&]() {
        ++ticks;
        if (ticks < 5) downloads.update(QStringLiteral("dl1"), 2, 0.2 * ticks);
        else { downloads.update(QStringLiteral("dl1"), 6, 1.0); timer.stop(); }
        sinceTick.start();
    });
    timer.start();
    for (int k = 1; k <= 5; ++k) {
        CHECK(waitFor([&]() { return ticks >= k; }, 1500));
        const QString want = k < 5 ? QString::number(20 * k) + QStringLiteral(" %") : QStringLiteral("Saved");
        const QString wantPage = k < 5 ? QStringLiteral("Downloading ") + QString::number(20 * k) + QStringLiteral(" %")
                                       : QStringLiteral("Done");
        const bool shown = waitFor([&]() {
            for (QQuickItem *b : badges1) if (!b->isVisible() || text(b) != want) return false;
            return pageRow1 && pageStatus(pageRow1).endsWith(wantPage);
        }, 1000);
        CHECK(shown);
        CHECK(sinceTick.elapsed() <= 1000);
        if (!shown) {
            for (QQuickItem *b : badges1) std::printf("  tick %d badge '%s'\n", k, qPrintable(text(b)));
            if (pageRow1) std::printf("  tick %d page '%s'\n", k, qPrintable(pageStatus(pageRow1)));
        }
        if (k < 5 && pageBar1) CHECK(qAbs(prop(pageBar1, "value").toDouble() - 0.2 * k) < 1e-6);
        // The row of the other video never changes and is never asked again.
        for (QQuickItem *b : badges2) CHECK(b->isVisible() && text(b) == QLatin1String("50 %"));
        CHECK(downloads.stateForCalls.value(QStringLiteral("dl2")) == dl2Calls);
        CHECK(pageStatus(pageRow2) == status2);
    }
    close(page);
    close(lists);
}

// ---- LIB-02: removal with remorse on PlaylistPage and PlaylistsPage -------------

// One page under test: cancel the remorse on one row, let it run out on another.
struct RemorseCase {
    const char *name;
    Scene *view;
    std::function<QQuickItem *(const QString &)> row;
    QString entry;              // the menu entry that removes
    QString keep, drop;         // row cancelled, row committed
    std::function<QStringList()> removed;
    std::function<QVector<qint64>()> removedAt;
    std::function<qint64()> now;
    qint64 started = -1;        // when the committed row's remorse began
};

static bool startRemorse(RemorseCase &c, const QString &key, QQuickItem **remorse) {
    QQuickItem *row = c.row(key);
    if (!row) { std::printf("  %s: no row %s\n", c.name, qPrintable(key)); return false; }
    if (!chooseMenuEntry(&c.view->window, row, c.entry)) return false;
    const bool ok = waitFor([&]() { *remorse = pendingRemorse(c.row(key)); return *remorse != nullptr; }, 1500);
    if (!ok) std::printf("  %s: no remorse on %s (%d remorse items)\n", c.name, qPrintable(key), c.row(key) ? find(c.row(key), "RemorseItem").size() : -1);
    return ok;
}

// Cancels on `keep`, then starts the committed removal on `drop`.
static void beginRemorseCase(RemorseCase &c) {
    QQuickItem *remorse = nullptr;
    const bool keepStarted = startRemorse(c, c.keep, &remorse);
    CHECK(keepStarted);
    if (keepStarted) {
        wait(300);
        tap(&c.view->window, centre(remorse)); // "Tap to undo"
        CHECK(waitFor([&]() { return pendingRemorse(c.row(c.keep)) == nullptr; }, 1000));
    }
    const bool dropStarted = startRemorse(c, c.drop, &remorse);
    CHECK(dropStarted);
    if (dropStarted) c.started = c.now();
    // During the countdown nothing is removed and the row is still there, under the remorse banner.
    wait(500);
    CHECK(c.removed().isEmpty());
    CHECK(c.row(c.drop) != nullptr && pendingRemorse(c.row(c.drop)) != nullptr);
}

static void finishRemorseCase(RemorseCase &c) {
    if (c.started < 0) return;
    if (c.now() - c.started < 3500) CHECK(c.removed().isEmpty() && c.row(c.drop) != nullptr);
    CHECK(waitFor([&]() { return !c.removed().isEmpty(); }, int(c.started + 9000 - c.now())));
    wait(500);
    CHECK(c.removed() == QStringList(c.drop));            // exactly once, only the committed row
    if (!c.removedAt().isEmpty()) {
        const qint64 after = c.removedAt().first() - c.started;
        std::printf("  %s: removal committed %lld ms after the remorse began\n", c.name, (long long)after);
        CHECK(after >= 4000);                             // the Silica default delay
    }
    CHECK(waitFor([&]() { return c.row(c.drop) == nullptr; }, 1000));
    // The cancelled row is restored.
    QQuickItem *kept = c.row(c.keep);
    CHECK(kept != nullptr);
    if (kept) {
        QObject *content = prop(kept, "contentItem").value<QObject *>();
        CHECK(content && waitFor([&]() { return qFuzzyCompare(prop(content, "opacity").toDouble(), 1.0); }, 1000));
        CHECK(pendingRemorse(kept) == nullptr);
    }
}

static void testRemorseRemoval() {
    FakeWatchIndex watch;
    FakePlaylistItems items;
    items.id = QStringLiteral("p1");
    items.videos << QStringLiteral("a") << QStringLiteral("b");
    FakePlaylists playlists;
    playlists.list = &items;
    playlists.ids << QStringLiteral("l1") << QStringLiteral("l2"); // both fit the minimal platform's small page
    const QList<QPair<QString, QObject *> > ctx = { qMakePair(QStringLiteral("watchIndex"), (QObject *)&watch),
                                                    qMakePair(QStringLiteral("playlistsModel"), (QObject *)&playlists) };

    RemorseCase one;
    one.name = "PlaylistPage";
    one.view = open(QStringLiteral("PlaylistPageScene.qml"), ctx);
    one.row = [&](const QString &id) { return videoRow(one.view->window.contentItem(), id); };
    one.entry = QStringLiteral("Remove from playlist");
    one.keep = QStringLiteral("b");
    one.drop = QStringLiteral("a");
    one.removed = [&]() { return items.removed; };
    one.removedAt = [&]() { return items.removedAt; };
    one.now = [&]() { return items.clock.elapsed(); };

    RemorseCase all;
    all.name = "PlaylistsPage";
    all.view = open(QStringLiteral("PlaylistsPageScene.qml"), ctx);
    all.row = [&](const QString &id) { return rowWithLabel(all.view->window.contentItem(), QStringLiteral("List ") + id); };
    all.entry = QStringLiteral("Delete");
    all.keep = QStringLiteral("l2");
    all.drop = QStringLiteral("l1");
    all.removed = [&]() { return playlists.removed; };
    all.removedAt = [&]() { return playlists.removedAt; };
    all.now = [&]() { return playlists.clock.elapsed(); };

    // Both pages count down at the same time to keep the suite short.
    beginRemorseCase(one);
    beginRemorseCase(all);
    finishRemorseCase(one);
    finishRemorseCase(all);
    close(all.view);
    close(one.view);
}

// QML warnings and errors go to the report; Silica's notes about the minimal platform's
// missing device configuration do not.
// ---- "Download quality" setting: the dialog starts from it ------------------------

// Opens the real dialog with the setting's values on the fake model, checks what it shows,
// accepts it and returns what was enqueued (kind -2 when nothing was).
static FakeDownloads::Enqueued dialogDefault(int settingKind, int settingHeight, int *shownKind, int *shownHeight,
                                             bool *qualityVisible, QString *qualityText) {
    FakeDownloads downloads;
    downloads.defaultKind = settingKind;
    downloads.defaultQualityHeight = settingHeight;
    Scene *s = open(QStringLiteral("DownloadDialogScene.qml"), { qMakePair(QStringLiteral("downloads"), (QObject *)&downloads) });
    FakeDownloads::Enqueued none; none.kind = -2; none.qualityHeight = 0;
    if (!s->root) { close(s); return none; }
    QMetaObject::invokeMethod(s->root, "openDialog");
    wait(400);
    QObject *dialog = prop(s->root, "dialog").value<QObject *>();
    CHECK(dialog != nullptr);
    if (!dialog) { close(s); return none; }
    *shownKind = prop(dialog, "kind").toInt();
    *shownHeight = prop(dialog, "qualityHeight").toInt();
    const QList<QQuickItem *> combos = find(qobject_cast<QQuickItem *>(dialog), "ComboBox");
    CHECK(combos.size() == 2);
    if (combos.size() == 2) {
        CHECK(prop(combos[0], "currentIndex").toInt() == *shownKind);   // the kind combo shows it
        *qualityVisible = prop(combos[1], "visible").toBool();
        *qualityText = prop(combos[1], "value").toString();
    }
    QMetaObject::invokeMethod(dialog, "accept");
    waitFor([&]() { return !downloads.enqueued.isEmpty(); }, 2000);
    const FakeDownloads::Enqueued result = downloads.enqueued.isEmpty() ? none : downloads.enqueued.first();
    CHECK(downloads.enqueued.size() == 1);
    close(s);
    return result;
}

static void testDownloadQualityDefault() {
    int kind = -1, height = -1;
    bool visible = false;
    QString value;
    // 720p (the fresh-install default): video at 720p, not the playback ceiling.
    FakeDownloads::Enqueued e = dialogDefault(1, 720, &kind, &height, &visible, &value);
    CHECK(kind == 1 && height == 720 && visible && value == QStringLiteral("720p"));
    CHECK(e.videoId == QStringLiteral("dlgquality1") && e.kind == 1 && e.qualityHeight == 720);
    // 1080p.
    e = dialogDefault(1, 1080, &kind, &height, &visible, &value);
    CHECK(kind == 1 && height == 1080 && value == QStringLiteral("1080p"));
    CHECK(e.kind == 1 && e.qualityHeight == 1080);
    // Audio only: the dialog starts on audio, the quality row is hidden, an audio download is queued.
    e = dialogDefault(0, 720, &kind, &height, &visible, &value);
    CHECK(kind == 0 && !visible);
    CHECK(e.kind == 0 && e.qualityHeight == 0);
}

static void messages(QtMsgType, const QMessageLogContext &, const QString &m) {
    if (m.contains(QLatin1String("pixel ratio")) || m.contains(QLatin1String("DPI value"))) return;
    std::printf("  log: %s\n", qPrintable(m));
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    qInstallMessageHandler(messages);
    QGuiApplication app(argc, argv);
    sceneDir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    QTimer::singleShot(60000, []() { std::printf("library qml behaviour tests: FAIL (timeout)\n"); std::_Exit(2); });
    QElapsedTimer t;
    t.start();
    testWatchOverlay();
    testDownloadProgress();
    testRemorseRemoval();
    testDownloadQualityDefault();
    std::printf("library qml behaviour tests: %s (%d failure%s, %lld ms)\n", failures ? "FAIL" : "PASS", failures,
                failures == 1 ? "" : "s", (long long)t.elapsed());
    return failures ? 1 : 0;
}
