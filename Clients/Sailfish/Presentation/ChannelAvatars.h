// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The main-thread face of catalog::AvatarBook (channel id -> avatar address). Every catalog list
// model has a `channelAvatar` role: the row's own avatar when the answer carried one, else the
// book's entry for the row's channel, else "". When the book learns something (possibly on a
// worker lane) this object emits changed() once per event-loop turn on the main thread, and the
// models re-announce that role, so avatars appear in lists that are already on screen.
//
// Also the QML context property `channelAvatars`, for lists of other components (history, local
// playlists): `channelAvatars.revision >= 0 ? channelAvatars.get(model.channelId) : ""`.
// No request is ever made here; images load through the shared image cache.

#include "Core/Catalog/Domain/Item.h"

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <atomic>

namespace nordtile { namespace video { namespace sailfish {

class ChannelAvatars final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    // Created on first use; call it first on the main thread (the composition and the models do).
    static ChannelAvatars *instance();

    int revision() const;
    Q_INVOKABLE QString get(const QString &channelId) const;
    // Remembers an avatar (same rules as the book); returns true when it was new or changed.
    Q_INVOKABLE bool learn(const QString &channelId, const QString &avatarUrl);

    // The `channelAvatar` role value of a row: its own avatar, else the book's for its channel
    // (a channel row's channel is the row itself).
    static QString forRow(const catalog::Item &item);
    // Makes `model` re-announce `role` for all its rows whenever the book learns something.
    static void follow(QAbstractItemModel *model, int role);

signals:
    void changed();

private slots:
    void flush();

private:
    explicit ChannelAvatars(QObject *parent = nullptr);
    std::atomic<bool> pending_;
};

}}}
