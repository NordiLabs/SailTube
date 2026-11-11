// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "BetaClock.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

namespace nordtile { namespace video { namespace beta {

BetaClock::BetaClock(const QString &stateFile, const QDate &expiryDate, qint64 buildMs, Now now)
    : path_(stateFile), expiryEndMs_(endOfDayLocalMs(expiryDate)), buildMs_(buildMs),
      now_(now ? now : []() { return QDateTime::currentMSecsSinceEpoch(); }) {
    load();
}

qint64 BetaClock::endOfDayLocalMs(const QDate &date) {
    return QDateTime(date, QTime(23, 59, 59, 999), Qt::LocalTime).toMSecsSinceEpoch();
}

qint64 BetaClock::localDayIndex(qint64 ms) {
    return QDate(2000, 1, 1).daysTo(QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().date());
}

qint64 BetaClock::effectiveNowMs() const { return effectiveNow(now_(), highWater_, buildMs_); }

Evaluation BetaClock::evaluate() const {
    const qint64 now = effectiveNowMs();
    return beta::evaluate(now, expiryEndMs_, localDayIndex(now), lastBannerDay_);
}

void BetaClock::touch() {
    const qint64 now = effectiveNowMs();
    if (now > highWater_) { highWater_ = now; save(); }
}

void BetaClock::observeTrustedTime(qint64 ms) {
    if (ms > highWater_ && ms > 0) { highWater_ = ms; save(); }
}

void BetaClock::dismissBanner() {
    lastBannerDay_ = localDayIndex(effectiveNowMs());
    save();
}

void BetaClock::load() {
    QFile f(path_);
    if (!f.open(QIODevice::ReadOnly)) return;          // missing: start from the system and build time
    const QList<QByteArray> fields = f.readAll().trimmed().split(' ');
    bool ok = false;
    const qint64 hw = fields.value(0).toLongLong(&ok);
    if (ok && hw > 0) highWater_ = hw;                 // corrupt: ignored, the build time still applies
    const qint64 day = fields.value(1).toLongLong(&ok);
    if (ok) lastBannerDay_ = day;
}

void BetaClock::save() const {
    QDir().mkpath(QFileInfo(path_).absolutePath());
    QSaveFile f(path_);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    f.write(QByteArray::number(highWater_) + ' ' + QByteArray::number(lastBannerDay_) + '\n');
    if (f.commit()) QFile::setPermissions(path_, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

}}}
