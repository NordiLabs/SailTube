// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "CoverVideoItem.h"

#include <QMetaObject>

namespace nordtile { namespace player {

const int CoverVideoItem::kFrameIntervalMs;

CoverVideoItem::CoverVideoItem(QQuickItem *parent) : VideoSurfaceItem(parent) {
    setFrameInterval(kFrameIntervalMs);
    setPresenting(false);  // nothing is drawn until the cover is visible and the video wanted
    connect(this, &VideoSurfaceItem::sourceChanged, this, &CoverVideoItem::lookUpCounter);
}

void CoverVideoItem::lookUpCounter() {
    counter_ = QMetaMethod();
    QObject *s = source();
    if (!s) return;
    const int index = s->metaObject()->indexOfMethod("noteCoverFrameDrawn()");
    if (index >= 0) counter_ = s->metaObject()->method(index);
}

void CoverVideoItem::framePresented() {
    ++framesDrawn_;
    emit framesDrawnChanged();
    QObject *s = source();
    if (s && counter_.isValid()) counter_.invoke(s, Qt::DirectConnection);
}

}}
