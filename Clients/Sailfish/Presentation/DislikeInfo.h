// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Return YouTube Dislike for the video page (RYD-01, MAY). Opt-in: the setting
// catalog/ryd/enabled defaults to false, and while it is off nothing is requested. The number is
// labelled as an estimate and the page shows the third-party disclosure.

#include "Core/Catalog/Infrastructure/DislikeService.h"

#include <QObject>
#include <QString>

class QSettings;

namespace nordtile { namespace video { namespace sailfish {

class DislikeInfo final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool enabled READ enabled WRITE setEnabled NOTIFY enabledChanged)
    Q_PROPERTY(QString videoId READ videoId NOTIFY changed)
    Q_PROPERTY(bool loading READ loading NOTIFY changed)
    Q_PROPERTY(bool available READ available NOTIFY changed)
    // Estimated dislikes, or -1 when unknown.
    Q_PROPERTY(double estimate READ estimate NOTIFY changed)
    Q_PROPERTY(QString estimateLabel READ estimateLabel CONSTANT)
    Q_PROPERTY(QString disclosure READ disclosure CONSTANT)
public:
    static const char *const kSettingsKey;   // "catalog/ryd/enabled"

    // `settings` (not owned) is for tests; null uses the application QSettings.
    explicit DislikeInfo(catalog::DislikeService *service, QSettings *settings = nullptr, QObject *parent = nullptr);
    ~DislikeInfo() override;

    bool enabled() const { return enabled_; }
    void setEnabled(bool on);
    QString videoId() const { return videoId_; }
    bool loading() const { return loading_; }
    bool available() const { return estimate_ >= 0; }
    double estimate() const { return estimate_; }
    QString estimateLabel() const;
    QString disclosure() const;

    // Remembers the video; requests only while enabled.
    Q_INVOKABLE void fetch(const QString &videoId);
    Q_INVOKABLE void clear();

signals:
    void enabledChanged();
    void changed();

private:
    void request();
    catalog::DislikeService *service_;
    QSettings *settings_;
    bool ownSettings_;
    bool enabled_ = false;
    QString videoId_;
    quint64 generation_ = 0;
    bool loading_ = false;
    double estimate_ = -1;
};

}}}
