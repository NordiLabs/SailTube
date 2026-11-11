// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Artwork loading for QML (DSC-19). Every QNetworkAccessManager the QML engine asks for (one
// per loader thread) shares one persistent, size-bounded QNetworkDiskCache:
//
//  * 64 MB on disk in a private cache directory, kept across runs;
//  * no cookies are sent or stored;
//  * only artwork hosts are fetched: i.ytimg.com, *.ytimg.com, yt3.ggpht.com and
//    *.googleusercontent.com; anything else fails at once with ContentAccessDenied and never
//    reaches the network;
//  * protocol-relative ("//host/...") and http URLs are fetched over https;
//  * avatars (yt3.ggpht.com, *.googleusercontent.com) are requested at 176 px through their
//    "=sNNN" size segment (Images.h rewriteAvatarUrl) and kept fresh for 7 days from the time
//    they were stored, whatever the server's cache headers say; after that they are fetched
//    again (conditionally when the server gave a validator).
//
//  * a 404 for a video's 1280x720 thumbnail (/vi/<id>/hq720.jpg) is noted in
//    catalog::ThumbnailBook, so the lists name the 480x360 one for that video from then on, and
//    kept in "hq720-missing.txt" in the cache directory, so it is asked once per video.
//
// QNetworkDiskCache is not thread-safe and a QNetworkAccessManager owns the cache it is given,
// so each manager gets its own front (a QAbstractNetworkCache) that forwards to the shared
// disk cache under one mutex. The shared state is reference-counted: managers that outlive
// the factory keep working.

#include <QAbstractNetworkCache>
#include <QDateTime>
#include <QNetworkAccessManager>
#include <QNetworkCacheMetaData>
#include <QQmlNetworkAccessManagerFactory>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

namespace nordtile { namespace video { namespace sailfish {

struct ImageCacheOptions {
    ImageCacheOptions();
    QString directory;                  // persistent private cache directory
    qint64 maximumBytes;                // 64 MB
    // Host rules: "host" matches exactly, ".suffix" matches any subdomain of suffix.
    QStringList allowedHosts;           // i.ytimg.com, .ytimg.com, yt3.ggpht.com, .googleusercontent.com
    QStringList avatarHosts;            // yt3.ggpht.com, .googleusercontent.com
    qint64 avatarLifetimeSeconds;       // 7 days
    bool httpsOnly;                     // http and scheme-less URLs are upgraded (tests use plain http)
    std::function<QDateTime()> clock;   // the time an entry is stored; UTC now by default
};

// The URL and cache decisions, exposed for tests.
namespace imagepolicy {
bool hostMatches(const QString &host, const QStringList &rules);
// The URL actually fetched: https (when httpsOnly), avatars at 176 px. An invalid QUrl when
// the host or scheme is not allowed.
QUrl requestUrl(const QUrl &url, const ImageCacheOptions &options);
bool isAvatar(const QUrl &url, const ImageCacheOptions &options);
// What is stored for a response: avatars get saveToDisk, an expiration `lifetime` after
// `storedAt`, a matching Date/Cache-Control/Expires and no Age/Pragma, so the network layer
// treats them as fresh for exactly that long. Other images are stored as the server said.
QNetworkCacheMetaData storedMetaData(const QNetworkCacheMetaData &metaData, const ImageCacheOptions &options,
                                     const QDateTime &storedAt);
// Whether a stored entry is still fresh at `now` by its expiration date (the decision the
// network layer makes for the entries storedMetaData() wrote).
bool isFresh(const QNetworkCacheMetaData &metaData, const QDateTime &now);
// The video id when `url` is a video's 1280x720 thumbnail (/vi/<id>/hq720.jpg), else "".
QString largeThumbnailVideoId(const QUrl &url);
}

class ImageCacheFactory final : public QQmlNetworkAccessManagerFactory {
public:
    // Default options with `directory` (defaultDirectory() in the app).
    explicit ImageCacheFactory(const QString &directory);
    explicit ImageCacheFactory(const ImageCacheOptions &options);
    ~ImageCacheFactory() override;

    // QQmlNetworkAccessManagerFactory: called from the engine's loader threads.
    QNetworkAccessManager *create(QObject *parent) override;

    // The file that remembers videos without a 1280x720 thumbnail (one id per line).
    QString missingLargeFile() const;
    // The shared cache's size on disk (all managers together).
    qint64 cacheSize() const;
    const ImageCacheOptions &options() const;

    // CacheLocation + "/thumbnails": the directory the previous thumbnail cache used, so its
    // entries stay usable (same QNetworkDiskCache format) and nothing is orphaned.
    static QString defaultDirectory();

    struct Shared;
private:
    std::shared_ptr<Shared> shared_;
};

}}}
