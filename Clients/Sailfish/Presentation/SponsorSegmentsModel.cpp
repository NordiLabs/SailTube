// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SponsorSegmentsModel.h"

#include <QVariantMap>

namespace nordtile { namespace video { namespace sailfish {

SponsorSegmentsModel::SponsorSegmentsModel(catalog::SponsorBlockService *service, QObject *parent)
    : QObject(parent), service_(service) {}

QStringList SponsorSegmentsModel::categories() const {
    QStringList out;
    for (const std::string &c : catalog::sponsorCategories()) out << QString::fromStdString(c);
    return out;
}

void SponsorSegmentsModel::fetch(const QString &videoId) {
    if (local_ && videoId == videoId_) return;   // offline: the saved segments stand
    if (videoId == videoId_ && (loading_ || (!failed_ && !videoId_.isEmpty()))) return;
    clear();
    if (videoId.isEmpty()) return;
    videoId_ = videoId;
    loading_ = true;
    const quint64 generation = generation_;
    service_->fetch(videoId, this, [this, generation](const catalog::SponsorResult &r) {
        if (generation != generation_ || r.videoId != videoId_) return;   // the session moved on
        loading_ = false;
        failed_ = catalog::isFailure(r.outcome);
        segments_.clear();
        for (const catalog::SponsorSegment &s : r.list.segments) {
            QVariantMap m;
            m.insert(QStringLiteral("start"), s.startSeconds * 1000.0);
            m.insert(QStringLiteral("end"), s.endSeconds * 1000.0);
            m.insert(QStringLiteral("category"), QString::fromStdString(s.category));
            m.insert(QStringLiteral("actionType"), QString::fromStdString(s.actionType));
            m.insert(QStringLiteral("uuid"), QString::fromStdString(s.uuid));
            segments_.append(m);
        }
        highlight_ = r.list.hasHighlight ? r.list.highlightSeconds * 1000.0 : -1;
        emit segmentsChanged();
    });
    emit segmentsChanged();
}

void SponsorSegmentsModel::setLocal(const QString &videoId, const QVariantList &savedSegments) {
    clear();
    if (videoId.isEmpty()) return;
    videoId_ = videoId;
    local_ = true;
    for (const QVariant &v : savedSegments) {
        const QVariantMap s = v.toMap();
        const double start = s.value(QStringLiteral("start")).toDouble() * 1000.0;
        const double end = s.value(QStringLiteral("end")).toDouble() * 1000.0;
        const QString category = s.value(QStringLiteral("category")).toString();
        if (category == QLatin1String("poi_highlight")) {
            if (highlight_ < 0) highlight_ = start;
            continue;
        }
        if (category.isEmpty() || end <= start) continue;
        QVariantMap m;
        m.insert(QStringLiteral("start"), start);
        m.insert(QStringLiteral("end"), end);
        m.insert(QStringLiteral("category"), category);
        m.insert(QStringLiteral("actionType"), s.value(QStringLiteral("actionType"), QStringLiteral("skip")).toString());
        m.insert(QStringLiteral("uuid"), QString());
        segments_.append(m);
    }
    emit segmentsChanged();
}

void SponsorSegmentsModel::clear() {
    ++generation_;
    local_ = false;
    const bool changed = !videoId_.isEmpty() || !segments_.isEmpty() || loading_ || failed_;
    videoId_.clear();
    segments_.clear();
    loading_ = false;
    failed_ = false;
    highlight_ = -1;
    if (changed) emit segmentsChanged();
}

}}}
