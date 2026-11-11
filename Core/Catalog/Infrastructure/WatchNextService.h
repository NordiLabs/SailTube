// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Watch page data (DSC-13 related, DSC-14 video info, DSC-15 chapter markers, DSC-16 first
// comments token, I18N-02 structured fields).
//
// Per video, one `POST /youtubei/v1/next {videoId, contentCheckOk, racyCheckOk}` on the
// *interactive* discovery transport, and in parallel one WEB `POST /youtubei/v1/player` for the
// structured fields (videoDetails.viewCount / lengthSeconds, microformat publishDate,
// uploadDate, likeCount). The WEB player answer is usually UNPLAYABLE for anonymous WEB
// clients, but its videoDetails and microformat are present; it is never used for playback and
// this service never touches the playback component. Both are fire-and-forget: the watch page
// starts them next to PlaybackSession::open and fills in whatever arrives first.
//
// One answer serves related rows, the info line, the description, chapters and the comments
// token: cached per video for 10 min, 32 entries, single-flight (a second caller joins the
// request in flight). Related rows follow the continuation (`POST /next {continuation}`), so
// 60+ rows are reachable; the seed video is excluded and filterRows applies (Shorts /
// members-only / restricted / duplicates across pages).
//
// Outcomes (DSC-01): HTTP failures are what they are; ShapeDrift only when the container
// markers are missing (twoColumnWatchNextResults, secondaryResults,
// appendContinuationItemsAction, videoDetails/playabilityStatus); present-but-empty is
// ValidEmpty. Every call is counted under Area::WatchNext.

#include "Core/Catalog/Domain/Chapters.h"
#include "Core/Catalog/Domain/Description.h"
#include "Core/Catalog/Domain/Item.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/RowFilter.h"
#include "Core/Catalog/Domain/WatchCache.h"

#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

class InnerTubeClient;

// The info line and description of one video. -1 / empty = unknown; raw text is kept when a
// value did not parse (I18N-02).
struct WatchInfoData {
    std::string videoId;
    std::string title;
    std::int64_t viewCount = -1;
    std::string viewText;
    bool viewsStructured = false;      // from videoDetails.viewCount
    std::int64_t likeCount = -1;
    std::string likeText;
    bool likesStructured = false;      // from microformat likeCount
    std::int64_t publishedEpoch = -1;  // UTC midnight of the publication day
    std::string dateText;              // raw date text ("Dec 12, 2024", "12.12.2024")
    bool dateStructured = false;       // from microformat publishDate / uploadDate
    std::int64_t publishedAgeSeconds = -1;
    std::string relativeDateText;
    std::string ownerName;
    std::string ownerId;               // UC id
    std::string ownerHandle;           // "@name" when known
    std::string ownerAvatar;
    std::int64_t subscriberCount = -1;
    std::string subscriberText;
    std::int64_t durationSeconds = -1; // videoDetails.lengthSeconds
    bool live = false;
    // RES-19: a stream or premiere that has not started, and its scheduled start (seconds since
    // the epoch, -1 unknown), from the player answer.
    bool upcoming = false;
    std::int64_t scheduledStartEpoch = -1;
    std::string description;           // plain text
    std::vector<DescriptionRun> descriptionRuns;
};

// What the watch-next answer yields (parsed on the lane).
struct NextParse {
    Outcome outcome = Outcome::Network;          // the call as a whole (counted)
    Outcome infoOutcome = Outcome::Network;
    Outcome relatedOutcome = Outcome::Network;
    WatchInfoData info;
    std::vector<ChapterMark> markers;            // response chapter markers
    std::vector<Item> related;                   // unfiltered rows in order
    QString relatedContinuation;
    QString commentsContinuation;                // first token of comment-item-section (DSC-16)
    QMap<QString, int> unknownRenderers;
};

// What the WEB player answer yields (structured fields only).
struct PlayerParse {
    Outcome outcome = Outcome::Network;
    std::string playabilityStatus;               // "OK", "UNPLAYABLE", "ERROR", ...
    std::string title;
    std::string author;
    std::string channelId;
    std::int64_t viewCount = -1;
    std::int64_t lengthSeconds = -1;
    std::int64_t likeCount = -1;
    std::int64_t publishedEpoch = -1;
    std::string publishDate;                     // "YYYY-MM-DD" part as supplied
    bool live = false;
    bool upcoming = false;                       // videoDetails.isUpcoming, or an offline slate with a start
    std::int64_t scheduledStartEpoch = -1;       // seconds; the offline slate's, else the microformat's
    std::string shortDescription;
};

