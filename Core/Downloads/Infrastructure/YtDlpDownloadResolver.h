// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DownloadResolver over YtDlpVideoService::details (full profile). Temporary
// adapter until the shared StreamResolver is pinned (docs/SHARED_PLAYBACK.md).

#include "DownloadResolver.h"
#include <QSet>
#include <memory>

namespace nordtile { namespace video { namespace resolver { class YtDlpVideoService; } } }

namespace nordtile { namespace video { namespace downloads {

class YtDlpDownloadResolver final : public DownloadResolver {
public:
    // The service must outlive this adapter; give downloads their own runner
    // so they never queue in front of playback resolves (RES-02).
    explicit YtDlpDownloadResolver(resolver::YtDlpVideoService *service);
    quint64 resolve(const QString &videoId, Completion completion) override;
    void cancel(quint64 id) override;

    static ResolvedVideo fromDetails(const catalog::VideoDetails &details, qint64 resolvedAtMs);

private:
    // Shared with pending completions, which may outlive this adapter.
    struct Shared { QSet<quint64> cancelled; };
    resolver::YtDlpVideoService *service_;
    std::shared_ptr<Shared> shared_;
};

}}}
