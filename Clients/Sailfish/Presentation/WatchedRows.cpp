// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WatchedRows.h"

#include <QTimer>

namespace nordtile { namespace video { namespace sailfish {

WatchedRows *WatchedRows::instance() {
    static WatchedRows *rows = new WatchedRows;   // lives for the process (QML-created lists use it)
    return rows;
}

void WatchedRows::setPredicate(const Predicate &predicate) {
    predicate_ = predicate;
    notifyChanged();
}

std::function<bool(const std::string &)> WatchedRows::forContext(const QString &context) const {
    if (!predicate_) return std::function<bool(const std::string &)>();
    const Predicate predicate = predicate_;
    return [predicate, context](const std::string &videoId) {
        return predicate(QString::fromStdString(videoId), context);
    };
}

void WatchedRows::notifyChanged() {
    if (pending_) return;
    pending_ = true;
    QTimer::singleShot(0, this, [this] {
        pending_ = false;
        emit changed();
    });
}

}}}