// One related continuation page.
struct RelatedParse {
    Outcome outcome = Outcome::Network;
    std::vector<Item> items;
    QString continuation;
    QMap<QString, int> unknownRenderers;
};

// Everything known about one video's watch page (the cache value).
struct WatchPage {
    std::string videoId;
    std::string hl;
    bool nextPending = false;
    bool playerPending = false;
    bool relatedPending = false;
    bool nextDone = false;
    bool playerDone = false;
    NextParse next;
    PlayerParse player;
    WatchInfoData info;                          // next merged with the player's structured fields
    std::vector<Item> related;                   // filtered, seed excluded, no duplicates, in order
    std::string relatedContinuation;
    Outcome relatedPageOutcome = Outcome::Ok;    // last continuation page
    int relatedPages = 0;                        // pages merged (1 = the watch-next page)
    int hiddenShorts = 0, hiddenMembersOnly = 0, hiddenRestricted = 0, duplicates = 0, seedsExcluded = 0;
    std::set<std::string> seen;
    QString commentsContinuation;

    bool loading() const { return nextPending || playerPending; }
    // Response markers, else the description rebuild (DSC-15).
    std::vector<ChapterMark> chapters(std::int64_t durationHint = -1, bool *fromResponse = nullptr) const;
};

class WatchNextService final : public QObject {
    Q_OBJECT
public:
    // `client` must use the interactive discovery transport.
    WatchNextService(InnerTubeClient *client, CatalogCounters *counters = nullptr, QObject *parent = nullptr,
                     std::function<qint64()> nowMs = std::function<qint64()>());
    ~WatchNextService() override;

    // Applies to pages loaded from now on (a cached page keeps the rows it was filtered to).
    void setRowFilterSettings(const RowFilterSettings &settings) { filter_ = settings; }
    const RowFilterSettings &rowFilterSettings() const { return filter_; }
    void setShortsLookup(const ShortsLookup &lookup) { shorts_ = lookup; }
    // Off for tests that only exercise watch-next.
    void setPlayerEnabled(bool on) { playerEnabled_ = on; }

    // Starts (or joins) the watch-next and WEB player requests for `videoId`; nothing is sent
    // when a fresh page is cached or a request is in flight. Never blocks. Returns false for an
    // invalid id.
    bool request(const QString &videoId);
    // The cached page (possibly partial while loading), or null.
    std::shared_ptr<const WatchPage> page(const QString &videoId) const;
    // Follows the related continuation; false when there is none or one is in flight.
    bool loadMoreRelated(const QString &videoId);
    QString commentsContinuation(const QString &videoId) const;
    void forget(const QString &videoId);

    // Parsers (pure; run on the lane and from tests).
    static NextParse parseNext(const QJsonObject &json, const QString &hl, const QString &videoId);
    static PlayerParse parsePlayer(const QJsonObject &json, const QString &hl);
    static RelatedParse parseRelatedPage(const QJsonObject &json, const QString &hl);
    // next + player -> the info shown (structured fields win, I18N-02).
    static WatchInfoData mergeInfo(const NextParse *next, const PlayerParse *player);
    // "2024-12-12T07:00:04-08:00" -> UTC midnight of 2024-12-12; -1 when not a date.
    static std::int64_t isoDateEpoch(const QString &text);
    // "2026-11-11T02:00:00+00:00" -> that instant in seconds since the epoch; -1 when not one.
    static std::int64_t isoInstantEpoch(const QString &text);
    // The longest grouped number in a label ("along with 219,810 other people" -> 219810).
    static std::int64_t largestNumber(const QString &text, const std::string &hl);

signals:
    // Info, description, markers or the comments token changed (watch-next or player arrived).
    void pageChanged(const QString &videoId);
    // Related rows were appended (or the related state changed).
    void relatedChanged(const QString &videoId);

private:
    std::shared_ptr<WatchPage> entry(const QString &videoId);
    void startNext(const std::shared_ptr<WatchPage> &page);
    void startPlayer(const std::shared_ptr<WatchPage> &page);
    void nextArrived(const std::string &key, NextParse parse);
    void playerArrived(const std::string &key, PlayerParse parse);
    void relatedArrived(const std::string &key, RelatedParse parse);
    void appendRelated(WatchPage &page, const std::vector<Item> &rows);
    void updatePin(const std::string &key);
    void count(Outcome o, const QMap<QString, int> &unknown = QMap<QString, int>());

    InnerTubeClient *client_;
    CatalogCounters *counters_;
    std::function<qint64()> now_;
    TtlLruCache<WatchPage> cache_;
    RowFilterSettings filter_;
    ShortsLookup shorts_;
    bool playerEnabled_ = true;
};

}}}
