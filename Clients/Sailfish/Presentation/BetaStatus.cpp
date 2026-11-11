// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "BetaStatus.h"

#include <QLocale>

namespace nordtile { namespace video { namespace sailfish {

BetaStatus::BetaStatus(beta::BetaClock *clock, const QDate &expiryDate, const QString &buildText, QObject *parent)
    : QObject(parent), clock_(clock), expiryText_(QLocale().toString(expiryDate, QStringLiteral("d MMM yyyy"))),
      buildText_(buildText) {
    clock_->touch();
    evaluation_ = clock_->evaluate();
    hourly_.setInterval(3600 * 1000);
    connect(&hourly_, &QTimer::timeout, this, [this]() { clock_->touch(); refresh(); });
    hourly_.start();
}

void BetaStatus::refresh() {
    const beta::Evaluation e = clock_->evaluate();
    const bool differs = e.phase != evaluation_.phase || e.daysLeft != evaluation_.daysLeft || e.showBanner != evaluation_.showBanner;
    evaluation_ = e;
    if (differs) emit changed();
}

void BetaStatus::dismissBanner() {
    clock_->dismissBanner();
    dismissed_ = true;
    emit changed();
}

}}}
