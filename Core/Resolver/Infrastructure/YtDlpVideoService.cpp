// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "YtDlpVideoService.h"

#include <QRegularExpression>
#include <QTimer>

namespace nordtile { namespace video { namespace resolver {

namespace {
const int searchTimeoutMs = 45000;
const int fastDetailsTimeoutMs = 60000;
const int fullDetailsTimeoutMs = 90000;
const qint64 listingLimit = 2 * 1024 * 1024;
const qint64 detailsLimit = 6 * 1024 * 1024;

const char listingTemplate[] =
    "%(.{id,title,channel,channel_id,uploader,duration,view_count,live_status,thumbnails})j";
const char detailsTemplate[] =
    "%(.{id,title,channel,channel_id,uploader,channel_follower_count,duration,view_count,like_count,"
    "upload_date,description,thumbnail,thumbnails,live_status,is_live,chapters,formats,http_headers,"
    "manifest_url,tags,release_timestamp})j";

ErrorCode runError(const RunResult &r) {
    if (r.cancelled) return ErrorCode::Cancelled;
    if (r.timedOut) return ErrorCode::Timeout;
    if (!r.started) return ErrorCode::RuntimeUnavailable;
    if (r.overflow) return ErrorCode::InvalidResponse;
    if (r.crashed) return ErrorCode::Failed;
    if (r.exitCode != 0) return classifyFailure(r.diagnostics);
    return ErrorCode::None;
}
}

YtDlpVideoService::YtDlpVideoService(YtDlpRunner *runner, QObject *parent)
    : QObject(parent), runner_(runner) {}

bool YtDlpVideoService::available() const { return runner_ && runner_->available(); }

QString YtDlpVideoService::normalizedQuery(const QString &query) {
    QString q = query;
    q.replace(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\x7f]")), QStringLiteral(" "));
    return q.simplified().left(200);
}

QStringList YtDlpVideoService::searchArguments(const QStringList &common, const QString &query, int offset, int count) {
    QStringList a = common;
    a << QStringLiteral("--flat-playlist")
      << QStringLiteral("--playlist-items") << QStringLiteral("%1:%2").arg(offset + 1).arg(offset + count)
      << QStringLiteral("--print") << QString::fromLatin1(listingTemplate)
      << QStringLiteral("--") << QStringLiteral("ytsearch%1:%2").arg(offset + count).arg(query);
    return a;
}

namespace {
// RES-08 step 1 / RES-19: yt-dlp without a JavaScript runtime speaks only visionos by default, which
// is the fast path's own client. android_vr is asked as well for live: its HLS needs no PO token and
// carries the muxed variants (itags 91-96) that RES-19 asks for. Its https formats are skipped by
// yt-dlp without a GVS PO token, so it adds nothing for recorded videos (docs/PLAYABILITY.md).
const char tokenFreeYoutubeArguments[] = "youtube:skip=dash;player_client=android_vr,visionos";
const char widenedYoutubeArguments[] = "youtube:skip=dash";

QStringList buildDetailsArguments(const QStringList &common, const QString &videoId, const QStringList &extra,
                                  const QString &youtube) {
    QStringList a = common;
    // DASH manifests are not needed: adaptive streams arrive as direct HTTPS
    // formats; HLS stays enabled because live streams need it. One youtube
    // extractor-args value: further youtube: arguments go after ';'.
    a << QStringLiteral("--no-playlist") << QStringLiteral("--simulate")
      << QStringLiteral("--no-check-formats") << QStringLiteral("--ignore-no-formats-error")
      << QStringLiteral("--extractor-args") << youtube
      << QStringLiteral("--format") << QStringLiteral("b/bv*/ba/w*")
      << QStringLiteral("--print") << QString::fromLatin1(detailsTemplate);
    a << extra;
    a << QStringLiteral("--") << QStringLiteral("https://www.youtube.com/watch?v=") + videoId;
    return a;
}
}

QString YtDlpVideoService::youtubeExtractorArguments(bool tokenFree) {
    return QString::fromLatin1(tokenFree ? tokenFreeYoutubeArguments : widenedYoutubeArguments);
}

QStringList YtDlpVideoService::detailsArguments(const QStringList &common, const QString &videoId) {
    return buildDetailsArguments(common, videoId, QStringList(), youtubeExtractorArguments(false));
}

QStringList YtDlpVideoService::passArguments(const QStringList &common, const QString &videoId, bool tokenFree) {
    return buildDetailsArguments(common, videoId, QStringList(), youtubeExtractorArguments(tokenFree));
}

#ifndef SAILTUBE_STORE
QStringList YtDlpVideoService::detailsArguments(const QStringList &common, const QString &videoId,
                                                const QStringList &poTokenArguments, bool forcePoToken) {
    return passArguments(common, videoId, poTokenArguments, forcePoToken, false);
}

QStringList YtDlpVideoService::passArguments(const QStringList &common, const QString &videoId,
                                             const QStringList &poTokenArguments, bool forcePoToken, bool tokenFree) {
    // Only option/value pairs of --plugin-dirs and youtubepot-* extractor-args pass; anything
    // else (a cookie or login option above all) drops the provider arguments altogether.
    bool accepted = poTokenArguments.size() % 2 == 0;
    for (int i = 0; accepted && i + 1 < poTokenArguments.size(); i += 2) {
        const QString &option = poTokenArguments.at(i), &value = poTokenArguments.at(i + 1);
        accepted = (option == QLatin1String("--plugin-dirs") && value.startsWith(QLatin1Char('/')))
            || (option == QLatin1String("--extractor-args") && value.startsWith(QLatin1String("youtubepot-")));
    }
    const QStringList extra = accepted ? poTokenArguments : QStringList();
    // RES-08 step 4: token-forced only when a provider is active. Same value as
    // potoken::forcedTokenArgument() (kept literal so this adapter links alone).
    QString youtube = youtubeExtractorArguments(tokenFree);
    if (forcePoToken && !extra.isEmpty()) youtube += QStringLiteral(";fetch_pot=always");
    return buildDetailsArguments(common, videoId, extra, youtube);
}
#endif

quint64 YtDlpVideoService::search(const QString &query, int offset, int count, ListingCompletion completion) {
    const QString q = normalizedQuery(query);
    offset = qBound(0, offset, 480);
    count = qBound(1, count, 50);
    if (q.isEmpty()) {
        QTimer::singleShot(0, this, [completion]() {
            completion(ErrorCode::InvalidRequest, std::vector<catalog::VideoSummary>());
        });
        return 0;
    }
    return runner_->run(searchArguments(runner_->commonArguments(false), q, offset, count),
                        searchTimeoutMs, listingLimit, [completion, count](const RunResult &r) {
        const ErrorCode error = runError(r);
        if (error != ErrorCode::None) { completion(error, std::vector<catalog::VideoSummary>()); return; }
        completion(ErrorCode::None, parseListing(r.output, count));
    });
}

quint64 YtDlpVideoService::details(const QString &videoId, bool fullProfile, DetailsCompletion completion) {
    return details(videoId, fullProfile, DetailsOptions(), std::move(completion));
}

quint64 YtDlpVideoService::details(const QString &videoId, bool fullProfile, const DetailsOptions &options,
                                   DetailsCompletion completion) {
    return detailsReport(videoId, fullProfile, options,
                         [completion](const DetailsReport &report, const catalog::VideoDetails &details) {
        completion(report.error, details);
    });
}

quint64 YtDlpVideoService::detailsReport(const QString &videoId, bool fullProfile, const DetailsOptions &options,
                                         ReportCompletion completion) {
    const std::string id = videoId.toStdString();
    if (!catalog::validVideoId(id)) {
        QTimer::singleShot(0, this, [completion]() {
            DetailsReport report;
            report.error = ErrorCode::InvalidRequest;
            completion(report, catalog::VideoDetails());
        });
        return 0;
    }
    // Warnings stay on for extraction: with --ignore-no-formats-error a bot
    // wall or sign-in demand only shows up there, not in the exit code.
    QStringList common = runner_->commonArguments(fullProfile);
    common.removeAll(QStringLiteral("--no-warnings"));
    // RES-08: the pass without JavaScript is the token-free one.
    const bool tokenFree = !fullProfile;
#ifndef SAILTUBE_STORE
    // RES-14: provider arguments only when the request asked and the provider is ready.
    const QStringList poTokenArguments = options.poProvider && poTokenArguments_ ? poTokenArguments_() : QStringList();
    const QStringList arguments = passArguments(common, videoId, poTokenArguments, options.forcePoToken, tokenFree);
#else
    (void)options;
    const QStringList arguments = passArguments(common, videoId, tokenFree);
#endif
    return runner_->run(arguments,
                        fullProfile ? fullDetailsTimeoutMs : fastDetailsTimeoutMs, detailsLimit,
                        [completion, id](const RunResult &r) {
        catalog::VideoDetails details;
        DetailsReport report;
        DetailsExtras extras;
        ErrorCode error = runError(r);
        if (error == ErrorCode::None) error = parseDetails(r.output, id, &details, &extras);
        if (error == ErrorCode::None && details.formats.empty()) {
            // Metadata without streams: name the reason when stderr gives one.
            const ErrorCode reason = classifyFailure(r.diagnostics);
            if (reason != ErrorCode::Failed && reason != ErrorCode::Network) error = reason;
        }
        report.error = error;
        // The finer reason of a failure or of an answer without streams (warnings included).
        if (!r.cancelled && !r.timedOut && (error != ErrorCode::None || details.formats.empty()))
            report.reason = classifyFailureReason(r.diagnostics);
        report.scheduledStartUnix = extras.scheduledStartUnix;
        report.formatsWithoutUrl = extras.formatsWithoutUrl;
        report.storyboard = extras.storyboard;
        completion(report, details);
    });
}

void YtDlpVideoService::cancel(quint64 id) { if (id) runner_->cancel(id); }

}}}
