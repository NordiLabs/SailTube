// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The anonymous WEB client identity shared by every discovery call (DSC-02) and the request
// locale (I18N-01).
//
// * The client version comes from the extractor table when core supplies it
//   (setExtractorVersion), else from the homepage's INNERTUBE_CLIENT_VERSION scraped with the
//   SOCS=CAI consent cookie, else from the built-in value.
// * A scraped value is cached on disk for 12 h and committed only when it looks like
//   N.YYYYMMDD... .
// * An HTTP 400 schedules a background re-scrape, at most once every 10 min. Nothing ever waits
//   for a scrape: requests always use the current value.

#include <QLocale>
#include <QObject>
#include <QString>
#include <functional>

namespace nordtile { namespace video { namespace catalog {

class HttpTransport;

struct DiscoveryLocale {
    QString hl = QStringLiteral("en");
    QString gl = QStringLiteral("US");
    // Device locale -> YouTube's hl/gl ("de_DE" -> de/DE, "pt_PT" -> pt-PT/PT, "zh_CN" ->
    // zh-CN/CN, "es_MX" -> es-419/MX, "C" -> en/US).
    static DiscoveryLocale fromLocale(const QLocale &locale);
    static DiscoveryLocale fromSystem() { return fromLocale(QLocale::system()); }
};

class ClientIdentity final : public QObject {
    Q_OBJECT
public:
    static const char *builtInVersion();
    static const qint64 kCacheMs = 12LL * 3600 * 1000;
    static const qint64 kRescrapeMinIntervalMs = 10LL * 60 * 1000;

    // `cacheFile` may be empty (no persistence). `nowMs` is injectable for tests.
    ClientIdentity(HttpTransport *transport, const QString &cacheFile, QObject *parent = nullptr,
                   std::function<qint64()> nowMs = std::function<qint64()>());

    QString clientVersion() const { return version_; }
    QString source() const { return source_; }   // "built-in", "cache", "scrape", "extractor"
    bool scrapeInFlight() const { return scrapeId_ != 0; }
    int scrapeCount() const { return scrapes_; }

    // Loads the disk cache; starts a background scrape when it is missing or older than 12 h.
    void start();
    // Called before each discovery call: scrapes in the background once the value is older
    // than 12 h (rate limited like reportHttp400). Never blocks.
    void refreshIfStale();
    // A discovery call answered HTTP 400: re-scrape in the background (rate limited).
    void reportHttp400();
    // Core may feed the extractor's current WEB version (DSC-02 first source).
    void setExtractorVersion(const QString &version);

    static bool plausibleVersion(const QString &version);
    static QString extractVersion(const QByteArray &html);

signals:
    void versionChanged(const QString &version);

private:
    void scrape();
    void commit(const QString &version, const QString &source, bool persist);
    qint64 now() const;
    HttpTransport *transport_;
    QString cacheFile_;
    std::function<qint64()> nowMs_;
    QString version_;
    QString source_;
    qint64 fetchedAtMs_ = 0;
    qint64 lastScrapeMs_ = -1;
    quint64 scrapeId_ = 0;
    int scrapes_ = 0;
};

}}}
