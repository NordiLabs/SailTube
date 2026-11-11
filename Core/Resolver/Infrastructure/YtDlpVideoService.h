// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Catalog and extraction operations expressed as yt-dlp runs. This adapter
// contains no YouTube protocol code: upstream yt-dlp owns that.

#include "YtDlpJson.h"
#include "YtDlpRunner.h"
#include <QHash>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace resolver {

class YtDlpVideoService final : public QObject {
    Q_OBJECT
public:
    using ListingCompletion = std::function<void(ErrorCode, const std::vector<catalog::VideoSummary> &)>;
    using DetailsCompletion = std::function<void(ErrorCode, const catalog::VideoDetails &)>;

    // Per-run extraction choices (RES-08 step 4, RES-14).
    struct DetailsOptions {
        bool poProvider = false;    // the request asked for the PO-token provider (StreamRequest::poProvider)
        bool forcePoToken = false;  // ladder step 4: force a token (FULL, only with a ready provider)
    };

    // RES-08 / RES-09 / RES-19: what a details run found besides the details: its code, the finer
    // reason (from stderr, warnings included) and the scheduled start of an upcoming video.
    struct DetailsReport {
        ErrorCode error = ErrorCode::None;
        FailureReason reason = FailureReason::None;
        std::int64_t scheduledStartUnix = 0;  // release_timestamp, 0 unknown
        int formatsWithoutUrl = 0;            // listed formats dropped for lack of an accepted URL
        StoryboardLevel storyboard;           // scrub-preview sprite sheets, empty when the answer has none
    };
    using ReportCompletion = std::function<void(const DetailsReport &, const catalog::VideoDetails &)>;

    explicit YtDlpVideoService(YtDlpRunner *runner, QObject *parent = nullptr);
    bool available() const;

    // Results offset..offset+count-1 of a video search.
    quint64 search(const QString &query, int offset, int count, ListingCompletion);
    // fullProfile adds the JavaScript runtime (player script, challenge solving).
    quint64 details(const QString &videoId, bool fullProfile, DetailsCompletion);
    // RES-14: as above; with options.poProvider and a ready provider the run gets the provider's
    // arguments. On STORE the options change nothing.
    quint64 details(const QString &videoId, bool fullProfile, const DetailsOptions &, DetailsCompletion);
    // As above, with the whole report (the playback adapter's ladder reads the reason).
    quint64 detailsReport(const QString &videoId, bool fullProfile, const DetailsOptions &, ReportCompletion);
    void cancel(quint64 id);

    static QString normalizedQuery(const QString &);
    static QStringList searchArguments(const QStringList &common, const QString &query, int offset, int count);
    static QStringList detailsArguments(const QStringList &common, const QString &videoId);
    // RES-08 step 1 vs 2: the youtube extractor-args value of a pass (one value, options after ';').
    // The token-free pass (no JavaScript) names player_client=android_vr,visionos, so it asks other
    // clients than the fast path's; the widened pass (fullProfile) keeps yt-dlp's default clients.
    static QString youtubeExtractorArguments(bool tokenFree);
    // detailsArguments() for one pass (tokenFree: see youtubeExtractorArguments).
    static QStringList passArguments(const QStringList &common, const QString &videoId, bool tokenFree);
#ifndef SAILTUBE_STORE
    // RES-14 [FULL]: the provider's yt-dlp arguments (plugin directory, bgutil HTTP base URL)
    // are inserted before the URL; forcePoToken appends fetch_pot=always to the youtube
    // extractor-args (RES-08 step 4) and needs non-empty poTokenArguments.
    static QStringList detailsArguments(const QStringList &common, const QString &videoId,
                                        const QStringList &poTokenArguments, bool forcePoToken);
    // As above, for one pass (tokenFree: see youtubeExtractorArguments).
    static QStringList passArguments(const QStringList &common, const QString &videoId,
                                     const QStringList &poTokenArguments, bool forcePoToken, bool tokenFree);
    // RES-14 [FULL]: source of the provider's arguments, asked once per run with
    // options.poProvider; it returns an empty list unless the provider is ready.
    using PoTokenArguments = std::function<QStringList()>;
    void setPoTokenArguments(PoTokenArguments source) { poTokenArguments_ = std::move(source); }
#endif

private:
    YtDlpRunner *runner_;
#ifndef SAILTUBE_STORE
    PoTokenArguments poTokenArguments_;
#endif
};

}}}
