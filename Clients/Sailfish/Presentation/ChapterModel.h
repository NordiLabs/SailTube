// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Chapter list with jump-to (DSC-15). Rows: title, start/end (seconds), startText. The view
// calls jumpTo(index); the model emits jumpRequested(seconds) and the page seeks the player
// (PlaybackSession::seek takes milliseconds). setPosition() keeps currentIndex in step with
// playback.

#include "Core/Catalog/Domain/Chapters.h"

#include <QAbstractListModel>
#include <QVariantList>
#include <vector>

namespace nordtile { namespace video { namespace sailfish {

class ChapterModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
    Q_PROPERTY(QString currentTitle READ currentTitle NOTIFY currentIndexChanged)
    Q_PROPERTY(bool fromResponse READ fromResponse NOTIFY countChanged)
public:
    enum Role { TitleRole = Qt::UserRole + 1, StartRole, EndRole, StartTextRole, StartMsRole };
    explicit ChapterModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    int count() const { return static_cast<int>(chapters_.size()); }
    int currentIndex() const { return current_; }
    QString currentTitle() const;
    bool fromResponse() const { return fromResponse_; }

    // Replaces the list (no reset when unchanged, so an open view keeps its position).
    void setChapters(const std::vector<catalog::ChapterMark> &chapters, bool fromResponse);
    const std::vector<catalog::ChapterMark> &chapters() const { return chapters_; }

    Q_INVOKABLE int indexAt(double seconds) const;
    Q_INVOKABLE void jumpTo(int index);
    Q_INVOKABLE void setPosition(double seconds);
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void countChanged();
    void currentIndexChanged();
    void jumpRequested(double seconds);

private:
    std::vector<catalog::ChapterMark> chapters_;
    bool fromResponse_ = false;
    int current_ = -1;
    double position_ = 0;
};

}}}
