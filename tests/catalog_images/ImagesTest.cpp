// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DSC-19 artwork cache: URL policy (https, avatar 176 px, host allow-list), the stored-entry
// policy (avatars fresh for 7 days whatever the server says, then fetched again), one shared
// disk cache bounded at 64 MB across managers and runs, no cookies. The end-to-end part runs a
// QNetworkAccessManager from the factory against a loopback HTTP server (no outside network).
#include "tests/catalog_common/TestSupport.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"
#include <QFile>

#include "Clients/Sailfish/Presentation/ImageCacheFactory.h"

#include <QAbstractNetworkCache>
#include <QBuffer>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>

using namespace nordtile::video::sailfish;

namespace {

QString requested(const char *url) {
    return imagepolicy::requestUrl(QUrl(QString::fromLatin1(url)), ImageCacheOptions()).toString(QUrl::FullyEncoded);
}

void urlPolicy() {
    // Avatars: protocol-relative becomes https, =s88 becomes =s176 (both avatar hosts).
    CHECK_EQ(requested("//yt3.ggpht.com/abcDEF=s88-c-k-c0x00ffffff-no-rj"),
             QStringLiteral("https://yt3.ggpht.com/abcDEF=s176-c-k-c0x00ffffff-no-rj"));
    CHECK_EQ(requested("https://yt3.googleusercontent.com/xyz=s68-c-k-c0x00ffffff-no-rj"),
             QStringLiteral("https://yt3.googleusercontent.com/xyz=s176-c-k-c0x00ffffff-no-rj"));
    CHECK_EQ(requested("http://lh3.googleusercontent.com/a/photo=s88"), QStringLiteral("https://lh3.googleusercontent.com/a/photo=s176"));
    // Thumbnails: upgraded to https, otherwise untouched (query kept).
    CHECK_EQ(requested("http://i.ytimg.com/vi/9zCH37330f8/mqdefault.jpg"), QStringLiteral("https://i.ytimg.com/vi/9zCH37330f8/mqdefault.jpg"));
    CHECK_EQ(requested("//i.ytimg.com/vi/9zCH37330f8/hqdefault.jpg"), QStringLiteral("https://i.ytimg.com/vi/9zCH37330f8/hqdefault.jpg"));
    CHECK_EQ(requested("https://i9.ytimg.com/vi/9zCH37330f8/mqdefault.jpg?sqp=abc&rs=x"),
             QStringLiteral("https://i9.ytimg.com/vi/9zCH37330f8/mqdefault.jpg?sqp=abc&rs=x"));
    // Host allow-list: everything else is refused (invalid URL).
    for (const char *refused : {"https://example.com/a.jpg", "https://evilytimg.com/vi/x/mqdefault.jpg",
                                "https://i.ytimg.com.evil.example/x.jpg", "https://ytimg.com/x.jpg",
                                "https://yt4.ggpht.com/x=s88", "https://googleusercontent.com.example/x",
                                "file:///etc/passwd", "ftp://i.ytimg.com/x.jpg", "https://user@i.ytimg.com/x.jpg",
                                "https://www.youtube.com/watch?v=9zCH37330f8", "data:image/png;base64,AAAA", "/relative/path.jpg"})
        CHECK(!imagepolicy::requestUrl(QUrl(QString::fromLatin1(refused)), ImageCacheOptions()).isValid());
    CHECK(imagepolicy::hostMatches(QStringLiteral("I.YTIMG.COM"), ImageCacheOptions().allowedHosts));
    CHECK(imagepolicy::isAvatar(QUrl(QStringLiteral("https://yt3.ggpht.com/a=s88")), ImageCacheOptions()));
    CHECK(!imagepolicy::isAvatar(QUrl(QStringLiteral("https://i.ytimg.com/vi/x/mqdefault.jpg")), ImageCacheOptions()));
}

QNetworkCacheMetaData response(const QString &url, const QList<QPair<QByteArray, QByteArray>> &headers, bool saveToDisk) {
    QNetworkCacheMetaData m;
    m.setUrl(QUrl(url));
    m.setRawHeaders(headers);
    m.setSaveToDisk(saveToDisk);
    return m;
}

bool hasHeader(const QNetworkCacheMetaData &m, const QByteArray &name, const QByteArray &value = QByteArray()) {
    for (const auto &h : m.rawHeaders())
        if (h.first.toLower() == name && (value.isNull() || h.second == value)) return true;
    return false;
}

void storedEntryPolicy() {
    const ImageCacheOptions options;
    const QDateTime storedAt = QDateTime(QDate(2026, 11, 1), QTime(12, 0), Qt::UTC);
    // An avatar served with "do not cache" headers is kept for exactly 7 days.
    const QNetworkCacheMetaData avatar = imagepolicy::storedMetaData(
        response(QStringLiteral("https://yt3.ggpht.com/abc=s176-c-k-c0x00ffffff-no-rj"),
                 {qMakePair(QByteArray("Cache-Control"), QByteArray("no-cache, no-store, max-age=0")),
                  qMakePair(QByteArray("Pragma"), QByteArray("no-cache")), qMakePair(QByteArray("Age"), QByteArray("999999")),
                  qMakePair(QByteArray("Set-Cookie"), QByteArray("x=y")), qMakePair(QByteArray("Content-Type"), QByteArray("image/jpeg")),
                  qMakePair(QByteArray("ETag"), QByteArray("\"v1\""))},
                 false),
        options, storedAt);
    CHECK(avatar.saveToDisk());
    CHECK(avatar.expirationDate().toUTC() == storedAt.addDays(7));
    CHECK(hasHeader(avatar, "cache-control", "max-age=604800"));
    CHECK(hasHeader(avatar, "date", "Sun, 01 Nov 2026 12:00:00 GMT"));
    CHECK(!hasHeader(avatar, "pragma") && !hasHeader(avatar, "age") && !hasHeader(avatar, "set-cookie"));
    CHECK(hasHeader(avatar, "etag", "\"v1\"") && hasHeader(avatar, "content-type", "image/jpeg"));
    CHECK(imagepolicy::isFresh(avatar, storedAt.addDays(6)));
    CHECK(!imagepolicy::isFresh(avatar, storedAt.addDays(8)));      // cached 8 days ago: fetched again
    // Thumbnails keep the server's own policy (only cookies are dropped).
    const QDateTime serverExpiry = storedAt.addSecs(7200);
    QNetworkCacheMetaData thumbIn = response(QStringLiteral("https://i.ytimg.com/vi/9zCH37330f8/mqdefault.jpg"),
                                             {qMakePair(QByteArray("Cache-Control"), QByteArray("public, max-age=7200")),
                                              qMakePair(QByteArray("Set-Cookie"), QByteArray("x=y"))},
                                             true);
    thumbIn.setExpirationDate(serverExpiry);
    const QNetworkCacheMetaData thumb = imagepolicy::storedMetaData(thumbIn, options, storedAt);
    CHECK(thumb.expirationDate() == serverExpiry);
    CHECK(hasHeader(thumb, "cache-control", "public, max-age=7200"));
    CHECK(!hasHeader(thumb, "set-cookie"));
    CHECK(!imagepolicy::isFresh(thumb, storedAt.addDays(1)));
}

void storeThrough(QAbstractNetworkCache *cache, const QString &url, int bytes) {
    QNetworkCacheMetaData m = response(url, {qMakePair(QByteArray("Content-Type"), QByteArray("image/jpeg"))}, true);
    m.setLastModified(QDateTime::currentDateTimeUtc());
    QIODevice *device = cache->prepare(m);
    CHECK(device != nullptr);
    if (!device) return;
    device->write(QByteArray(bytes, 'x'));
    cache->insert(device);
}

void sharedBoundedCache() {
    QTemporaryDir dir;
    CHECK(dir.isValid());
    ImageCacheOptions options;
    options.directory = dir.path();
    {
        ImageCacheFactory factory(options);
        QScopedPointer<QNetworkAccessManager> a(factory.create(nullptr));
        QScopedPointer<QNetworkAccessManager> b(factory.create(nullptr));
        CHECK(a->cache() != nullptr && b->cache() != nullptr && a->cache() != b->cache());
        // One cache behind both managers: what one stores the other sees.
        storeThrough(a->cache(), QStringLiteral("https://i.ytimg.com/vi/shared00001/mqdefault.jpg"), 1000);
        CHECK(b->cache()->metaData(QUrl(QStringLiteral("https://i.ytimg.com/vi/shared00001/mqdefault.jpg"))).isValid());
        // 80 MB of images through both managers: the disk holds at most 64 MB.
        for (int i = 0; i < 80; ++i)
            storeThrough(i % 2 ? a->cache() : b->cache(), QStringLiteral("https://i.ytimg.com/vi/img%1/mqdefault.jpg").arg(i, 8, 10, QLatin1Char('0')),
                         1024 * 1024);
        const qint64 size = factory.cacheSize();
        std::printf("  disk cache after 80 MB: %lld bytes\n", static_cast<long long>(size));
        CHECK(size > 32 * 1024 * 1024);
        CHECK(size <= 64 * 1024 * 1024);
        CHECK(a->cache()->metaData(QUrl(QStringLiteral("https://i.ytimg.com/vi/img00000079/mqdefault.jpg"))).isValid());
    }
    // Persistent: a later run (new factory, same directory) still has the newest entries.
    ImageCacheFactory again(options);
    QScopedPointer<QNetworkAccessManager> c(again.create(nullptr));
    CHECK(c->cache()->metaData(QUrl(QStringLiteral("https://i.ytimg.com/vi/img00000079/mqdefault.jpg"))).isValid());
    CHECK(again.cacheSize() <= 64 * 1024 * 1024);
    CHECK_EQ(ImageCacheOptions().maximumBytes, static_cast<qint64>(64 * 1024 * 1024));
    CHECK(ImageCacheFactory::defaultDirectory().endsWith(QStringLiteral("/thumbnails")));
}

// Loopback HTTP server: answers every GET with an image and hostile cache headers.
struct LoopbackServer {
    QTcpServer server;
    QStringList paths;
    QStringList notFound;   // paths answered 404
    QList<QByteArray> requests;
    LoopbackServer() {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, [this] {
            while (QTcpSocket *s = server.nextPendingConnection()) {
                QObject::connect(s, &QTcpSocket::readyRead, [this, s] {
                    const QByteArray head = s->peek(65536);
                    if (!head.contains("\r\n\r\n")) return;
                    s->readAll();
                    requests << head;
                    paths << QString::fromLatin1(head.split(' ').value(1));
                    const QByteArray body(512, 'p');
                    if (notFound.contains(paths.last())) {
                        s->write("HTTP/1.1 404 Not Found\r\nContent-Type: image/jpeg\r\nConnection: close\r\nContent-Length: " +
                                 QByteArray::number(body.size()) + "\r\n\r\n" + body);
                        s->disconnectFromHost();
                        return;
                    }
                    s->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nCache-Control: no-cache, must-revalidate, max-age=0\r\n"
                             "Pragma: no-cache\r\nSet-Cookie: tracker=1; Path=/\r\nConnection: close\r\nContent-Length: " +
                             QByteArray::number(body.size()) + "\r\n\r\n" + body);
                    s->disconnectFromHost();
                });
                QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            }
        });
    }
    QString url(const char *path) const { return QStringLiteral("http://127.0.0.1:%1%2").arg(server.serverPort()).arg(QLatin1String(path)); }
};

