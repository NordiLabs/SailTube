// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ChannelAvatars.h"

#include "Core/Catalog/Domain/AvatarBook.h"

#include <QMetaObject>
#include <QVector>

namespace nordtile { namespace video { namespace sailfish {

using catalog::AvatarBook;

ChannelAvatars *ChannelAvatars::instance() {
    static ChannelAvatars *avatars = new ChannelAvatars;   // lives for the process (QML-created lists use it)
    return avatars;
}

ChannelAvatars::ChannelAvatars(QObject *parent) : QObject(parent), pending_(false) {
    // The book may learn on a worker lane: hop to this object's thread, once per burst.
    AvatarBook::instance().setListener([this] {
        if (!pending_.exchange(true)) QMetaObject::invokeMethod(this, "flush", Qt::QueuedConnection);
    });
}

void ChannelAvatars::flush() {
    pending_.store(false);
    emit changed();
}

int ChannelAvatars::revision() const { return static_cast<int>(AvatarBook::instance().revision() & 0x7fffffff); }

QString ChannelAvatars::get(const QString &channelId) const {
    return QString::fromStdString(AvatarBook::instance().lookup(channelId.toStdString()));
}

bool ChannelAvatars::learn(const QString &channelId, const QString &avatarUrl) {
    return AvatarBook::instance().learn(channelId.toStdString(), avatarUrl.toStdString());
}

void ChannelAvatars::follow(QAbstractItemModel *model, int role) {
    connect(instance(), &ChannelAvatars::changed, model, [model, role] {
        const int rows = model->rowCount();
        if (rows > 0) emit model->dataChanged(model->index(0, 0), model->index(rows - 1, 0), QVector<int>() << role);
    });
}

QString ChannelAvatars::forRow(const catalog::Item &item) {
    if (!item.avatarUrl.empty()) return QString::fromStdString(item.avatarUrl);
    const std::string &channel = item.kind == catalog::ItemKind::Channel ? item.id : item.channelId;
    return QString::fromStdString(AvatarBook::instance().lookup(channel));
}

}}}
