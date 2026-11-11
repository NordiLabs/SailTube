// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "QueueSeeder.h"

#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"

namespace nordtile { namespace video { namespace sailfish {

QueueSeeder::QueueSeeder(catalog::PlaylistService *service, QObject *parent) : QObject(parent), service_(service) {}

void QueueSeeder::seed(const QString &playlistId, const QString &videoId, int index) {
    cancel();
    const QString list = catalog::PlaylistService::normalizeListId(playlistId);
    if (!service_ || list.isEmpty() || videoId.isEmpty()) return;
    playlistId_ = list;
    videoId_ = videoId;
    index_ = index;
    const quint64 generation = generation_;
    service_->open(list, this, [this, generation](catalog::PlaylistPage page) { onPage(generation, page); });
}

void QueueSeeder::cancel() {
    ++generation_;
    videoId_.clear();
    ids_.clear();
}

void QueueSeeder::onPage(quint64 generation, const catalog::PlaylistPage &page) {
    if (generation != generation_ || videoId_.isEmpty()) return;
    if (page.outcome != catalog::Outcome::Ok && page.outcome != catalog::Outcome::ValidEmpty) {
        if (ids_.isEmpty()) { cancel(); return; }   // nothing usable: the video plays alone
        finish();
        return;
    }
    for (const catalog::Item &item : page.items) {
        if (ids_.size() >= kMaxItems) break;
        if (!catalog::isVideoLike(item) || item.id.empty()) continue;
        const QString id = QString::fromStdString(item.id);
        if (!ids_.contains(id)) ids_.append(id);
    }
    const int at = ids_.indexOf(videoId_);
    const bool enough = (at >= 0 && ids_.size() - at > kAhead) || ids_.size() >= kMaxItems;
    if (!enough && !page.continuation.isEmpty() && service_) {
        service_->more(playlistId_, page.continuation, ids_.size(), this,
                       [this, generation](catalog::PlaylistPage next) { onPage(generation, next); });
        return;
    }
    finish();
}

void QueueSeeder::finish() {
    QStringList ids = ids_;
    int current = ids.indexOf(videoId_);
    if (current < 0) {
        current = index_ >= 0 && index_ <= ids.size() ? index_ : 0;
        ids.insert(current, videoId_);
    }
    const QString videoId = videoId_;
    cancel();
    if (ids.size() > 1) emit queueReady(videoId, ids, current);
}

}}}
