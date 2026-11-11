// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CatalogText.h"

#include "Core/Catalog/Domain/LocalizedText.h"

#include <QDateTime>

namespace nordtile { namespace video { namespace sailfish {

namespace {
QString q(const std::string &s) { return QString::fromStdString(s); }
qint64 n(double v) { return v < 0 ? -1 : static_cast<qint64>(v); }
}

QString CatalogText::views(double c) const { return q(catalog::formatViews(n(c), hl_.toStdString())); }
QString CatalogText::subscribers(double c) const { return q(catalog::formatSubscribers(n(c), hl_.toStdString())); }
QString CatalogText::videos(double c) const { return q(catalog::formatVideoCount(n(c), hl_.toStdString())); }
QString CatalogText::count(double c) const { return q(catalog::formatCompactCount(n(c), hl_.toStdString())); }

QString CatalogText::age(double epochSeconds) const {
    if (epochSeconds < 0) return QString();
    const qint64 now = QDateTime::currentMSecsSinceEpoch() / 1000;
    return q(catalog::formatRelativeAge(qMax<qint64>(0, now - static_cast<qint64>(epochSeconds)), hl_.toStdString()));
}

}}}
