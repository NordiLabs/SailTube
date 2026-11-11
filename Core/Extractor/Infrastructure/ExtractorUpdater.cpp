// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ExtractorUpdater.h"

#include "Core/Process/Infrastructure/HelperProcess.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTimer>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace nordtile { namespace video { namespace extractor {

namespace {
const qint64 kMaxManifestBytes = 16 * 1024;
const qint64 kMaxVerifierOutput = 64 * 1024;

std::string s(const QString &q) { return q.toStdString(); }
QString q(const std::string &s) { return QString::fromStdString(s); }

bool exists(const QString &path) { return QFileInfo(path).exists() || QFileInfo(path).isSymLink(); }

// The verifier's fixed label, or "verifier" when it is not one (nothing else ever reaches the UI).
QString cleanLabel(const QString &label) {
    if (label.isEmpty() || label.size() > 40) return QStringLiteral("verifier");
    for (const QChar c : label)
        if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || (c >= QLatin1Char('0') && c <= QLatin1Char('9')) || c == QLatin1Char('-')))
            return QStringLiteral("verifier");
    return label;
}

bool syncDirectory(const QString &dir) {
    const int fd = ::open(QFile::encodeName(dir).constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    const bool ok = ::fsync(fd) == 0;
    ::close(fd);
    return ok;
}

// A private directory: created 0700 (umask cannot widen it), or an existing directory narrowed to 0700.
bool makePrivateDirectory(const QString &dir) {
    const QByteArray path = QFile::encodeName(dir);
    if (::mkdir(path.constData(), S_IRWXU) != 0 && errno != EEXIST) return false;
    struct stat st;
    if (::lstat(path.constData(), &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != ::getuid()) return false;
    if ((st.st_mode & 0777) != S_IRWXU && ::chmod(path.constData(), S_IRWXU) != 0) return false;
    return true;
}

QString sha256File(const QString &path, qint64 *size) {
    QFile f(path);
    *size = -1;
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (!f.atEnd()) {
        const QByteArray block = f.read(1 << 16);
        if (block.isEmpty()) return QString();
        total += block.size();
        if (total > kMaxArchiveBytes) return QString();
        h.addData(block);
    }
    *size = total;
    return QString::fromLatin1(h.result().toHex());
}

bool readManifest(const QString &path, Manifest *m) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() > kMaxManifestBytes) return false;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(f.read(kMaxManifestBytes), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return false;
    const QJsonObject o = doc.object();
    m->version = s(o.value(QStringLiteral("version")).toString());
    m->sha256 = s(o.value(QStringLiteral("sha256")).toString());
    m->source = s(o.value(QStringLiteral("source")).toString());
    m->channel = s(o.value(QStringLiteral("channel")).toString());
    m->size = static_cast<long long>(o.value(QStringLiteral("size")).toDouble(-1));
    m->installedAtMs = static_cast<long long>(o.value(QStringLiteral("installedAtMs")).toDouble(-1));
    return true;
}

InstalledCopy inspect(const Layout &layout, const std::string &dir) {
    InstalledCopy copy;
    copy.manifestReadable = readManifest(q(layout.manifest(dir)), &copy.manifest);
    const QString archive = q(layout.archive(dir));
    copy.archivePresent = QFileInfo(archive).isFile() && !QFileInfo(archive).isSymLink();
    if (copy.manifestReadable && manifestValid(copy.manifest) && copy.archivePresent) {
        qint64 size = -1;
        copy.archiveSha256 = s(sha256File(archive, &size));
        copy.archiveSize = size;
    }
    return copy;
}
}

// ---------------------------------------------------------------------------------------------- fetch

struct ExtractorUpdater::Fetch {
    Fetch(Channel channel, const std::string &file, const std::string &expectedTag)
        : chain(channel, file, expectedTag), hash(QCryptographicHash::Sha256) {}
    RedirectChain chain;
    QByteArray method;
    qint64 maxBytes = 0;
    std::unique_ptr<QFile> sink;   // the staged archive; null: the body is kept in memory
    QByteArray body;
    QCryptographicHash hash;
    qint64 received = 0;
    qint64 contentLength = -1;
    int status = 0;
    QString logicalUrl;
    QString error;
    bool certificateError = false;
    QPointer<QNetworkReply> reply;
    QTimer *timer = nullptr;
    FetchDone done;
    quint64 generation = 0;
};

