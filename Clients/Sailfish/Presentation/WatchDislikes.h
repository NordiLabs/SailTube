// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// The dislike estimate of the playing video (RYD-01) on the watch info: watchInfo.dislikeCount
// and watchInfo.dislikeText, for the like chip.
//
// It follows the watch page's WatchInfo (the context `watchInfo`) and the `dislikes` object:
//  * when the watch data of a video is asked for (WatchInfo::requested: after the hold that
//    keeps discovery away from the first video bytes, or at once for a cached page) the video is
//    handed to DislikeInfo::fetch. DislikeInfo asks the third-party service only while the
//    setting is on (it is off by default); with the setting off no request is made at all.
//    One request per video: the service keeps an answer for ten minutes;
//  * turning the setting on while a video is open asks for that video, turning it off removes
//    the number;
//  * a new video removes the previous number at once; nothing is asked while the video plays
//    offline.
// Nothing is owned.

#include <QObject>
#include <QPointer>

namespace nordtile { namespace video { namespace sailfish {

class DislikeInfo;
class WatchInfo;

class WatchDislikes final : public QObject {
    Q_OBJECT
public:
    WatchDislikes(WatchInfo *watch, DislikeInfo *dislikes, QObject *parent = nullptr);

private:
    void ask(const QString &videoId);
    void publish();
    QPointer<WatchInfo> watch_;
    QPointer<DislikeInfo> dislikes_;
    bool wasOffline_ = false;
};

}}}
