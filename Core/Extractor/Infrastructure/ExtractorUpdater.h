// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// [FULL] NFR-MNT-01 / NFR-SEC-04: in-app update of the yt-dlp archive (a data file the warm worker
// imports with zipimport; it is never executed). QtCore + QtNetwork.
//
//   check(channel)   HEAD the channel's latest/download/yt-dlp, following each redirect by hand
//                    (Domain/ReleasePolicy RedirectChain re-checks every hop: https, 443, github.com then
//                    the release asset hosts only; certificate errors refuse); the tag in the redirect
//                    path is the version, the asset host's Content-Length the size. Then GET
//                    SHA2-256SUMS and SHA2-256SUMS.sig of that tag. Emits offered(version, size, source)
//                    when the release is newer than what runs now, else state "upToDate".
//   install(v,s,src) only with exactly the offer's {version, size, source} (what the confirmation showed):
//                    downloads the archive into <root>/staging (0700 dir, 0600 files, bounded by the
//                    offered size), runs the injected verifier (signature, checksum, archive structure,
//                    version; nothing is imported or run), writes the manifest, then renames
//                    current -> previous and staging -> current. current is never touched before
//                    staging verified; an abort or crash at any step leaves the old current.
//   rollback()       previous <-> current.  revertToBundled(): current -> trash (previous kept).
//   activeArchive()  current/yt-dlp.pyz if its manifest and SHA-256 verified at start, else the bundled path.
//
// The new archive is used from the next start of the warm workers (activeArchiveChanged()).
// Errors are fixed labels (lastError); nothing from the network or the verifier reaches the UI or the log.

#ifdef SAILTUBE_STORE
#error "The extractor updater is FULL only (NFR-MNT-01 [STORE] uses only the bundled extractor)"
#endif

#include "Core/Extractor/Domain/ReleasePolicy.h"

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QProcessEnvironment>
#include <QSslCertificate>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <functional>
#include <memory>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;

namespace nordtile { namespace video { namespace extractor {

struct VerifyRequest {
    QString sums, signature, archive, asset, expectVersion;
};
struct VerifyResult {
    bool ok = false;
    QString version, sha256, error;   // error: the verifier's fixed label, or "verifier"
    qint64 size = -1;
};
using VerifyDone = std::function<void(const VerifyResult &)>;
// Starts one verification; calls done exactly once (asynchronously) unless the returned cancel
// function runs first. The production verifier is processVerifier() over the app binary.
using Verifier = std::function<std::function<void()>(const VerifyRequest &, VerifyDone)>;

struct ProcessVerifierOptions {
    QString program;                // absolute: QCoreApplication::applicationFilePath() (or python3 in tests)
    QStringList arguments;          // empty for the app binary; {"-I", ".../verify_release.py", "--key", K, ...} in tests
    // true: the app binary's warm-worker mode, task "verify" (NORDTILE_RESOLVER_WORKER=1,
    // NORDTILE_WORKER_TASK=verify, paths in NORDTILE_VERIFY_*; the key is the installed one).
    // false: verify_release.py's command line (paths appended as --sums/--sig/--archive/--asset/--expect-version).
    bool workerTask = true;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    int timeoutMs = 120000;
};
// A Verifier running each verification in its own HelperProcess (own process group, parent-death signal).
Verifier processVerifier(const ProcessVerifierOptions &);

struct UpdaterOptions {
    QString root;                   // <AppDataLocation>/extractor (created 0700)
    QString bundledArchive;         // the packaged runtime/yt-dlp.pyz
    QString bundledVersion;         // its yt-dlp version if known (an older current is then ignored)
    Verifier verifier;
    qint64 maxArchiveBytes = kMaxArchiveBytes;
    int requestTimeoutMs = 30000;   // per hop, reset while bytes arrive
    std::function<qint64()> clockMs;   // default: QDateTime::currentMSecsSinceEpoch
    // ---- test hooks only; production leaves both empty (certificates verified against the system CAs)
    // Connect a logical host (github.com, an asset host) elsewhere; the policy still checks the logical URL.
    std::function<bool(const QString &host, QString *connectHost, quint16 *connectPort)> endpointOverride;
    // Extra CA certificates to trust (the fake release host's throwaway certificate). Never ignores errors.
    QList<QSslCertificate> extraCaCertificatesForTesting;
};

class ExtractorUpdater : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY changed)
    Q_PROPERTY(QString channel READ channel NOTIFY changed)
    Q_PROPERTY(QString offeredVersion READ offeredVersion NOTIFY changed)
    Q_PROPERTY(qint64 offeredSize READ offeredSize NOTIFY changed)
    Q_PROPERTY(QString offeredSource READ offeredSource NOTIFY changed)
    Q_PROPERTY(QString installedVersion READ installedVersion NOTIFY changed)
    Q_PROPERTY(QString previousVersion READ previousVersion NOTIFY changed)
    Q_PROPERTY(QString activeVersion READ activeVersion NOTIFY changed)
    Q_PROPERTY(bool usingBundled READ usingBundled NOTIFY changed)
    Q_PROPERTY(bool restartNeeded READ restartNeeded NOTIFY changed)
    Q_PROPERTY(qint64 bytesReceived READ bytesReceived NOTIFY progress)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
public:
    explicit ExtractorUpdater(const UpdaterOptions &options, QObject *parent = nullptr);
    ~ExtractorUpdater() override;

