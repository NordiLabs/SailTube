// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// SponsorBlock segments of the current video for the playback component (SB-01 data only; the skip
// decision of SB-02 is the playback component's). `segments` is a list of maps
// {start, end (milliseconds), category, actionType, uuid}, sorted, end > start. An answer for a
// video the session has already left is dropped.

#include "Core/Catalog/Infrastructure/SponsorBlockService.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

namespace nordtile { namespace video { namespace sailfish {

class SponsorSegmentsModel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString videoId READ videoId NOTIFY segmentsChanged)
    Q_PROPERTY(QVariantList segments READ segments NOTIFY segmentsChanged)
    Q_PROPERTY(int count READ count NOTIFY segmentsChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY segmentsChanged)
    Q_PROPERTY(bool failed READ failed NOTIFY segmentsChanged)
    // Milliseconds of the poi_highlight point, or -1 (only when highlights are requested).
    Q_PROPERTY(double highlight READ highlight NOTIFY segmentsChanged)
    Q_PROPERTY(QStringList categories READ categories CONSTANT)
public:
    explicit SponsorSegmentsModel(catalog::SponsorBlockService *service, QObject *parent = nullptr);

    QString videoId() const { return videoId_; }
    QVariantList segments() const { return segments_; }
    int count() const { return segments_.size(); }
    bool loading() const { return loading_; }
    bool failed() const { return failed_; }
    double highlight() const { return highlight_; }
    QStringList categories() const;

    Q_INVOKABLE void fetch(const QString &videoId);
    // DL-07: a downloaded video plays with the segments saved in its record
    // (downloads.localMedia().segments: {category, actionType, start, end} in seconds); no
    // request, and fetch() of the same video is a no-op until another video is set.
    Q_INVOKABLE void setLocal(const QString &videoId, const QVariantList &savedSegments);
    bool local() const { return local_; }
    Q_INVOKABLE void clear();

signals:
    void segmentsChanged();

private:
    catalog::SponsorBlockService *service_;
    QString videoId_;
    QVariantList segments_;
    quint64 generation_ = 0;
    bool loading_ = false;
    bool failed_ = false;
    double highlight_ = -1;
    bool local_ = false;
};

}}}
