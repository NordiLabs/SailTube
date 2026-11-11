// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Second search tier (DSC-01): the existing yt-dlp flat search ("ytsearch"), adapted to the
// SearchService fallback hook. It covers plain video searches only: a filter that narrows or
// reorders results (sort, upload date, duration, live, channel/playlist type) cannot be passed
// to the flat search, so the hook declines and the first-tier answer stands.

#include "SearchService.h"

namespace nordtile { namespace video { namespace resolver { class YtDlpVideoService; } } }

namespace nordtile { namespace video { namespace catalog {

SearchFallback makeYtDlpSearchFallback(resolver::YtDlpVideoService *service);

// True when the flat search can answer this filter.
bool ytDlpSearchSupports(const SearchFilter &filter);

}}}
