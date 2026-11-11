// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LargeThumbnails.h"

#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/ThumbnailBook.h"

#include <QMetaObject>
#include <QVector>

namespace nordtile { namespace video { namespace sailfish {

using catalog::ThumbnailBook;

namespace {
QString largeOf(const std::string &videoId) {
    if (!ThumbnailBook::isVideoId(videoId)) return QString();
    return QString::fromStdString(ThumbnailBook::instance().largeMissing(videoId) ? catalog::detailThumbnailUrl(videoId)
                                                                                  : catalog::largeThumbnailUrl(videoId));
}
}

LargeThumbnails *LargeThumbnails::instance() {
    static LargeThumbnails *thumbnails = new LargeThumbnails;   // lives for the process
    return thumbnails;
}

LargeThumbnails::LargeThumbnails(QObject *parent) : QObject(parent), pending_(false) {
    // The book changes on loader and parser threads: hop to this object's thread, once per burst.
    ThumbnailBook::instance().setListener([this] {
        if (!pending_.exchange(true)) QMetaObject::invokeMethod(this, "flush", Qt::QueuedConnection);
    });
}

void LargeThumbnails::flush() {
    pending_.store(false);
    emit changed();
}

int LargeThumbnails::revision() const { return static_cast<int>(ThumbnailBook::instance().revision() & 0x7fffffff); }

QString LargeThumbnails::get(const QString &videoId) const { return largeOf(videoId.toStdString()); }

QString LargeThumbnails::forThumbnail(const std::string &thumbnailUrl) {
    const std::string videoId = catalog::thumbnailVideoId(thumbnailUrl);
    return videoId.empty() ? QString::fromStdString(thumbnailUrl) : largeOf(videoId);
}

void LargeThumbnails::follow(QAbstractItemModel *model, int role) {
    connect(instance(), &LargeThumbnails::changed, model, [model, role] {
        const int rows = model->rowCount();
        if (rows > 0) emit model->dataChanged(model->index(0, 0), model->index(rows - 1, 0), QVector<int>() << role);
    });
}

}}}
