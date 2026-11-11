// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CaptionCues.h"
#include "CaptionTracksModel.h"

namespace nordtile { namespace video { namespace sailfish {

CaptionCues::CaptionCues(catalog::CaptionService *service, QObject *parent)
    : QObject(parent), service_(service), cues_(std::make_shared<const std::vector<catalog::CaptionCue>>()) {}

void CaptionCues::follow(CaptionTracksModel *tracks) {
    if (tracks_) disconnect(tracks_, nullptr, this, nullptr);
    tracks_ = tracks;
    if (!tracks) return;
    connect(tracks, &CaptionTracksModel::currentChanged, this, [this] {
        if (tracks_) load(tracks_->currentUrl());
    });
    load(tracks->currentUrl());
}

void CaptionCues::load(const QVariant &track) {
    QString url;
    if (track.type() != QVariant::String && track.canConvert<QVariantMap>())
        url = track.toMap().value(QStringLiteral("url")).toString();
    else url = track.toString();
    if (url == url_ && (ready_ || loading())) return;   // same track: keep cues and cursor

    // The previous request is not cancelled: its answer still fills the session cache (the user
    // may switch back), but it no longer reaches this object.
    requestId_ = 0;
    waiting_ = false;
    const quint64 generation = ++generation_;
    url_ = url;
    cues_ = std::make_shared<const std::vector<catalog::CaptionCue>>();
    cursor_.reset();
    ready_ = false;
    failed_ = false;
    setCurrentText(QString());
    if (url.isEmpty()) { emit stateChanged(); return; }

    const quint64 id = service_->fetchCues(url, this, [this, generation](const catalog::CueResult &r) {
        // A late answer for a track the user already left is dropped.
        if (generation != generation_ || r.url != url_) return;
        requestId_ = 0;
        waiting_ = false;
        lastOutcome_ = r.outcome;
        failed_ = catalog::isFailure(r.outcome);
        cues_ = r.cues;
        ready_ = !failed_;
        emit stateChanged();
        if (position_ >= 0) setPosition(position_);
    });
    requestId_ = id;
    waiting_ = id == 0;
    emit stateChanged();
}

void CaptionCues::clear() { load(QVariant()); }

void CaptionCues::setPosition(qint64 ms) {
    position_ = ms;
    if (!ready_) { setCurrentText(QString()); return; }
    const int i = cursor_.at(*cues_, ms);
    setCurrentText(i < 0 ? QString() : QString::fromStdString((*cues_)[static_cast<std::size_t>(i)].text));
}

QString CaptionCues::textAt(qint64 ms) const {
    if (!ready_) return QString();
    const int i = catalog::cueIndexAt(*cues_, ms);
    return i < 0 ? QString() : QString::fromStdString((*cues_)[static_cast<std::size_t>(i)].text);
}

void CaptionCues::setCurrentText(const QString &text) {
    if (text == currentText_) return;
    currentText_ = text;
    emit currentTextChanged();
}

}}}
