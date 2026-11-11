// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LinkRouter.h"
#include <QRegularExpression>

#include <QDebug>
#include <vector>

namespace nordtile { namespace video { namespace sailfish {

LinkRouter::LinkRouter(QObject *parent) : QObject(parent) {}

const char *LinkRouter::kindName(catalog::LinkKind kind) {
    switch (kind) {
    case catalog::LinkKind::Video: return "video";
    case catalog::LinkKind::Playlist: return "playlist";
    case catalog::LinkKind::Channel: return "channel";
    case catalog::LinkKind::Clip: return "clip";
    case catalog::LinkKind::Unsupported: return "unsupported";
    }
    return "?";
}

QStringList LinkRouter::candidates(const QVariant &argument) {
    QStringList out;
    const int type = argument.userType();
    if (type == QMetaType::QString) {
        // A string array written as text ("['https://…']" or '["…"]'): gdbus call, given the
        // three openUrl overloads, sends its argument with the first one's signature (s).
        const QString text = argument.toString().trimmed();
        if (text.startsWith(QLatin1Char('[')) && text.endsWith(QLatin1Char(']'))) {
            static const QRegularExpression quoted(QStringLiteral("'([^']*)'|\"([^\"]*)\""));
            QRegularExpressionMatchIterator it = quoted.globalMatch(text);
            while (it.hasNext()) {
                const QRegularExpressionMatch m = it.next();
                out.append(m.captured(1).isEmpty() ? m.captured(2) : m.captured(1));
            }
            if (!out.isEmpty()) return out;
        }
        out.append(argument.toString());
    } else if (type == QMetaType::QStringList) {
        out = argument.toStringList();
    } else if (type == QMetaType::QVariantList) {
        const QVariantList list = argument.toList();
        for (const QVariant &v : list) {
            if (v.userType() == QMetaType::QString) out.append(v.toString());
        }
    } else if (type == QMetaType::QByteArray) {
        out.append(QString::fromUtf8(argument.toByteArray()));
    }
    return out;
}

void LinkRouter::openUrl(const QVariant &argument) {
    const QStringList list = candidates(argument);
    std::vector<std::string> strings;
    strings.reserve(list.size());
    for (const QString &s : list) strings.push_back(s.toStdString());
    const int chosen = catalog::chooseLinkCandidate(strings);
    catalog::LinkTarget target = catalog::classifyLink(chosen >= 0 ? strings[chosen] : std::string());
    // LNK-03 diagnostics (device steps L1/L2): what arrived and what happens to it. No URL is
    // logged, only its shape.
    qInfo("sailtube-links received type=%s candidates=%d kind=%s ready=%d", argument.typeName() ? argument.typeName() : "?",
          static_cast<int>(list.size()), kindName(target.kind), queue_.ready() ? 1 : 0);
    const std::size_t droppedBefore = queue_.dropped();
    if (!queue_.push(target)) {
        if (queue_.dropped() != droppedBefore) qWarning("sailtube: link queue full, %d link(s) dropped", static_cast<int>(queue_.dropped()));
        else qInfo("sailtube-links queued pending=%d", pending());
        emit readyChanged();
        return;
    }
    route(target);
}

void LinkRouter::setReady(bool ready) {
    if (ready == queue_.ready()) return;
    const std::vector<catalog::LinkTarget> queued = queue_.setReady(ready);
    qInfo("sailtube-links ready=%d replay=%d", ready ? 1 : 0, static_cast<int>(queued.size()));
    emit readyChanged();
    for (const catalog::LinkTarget &target : queued) route(target);
}

void LinkRouter::raise() {
    emit raiseRequested();
}

void LinkRouter::route(const catalog::LinkTarget &target) {
    const QString url = QString::fromStdString(target.url);
    qInfo("sailtube-links route kind=%s", kindName(target.kind));
    switch (target.kind) {
    case catalog::LinkKind::Video:
        emit openVideo(QString::fromStdString(target.videoId), target.startSeconds,
                       QString::fromStdString(target.playlistId), target.index);
        break;
    case catalog::LinkKind::Playlist:
        emit openPlaylist(QString::fromStdString(target.playlistId));
        break;
    case catalog::LinkKind::Channel:
        emit openChannel(QString::fromStdString(target.channelRef));
        break;
    // A clip is recognised (MAY) but needs a network resolve to its video: not routed yet.
    case catalog::LinkKind::Clip:
    case catalog::LinkKind::Unsupported:
        emit unsupported(url);
        break;
    }
    emit raiseRequested();
}

}}}
