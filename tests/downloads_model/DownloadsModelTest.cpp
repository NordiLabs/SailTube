// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DL-04/08/09 presentation: per-video lookups for badges and the watch-page
// indicator, one change signal per video, the list rows, download-all and
// the STORE flag. Transfers themselves are covered by tests/downloads_engine.
#include "Clients/Sailfish/Presentation/DownloadsModel.h"
#include "Core/Downloads/Infrastructure/DownloadManager.h"
#include "Core/Library/Infrastructure/LibraryStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QTimer>
#include <cstdio>

using namespace nordtile::video;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

// Never answers: items stay in Resolving, which is enough for the view.
class PendingResolver final : public downloads::DownloadResolver {
public:
    quint64 resolve(const QString &, Completion) override { return ++next; }
    void cancel(quint64) override {}
    quint64 next = 0;
};

static QVariantMap video(const char *id) {
    QVariantMap m;
    m.insert(QStringLiteral("videoId"), QString::fromLatin1(id));
    m.insert(QStringLiteral("title"), QStringLiteral("Title ") + QString::fromLatin1(id));
    m.insert(QStringLiteral("channel"), QStringLiteral("Channel"));
    m.insert(QStringLiteral("durationSeconds"), 120);
    m.insert(QStringLiteral("thumbnail"), QStringLiteral("https://i.ytimg.com/vi/x/mqdefault.jpg"));
    return m;
}

static void spin(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    const QString dir = QString::fromLocal8Bit(qgetenv("TEST_DIR")) + QStringLiteral("/dlmodel");
    QDir().mkpath(dir + QStringLiteral("/data"));
    library::LibraryStore store(dir + QStringLiteral("/library.db"), QStringLiteral("dlmodel-library"));
    CHECK(store.open());

    PendingResolver resolver;
    downloads::DownloadManagerConfig config;
    config.databasePath = store.databasePath();
    config.connectionName = QStringLiteral("dlmodel-downloads");
    config.roots.appData = dir + QStringLiteral("/data");
    config.roots.userFolders.clear();
    config.roots.removableRoot = dir + QStringLiteral("/media");
    config.roots.refuseTemporary = false;
    config.engine.unmetered = []() { return true; };
    downloads::DownloadManager manager(config, &resolver, nullptr);
    CHECK(manager.open());

    sailfish::DownloadsModel model(&manager, true);
    CHECK(model.backgroundCapable());
    sailfish::DownloadsModel storeModel(&manager, false);
    CHECK(!storeModel.backgroundCapable()); // STORE: the page states downloads run only while open

    QStringList changed;
    QObject::connect(&model, &sailfish::DownloadsModel::itemChanged, [&changed](const QString &id) { changed << id; });
    CHECK(model.stateFor(QStringLiteral("aaaaaaaaaaa")).value(QStringLiteral("state")).toInt() == -1);
    CHECK(!model.stateFor(QStringLiteral("aaaaaaaaaaa")).value(QStringLiteral("active")).toBool());

    const int id = model.enqueue(video("aaaaaaaaaaa"), 0, 0);
    CHECK(id > 0);
    spin(50);
    CHECK(model.count() == 1);
    QVariantMap st = model.stateFor(QStringLiteral("aaaaaaaaaaa"));
    CHECK(st.value(QStringLiteral("active")).toBool());
    CHECK(!st.value(QStringLiteral("done")).toBool());
    CHECK(st.value(QStringLiteral("kind")).toInt() == 0);
    CHECK(changed.contains(QStringLiteral("aaaaaaaaaaa")));
    CHECK(!changed.contains(QStringLiteral("bbbbbbbbbbb")));
    // the shared "thumbnail" key reaches the manager as its thumbnail URL
    CHECK(model.data(model.index(0, 0), sailfish::DownloadsModel::ThumbnailRole).toString().startsWith(QStringLiteral("https://i.ytimg.com/")));
    CHECK(model.data(model.index(0, 0), sailfish::DownloadsModel::TitleRole).toString() == QStringLiteral("Title aaaaaaaaaaa"));
    CHECK(model.activeCount() == 1 && model.keepAliveWanted());

    // DL-08 through the model: one already queued, two new
    QVariantList all;
    all << video("aaaaaaaaaaa") << video("bbbbbbbbbbb") << video("ccccccccccc");
    CHECK(model.enqueueAll(all, 0, 0) == 2);
    CHECK(model.enqueueAll(all, 0, 0) == 0);
    spin(50);
    CHECK(model.count() == 3);

    // pause shows as paused, still listed; cancel removes the row and tells the badge
    CHECK(model.pause(id));
    spin(20);
    CHECK(model.stateFor(QStringLiteral("aaaaaaaaaaa")).value(QStringLiteral("paused")).toBool());
    changed.clear();
    CHECK(model.cancel(id));
    spin(50);
    CHECK(model.count() == 2);
    CHECK(changed.contains(QStringLiteral("aaaaaaaaaaa")));
    CHECK(model.stateFor(QStringLiteral("aaaaaaaaaaa")).value(QStringLiteral("state")).toInt() == -1);

    // texts exist for every state and the folder errors are translated keys, not raw codes
    for (int s = 0; s <= 7; ++s) CHECK(!model.stateText(s, QStringLiteral("network")).isEmpty());
    CHECK(sailfish::DownloadsModel::noticeText(QStringLiteral("downloads_notice_folder_unavailable")) != QStringLiteral("downloads_notice_folder_unavailable"));
    const QVariantMap bad = model.chooseFolder(QStringLiteral("relative/path"));
    CHECK(!bad.value(QStringLiteral("ok")).toBool() && !bad.value(QStringLiteral("error")).toString().isEmpty());

    std::printf("downloads model tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