ExtractorUpdater::ExtractorUpdater(const UpdaterOptions &options, QObject *parent)
    : QObject(parent), options_(options), layout_(layoutFor(s(options.root))), network_(new QNetworkAccessManager(this)) {
    if (options_.maxArchiveBytes <= 0 || options_.maxArchiveBytes > kMaxArchiveBytes) options_.maxArchiveBytes = kMaxArchiveBytes;
    if (!options_.root.isEmpty() && QFileInfo(options_.root).isDir()) recover();
    reload();
}

ExtractorUpdater::~ExtractorUpdater() {
    ++generation_;
    if (cancelVerify_) { std::function<void()> c = cancelVerify_; cancelVerify_ = nullptr; c(); }
    if (fetch_ && fetch_->reply) { QNetworkReply *r = fetch_->reply; fetch_->reply = nullptr; r->abort(); r->deleteLater(); }
    fetch_.reset();
    // A half-written staging/ is left for the next start's recovery (it is never promoted).
}

qint64 ExtractorUpdater::now() const {
    return options_.clockMs ? options_.clockMs() : QDateTime::currentMSecsSinceEpoch();
}

bool ExtractorUpdater::busy() const {
    return state_ == QLatin1String("checking") || state_ == QLatin1String("downloading")
        || state_ == QLatin1String("verifying") || state_ == QLatin1String("installing");
}

QString ExtractorUpdater::offeredVersion() const { return offer_ ? q(offer_->version) : QString(); }
qint64 ExtractorUpdater::offeredSize() const { return offer_ ? offer_->size : -1; }
QString ExtractorUpdater::offeredSource() const { return offer_ ? q(offer_->source) : QString(); }
QString ExtractorUpdater::activeVersion() const { return activeIsBundled_ ? options_.bundledVersion : installedVersion_; }

QString ExtractorUpdater::bundledVersionFromManifest(const QString &sourceManifestPath) {
    QFile f(sourceManifestPath);
    if (!f.open(QIODevice::ReadOnly) || f.size() > 256 * 1024) return QString();
    const QJsonObject inputs = QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("inputs")).toObject();
    const QString version = inputs.value(QStringLiteral("yt_dlp_version")).toString();
    return validVersion(s(version)) ? version : QString();
}

QString ExtractorUpdater::activeArchive(const QString &bundledPath) const {
    return activeIsBundled_ ? bundledPath : q(layout_.archive(layout_.current));
}

void ExtractorUpdater::setState(const QString &state) {
    state_ = state;
    emit changed();
}

void ExtractorUpdater::fail(const QString &error) {
    ++generation_;
    if (cancelVerify_) { std::function<void()> c = cancelVerify_; cancelVerify_ = nullptr; c(); }
    if (fetch_) {
        std::shared_ptr<Fetch> f = fetch_;
        fetch_.reset();
        if (f->timer) { f->timer->stop(); f->timer->deleteLater(); f->timer = nullptr; }
        if (f->reply) { QNetworkReply *r = f->reply; f->reply = nullptr; r->abort(); r->deleteLater(); }
        f->sink.reset();
    }
    if (installing_) {
        installing_.reset();
        runStep(Step::ClearStaging);   // current was never touched
    }
    lastError_ = error;
    setState(QStringLiteral("failed"));
    emit failed(error);
}

std::shared_ptr<ExtractorUpdater::Fetch> ExtractorUpdater::newFetch(Channel channel, const QString &file, const QString &expectedTag,
                                                                    const QByteArray &method, qint64 maxBytes, FetchDone done) {
    std::shared_ptr<Fetch> f(new Fetch(channel, s(file), s(expectedTag)));
    f->method = method;
    f->maxBytes = maxBytes;
    f->done = std::move(done);
    f->generation = generation_;
    return f;
}

