// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DislikeInfo.h"
#include "SettingsFile.h"
#include "Core/Catalog/Domain/Video.h"

#include <QCoreApplication>
#include <QSettings>

namespace nordtile { namespace video { namespace sailfish {

const char *const DislikeInfo::kSettingsKey = "catalog/ryd/enabled";

DislikeInfo::DislikeInfo(catalog::DislikeService *service, QSettings *settings, QObject *parent)
    : QObject(parent), service_(service), settings_(settings ? settings : new QSettings(appSettingsFile(), QSettings::IniFormat)), ownSettings_(!settings) {
    enabled_ = settings_->value(QLatin1String(kSettingsKey), false).toBool();
    service_->setEnabled(enabled_);
}

DislikeInfo::~DislikeInfo() {
    if (ownSettings_) delete settings_;
}

QString DislikeInfo::estimateLabel() const {
    return QCoreApplication::translate("DislikeInfo", "Estimated dislikes");
}

QString DislikeInfo::disclosure() const {
    return QCoreApplication::translate("DislikeInfo",
        "Dislike counts come from the third-party Return YouTube Dislike service and are estimates.");
}

void DislikeInfo::setEnabled(bool on) {
    if (on == enabled_) return;
    enabled_ = on;
    settings_->setValue(QLatin1String(kSettingsKey), on);
    service_->setEnabled(on);
    ++generation_;
    loading_ = false;
    estimate_ = -1;
    emit enabledChanged();
    if (on) request();
    emit changed();
}

void DislikeInfo::fetch(const QString &videoId) {
    if (videoId == videoId_ && (loading_ || available())) return;
    ++generation_;
    videoId_ = videoId;
    loading_ = false;
    estimate_ = -1;
    request();
    emit changed();
}

void DislikeInfo::clear() {
    ++generation_;
    videoId_.clear();
    loading_ = false;
    estimate_ = -1;
    emit changed();
}

void DislikeInfo::request() {
    if (!enabled_ || videoId_.isEmpty()) return;
    const quint64 generation = generation_;
    loading_ = true;
    const quint64 id = service_->fetch(videoId_, this, [this, generation](const catalog::DislikeResult &r) {
        if (generation != generation_ || r.videoId != videoId_) return;
        loading_ = false;
        estimate_ = r.outcome == catalog::Outcome::Ok && r.votes.dislikes >= 0 ? static_cast<double>(r.votes.dislikes) : -1;
        emit changed();
    });
    // 0 with no callback: invalid id (a cache hit also returns 0 but answers asynchronously).
    if (id == 0 && !catalog::validVideoId(videoId_.toStdString())) loading_ = false;
}

}}}
