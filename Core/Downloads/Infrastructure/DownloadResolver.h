// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The download engine's view of a resolver: video id -> formats with fresh
// URLs. This is the seam for the shared resolver of docs/SHARED_PLAYBACK.md
// (`StreamResolver::resolve`); until it lands, YtDlpDownloadResolver adapts the
// pinned yt-dlp service. Downloads never share the playback component (RES-02): the
// engine owns its own resolver instance and request queue.

#include "Core/Media/Domain/MediaFormat.h"
#include "Core/Resolver/Infrastructure/YtDlpJson.h"
#include <QString>
#include <QtGlobal>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace downloads {

struct ResolvedVideo {
    QString title;
    QString channel;
    QString channelId;
    qint64 durationSeconds = -1;
    QString thumbnailUrl;
    std::vector<media::MediaFormat> formats; // URLs and headers are ephemeral: never persisted
    qint64 resolvedAtMs = 0;                 // epoch ms when the URLs were obtained
};

class DownloadResolver {
public:
    using Completion = std::function<void(resolver::ErrorCode, const ResolvedVideo &)>;
    virtual ~DownloadResolver() {}
    // Asynchronous; the completion runs on the caller's thread exactly once,
    // unless cancel() was called first. Returns 0 when the request could not
    // be started at all; no completion follows then.
    virtual quint64 resolve(const QString &videoId, Completion completion) = 0;
    virtual void cancel(quint64 id) = 0;
};

}}}
