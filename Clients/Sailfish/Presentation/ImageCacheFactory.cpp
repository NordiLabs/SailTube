// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ImageCacheFactory.h"

#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QLocale>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkDiskCache>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>

namespace nordtile { namespace video { namespace sailfish {

ImageCacheOptions::ImageCacheOptions()
    : maximumBytes(64 * 1024 * 1024),
      allowedHosts(QStringList() << QStringLiteral("i.ytimg.com") << QStringLiteral(".ytimg.com")
                                 << QStringLiteral("yt3.ggpht.com") << QStringLiteral(".googleusercontent.com")),
      avatarHosts(QStringList() << QStringLiteral("yt3.ggpht.com") << QStringLiteral(".googleusercontent.com")),
      avatarLifetimeSeconds(7 * 24 * 3600),
      httpsOnly(true) {}

namespace imagepolicy {

bool hostMatches(const QString &host, const QStringList &rules) {
    const QString h = host.toLower();
    if (h.isEmpty()) return false;
    for (const QString &rule : rules) {
        if (rule.startsWith(QLatin1Char('.')) ? h.endsWith(rule) : h == rule) return true;
    }
    return false;
}

bool isAvatar(const QUrl &url, const ImageCacheOptions &options) {
    return hostMatches(url.host(), options.avatarHosts);
}

QUrl requestUrl(const QUrl &input, const ImageCacheOptions &options) {
    QUrl url(input);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty()) return QUrl();
    const QString scheme = url.scheme().toLower();
    if (scheme.isEmpty() || (scheme == QLatin1String("http") && options.httpsOnly))
        url.setScheme(options.httpsOnly ? QStringLiteral("https") : QStringLiteral("http"));
    else if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return QUrl();
    if (!hostMatches(url.host(), options.allowedHosts)) return QUrl();
    if (isAvatar(url, options)) {
        const std::string rewritten = catalog::rewriteAvatarUrl(url.toString(QUrl::FullyEncoded).toStdString());
        url = QUrl::fromEncoded(QByteArray::fromStdString(rewritten));
    }
    return url;
}

namespace {
QByteArray httpDate(const QDateTime &t) {
    return QLocale::c().toString(t.toUTC(), QStringLiteral("ddd, dd MMM yyyy hh:mm:ss 'GMT'")).toLatin1();
}
}

QNetworkCacheMetaData storedMetaData(const QNetworkCacheMetaData &metaData, const ImageCacheOptions &options,
                                     const QDateTime &storedAt) {
    if (!metaData.isValid()) return metaData;
    QNetworkCacheMetaData out(metaData);
    const bool avatar = isAvatar(metaData.url(), options);
    QNetworkCacheMetaData::RawHeaderList headers;
    for (const QNetworkCacheMetaData::RawHeader &h : metaData.rawHeaders()) {
        const QByteArray name = h.first.toLower();
        if (name == "set-cookie" || name == "set-cookie2") continue;   // no cookies, not even on disk
        if (avatar && (name == "cache-control" || name == "expires" || name == "date" || name == "age" || name == "pragma"))
            continue;
        headers.append(h);
    }
    if (avatar) {
        // The network layer computes freshness from Date, Age and the expiration date and
        // revalidates on no-cache/must-revalidate, so all of them are restated.
        const QDateTime stored = storedAt.toUTC();
        const QDateTime expires = stored.addSecs(options.avatarLifetimeSeconds);
        headers.append(qMakePair(QByteArray("Date"), httpDate(stored)));
        headers.append(qMakePair(QByteArray("Cache-Control"), QByteArray("max-age=") + QByteArray::number(options.avatarLifetimeSeconds)));
        headers.append(qMakePair(QByteArray("Expires"), httpDate(expires)));
        out.setExpirationDate(expires);
        out.setSaveToDisk(true);
    }
    out.setRawHeaders(headers);
    return out;
}

QString largeThumbnailVideoId(const QUrl &url) {
    static const QRegularExpression re(QStringLiteral("^/vi/([A-Za-z0-9_-]{11})/hq720\\.jpg$"));
    const QRegularExpressionMatch m = re.match(url.path());
    return m.hasMatch() ? m.captured(1) : QString();
}

bool isFresh(const QNetworkCacheMetaData &metaData, const QDateTime &now) {
    return metaData.isValid() && metaData.expirationDate().isValid() && now.toUTC() < metaData.expirationDate().toUTC();
}

}  // namespace imagepolicy

struct ImageCacheFactory::Shared {
    explicit Shared(const ImageCacheOptions &o) : options(o) {
        if (!options.clock) options.clock = [] { return QDateTime::currentDateTimeUtc(); };
        QDir().mkpath(options.directory);
        cache.setCacheDirectory(options.directory);
        cache.setMaximumCacheSize(options.maximumBytes);
        loadMissingLarge();
    }
    QString missingLargeFile() const { return options.directory + QStringLiteral("/hq720-missing.txt"); }
    // Videos whose 1280x720 thumbnail answered 404 in an earlier run: known before any list asks.
    void loadMissingLarge() {
        QFile file(missingLargeFile());
        if (!file.open(QIODevice::ReadOnly)) return;
        QList<QByteArray> ids = file.readAll().split('\n');
        file.close();
        ids.removeAll(QByteArray());
        const int capacity = static_cast<int>(catalog::ThumbnailBook::kCapacity);
        const bool trim = ids.size() > capacity;
        if (trim) ids = ids.mid(ids.size() - capacity);
        for (const QByteArray &id : ids) catalog::ThumbnailBook::instance().markLargeMissing(id.trimmed().toStdString());
        if (trim && file.open(QIODevice::WriteOnly | QIODevice::Truncate)) file.write(ids.join('\n') + '\n');
    }
    // Called from a loader thread when hq720.jpg answered 404.
    void noteLargeMissing(const QString &videoId) {
        if (!catalog::ThumbnailBook::instance().markLargeMissing(videoId.toStdString())) return;
        QMutexLocker lock(&mutex);
        QFile file(missingLargeFile());
        if (file.open(QIODevice::WriteOnly | QIODevice::Append)) {
            file.write(videoId.toLatin1() + '\n');
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        }
    }
    // QNetworkDiskCache expires before it places a new entry, so a store can leave it one entry
    // over its maximum. Lowering the maximum by one byte and restoring it runs its expiry now
    // (down to 90 %), so the directory never stays above the bound. Called with the mutex held.
    void enforceBound() {
        if (cache.cacheSize() <= options.maximumBytes) return;
        cache.setMaximumCacheSize(options.maximumBytes - 1);
        cache.setMaximumCacheSize(options.maximumBytes);
    }
    ImageCacheOptions options;
    QMutex mutex;
    QNetworkDiskCache cache;
};

namespace {

using Shared = ImageCacheFactory::Shared;

// One per manager: serializes access to the shared disk cache and applies the stored-entry
// policy. The manager owns it; the disk cache itself is shared.
class CacheFront final : public QAbstractNetworkCache {
public:
    explicit CacheFront(const std::shared_ptr<Shared> &shared) : shared_(shared) {}
    QNetworkCacheMetaData metaData(const QUrl &url) override {
        QMutexLocker lock(&shared_->mutex);
        return shared_->cache.metaData(url);
    }
    void updateMetaData(const QNetworkCacheMetaData &metaData) override {
        QMutexLocker lock(&shared_->mutex);
        shared_->cache.updateMetaData(imagepolicy::storedMetaData(metaData, shared_->options, shared_->options.clock()));
        shared_->enforceBound();
    }
    QIODevice *data(const QUrl &url) override {
        QMutexLocker lock(&shared_->mutex);
        return shared_->cache.data(url);
    }
    bool remove(const QUrl &url) override {
        QMutexLocker lock(&shared_->mutex);
        return shared_->cache.remove(url);
    }
    qint64 cacheSize() const override {
        QMutexLocker lock(&shared_->mutex);
        return shared_->cache.cacheSize();
    }
    QIODevice *prepare(const QNetworkCacheMetaData &metaData) override {
        QMutexLocker lock(&shared_->mutex);
        return shared_->cache.prepare(imagepolicy::storedMetaData(metaData, shared_->options, shared_->options.clock()));
    }
    void insert(QIODevice *device) override {
        QMutexLocker lock(&shared_->mutex);
        shared_->cache.insert(device);
        shared_->enforceBound();
    }
    void clear() override {
        QMutexLocker lock(&shared_->mutex);
        shared_->cache.clear();
    }
private:
    std::shared_ptr<Shared> shared_;
};

class NoCookieJar final : public QNetworkCookieJar {
public:
    explicit NoCookieJar(QObject *parent) : QNetworkCookieJar(parent) {}
    QList<QNetworkCookie> cookiesForUrl(const QUrl &) const override { return QList<QNetworkCookie>(); }
    bool setCookiesFromUrl(const QList<QNetworkCookie> &, const QUrl &) override { return false; }
    bool insertCookie(const QNetworkCookie &) override { return false; }
    bool updateCookie(const QNetworkCookie &) override { return false; }
};

// A request the policy refuses: fails asynchronously without touching the network.
class RefusedReply final : public QNetworkReply {
public:
    RefusedReply(const QNetworkRequest &request, QNetworkAccessManager::Operation operation, QObject *parent)
        : QNetworkReply(parent) {
        setRequest(request);
        setUrl(request.url());
        setOperation(operation);
        open(QIODevice::ReadOnly | QIODevice::Unbuffered);
        setError(QNetworkReply::ContentAccessDenied, QStringLiteral("image host not allowed"));
        setFinished(true);
        QTimer::singleShot(0, this, [this] {
            emit error(QNetworkReply::ContentAccessDenied);
            emit finished();
        });
    }
    void abort() override {}
    qint64 bytesAvailable() const override { return 0; }
protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class ImageNetworkManager final : public QNetworkAccessManager {
public:
    ImageNetworkManager(const std::shared_ptr<Shared> &shared, QObject *parent)
        : QNetworkAccessManager(parent), shared_(shared) {
        setCookieJar(new NoCookieJar(this));
        setCache(new CacheFront(shared));
    }
protected:
    QNetworkReply *createRequest(Operation operation, const QNetworkRequest &request, QIODevice *outgoing) override {
        const QUrl url = imagepolicy::requestUrl(request.url(), shared_->options);
        if (!url.isValid() || (operation != GetOperation && operation != HeadOperation))
            return new RefusedReply(request, operation, this);
        QNetworkRequest r(request);
        r.setUrl(url);
        r.setRawHeader("Cookie", QByteArray());
        r.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        r.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
        QNetworkReply *reply = QNetworkAccessManager::createRequest(operation, r, outgoing);
        // A 404 for a video's 1280x720 thumbnail: the lists switch that video to the 480x360 one.
        const QString videoId = imagepolicy::largeThumbnailVideoId(url);
        if (!videoId.isEmpty()) {
            const std::shared_ptr<Shared> shared = shared_;
            QObject::connect(reply, &QNetworkReply::finished, reply, [reply, videoId, shared] {
                if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 404) shared->noteLargeMissing(videoId);
            });
        }
        return reply;
    }
private:
    std::shared_ptr<Shared> shared_;
};

}  // namespace

ImageCacheFactory::ImageCacheFactory(const QString &directory) {
    ImageCacheOptions options;
    options.directory = directory;
    shared_ = std::make_shared<Shared>(options);
}

ImageCacheFactory::ImageCacheFactory(const ImageCacheOptions &options) : shared_(std::make_shared<Shared>(options)) {}

ImageCacheFactory::~ImageCacheFactory() = default;

QNetworkAccessManager *ImageCacheFactory::create(QObject *parent) {
    return new ImageNetworkManager(shared_, parent);
}

QString ImageCacheFactory::missingLargeFile() const { return shared_->missingLargeFile(); }

qint64 ImageCacheFactory::cacheSize() const {
    QMutexLocker lock(&shared_->mutex);
    return shared_->cache.cacheSize();
}

const ImageCacheOptions &ImageCacheFactory::options() const { return shared_->options; }

QString ImageCacheFactory::defaultDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + QStringLiteral("/thumbnails");
}

}}}
