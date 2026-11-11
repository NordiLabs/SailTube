// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The `thumbnailLarge` role of the catalog list models (full-width cards): the 1280x720 picture
// of the row's video (hq720.jpg), or the 480x360 one (hqdefault.jpg, which exists for every
// video) once the 1280x720 one is known to be missing (catalog::ThumbnailBook: the image loader
// was answered 404 for it, or an answer named only smaller pictures for it). Rows whose small
// thumbnail is not a video's (channel avatars, vertical Shorts frames) keep that address.
//
// When a video joins the book this object emits changed() once per event-loop turn on the main
// thread and the models re-announce the role, so a card whose picture failed gets the smaller
// one without any code in the view. Also the QML context property `largeThumbnails`, for lists
// of other components: `largeThumbnails.revision >= 0 ? largeThumbnails.get(model.videoId) : ""`.
// No request is made here; images load through the shared image cache.

#include <QAbstractItemModel>
#include <QObject>
#include <QString>
#include <atomic>
#include <string>

namespace nordtile { namespace video { namespace sailfish {

class LargeThumbnails final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    // Created on first use; call it first on the main thread (the composition and the models do).
    static LargeThumbnails *instance();

    int revision() const;
    // The large picture of a video id ("" for anything that is not a video id).
    Q_INVOKABLE QString get(const QString &videoId) const;

    // The role value for a row whose small thumbnail is `thumbnailUrl`.
    static QString forThumbnail(const std::string &thumbnailUrl);
    // Makes `model` re-announce `role` for all its rows whenever a video joins the book.
    static void follow(QAbstractItemModel *model, int role);

signals:
    void changed();

private slots:
    void flush();

private:
    explicit LargeThumbnails(QObject *parent = nullptr);
    std::atomic<bool> pending_;
};

}}}
