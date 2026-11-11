// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Time-limited beta (docs/BETA.md) with an injected clock: before, at and after the end of the expiry
// day (local time), the 14-day once-a-day banner, the last-3-days banner on every launch, clock rollback
// after expiry, a trusted time ahead of the clock, a missing or corrupt state file, 0600 state.
#include "Core/Beta/Domain/BetaPolicy.h"
#include "Core/Beta/Infrastructure/BetaClock.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>
#include <cstdio>

using namespace nordtile::video::beta;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static qint64 local(int y, int m, int d, int h, int mi, int s = 0) {
    return QDateTime(QDate(y, m, d), QTime(h, mi, s), Qt::LocalTime).toMSecsSinceEpoch();
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    // Domain
    Date d;
    CHECK(parseIsoDate("2026-12-30", &d) && d.year == 2026 && d.month == 12 && d.day == 30);
    CHECK(!parseIsoDate("2026-02-30", &d) && !parseIsoDate("2026-12-3", &d) && !parseIsoDate("", &d) && !parseIsoDate("2026/12/30", &d));
    CHECK(parseIsoDate("2028-02-29", &d) && !parseIsoDate("2027-02-29", &d));
    CHECK(parseIsoDate("2026-12-30", &d) && notLaterThanLimit(d));
    CHECK(parseIsoDate("2026-12-31", &d) && !notLaterThanLimit(d));
    CHECK(effectiveNow(10, 30, 20) == 30 && effectiveNow(40, 30, 20) == 40 && effectiveNow(1, 2, 50) == 50);

    const QDate expiry(2026, 12, 30);
    const qint64 build = local(2026, 11, 8, 12, 0);
    qint64 now = local(2026, 11, 20, 10, 0);
    QTemporaryDir tmp;
    const QString state = tmp.path() + QStringLiteral("/beta/clock");
    {
        BetaClock c(state, expiry, build, [&]() { return now; });
        CHECK(c.expiryEndMs() == local(2026, 12, 30, 23, 59, 59) + 999);
        Evaluation e = c.evaluate();
        CHECK(e.phase == Phase::Active && !e.showBanner && e.daysLeft > 14);
        // 14-day window: once per local day
        now = local(2026, 12, 17, 9, 0);
        e = c.evaluate();
        CHECK(e.phase == Phase::Warning && e.showBanner && e.daysLeft == 14);
        c.dismissBanner();
        CHECK(!c.evaluate().showBanner);                       // dismissed today
        now = local(2026, 12, 17, 22, 0);
        CHECK(!c.evaluate().showBanner);                       // still today
        now = local(2026, 12, 18, 8, 0);
        CHECK(c.evaluate().showBanner);                        // next day: once more
        // last 3 days: every launch, even after dismissing
        now = local(2026, 12, 28, 9, 0);
        c.dismissBanner();
        e = c.evaluate();
        CHECK(e.phase == Phase::FinalDays && e.showBanner && e.daysLeft == 3);
        // the last second of the expiry day still runs; one ms later it has ended
        now = local(2026, 12, 30, 23, 59, 59) + 999;
        CHECK(c.evaluate().phase == Phase::FinalDays && c.evaluate().daysLeft == 0);
        now += 1;
        CHECK(c.evaluate().phase == Phase::Expired);
        c.touch();                                             // the high-water mark records it
        CHECK(c.highWaterMs() == now);
    }
    {   // clock turned back after expiry: still expired (persisted high-water mark)
        now = local(2026, 12, 1, 9, 0);
        BetaClock c(state, expiry, build, [&]() { return now; });
        CHECK(c.evaluate().phase == Phase::Expired);
        CHECK((QFile(state).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0);
    }
    {   // a trusted (HTTPS Date) time ahead of the clock raises the mark, a time behind never lowers it
        QFile::remove(state);
        now = local(2026, 12, 1, 9, 0);
        BetaClock c(state, expiry, build, [&]() { return now; });
        CHECK(c.evaluate().phase == Phase::Active);
        c.observeTrustedTime(local(2026, 12, 31, 1, 0));
        CHECK(c.evaluate().phase == Phase::Expired);
        c.observeTrustedTime(local(2026, 11, 1, 1, 0));
        CHECK(c.evaluate().phase == Phase::Expired);
    }
    {   // a corrupt state file is ignored; the build time still bounds a clock set before the build
        QFile f(state);
        CHECK(f.open(QIODevice::WriteOnly)); f.write("garbage\n"); f.close();
        now = local(2020, 1, 1, 0, 0);
        BetaClock c(state, expiry, build, [&]() { return now; });
        CHECK(c.effectiveNowMs() == build && c.evaluate().phase == Phase::Active);
    }
    {   // missing state file: starts from the system time
        QFile::remove(state);
        now = local(2026, 12, 31, 0, 0, 1);
        BetaClock c(state, expiry, build, [&]() { return now; });
        CHECK(c.evaluate().phase == Phase::Expired);
    }
    CHECK(BetaClock::localDayIndex(local(2000, 1, 2, 12, 0)) == 1);
    CHECK(BetaClock::endOfDayLocalMs(QDate(2026, 12, 30)) == local(2026, 12, 30, 23, 59, 59) + 999);
    if (failures) { std::printf("beta: %d failure(s)\n", failures); return 1; }
    std::printf("beta BetaTest: all checks passed\n");
    return 0;
}
