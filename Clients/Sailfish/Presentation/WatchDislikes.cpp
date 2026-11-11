// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WatchDislikes.h"
#include "DislikeInfo.h"
#include "WatchInfo.h"
#include "Core/Catalog/Domain/LocalizedText.h"

namespace nordtile { namespace video { namespace sailfish {

WatchDislikes::WatchDislikes(WatchInfo *watch, DislikeInfo *dislikes, QObject *parent)
    : QObject(parent), watch_(watch), dislikes_(dislikes) {
    if (!watch || !dislikes) return;
    connect(watch, &WatchInfo::videoIdChanged, this, [this] {
        if (!watch_ || !dislikes_) return;
        if (dislikes_->videoId() != watch_->videoId()) dislikes_->clear();
        publish();
    });
    connect(watch, &WatchInfo::requested, this, [this](const QString &videoId) { ask(videoId); });
    wasOffline_ = watch->offline();
    connect(watch, &WatchInfo::holdChanged, this, [this] {
        if (!watch_) return;
        const bool cameOnline = wasOffline_ && !watch_->offline();
        wasOffline_ = watch_->offline();
        if (cameOnline && !watch_->deferred()) ask(watch_->videoId());
    });
    connect(dislikes, &DislikeInfo::changed, this, [this] { publish(); });
    publish();
}

void WatchDislikes::ask(const QString &videoId) {
    if (!watch_ || !dislikes_) return;
    if (videoId.isEmpty() || videoId != watch_->videoId() || watch_->offline()) return;
    dislikes_->fetch(videoId);   // remembers the video; requests only while the setting is on
    publish();
}

void WatchDislikes::publish() {
    if (!watch_ || !dislikes_) return;
    const bool mine = !watch_->videoId().isEmpty() && dislikes_->videoId() == watch_->videoId();
    const double count = mine && dislikes_->enabled() && dislikes_->available() ? dislikes_->estimate() : -1;
    const QString text = count < 0 ? QString()
        : QString::fromStdString(catalog::formatCompactCount(static_cast<std::int64_t>(count), catalog::displayLanguage()));
    watch_->setDislikeEstimate(count, text);
}

}}}
