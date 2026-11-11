// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Reads the leading index of each adaptive MP4 stream (two small range
// requests) and writes a private on-demand DASH manifest that the media
// pipeline opens as a local file. The manifest holds signed, short-lived URLs:
// it lives in the private cache only for the current playback and is removed
// when playback moves on.

#include "../Domain/StreamSelection.h"
#include <QHash>
#include <QObject>
#include <QString>
#include <QUrl>
#include <functional>

class QNetworkAccessManager;
class QNetworkReply;

namespace nordtile { namespace video { namespace media {

class DashManifestPreparer final : public QObject {
    Q_OBJECT
public:
    using Completion = std::function<void(bool ok, const QUrl &manifest)>;
    DashManifestPreparer(QNetworkAccessManager *network, const QString &directory, QObject *parent = nullptr);
    ~DashManifestPreparer() override;

    quint64 prepare(const StreamCandidate &adaptive, double durationSeconds, Completion);
    void cancel(quint64 id);
    // Deletes every manifest this process or an earlier one left behind.
    void clear();

private:
    struct Job;
    struct Part;
    void request(Job *, Part *, qint64 bytes);
    void partDone(Job *, Part *);
    void finish(quint64 id, bool ok, const QUrl &url);
    QNetworkAccessManager *network_;
    QString directory_;
    quint64 nextId_ = 1;
    QHash<quint64, Job *> jobs_;
};

}}}
