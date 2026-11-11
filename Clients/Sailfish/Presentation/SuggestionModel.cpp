// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "SuggestionModel.h"
#include "DiscoverySearchModel.h"
#include "Core/Catalog/Infrastructure/SuggestService.h"

#include <QSettings>

namespace nordtile { namespace video { namespace sailfish {

const int SuggestionModel::kDebounceMs;
const int SuggestionModel::kMaxHistory;
const int SuggestionModel::kMaxShown;

namespace {
const char kHistoryKey[] = "catalog/search/history";
const char kPausedKey[] = "catalog/search/historyPaused";
}

SuggestionModel::SuggestionModel(catalog::SuggestService *service, QSettings *settings, QObject *parent)
    : QAbstractListModel(parent), service_(service), settings_(settings) {
    debounce_.setSingleShot(true);
    debounce_.setInterval(kDebounceMs);
    connect(&debounce_, &QTimer::timeout, this, &SuggestionModel::request);
    if (settings_) {
        history_ = settings_->value(QLatin1String(kHistoryKey)).toStringList().mid(0, kMaxHistory);
        historyPaused_ = settings_->value(QLatin1String(kPausedKey), false).toBool();
    }
    showHistory();
}

SuggestionModel::~SuggestionModel() { if (inFlight_ && service_) service_->cancel(inFlight_); }

int SuggestionModel::rowCount(const QModelIndex &parent) const { return parent.isValid() ? 0 : rows_.size(); }

QVariant SuggestionModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) return QVariant();
    if (role == TextRole) return rows_.at(index.row());
    if (role == HistoryRole) return rowsAreHistory_;
    return QVariant();
}

QHash<int, QByteArray> SuggestionModel::roleNames() const {
    QHash<int, QByteArray> r;
    r.insert(TextRole, "text");
    r.insert(HistoryRole, "history");
    return r;
}

QString SuggestionModel::get(int row) const { return row >= 0 && row < rows_.size() ? rows_.at(row) : QString(); }

void SuggestionModel::setQuery(const QString &text) {
    const QString nq = DiscoverySearchModel::normalizedQuery(text);
    if (nq == query_) return;
    query_ = nq;
    ++generation_;          // any reply for an older query is now stale
    emit queryChanged();
    if (nq.isEmpty()) { debounce_.stop(); showHistory(); return; }
    debounce_.start();
}

void SuggestionModel::request() {
    if (!service_ || query_.isEmpty()) return;
    const quint64 generation = generation_;
    const QString asked = query_;
    timing_.start();
    // The earlier request is left to finish: its answer still fills the cache.
    inFlight_ = service_->fetch(asked, [this, generation](const catalog::SuggestResult &r) {
        if (generation != generation_) return;   // stale or suppressed by submit()
        inFlight_ = 0;
        if (catalog::isFailure(r.outcome)) { show(QStringList(), false); return; }
        show(r.suggestions.mid(0, kMaxShown), false);
        if (!r.fromCache) qInfo("sailtube-timing suggest %lld", static_cast<long long>(timing_.elapsed()));
    });
}

void SuggestionModel::submit(const QString &text) {
    const QString nq = DiscoverySearchModel::normalizedQuery(text);
    debounce_.stop();
    ++generation_;
    if (inFlight_ && service_) service_->cancel(inFlight_);
    inFlight_ = 0;
    if (query_ != nq) { query_ = nq; emit queryChanged(); }
    if (!nq.isEmpty() && !historyPaused_) {
        for (int i = history_.size() - 1; i >= 0; --i)
            if (history_.at(i).compare(nq, Qt::CaseInsensitive) == 0) history_.removeAt(i);
        history_.prepend(nq);
        while (history_.size() > kMaxHistory) history_.removeLast();
        saveHistory();
        emit historyChanged();
    }
    show(QStringList(), false);
}

void SuggestionModel::clearHistory() {
    history_.clear();
    saveHistory();
    emit historyChanged();
    if (query_.isEmpty()) showHistory();
}

void SuggestionModel::removeHistory(const QString &entry) {
    if (!history_.removeAll(entry)) return;
    saveHistory();
    emit historyChanged();
    if (query_.isEmpty()) showHistory();
}

void SuggestionModel::setHistoryPaused(bool paused) {
    if (paused == historyPaused_) return;
    historyPaused_ = paused;
    if (settings_) settings_->setValue(QLatin1String(kPausedKey), paused);
    emit historyChanged();
}

void SuggestionModel::show(const QStringList &rows, bool history) {
    if (rows == rows_ && history == rowsAreHistory_) return;
    const int before = rows_.size();
    beginResetModel();
    rows_ = rows;
    rowsAreHistory_ = history;
    endResetModel();
    if (before != rows_.size()) emit countChanged();
}

void SuggestionModel::showHistory() { show(history_.mid(0, kMaxShown), true); }

void SuggestionModel::saveHistory() {
    if (settings_) settings_->setValue(QLatin1String(kHistoryKey), history_);
}

}}}
