// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The app-wide content switches of the discovery lists (SET-01). "Hide Shorts"
// (content/hideShorts) is one setting for every list. The composition registers:
//  * the Subscriptions feed (FeedService: the cached feed is merged again at once, offline),
//  * Home, popular or recommended (HomeService: the list is loaded again with it),
//  * related videos (WatchNextService: from the next watch page on).
// Search and the channel page take the same setting in QML (searchModel.hideShorts, set by the
// shell, which the search filter page can still change for the session; ChannelModel.hideShorts,
// set per page). The Shorts section and playlists are never filtered.
//
// Only the hiding switch is written into a list's filter; its other fields (the library's
// hide-watched hook of the feed) stay as they are. Main thread.

#include "Core/Catalog/Domain/RowFilter.h"

#include <QObject>
#include <functional>
#include <vector>

namespace nordtile { namespace video { namespace sailfish {

class ContentFilters final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool hideShorts READ hideShorts WRITE setHideShorts NOTIFY changed)
public:
    static const char *const kHideShortsKey;   // "content/hideShorts"

    typedef std::function<catalog::RowFilterSettings()> Getter;
    typedef std::function<void(const catalog::RowFilterSettings &)> Setter;

    explicit ContentFilters(QObject *parent = nullptr) : QObject(parent) {}

    // Registers a list by the accessors of its row filter; the current switch is applied to it
    // at once. The accessors must stay valid for this object's lifetime.
    void addList(const Getter &get, const Setter &set);
    int lists() const { return static_cast<int>(lists_.size()); }

    bool hideShorts() const { return hideShorts_; }
    void setHideShorts(bool on);

signals:
    void changed();

private:
    struct List {
        Getter get;
        Setter set;
    };
    void apply(const List &list) const;
    std::vector<List> lists_;
    bool hideShorts_ = false;
};

}}}
