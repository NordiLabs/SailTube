// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Persistence of the Shorts knowledge (DSC-17) and the background Shorts-tab check.
//
//  * The stores (ShortsKnowledge) are written atomically (QSaveFile, mode 0600), debounced
//    after changes and on destruction. A missing or unreadable file starts empty.
//  * requestChecks(rows) plans the channels whose rows hold candidates (180 s or less without
//    structural evidence, not checked in the last 30 min) and walks each channel's Shorts tab on
//    the *background* client (never the interactive transport, RES-02): at most 2 channels at a
//    time and 5 pages per channel, stopping early once every candidate was found. Listed
//    candidates become Shorts; the rest become "checked, not a Short". A channel without a
//    Shorts tab settles all its candidates as not Shorts. A failed check settles nothing.
//  * changed() tells lists to re-apply the row filter.

#include "InnerTubeClient.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/ShortsKnowledge.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <functional>
#include <memory>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class ShortsStore final : public QObject {
    Q_OBJECT
public:
    static const int kParallelChecks = 2;
    static const int kMaxPagesPerCheck = 5;
    static const int kSaveDelayMs = 2000;

    // `background` may be null (no checks). `file` may be empty (no persistence).
    ShortsStore(InnerTubeClient *background, const QString &file, QObject *parent = nullptr,
                std::function<qint64()> nowMs = std::function<qint64()>());
    ~ShortsStore() override;

    ShortsKnowledge &knowledge() { return knowledge_; }
    const ShortsKnowledge &knowledge() const { return knowledge_; }
    ShortsLookup lookup() const { return knowledge_.lookup(); }
    QString file() const { return file_; }

    bool load();
    bool save();

    // Learns from a listing (debounced save; changed() when anything new was learnt).
    void observe(const std::vector<Item> &rows, bool fromShortsTab = false);
    // Plans and queues Shorts-tab checks for the channels of `rows` that hold candidates.
    // Returns the number of channels queued.
    int requestChecks(const std::vector<Item> &rows);

    // The Shorts-tab params of a channel (the channel service's knowledge); fallback otherwise.
    void setTabParamsLookup(std::function<QString(const QString &channelId)> lookup) { tabParams_ = std::move(lookup); }

    int pendingChecks() const { return queue_.size() + running_; }
    int runningChecks() const { return running_; }
    QStringList checkedChannels() const { return checked_; }

signals:
    void changed();
    void checkFinished(const QString &channelId, int settled);

private:
    struct Check {
        QString channelId;
        std::vector<std::string> candidates;
        std::vector<std::string> found;
        int pages = 0;
    };
    void pump();
    void page(std::shared_ptr<Check> check, const QString &continuation);
    void finishCheck(std::shared_ptr<Check> check, bool ok);
    void scheduleSave();
    InnerTubeClient *client_;
    QString file_;
    ShortsKnowledge knowledge_;
    std::function<QString(const QString &)> tabParams_;
    QList<std::shared_ptr<Check>> queue_;
    QStringList queuedIds_;
    QStringList checked_;
    int running_ = 0;
    bool dirty_ = false;
    QTimer saveTimer_;
};

}}}
