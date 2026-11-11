// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "YtDlpSearchFallback.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/Video.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"

#include <QPointer>

namespace nordtile { namespace video { namespace catalog {

namespace {

Outcome outcomeFor(resolver::ErrorCode e) {
    using resolver::ErrorCode;
    switch (e) {
    case ErrorCode::None: return Outcome::Ok;
    case ErrorCode::Network: case ErrorCode::Timeout: case ErrorCode::Cancelled: return Outcome::Network;
    case ErrorCode::BotCheck: return Outcome::RateLimited;
    case ErrorCode::RuntimeUnavailable: return Outcome::Unavailable;
    case ErrorCode::InvalidResponse: return Outcome::ShapeDrift;
    default: return Outcome::Http5xx;
    }
}

Item toItem(const VideoSummary &v) {
    Item i;
    i.kind = ItemKind::Video;
    i.id = v.videoId;
    i.title = v.title;
    i.channel = v.channel;
    i.channelId = v.channelId;
    i.durationSeconds = v.durationSeconds;
    i.viewCount = v.viewCount;
    i.liveStatus = v.liveStatus;
    i.thumbnailUrl = listThumbnailUrl(v.videoId);
    return i;
}

}  // namespace

bool ytDlpSearchSupports(const SearchFilter &filter) {
    return filter.sort == SearchSort::Relevance && filter.uploadDate == UploadDateFilter::Any &&
           filter.duration == DurationFilter::Any && !filter.live &&
           (filter.type == ResultTypeFilter::Any || filter.type == ResultTypeFilter::Video);
}

SearchFallback makeYtDlpSearchFallback(resolver::YtDlpVideoService *service) {
    SearchFallback f;
    QPointer<resolver::YtDlpVideoService> svc(service);
    f.search = [svc](const QString &query, const SearchFilter &filter, int offset, int count, QObject *context,
                     SearchFallback::Done done) -> quint64 {
        if (!svc || !svc->available() || !ytDlpSearchSupports(filter)) return 0;
        QPointer<QObject> guard(context);
        return svc->search(query, offset, count, [guard, done](resolver::ErrorCode e, const std::vector<VideoSummary> &found) {
            if (!guard) return;
            std::vector<Item> rows;
            rows.reserve(found.size());
            for (const VideoSummary &v : found) if (validVideoId(v.videoId)) rows.push_back(toItem(v));
            done(outcomeFor(e), rows);
        });
    };
    f.cancel = [svc](quint64 id) { if (svc) svc->cancel(id); };
    return f;
}

}}}
