// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// LNK-04: a playlist link (list=..., index=...) plays on through the list. seed() walks the
// playlist (page 1 and continuations, at most kMaxItems ids, stopping kAhead ids past the linked
// video) and emits queueReady() once with the ids and the linked video's position, which the
// shell hands to playback.setQueue(). When the linked video is not in the list it is put at its
// index hint (0-based), or first. A newer seed() or cancel() drops the walk; a failed first
// page emits nothing (the video plays alone). Exposed to QML as `queueSeeder`.

#include <QObject>
#include <QPointer>
#include <QStringList>

namespace nordtile { namespace video {
namespace catalog { class PlaylistService; struct PlaylistPage; }
namespace sailfish {

class QueueSeeder final : public QObject {
    Q_OBJECT
public:
    static const int kMaxItems = 500;
    static const int kAhead = 50;

    explicit QueueSeeder(catalog::PlaylistService *service, QObject *parent = nullptr);

    Q_INVOKABLE void seed(const QString &playlistId, const QString &videoId, int index);
    Q_INVOKABLE void cancel();
    bool busy() const { return !videoId_.isEmpty(); }

signals:
    void queueReady(const QString &videoId, const QStringList &videoIds, int currentIndex);

private:
    void onPage(quint64 generation, const catalog::PlaylistPage &page);
    void finish();

    QPointer<catalog::PlaylistService> service_;
    quint64 generation_ = 0;
    QString playlistId_;
    QString videoId_;
    int index_ = -1;
    QStringList ids_;
};

}}}
