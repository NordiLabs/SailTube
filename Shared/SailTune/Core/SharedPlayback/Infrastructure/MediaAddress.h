// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// The one rule for a media address this component will ever fetch (and for a redirect target it will follow): HTTPS on
// a host of the media CDN suffix, default port, no user information, no fragment. Qt Core only.
#include <QString>
#include <QUrl>
#include <string>

namespace nordtile { namespace music { namespace sharedplayback { namespace infrastructure {

inline bool isMediaCdnAddress(const std::string &url) {
    if (url.empty() || url.size() > 32768) return false;
    const QUrl parsed(QString::fromStdString(url), QUrl::StrictMode);
    return parsed.isValid() && parsed.scheme() == QLatin1String("https") && parsed.userInfo().isEmpty() &&
           !parsed.hasFragment() && parsed.port(443) == 443 && parsed.host().toLower().endsWith(QLatin1String(".googlevideo.com")) &&
           !parsed.path().isEmpty();
}

}}}}