struct Fetched { QNetworkReply::NetworkError error; QByteArray body; bool fromCache; };

Fetched fetch(QNetworkAccessManager *manager, const QString &url) {
    QNetworkReply *reply = manager->get(QNetworkRequest(QUrl(url)));
    bool done = false;
    QObject::connect(reply, &QNetworkReply::finished, [&done] { done = true; });
    CHECK(catalog_test::waitFor([&done] { return done; }, 5000));
    Fetched f;
    f.error = reply->error();
    f.body = reply->readAll();
    f.fromCache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();
    reply->deleteLater();
    return f;
}

void endToEnd() {
    LoopbackServer server;
    CHECK(server.server.isListening());
    QTemporaryDir dir;
    ImageCacheOptions options;
    options.directory = dir.path();
    options.allowedHosts = QStringList() << QStringLiteral("127.0.0.1");
    options.avatarHosts = QStringList() << QStringLiteral("127.0.0.1");   // treat loopback images as avatars
    options.httpsOnly = false;
    {
        ImageCacheFactory factory(options);
        QScopedPointer<QNetworkAccessManager> manager(factory.create(nullptr));
        manager->setProxy(QNetworkProxy::NoProxy);
        Fetched first = fetch(manager.data(), server.url("/avatar=s88-c-k"));
        CHECK(first.error == QNetworkReply::NoError);
        CHECK_EQ(first.body.size(), 512);
        CHECK_EQ(server.paths.size(), 1);
        CHECK_EQ(server.paths.value(0), QStringLiteral("/avatar=s176-c-k"));      // 176 px requested
        // Fresh for 7 days although the server said no-cache: served from the disk cache.
        Fetched second = fetch(manager.data(), server.url("/avatar=s88-c-k"));
        CHECK(second.error == QNetworkReply::NoError);
        CHECK(second.fromCache);
        CHECK_EQ(second.body.size(), 512);
        CHECK_EQ(server.paths.size(), 1);
        // A Set-Cookie answer is never replayed.
        fetch(manager.data(), server.url("/other=s88"));
        CHECK_EQ(server.paths.size(), 2);
        for (const QByteArray &r : server.requests) CHECK(!r.toLower().contains("\r\ncookie:"));
        // Refused hosts fail at once and never reach a server.
        Fetched refused = fetch(manager.data(), QStringLiteral("http://localhost:%1/x=s88").arg(server.server.serverPort()));
        CHECK(refused.error == QNetworkReply::ContentAccessDenied);
        Fetched outside = fetch(manager.data(), QStringLiteral("https://example.com/a.png"));
        CHECK(outside.error == QNetworkReply::ContentAccessDenied);
        CHECK_EQ(server.paths.size(), 2);
    }
    // An avatar stored 8 days ago is fetched again.
    QTemporaryDir oldDir;
    ImageCacheOptions old(options);
    old.directory = oldDir.path();
    old.clock = [] { return QDateTime::currentDateTimeUtc().addDays(-8); };
    ImageCacheFactory oldFactory(old);
    QScopedPointer<QNetworkAccessManager> manager(oldFactory.create(nullptr));
    manager->setProxy(QNetworkProxy::NoProxy);
    const int before = server.paths.size();
    fetch(manager.data(), server.url("/old=s88"));
    Fetched again = fetch(manager.data(), server.url("/old=s88"));
    CHECK(again.error == QNetworkReply::NoError);
    CHECK(!again.fromCache);
    CHECK_EQ(server.paths.size(), before + 2);
}