void ExtractorUpdater::startFetch(const std::shared_ptr<Fetch> &f, const QString &logicalUrl) {
    const Refusal refusal = f->chain.next(s(logicalUrl));
    if (refusal != Refusal::None) {
        // Asynchronous like every other outcome, so callers see one shape.
        const quint64 generation = f->generation;
        const QString label = QString::fromLatin1(refusalLabel(refusal));
        QTimer::singleShot(0, this, [this, generation, label]() { if (generation == generation_) fail(label); });
        return;
    }
    fetch_ = f;
    f->logicalUrl = logicalUrl;
    f->status = 0;
    f->contentLength = -1;
    f->received = 0;
    f->body.clear();
    f->hash.reset();
    f->certificateError = false;
    if (f->sink) { f->sink->seek(0); f->sink->resize(0); }

    QUrl physical(logicalUrl, QUrl::StrictMode);
    if (options_.endpointOverride) {
        QString host;
        quint16 port = 0;
        if (options_.endpointOverride(physical.host(), &host, &port)) { physical.setHost(host); physical.setPort(port); }
    }
    QNetworkRequest request(physical);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, false);   // every hop is checked here
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setAttribute(QNetworkRequest::CacheSaveControlAttribute, false);
    request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
    request.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    request.setRawHeader("User-Agent", "SailTube-extractor-update");
    request.setRawHeader("Accept-Encoding", "identity");
    QSslConfiguration tls = QSslConfiguration::defaultConfiguration();
    tls.setPeerVerifyMode(QSslSocket::VerifyPeer);
    tls.setProtocol(QSsl::TlsV1_2OrLater);
    if (!options_.extraCaCertificatesForTesting.isEmpty())
        tls.setCaCertificates(tls.caCertificates() + options_.extraCaCertificatesForTesting);
    request.setSslConfiguration(tls);

    QNetworkReply *reply = f->method == "HEAD" ? network_->head(request) : network_->get(request);
    f->reply = reply;
    if (!f->timer) {
        f->timer = new QTimer(this);
        f->timer->setSingleShot(true);
        std::weak_ptr<Fetch> weak(f);
        connect(f->timer, &QTimer::timeout, this, [weak]() {
            std::shared_ptr<Fetch> fetch = weak.lock();
            if (!fetch || !fetch->reply) return;
            if (fetch->error.isEmpty()) fetch->error = QStringLiteral("timeout");
            fetch->reply->abort();
        });
    }
    f->timer->start(options_.requestTimeoutMs);
    std::weak_ptr<Fetch> weak(f);
    connect(reply, &QNetworkReply::sslErrors, this, [weak](const QList<QSslError> &) {
        // Never ignored: the handshake fails and the reply finishes with an error.
        if (std::shared_ptr<Fetch> fetch = weak.lock()) fetch->certificateError = true;
    });
    connect(reply, &QNetworkReply::metaDataChanged, this, [weak, reply]() {
        std::shared_ptr<Fetch> fetch = weak.lock();
        if (!fetch || fetch->reply != reply) return;
        fetch->status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        bool known = false;
        const qint64 length = reply->header(QNetworkRequest::ContentLengthHeader).toLongLong(&known);
        fetch->contentLength = known ? length : -1;
        if (fetch->status == 200 && known && length > fetch->maxBytes && fetch->method != "HEAD") {
            fetch->error = QStringLiteral("too-large");
            reply->abort();
        }
    });
    connect(reply, &QIODevice::readyRead, this, [this, weak, reply]() {
        std::shared_ptr<Fetch> fetch = weak.lock();
        if (!fetch || fetch->reply != reply) return;
        const QByteArray data = reply->read(fetch->maxBytes + 1 - fetch->received);
        if (fetch->status < 200 || fetch->status >= 300) { reply->readAll(); return; }   // redirect bodies are dropped
        fetch->received += data.size();
        if (fetch->received > fetch->maxBytes) {
            fetch->error = QStringLiteral("too-large");
            reply->abort();
            return;
        }
        if (fetch->sink) {
            if (fetch->sink->write(data) != data.size()) { fetch->error = QStringLiteral("storage"); reply->abort(); return; }
            fetch->hash.addData(data);
            bytesReceived_ = fetch->received;
            emit progress();
        } else {
            fetch->body.append(data);
        }
        if (fetch->timer) fetch->timer->start(options_.requestTimeoutMs);
    });
    connect(reply, &QNetworkReply::finished, this, [this, weak, reply]() {
        std::shared_ptr<Fetch> fetch = weak.lock();
        if (!fetch || fetch->reply != reply) { reply->deleteLater(); return; }
        finishFetch(fetch);
    });
}

