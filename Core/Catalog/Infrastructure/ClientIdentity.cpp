// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ClientIdentity.h"
#include "InnerTubeDefinitions.h"
#include "HttpTransport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>

namespace nordtile { namespace video { namespace catalog {

DiscoveryLocale DiscoveryLocale::fromLocale(const QLocale &locale) {
    DiscoveryLocale out;
    if (locale.language() == QLocale::C) return out;
    const QStringList parts = locale.name().split(QLatin1Char('_'));
    const QString lang = parts.value(0).toLower();
    const QString country = parts.value(1).toUpper();
    if (lang.isEmpty()) return out;
    out.hl = lang;
    if (country.size() == 2) out.gl = country;
    if (lang == QLatin1String("zh")) {
        out.hl = (country == QLatin1String("TW") || country == QLatin1String("HK")) ? QStringLiteral("zh-") + country
                                                                                     : QStringLiteral("zh-CN");
    } else if (lang == QLatin1String("pt") && country == QLatin1String("PT")) {
        out.hl = QStringLiteral("pt-PT");
    } else if (lang == QLatin1String("en") && (country == QLatin1String("GB") || country == QLatin1String("IN"))) {
        out.hl = QStringLiteral("en-") + country;
    } else if (lang == QLatin1String("es") && !country.isEmpty() && country != QLatin1String("ES") && country != QLatin1String("US")) {
        out.hl = QStringLiteral("es-419");
    } else if (lang == QLatin1String("es") && country == QLatin1String("US")) {
        out.hl = QStringLiteral("es-US");
    } else if (lang == QLatin1String("fr") && country == QLatin1String("CA")) {
        out.hl = QStringLiteral("fr-CA");
    }
    return out;
}

const char *ClientIdentity::builtInVersion() { return innertube::kWebClientVersion; }

ClientIdentity::ClientIdentity(HttpTransport *transport, const QString &cacheFile, QObject *parent, std::function<qint64()> nowMs)
    : QObject(parent), transport_(transport), cacheFile_(cacheFile), nowMs_(std::move(nowMs)),
      version_(QString::fromLatin1(builtInVersion())), source_(QStringLiteral("built-in")) {}

qint64 ClientIdentity::now() const { return nowMs_ ? nowMs_() : QDateTime::currentMSecsSinceEpoch(); }

bool ClientIdentity::plausibleVersion(const QString &version) {
    static const QRegularExpression re(QStringLiteral("^\\d{1,3}\\.(19|20)\\d{2}(0[1-9]|1[0-2])(0[1-9]|[12]\\d|3[01])(\\.\\d{1,4}){0,3}$"));
    return re.match(version).hasMatch();
}

QString ClientIdentity::extractVersion(const QByteArray &html) {
    static const QRegularExpression re(QStringLiteral("\"") + QLatin1String(innertube::kClientVersionKey) + QStringLiteral("\"\\s*:\\s*\"([^\"]+)\""));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(html));
    return m.hasMatch() ? m.captured(1) : QString();
}

void ClientIdentity::start() {
    if (!cacheFile_.isEmpty()) {
        QFile f(cacheFile_);
        if (f.open(QIODevice::ReadOnly)) {
            const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
            const QString v = o.value(QStringLiteral("clientVersion")).toString();
            const qint64 at = static_cast<qint64>(o.value(QStringLiteral("fetchedAtMs")).toDouble());
            if (plausibleVersion(v) && at > 0 && source_ != QLatin1String("extractor")) {
                version_ = v; source_ = QStringLiteral("cache"); fetchedAtMs_ = at;
            }
        }
    }
    refreshIfStale();
}

void ClientIdentity::refreshIfStale() {
    if (source_ == QLatin1String("extractor")) return;
    if (fetchedAtMs_ != 0 && now() - fetchedAtMs_ < kCacheMs) return;
    if (lastScrapeMs_ >= 0 && now() - lastScrapeMs_ < kRescrapeMinIntervalMs) return;
    scrape();
}

void ClientIdentity::reportHttp400() {
    if (lastScrapeMs_ >= 0 && now() - lastScrapeMs_ < kRescrapeMinIntervalMs) return;
    scrape();
}

void ClientIdentity::setExtractorVersion(const QString &version) {
    if (!plausibleVersion(version)) return;
    commit(version, QStringLiteral("extractor"), false);
}

void ClientIdentity::scrape() {
    if (scrapeId_ || !transport_) return;
    lastScrapeMs_ = now();
    ++scrapes_;
    HttpRequest req;
    req.url = QUrl(QStringLiteral("https://www.youtube.com/"));
    req.headers.append(qMakePair(QByteArray("Cookie"), QByteArray("SOCS=CAI")));
    req.headers.append(qMakePair(QByteArray("Accept-Language"), QByteArray("en")));
    req.timeoutMs = 10000;
    scrapeId_ = transport_->send(req, this, [this](const HttpReply &reply) {
        scrapeId_ = 0;
        if (reply.status != 200) return;
        const QString v = extractVersion(reply.body);
        if (plausibleVersion(v)) commit(v, QStringLiteral("scrape"), true);
    });
}

void ClientIdentity::commit(const QString &version, const QString &source, bool persist) {
    const bool changed = version != version_;
    version_ = version;
    source_ = source;
    fetchedAtMs_ = now();
    if (persist && !cacheFile_.isEmpty()) {
        QDir().mkpath(QFileInfo(cacheFile_).absolutePath());
        QSaveFile f(cacheFile_);
        if (f.open(QIODevice::WriteOnly)) {
            QJsonObject o;
            o.insert(QStringLiteral("clientVersion"), version_);
            o.insert(QStringLiteral("fetchedAtMs"), static_cast<double>(fetchedAtMs_));
            f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
            f.commit();
        }
    }
    if (changed) emit versionChanged(version_);
}

}}}
