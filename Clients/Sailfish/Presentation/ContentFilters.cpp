// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ContentFilters.h"

namespace nordtile { namespace video { namespace sailfish {

const char *const ContentFilters::kHideShortsKey = "content/hideShorts";

void ContentFilters::addList(const Getter &get, const Setter &set) {
    if (!get || !set) return;
    List list;
    list.get = get;
    list.set = set;
    lists_.push_back(list);
    apply(list);
}

void ContentFilters::setHideShorts(bool on) {
    if (on == hideShorts_) return;
    hideShorts_ = on;
    for (const List &list : lists_) apply(list);
    emit changed();
}

void ContentFilters::apply(const List &list) const {
    catalog::RowFilterSettings settings = list.get();
    if (settings.hideShorts == hideShorts_) return;   // nothing to write: no re-merge, no reload
    settings.hideShorts = hideShorts_;
    list.set(settings);
}

}}}