void ExtractorUpdater::finishFetch(const std::shared_ptr<Fetch> &f) {
    QNetworkReply *reply = f->reply;
    f->reply = nullptr;
    reply->deleteLater();
    if (f->timer) f->timer->stop();
    if (f->generation != generation_) return;
    if (f->error.isEmpty()) {
        const QNetworkReply::NetworkError error = reply->error();
        f->status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (f->certificateError || error == QNetworkReply::SslHandshakeFailedError) {
            f->error = QStringLiteral("certificate");
        } else if (f->status == 301 || f->status == 302 || f->status == 303 || f->status == 307 || f->status == 308) {
            const std::string location = resolveLocation(s(f->logicalUrl), reply->rawHeader("Location").toStdString());
            if (location.empty()) { f->error = QStringLiteral("insecure-url"); }
            else {
                if (f->status == 303 && f->method != "HEAD") f->method = "GET";
                startFetch(f, q(location));   // re-checked by the chain before any connection
                return;
            }
        } else if (f->status == 200 && error == QNetworkReply::NoError) {
            if (!f->chain.finalOk()) f->error = QStringLiteral("bad-release-path");
            else if (f->sink && !f->sink->flush()) f->error = QStringLiteral("storage");
        } else if (f->status >= 400) {
            f->error = QStringLiteral("http-status");
        } else {
            f->error = QStringLiteral("network");
        }
    }
    if (f->timer) { f->timer->deleteLater(); f->timer = nullptr; }
    if (fetch_ == f) fetch_.reset();
    FetchDone done = f->done;
    done(*f);
}

// ---------------------------------------------------------------------------------------------- check

bool ExtractorUpdater::check(const QString &channel) {
    if (busy()) { lastError_ = QStringLiteral("busy"); emit changed(); return false; }
    Channel c;
    if (!parseChannel(s(channel), &c)) { fail(QStringLiteral("channel")); return false; }
    ++generation_;
    offer_.reset();
    lastError_.clear();
    channel_ = channel;
    checkChannel_ = c;
    pendingTag_.clear();
    pendingSize_ = -1;
    pendingSums_.clear();
    setState(QStringLiteral("checking"));
    startFetch(newFetch(c, QString::fromLatin1(kAssetName), QString(), "HEAD", options_.maxArchiveBytes,
                        [this](Fetch &f) { onArchiveHead(f); }),
               q(latestUrl(c, kAssetName)));
    return true;
}

void ExtractorUpdater::onArchiveHead(Fetch &f) {
    if (!f.error.isEmpty()) { fail(f.error); return; }
    if (f.contentLength < kMinArchiveBytes) { fail(QStringLiteral("size-mismatch")); return; }
    if (f.contentLength > options_.maxArchiveBytes) { fail(QStringLiteral("too-large")); return; }
    pendingTag_ = q(f.chain.tag());
    pendingSize_ = f.contentLength;
    startFetch(newFetch(checkChannel_, QString::fromLatin1(kSumsName), pendingTag_, "GET", kMaxSumsBytes,
                        [this](Fetch &g) { onSums(g); }),
               q(taggedUrl(checkChannel_, s(pendingTag_), kSumsName)));
}

