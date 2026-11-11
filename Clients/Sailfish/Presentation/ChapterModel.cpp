// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ChapterModel.h"

namespace nordtile { namespace video { namespace sailfish {

ChapterModel::ChapterModel(QObject *parent) : QAbstractListModel(parent) {}

int ChapterModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : count();
}

QVariant ChapterModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= count()) return QVariant();
    const catalog::ChapterMark &c = chapters_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case TitleRole: return QString::fromStdString(c.title);
    case StartRole: return static_cast<double>(c.startSeconds);
    case EndRole: return static_cast<double>(c.endSeconds);
    case StartTextRole: return QString::fromStdString(catalog::formatTimestamp(c.startSeconds));
    case StartMsRole: return static_cast<double>(c.startSeconds) * 1000.0;
    default: return QVariant();
    }
}

QHash<int, QByteArray> ChapterModel::roleNames() const {
    return {{TitleRole, "title"}, {StartRole, "start"}, {EndRole, "end"}, {StartTextRole, "startText"},
            {StartMsRole, "startMs"}};
}

QString ChapterModel::currentTitle() const {
    if (current_ < 0 || current_ >= count()) return QString();
    return QString::fromStdString(chapters_[static_cast<std::size_t>(current_)].title);
}

void ChapterModel::setChapters(const std::vector<catalog::ChapterMark> &chapters, bool fromResponse) {
    bool same = chapters.size() == chapters_.size() && fromResponse == fromResponse_;
    for (std::size_t i = 0; same && i < chapters.size(); ++i)
        same = chapters[i].title == chapters_[i].title && chapters[i].startSeconds == chapters_[i].startSeconds &&
               chapters[i].endSeconds == chapters_[i].endSeconds;
    if (same) return;
    beginResetModel();
    chapters_ = chapters;
    fromResponse_ = fromResponse;
    endResetModel();
    emit countChanged();
    current_ = -2;   // force the notification below
    setPosition(position_);
}

int ChapterModel::indexAt(double seconds) const {
    return catalog::chapterAt(chapters_, seconds);
}

void ChapterModel::jumpTo(int index) {
    if (index < 0 || index >= count()) return;
    emit jumpRequested(static_cast<double>(chapters_[static_cast<std::size_t>(index)].startSeconds));
}

void ChapterModel::setPosition(double seconds) {
    position_ = seconds;
    const int index = indexAt(seconds);
    if (index == current_) return;
    current_ = index;
    emit currentIndexChanged();
}

QVariantMap ChapterModel::get(int row) const {
    QVariantMap m;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.constBegin(); it != roles.constEnd(); ++it)
        m.insert(QString::fromLatin1(it.value()), data(index(row, 0), it.key()));
    return m;
}

}}}
