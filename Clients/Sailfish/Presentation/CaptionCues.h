// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The cues of the chosen caption track (SEL-06) for the playback component's overlay: load(track)
// fetches through CaptionService (session cache), setPosition(ms) moves a forward cursor and
// updates `currentText`, textAt(ms) is a stateless lookup. An answer for a track the user has
// already left is dropped.

#include "Core/Catalog/Infrastructure/CaptionService.h"

#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariant>

namespace nordtile { namespace video { namespace sailfish {

class CaptionTracksModel;

class CaptionCues final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY stateChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY stateChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY stateChanged)
    Q_PROPERTY(QString url READ url NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
    Q_PROPERTY(QString currentText READ currentText NOTIFY currentTextChanged)
public:
    explicit CaptionCues(catalog::CaptionService *service, QObject *parent = nullptr);

    bool ready() const { return ready_; }
    bool loading() const { return requestId_ != 0 || waiting_; }
    bool failed() const { return failed_; }
    QString url() const { return url_; }
    int count() const { return static_cast<int>(cues_->size()); }
    QString currentText() const { return currentText_; }
    catalog::Outcome lastOutcome() const { return lastOutcome_; }
    const std::vector<catalog::CaptionCue> &cues() const { return *cues_; }

    // Follows a tracks model: every current-track change loads (or clears) the cues.
    void follow(CaptionTracksModel *tracks);

    // `track`: a map with "url" (CaptionTracksModel::currentTrack()) or a URL string. An empty
    // value clears.
    Q_INVOKABLE void load(const QVariant &track);
    Q_INVOKABLE void clear();
    Q_INVOKABLE void setPosition(qint64 ms);
    Q_INVOKABLE QString textAt(qint64 ms) const;

signals:
    void stateChanged();
    void currentTextChanged();

private:
    void setCurrentText(const QString &text);
    catalog::CaptionService *service_;
    QPointer<CaptionTracksModel> tracks_;
    QString url_;
    catalog::CueList cues_;
    catalog::CueCursor cursor_;
    quint64 requestId_ = 0;
    quint64 generation_ = 0;
    bool waiting_ = false;    // a cache answer is on its way
    bool ready_ = false;
    bool failed_ = false;
    catalog::Outcome lastOutcome_ = catalog::Outcome::ValidEmpty;
    qint64 position_ = -1;
    QString currentText_;
};

}}}
