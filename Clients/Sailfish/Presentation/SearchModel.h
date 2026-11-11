// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

#include "Core/Catalog/Domain/Video.h"
#include <QAbstractListModel>
#include <QString>
#include <vector>

namespace nordtile { namespace video { namespace resolver { class YtDlpVideoService; } } }

namespace nordtile { namespace video { namespace sailfish {

class SearchModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canLoadMore READ canLoadMore NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)
    Q_PROPERTY(int count READ count NOTIFY stateChanged)
public:
    enum Role { VideoIdRole = Qt::UserRole + 1, TitleRole, ChannelRole, ChannelIdRole, DurationRole,
                DurationSecondsRole, ViewCountRole, ThumbnailRole, LiveRole };
    explicit SearchModel(resolver::YtDlpVideoService *service, QObject *parent = nullptr);
    ~SearchModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return query_; }
    bool busy() const { return operation_ != 0; }
    bool canLoadMore() const { return canLoadMore_ && !busy(); }
    QString errorText() const { return errorText_; }
    int count() const { return static_cast<int>(items_.size()); }

    Q_INVOKABLE void search(const QString &query);
    Q_INVOKABLE void loadMore();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE QVariantMap get(int row) const;

signals:
    void stateChanged();

private:
    void request(int offset);
    resolver::YtDlpVideoService *service_;
    std::vector<catalog::VideoSummary> items_;
    QString query_;
    QString errorText_;
    quint64 operation_ = 0;
    quint64 generation_ = 0;
    bool canLoadMore_ = false;
};

QString formatDuration(qint64 seconds);

}}}
