// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "YtDlpDownloadResolver.h"
#include "Core/Resolver/Infrastructure/YtDlpVideoService.h"

#include <QDateTime>
#include <memory>

namespace nordtile { namespace video { namespace downloads {

YtDlpDownloadResolver::YtDlpDownloadResolver(resolver::YtDlpVideoService *service)
    : service_(service), shared_(std::make_shared<Shared>()) {}

ResolvedVideo YtDlpDownloadResolver::fromDetails(const catalog::VideoDetails &d, qint64 resolvedAtMs) {
    ResolvedVideo v;
    v.title = QString::fromStdString(d.summary.title);
    v.channel = QString::fromStdString(d.summary.channel);
    v.channelId = QString::fromStdString(d.summary.channelId);
    v.durationSeconds = d.summary.durationSeconds;
    v.thumbnailUrl = QString::fromStdString(d.summary.thumbnailUrl);
    v.formats = d.formats;
    v.resolvedAtMs = resolvedAtMs;
    return v;
}

quint64 YtDlpDownloadResolver::resolve(const QString &videoId, Completion completion) {
    if (!service_) return 0;
    std::weak_ptr<Shared> weak = shared_;
    std::shared_ptr<quint64> own = std::make_shared<quint64>(0);
    const quint64 id = service_->details(videoId, true,
        [completion, weak, own](resolver::ErrorCode code, const catalog::VideoDetails &details) {
            std::shared_ptr<Shared> shared = weak.lock();
            if (!shared || shared->cancelled.remove(*own)) return;
            completion(code, fromDetails(details, QDateTime::currentMSecsSinceEpoch()));
        });
    *own = id;
    return id;
}

void YtDlpDownloadResolver::cancel(quint64 id) {
    if (!id || !service_) return;
    shared_->cancelled.insert(id);
    service_->cancel(id);
}

}}}
