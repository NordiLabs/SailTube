// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// LIB-07 bridge between the library component's watch index and the catalog lists. The composition
// installs one predicate ("is this video hidden as watched in list context X?"); every
// discovery list asks it through RowFilterSettings::hideWatched and re-filters on changed().
// Lists created from QML (channel pages) reach it through the process-wide instance, the same
// way they reach their default service. Main thread only.

#include "Core/Catalog/Domain/RowFilter.h"

#include <QObject>
#include <QString>
#include <functional>

namespace nordtile { namespace video { namespace sailfish {

class WatchedRows final : public QObject {
    Q_OBJECT
public:
    // (videoId, context) -> hidden; context is "feed", "channel" or "search".
    using Predicate = std::function<bool(const QString &videoId, const QString &context)>;

    static WatchedRows *instance();

    // Replaces the predicate (empty = hide nothing) and notifies the lists.
    void setPredicate(const Predicate &predicate);
    bool active() const { return static_cast<bool>(predicate_); }
    // The filter hook for one list context; empty when no predicate is installed.
    std::function<bool(const std::string &)> forContext(const QString &context) const;

    // Coalesces bursts (a watch-state change per progress report) into one changed().
    void notifyChanged();

signals:
    void changed();

private:
    explicit WatchedRows(QObject *parent = nullptr) : QObject(parent) {}
    Predicate predicate_;
    bool pending_ = false;
};

}}}
