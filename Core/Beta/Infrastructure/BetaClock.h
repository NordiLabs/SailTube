// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Beta clock and its private state (Core/Beta/Domain/BetaPolicy.h): a 0600 file in the app's data
// directory holding the high-water mark of every time seen (system time at each launch and hourly, and
// optionally HTTPS Date headers of responses the app already receives) and the last local day the banner
// was dismissed. The file is the only thing the beta writes; it never touches user data.

#include "Core/Beta/Domain/BetaPolicy.h"

#include <QDate>
#include <QString>
#include <functional>

namespace nordtile { namespace video { namespace beta {

class BetaClock {
public:
    using Now = std::function<qint64()>;   // system time, ms since epoch (injected in tests)
    BetaClock(const QString &stateFile, const QDate &expiryDate, qint64 buildMs, Now now = Now());

    qint64 effectiveNowMs() const;
    qint64 expiryEndMs() const { return expiryEndMs_; }   // 23:59:59.999 local on the expiry day
    Evaluation evaluate() const;
    // Raises the high-water mark (never lowers it) and persists it. Call at launch and hourly.
    void touch();
    // A trusted time from a response the app already received (HTTPS Date header).
    void observeTrustedTime(qint64 ms);
    void dismissBanner();                                // stores today's local day
    qint64 highWaterMs() const { return highWater_; }
    static qint64 localDayIndex(qint64 ms);              // days since 2000-01-01 in local time
    static qint64 endOfDayLocalMs(const QDate &date);

private:
    void load();
    void save() const;
    QString path_;
    qint64 expiryEndMs_;
    qint64 buildMs_;
    Now now_;
    qint64 highWater_ = 0;
    qint64 lastBannerDay_ = -1;
};

}}}