void ExtractorUpdater::onSums(Fetch &f) {
    if (f.error == QLatin1String("http-status")) { fail(QStringLiteral("sums")); return; }
    if (!f.error.isEmpty()) { fail(f.error); return; }
    if (sumsEntry(f.body.toStdString(), kAssetName).empty()) { fail(QStringLiteral("sums")); return; }
    pendingSums_ = f.body;
    startFetch(newFetch(checkChannel_, QString::fromLatin1(kSignatureName), pendingTag_, "GET", kMaxSignatureBytes,
                        [this](Fetch &g) { onSignature(g); }),
               q(taggedUrl(checkChannel_, s(pendingTag_), kSignatureName)));
}

void ExtractorUpdater::onSignature(Fetch &f) {
    if (f.error == QLatin1String("http-status") || (f.error.isEmpty() && f.body.isEmpty())) {
        fail(QStringLiteral("signature-missing"));
        return;
    }
    if (!f.error.isEmpty()) { fail(f.error); return; }
    std::unique_ptr<Offer> offer(new Offer);
    offer->version = s(pendingTag_);
    offer->size = pendingSize_;
    offer->source = taggedUrl(checkChannel_, offer->version, kAssetName);
    offer->channel = channelName(checkChannel_);
    offer->sums = pendingSums_.toStdString();
    offer->signature = f.body.toStdString();
    offer->sumsSha256 = sumsEntry(offer->sums, kAssetName);
    offer->offeredAtMs = now();
    if (!worthOffering(offer->version, s(activeVersion()))) {
        setState(QStringLiteral("upToDate"));
        return;
    }
    offer_ = std::move(offer);
    setState(QStringLiteral("offered"));
    emit offered(q(offer_->version), offer_->size, q(offer_->source));
}

// ---------------------------------------------------------------------------------------------- install

