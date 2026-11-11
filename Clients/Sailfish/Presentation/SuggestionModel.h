// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Suggestions under the search field (DSC-11) and the local search history (SHOULD).
//
//  * setQuery() debounces 300 ms; only the latest query's reply is shown (stale replies are
//    dropped) and submit() suppresses replies still in flight.
//  * Failures and timeouts show no suggestions and raise no error.
//  * An empty query lists the history (most recent first). History is clearable and can be
//    paused; it lives in QSettings under catalog/search/ (history, historyPaused).
//  * Each shown network answer logs "sailtube-timing suggest <ms>" (NFR-PERF-09).

#include <QAbstractListModel>
#include <QElapsedTimer>
#include <QStringList>
#include <QTimer>

class QSettings;

namespace nordtile { namespace video { namespace catalog { class SuggestService; } } }

namespace nordtile { namespace video { namespace sailfish {

class SuggestionModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(QString query READ query WRITE setQuery NOTIFY queryChanged)
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool historyPaused READ historyPaused WRITE setHistoryPaused NOTIFY historyChanged)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY historyChanged)
public:
    enum Role { TextRole = Qt::UserRole + 1, HistoryRole };
    static const int kDebounceMs = 300;
    static const int kMaxHistory = 50;
    static const int kMaxShown = 10;

    // `settings` may be null (history kept in memory only).
    SuggestionModel(catalog::SuggestService *service, QSettings *settings = nullptr, QObject *parent = nullptr);
    ~SuggestionModel() override;

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

    QString query() const { return query_; }
    void setQuery(const QString &query);
    int count() const { return rows_.size(); }
    bool historyPaused() const { return historyPaused_; }
    void setHistoryPaused(bool paused);
    int historyCount() const { return history_.size(); }
    QStringList history() const { return history_; }

    // The user ran a search: drop pending and in-flight replies, record the history entry.
    Q_INVOKABLE void submit(const QString &query);
    Q_INVOKABLE void clearHistory();
    Q_INVOKABLE void removeHistory(const QString &entry);
    Q_INVOKABLE QString get(int row) const;

signals:
    void queryChanged();
    void countChanged();
    void historyChanged();

private:
    void request();
    void show(const QStringList &rows, bool history);
    void showHistory();
    void saveHistory();

    catalog::SuggestService *service_;
    QSettings *settings_;
    QTimer debounce_;
    QString query_;
    QStringList rows_;
    bool rowsAreHistory_ = false;
    QStringList history_;
    bool historyPaused_ = false;
    quint64 inFlight_ = 0;
    quint64 generation_ = 0;
    QElapsedTimer timing_;
};

}}}