// thumbnailLarge: a 404 for a video's hq720.jpg is noted (ThumbnailBook) and kept on disk, so the
// lists name the 480x360 picture for it and the 1280x720 one is asked once per video.
void largeThumbnailMissing() {
    using nordtile::video::catalog::ThumbnailBook;
    ThumbnailBook::instance().clear();
    CHECK_EQ(imagepolicy::largeThumbnailVideoId(QUrl(QStringLiteral("https://i.ytimg.com/vi/aaaaaaaaaaa/hq720.jpg"))), QStringLiteral("aaaaaaaaaaa"));
    CHECK(imagepolicy::largeThumbnailVideoId(QUrl(QStringLiteral("https://i.ytimg.com/vi/aaaaaaaaaaa/mqdefault.jpg"))).isEmpty());
    CHECK(imagepolicy::largeThumbnailVideoId(QUrl(QStringLiteral("https://i.ytimg.com/vi/short/hq720.jpg"))).isEmpty());
    LoopbackServer server;
    server.notFound << QStringLiteral("/vi/bbbbbbbbbbb/hq720.jpg");
    QTemporaryDir dir;
    ImageCacheOptions options;
    options.directory = dir.path();
    options.allowedHosts = QStringList() << QStringLiteral("127.0.0.1");
    options.avatarHosts = QStringList();
    options.httpsOnly = false;
    QString file;
    {
        ImageCacheFactory factory(options);
        file = factory.missingLargeFile();
        QScopedPointer<QNetworkAccessManager> manager(factory.create(nullptr));
        manager->setProxy(QNetworkProxy::NoProxy);
        // A video that has the picture: nothing is noted.
        Fetched ok = fetch(manager.data(), server.url("/vi/aaaaaaaaaaa/hq720.jpg"));
        CHECK(ok.error == QNetworkReply::NoError);
        CHECK(!ThumbnailBook::instance().largeMissing("aaaaaaaaaaa"));
        // A video without it: noted, and written down.
        Fetched missing = fetch(manager.data(), server.url("/vi/bbbbbbbbbbb/hq720.jpg"));
        CHECK(missing.error == QNetworkReply::ContentNotFoundError);
        CHECK(catalog_test::waitFor([] { return ThumbnailBook::instance().largeMissing("bbbbbbbbbbb"); }, 2000));
        QFile saved(file);
        CHECK(saved.open(QIODevice::ReadOnly));
        CHECK_EQ(saved.readAll(), QByteArray("bbbbbbbbbbb\n"));
        // A 404 for another picture is not the card picture's business.
        server.notFound << QStringLiteral("/vi/ccccccccccc/mqdefault.jpg");
        fetch(manager.data(), server.url("/vi/ccccccccccc/mqdefault.jpg"));
        CHECK(!ThumbnailBook::instance().largeMissing("ccccccccccc"));
    }
    // The next start knows it before any list asks.
    ThumbnailBook::instance().clear();
    ImageCacheFactory again(options);
    CHECK(ThumbnailBook::instance().largeMissing("bbbbbbbbbbb"));
    CHECK_EQ(static_cast<int>(ThumbnailBook::instance().size()), 1);
    ThumbnailBook::instance().clear();
}

}  // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    urlPolicy();
    storedEntryPolicy();
    sharedBoundedCache();
    endToEnd();
    largeThumbnailMissing();
    return catalog_test::finish("catalog_images");
}