bool ExtractorUpdater::install(const QString &version, qint64 size, const QString &source) {
    if (busy()) { lastError_ = QStringLiteral("busy"); emit changed(); return false; }
    switch (checkToken(offer_.get(), s(version), size, s(source), now())) {
    case TokenVerdict::Ok: break;
    case TokenVerdict::NoOffer: fail(QStringLiteral("no-offer")); return false;
    case TokenVerdict::Mismatch: fail(QStringLiteral("token-mismatch")); return false;
    case TokenVerdict::Expired: offer_.reset(); fail(QStringLiteral("offer-expired")); return false;
    }
    ++generation_;
    installing_ = std::move(offer_);
    lastError_.clear();
    bytesReceived_ = 0;
    Channel c = Channel::Stable;
    parseChannel(installing_->channel, &c);
    if (!prepareStaging()) { fail(QStringLiteral("storage")); return false; }
    const QString archive = q(layout_.archive(layout_.staging));
    const int fd = ::open(QFile::encodeName(archive).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    std::unique_ptr<QFile> sink(new QFile);
    if (fd < 0 || !sink->open(fd, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        if (fd >= 0) ::close(fd);
        fail(QStringLiteral("storage"));
        return false;
    }
    std::shared_ptr<Fetch> f = newFetch(c, QString::fromLatin1(kAssetName), q(installing_->version), "GET", installing_->size,
                                        [this](Fetch &g) { onArchiveBody(g); });
    f->sink = std::move(sink);
    setState(QStringLiteral("downloading"));
    startFetch(f, q(installing_->source));
    return true;
}

bool ExtractorUpdater::prepareStaging() {
    if (options_.root.isEmpty()) return false;
    const QFileInfo parent(QFileInfo(options_.root).absolutePath());
    if (!parent.isDir() && !QDir().mkpath(parent.absoluteFilePath())) return false;
    if (!makePrivateDirectory(options_.root)) return false;
    if (!runStep(Step::ClearStaging)) return false;
    return makePrivateDirectory(q(layout_.staging));
}

bool ExtractorUpdater::writePrivate(const QString &path, const QByteArray &data) {
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (fd < 0) return false;
    const char *p = data.constData();
    qint64 left = data.size();
    while (left > 0) {
        const ssize_t n = ::write(fd, p, static_cast<size_t>(left));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ::close(fd); return false; }
        p += n;
        left -= n;
    }
    const bool ok = ::fsync(fd) == 0;
    return ::close(fd) == 0 && ok;
}

void ExtractorUpdater::onArchiveBody(Fetch &f) {
    if (!f.error.isEmpty()) { fail(f.error); return; }
    if (f.received != installing_->size) { fail(QStringLiteral("size-mismatch")); return; }
    stagedSha256_ = QString::fromLatin1(f.hash.result().toHex());
    const bool synced = f.sink && f.sink->flush() && ::fsync(f.sink->handle()) == 0;
    f.sink.reset();
    if (!synced) { fail(QStringLiteral("storage")); return; }
    if (s(stagedSha256_) != installing_->sumsSha256) { fail(QStringLiteral("checksum")); return; }
    const QString sums = q(layout_.sums(layout_.staging));
    const QString signature = q(layout_.signature(layout_.staging));
    if (!writePrivate(sums, QByteArray::fromStdString(installing_->sums))
        || !writePrivate(signature, QByteArray::fromStdString(installing_->signature))) {
        fail(QStringLiteral("storage"));
        return;
    }
    if (!options_.verifier) { fail(QStringLiteral("verifier")); return; }
    setState(QStringLiteral("verifying"));
    VerifyRequest request;
    request.sums = sums;
    request.signature = signature;
    request.archive = q(layout_.archive(layout_.staging));
    request.asset = QString::fromLatin1(kAssetName);
    request.expectVersion = q(installing_->version);
    const quint64 generation = generation_;
    std::function<void()> cancel = options_.verifier(request, [this, generation](const VerifyResult &result) {
        if (generation != generation_) return;
        cancelVerify_ = nullptr;
        onVerified(result);
    });
    if (generation == generation_ && state_ == QLatin1String("verifying")) cancelVerify_ = cancel;
}

void ExtractorUpdater::onVerified(const VerifyResult &r) {
    if (!r.ok) { fail(cleanLabel(r.error)); return; }
    if (s(r.version) != installing_->version) { fail(QStringLiteral("version-mismatch")); return; }
    if (r.sha256 != stagedSha256_ || r.size != installing_->size) { fail(QStringLiteral("checksum")); return; }
    QJsonObject manifest;
    manifest.insert(QStringLiteral("version"), q(installing_->version));
    manifest.insert(QStringLiteral("sha256"), stagedSha256_);
    manifest.insert(QStringLiteral("size"), static_cast<double>(installing_->size));
    manifest.insert(QStringLiteral("source"), q(installing_->source));
    manifest.insert(QStringLiteral("channel"), q(installing_->channel));
    manifest.insert(QStringLiteral("installedAtMs"), static_cast<double>(now()));
    if (!writePrivate(q(layout_.manifest(layout_.staging)), QJsonDocument(manifest).toJson(QJsonDocument::Compact))
        || !syncDirectory(q(layout_.staging))) {
        fail(QStringLiteral("storage"));
        return;
    }
    setState(QStringLiteral("installing"));
    const std::string version = installing_->version;
    // Up to here current/ was not touched. From here on each step is one rename(2); a crash between two
    // of them is undone by recover() at the next start.
    if (!runPlan(installPlan(exists(q(layout_.current)), exists(q(layout_.previous))))) {
        recover();
        reload();
        fail(QStringLiteral("storage"));
        return;
    }
    installing_.reset();
    reload();
    if (installedVersion_ != q(version)) { fail(QStringLiteral("storage")); return; }
    restartNeeded_ = true;
    setState(QStringLiteral("installed"));
    emit installed(q(version));
    emit activeArchiveChanged();
}

void ExtractorUpdater::cancel() {
    if (busy()) fail(QStringLiteral("cancelled"));
}

// ---------------------------------------------------------------------------------------------- rollback

bool ExtractorUpdater::rollback() {
    if (busy()) { lastError_ = QStringLiteral("busy"); emit changed(); return false; }
    if (previousVersion_.isEmpty()) { fail(QStringLiteral("no-previous")); return false; }
    if (!runPlan(rollbackPlan(exists(q(layout_.current)), true))) {
        recover();
        reload();
        fail(QStringLiteral("storage"));
        return false;
    }
    reload();
    offer_.reset();
    lastError_.clear();
    restartNeeded_ = true;
    setState(QStringLiteral("rolledBack"));
    emit activeArchiveChanged();
    return true;
}

bool ExtractorUpdater::revertToBundled() {
    if (busy()) { lastError_ = QStringLiteral("busy"); emit changed(); return false; }
    if (!exists(q(layout_.current))) { fail(QStringLiteral("no-update")); return false; }
    if (!runPlan(revertPlan(true))) { recover(); reload(); fail(QStringLiteral("storage")); return false; }
    reload();
    offer_.reset();
    lastError_.clear();
    restartNeeded_ = true;
    setState(QStringLiteral("reverted"));
    emit activeArchiveChanged();
    return true;
}

// ---------------------------------------------------------------------------------------------- store

void ExtractorUpdater::recover() {
    runPlan(recoveryPlan(exists(q(layout_.current)), exists(q(layout_.previous)), exists(q(layout_.swap))));
}

void ExtractorUpdater::reload() {
    if (options_.root.isEmpty()) {
        activeIsBundled_ = true;
        installedVersion_.clear();
        previousVersion_.clear();
        emit changed();
        return;
    }
    const InstalledCopy current = inspect(layout_, layout_.current);
    activeIsBundled_ = chooseActive(current, s(options_.bundledVersion)) == Active::Bundled;
    installedVersion_ = chooseActive(current, std::string()) == Active::Current ? q(current.manifest.version) : QString();
    const InstalledCopy previous = inspect(layout_, layout_.previous);
    previousVersion_ = chooseActive(previous, std::string()) == Active::Current ? q(previous.manifest.version) : QString();
    emit changed();
}

bool ExtractorUpdater::runPlan(const std::vector<Step> &plan) {
    for (Step step : plan)
        if (!runStep(step)) return false;
    return true;
}

bool ExtractorUpdater::runStep(Step step) {
    auto remove = [](const std::string &dir) {
        const QString path = q(dir);
        if (!exists(path)) return true;
        if (QFileInfo(path).isSymLink()) return QFile::remove(path);
        return QDir(path).removeRecursively();
    };
    auto move = [this](const std::string &from, const std::string &to) {
        if (!exists(q(from)) || exists(q(to))) return false;   // never over an existing name
        if (::rename(QFile::encodeName(q(from)).constData(), QFile::encodeName(q(to)).constData()) != 0) return false;
        return syncDirectory(q(layout_.root));
    };
    switch (step) {
    case Step::ClearStaging: return remove(layout_.staging);
    case Step::ClearTrash: return remove(layout_.trash);
    case Step::ClearSwap: return remove(layout_.swap);
    case Step::PreviousToTrash: return move(layout_.previous, layout_.trash);
    case Step::CurrentToPrevious: return move(layout_.current, layout_.previous);
    case Step::StagingToCurrent: return move(layout_.staging, layout_.current);
    case Step::CurrentToSwap: return move(layout_.current, layout_.swap);
    case Step::PreviousToCurrent: return move(layout_.previous, layout_.current);
    case Step::SwapToCurrent: return move(layout_.swap, layout_.current);
    case Step::SwapToPrevious: return move(layout_.swap, layout_.previous);
    case Step::CurrentToTrash: return move(layout_.current, layout_.trash);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------- verifier

Verifier processVerifier(const ProcessVerifierOptions &options) {
    return [options](const VerifyRequest &request, VerifyDone done) -> std::function<void()> {
        struct Run {
            QPointer<process::HelperProcess> process;
            QByteArray out;
            bool finished = false;
            VerifyDone done;
        };
        std::shared_ptr<Run> run(new Run);
        run->done = std::move(done);
        process::HelperProcess *p = new process::HelperProcess;
        run->process = p;
        QProcessEnvironment environment = options.environment;
        for (const QString &key : environment.keys())
            if (key.startsWith(QLatin1String("PYTHON")) || key.startsWith(QLatin1String("NORDTILE_VERIFY_"))) environment.remove(key);
        QStringList arguments = options.arguments;
        if (options.workerTask) {
            environment.insert(QStringLiteral("NORDTILE_RESOLVER_WORKER"), QStringLiteral("1"));
            environment.insert(QStringLiteral("NORDTILE_WORKER_TASK"), QStringLiteral("verify"));
            environment.insert(QStringLiteral("NORDTILE_VERIFY_SUMS"), request.sums);
            environment.insert(QStringLiteral("NORDTILE_VERIFY_SIG"), request.signature);
            environment.insert(QStringLiteral("NORDTILE_VERIFY_ARCHIVE"), request.archive);
            environment.insert(QStringLiteral("NORDTILE_VERIFY_ASSET"), request.asset);
            environment.insert(QStringLiteral("NORDTILE_VERIFY_VERSION"), request.expectVersion);
        } else {
            arguments << QStringLiteral("--sums") << request.sums << QStringLiteral("--sig") << request.signature
                      << QStringLiteral("--archive") << request.archive << QStringLiteral("--asset") << request.asset;
            if (!request.expectVersion.isEmpty()) arguments << QStringLiteral("--expect-version") << request.expectVersion;
        }
        p->setProcessEnvironment(environment);
        p->setStandardInputFile(QProcess::nullDevice());
        p->setStandardErrorFile(QProcess::nullDevice());
        auto finish = [run](const VerifyResult &result) {
            if (run->finished) return;
            run->finished = true;
            if (run->process) run->process->deleteLater();
            VerifyDone d = run->done;
            d(result);
        };
        QObject::connect(p, &QProcess::readyReadStandardOutput, p, [run, p]() {
            run->out.append(p->readAllStandardOutput());
            if (run->out.size() > kMaxVerifierOutput) p->stopGroup();
        });
        QObject::connect(p, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), p,
                         [run, p, finish](int code, QProcess::ExitStatus status) {
            run->out.append(p->readAllStandardOutput());
            VerifyResult result;
            result.error = QStringLiteral("verifier");
            const QList<QByteArray> lines = run->out.trimmed().split('\n');
            const QJsonDocument doc = run->out.size() <= kMaxVerifierOutput ? QJsonDocument::fromJson(lines.last()) : QJsonDocument();
            if (doc.isObject()) {
                const QJsonObject o = doc.object();
                result.error = o.value(QStringLiteral("error")).toString();
                if (result.error.isEmpty()) result.error = QStringLiteral("verifier");
                result.version = o.value(QStringLiteral("version")).toString();
                result.sha256 = o.value(QStringLiteral("sha256")).toString();
                result.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble(-1));
                result.ok = o.value(QStringLiteral("ok")).toBool(false) && status == QProcess::NormalExit && code == 0;
                if (result.ok) result.error.clear();
                else if (o.value(QStringLiteral("ok")).toBool(false)) result.error = QStringLiteral("verifier");
            }
            finish(result);
        });
        QObject::connect(p, &QProcess::errorOccurred, p, [finish](QProcess::ProcessError error) {
            if (error != QProcess::FailedToStart) return;   // crashes and kills arrive through finished()
            VerifyResult result;
            result.error = QStringLiteral("verifier");
            finish(result);
        });
        QTimer *timer = new QTimer(p);
        timer->setSingleShot(true);
        QObject::connect(timer, &QTimer::timeout, p, [p]() { p->stopGroup(); });
        timer->start(options.timeoutMs);
        if (!p->startHelper(options.program, arguments)) {
            QTimer::singleShot(0, p, [finish]() {
                VerifyResult result;
                result.error = QStringLiteral("verifier");
                finish(result);
            });
        }
        return [run]() {
            if (run->finished) return;
            run->finished = true;
            if (run->process) {
                run->process->disconnect();
                run->process->stopGroup();
                run->process->deleteLater();
            }
        };
    };
}

}}}