    // "idle", "checking", "offered", "upToDate", "downloading", "verifying", "installing", "installed",
    // "rolledBack", "reverted", "failed".
    QString state() const { return state_; }
    QString channel() const { return channel_; }
    QString offeredVersion() const;
    qint64 offeredSize() const;
    QString offeredSource() const;
    QString installedVersion() const { return installedVersion_; }   // current/ (valid manifest) or empty
    QString previousVersion() const { return previousVersion_; }     // previous/ (valid manifest) or empty
    QString activeVersion() const;                                   // what the workers load at their next start
    bool usingBundled() const { return activeIsBundled_; }
    bool restartNeeded() const { return restartNeeded_; }
    qint64 bytesReceived() const { return bytesReceived_; }
    // Fixed labels: "", "busy", "channel", "network", "timeout", "certificate", "insecure-url",
    // "host-not-allowed", "bad-release-path", "unexpected-redirect", "redirect-limit", "http-status",
    // "too-large", "size-mismatch", "sums", "signature-missing", "no-offer", "token-mismatch",
    // "offer-expired", "storage", "verifier", "checksum", "version-mismatch", "cancelled",
    // "no-previous", "no-update", or a verifier label ("signature", "signature-issuer",
    // "archive-entry", ...; see verify_release.ERRORS).
    QString lastError() const { return lastError_; }

    Q_INVOKABLE bool check(const QString &channel);
    Q_INVOKABLE bool install(const QString &version, qint64 size, const QString &source);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool rollback();
    Q_INVOKABLE bool revertToBundled();

    // The bundled yt-dlp version from the packaged licenses/python-runtime/source-manifest.json
    // (inputs.yt_dlp_version), or empty when unreadable. For UpdaterOptions::bundledVersion.
    static QString bundledVersionFromManifest(const QString &sourceManifestPath);

    // For WarmResolver's WorkerOptions::archivePath: the verified current archive or bundledPath.
    QString activeArchive(const QString &bundledPath) const;
    QString activeArchive() const { return activeArchive(options_.bundledArchive); }

signals:
    void changed();
    void progress();
    void offered(const QString &version, qint64 size, const QString &source);
    void installed(const QString &version);
    void failed(const QString &error);
    void activeArchiveChanged();

private:
    struct Fetch;
    using FetchDone = std::function<void(Fetch &)>;
    void startFetch(const std::shared_ptr<Fetch> &fetch, const QString &logicalUrl);
    void finishFetch(const std::shared_ptr<Fetch> &fetch);
    std::shared_ptr<Fetch> newFetch(Channel channel, const QString &file, const QString &expectedTag,
                                    const QByteArray &method, qint64 maxBytes, FetchDone done);
    void onArchiveHead(Fetch &);
    void onSums(Fetch &);
    void onSignature(Fetch &);
    void onArchiveBody(Fetch &);
    void onVerified(const VerifyResult &);
    void fail(const QString &error);
    void setState(const QString &state);
    void recover();
    void reload();
    bool runPlan(const std::vector<Step> &plan);
    bool runStep(Step step);
    bool prepareStaging();
    bool writePrivate(const QString &path, const QByteArray &data);
    qint64 now() const;
    bool busy() const;

    UpdaterOptions options_;
    Layout layout_;
    QNetworkAccessManager *network_;
    QString state_ = QStringLiteral("idle");
    QString channel_;
    QString lastError_;
    QString installedVersion_, previousVersion_;
    bool activeIsBundled_ = true;
    bool restartNeeded_ = false;
    qint64 bytesReceived_ = 0;
    Channel checkChannel_ = Channel::Stable;
    QString pendingTag_;
    qint64 pendingSize_ = -1;
    QByteArray pendingSums_;
    std::unique_ptr<Offer> offer_;
    std::unique_ptr<Offer> installing_;
    std::shared_ptr<Fetch> fetch_;
    std::function<void()> cancelVerify_;
    QString stagedSha256_;
    quint64 generation_ = 0;   // bumped by cancel/fail: late callbacks of an older operation are dropped
};

}}}
