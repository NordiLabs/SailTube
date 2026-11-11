// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "ShortsStore.h"
#include "ChannelService.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSaveFile>
#include <algorithm>
#include <set>

namespace nordtile { namespace video { namespace catalog {

namespace {
std::function<std::int64_t()> wrapClock(const std::function<qint64()> &nowMs) {
    if (!nowMs) return std::function<std::int64_t()>();
    return [nowMs] { return static_cast<std::int64_t>(nowMs()); };
}
}

ShortsStore::ShortsStore(InnerTubeClient *background, const QString &file, QObject *parent, std::function<qint64()> nowMs)
    : QObject(parent), client_(background), file_(file), knowledge_(wrapClock(nowMs)) {
    saveTimer_.setSingleShot(true);
    saveTimer_.setInterval(kSaveDelayMs);
    connect(&saveTimer_, &QTimer::timeout, this, [this] { save(); });
}

ShortsStore::~ShortsStore() {
    if (dirty_) save();
}

bool ShortsStore::load() {
    if (file_.isEmpty()) return false;
    QFile f(file_);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray data = f.readAll();
    return knowledge_.deserialize(std::string(data.constData(), static_cast<std::size_t>(data.size())));
}

bool ShortsStore::save() {
    saveTimer_.stop();
    if (file_.isEmpty()) return false;
    const QFileDevice::Permissions owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
    QDir().mkpath(QFileInfo(file_).absolutePath());
    QSaveFile f(file_);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.setPermissions(owner);
    const std::string data = knowledge_.serialize();
    if (f.write(data.data(), static_cast<qint64>(data.size())) != static_cast<qint64>(data.size())) {
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) return false;
    QFile::setPermissions(file_, owner);
    dirty_ = false;
    return true;
}

void ShortsStore::scheduleSave() {
    dirty_ = true;
    if (!file_.isEmpty() && !saveTimer_.isActive()) saveTimer_.start();
}

void ShortsStore::observe(const std::vector<Item> &rows, bool fromShortsTab) {
    if (knowledge_.observe(rows, fromShortsTab)) {
        scheduleSave();
        emit changed();
    }
}

int ShortsStore::requestChecks(const std::vector<Item> &rows) {
    if (!client_) return 0;
    int queued = 0;
    for (const std::string &channel : knowledge_.channelsNeedingCheck(rows)) {
        const QString id = QString::fromStdString(channel);
        if (queuedIds_.contains(id)) continue;
        std::shared_ptr<Check> check = std::make_shared<Check>();
        check->channelId = id;
        check->candidates = knowledge_.candidatesOf(channel, rows);
        if (check->candidates.empty()) continue;
        // Planned now: the channel is not planned again for 30 min, even if the check fails.
        knowledge_.noteChecked(channel);
        queue_.append(check);
        queuedIds_.append(id);
        ++queued;
    }
    pump();
    return queued;
}

void ShortsStore::pump() {
    while (running_ < kParallelChecks && !queue_.isEmpty()) {
        std::shared_ptr<Check> check = queue_.takeFirst();
        ++running_;
        page(check, QString());
    }
}

void ShortsStore::page(std::shared_ptr<Check> check, const QString &continuation) {
    const bool first = continuation.isEmpty();
    QJsonObject body;
    if (first) {
        body.insert(QStringLiteral("browseId"), check->channelId);
        body.insert(QStringLiteral("params"), tabParams_ ? tabParams_(check->channelId)
                                                         : ChannelService::fallbackTabParams(QStringLiteral("shorts")));
    } else {
        body.insert(QStringLiteral("continuation"), continuation);
    }
    ++check->pages;
    const QString hl = client_->locale().hl;
    const QString channelId = check->channelId;
    std::function<ChannelPage(const InnerTubeReply &)> parse = [first, channelId, hl](const InnerTubeReply &reply) {
        const QString tab = QStringLiteral("shorts");
        return first ? ChannelService::parseBrowse(reply, channelId, tab, hl)
                     : ChannelService::parseContinuation(reply, channelId, tab, hl);
    };
    std::function<void(ChannelPage)> done = [this, check](ChannelPage p) {
        if (p.tabMissing) { finishCheck(check, true); return; }      // no Shorts tab: no Shorts
        if (isFailure(p.outcome)) { finishCheck(check, false); return; }
        for (Item &item : p.items) {
            if (!isVideoLike(item)) continue;
            if (item.channelId.empty()) item.channelId = check->channelId.toStdString();
            check->found.push_back(item.id);
        }
        knowledge_.observe(p.items, true);
        const std::set<std::string> found(check->found.begin(), check->found.end());
        const bool allFound = std::all_of(check->candidates.begin(), check->candidates.end(),
                                          [&found](const std::string &id) { return found.count(id) > 0; });
        if (allFound || p.continuation.isEmpty() || check->pages >= kMaxPagesPerCheck) finishCheck(check, true);
        else page(check, p.continuation);
    };
    client_->post<ChannelPage>(QLatin1String(innertube::kEndpointBrowse), body, this, parse, done);
}

void ShortsStore::finishCheck(std::shared_ptr<Check> check, bool ok) {
    --running_;
    queuedIds_.removeAll(check->channelId);
    int settled = 0;
    if (ok) {
        // Candidates not listed within the walked pages are settled as "not a Short": the
        // row filter only ever hides confirmed Shorts, so a wrong "not a Short" only keeps a
        // row visible, and later structural evidence still overrides it.
        settled = knowledge_.applyShortsTab(check->channelId.toStdString(), check->found, check->candidates);
        checked_.append(check->channelId);
        scheduleSave();
        emit changed();
    }
    emit checkFinished(check->channelId, settled);
    pump();
}

}}}
