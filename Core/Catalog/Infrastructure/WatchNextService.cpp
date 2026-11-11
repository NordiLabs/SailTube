// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "WatchNextService.h"
#include "InnerTubeClient.h"
#include "Json.h"
#include "RendererParser.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include "Core/Catalog/Domain/Images.h"
#include "Core/Catalog/Domain/TextParse.h"

#include <QDate>
#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <QVariant>
#include <algorithm>

namespace nordtile { namespace video { namespace catalog {

namespace {

std::string s(const QString &q) { return q.toStdString(); }
QString qs(const std::string &v) { return QString::fromStdString(v); }

qint64 monotonicMs() {
    static QElapsedTimer timer;
    if (!timer.isValid()) timer.start();
    return timer.elapsed();
}

qint64 int64Field(const QJsonValue &v) {
    if (v.isDouble()) return static_cast<qint64>(v.toDouble());
    if (v.isString()) {
        bool ok = false;
        const qint64 n = v.toString().toLongLong(&ok);
        return ok ? n : -1;
    }
    return -1;
}

std::vector<ImageCandidate> imageCandidates(const QJsonValue &holder) {
    std::vector<ImageCandidate> out;
    QJsonArray list = holder.toObject().value(QStringLiteral("thumbnails")).toArray();
    if (list.isEmpty()) list = holder.toObject().value(QStringLiteral("sources")).toArray();
    for (const QJsonValue &v : list) {
        const QJsonObject o = v.toObject();
        out.push_back(ImageCandidate(s(o.value(QStringLiteral("url")).toString()), o.value(QStringLiteral("width")).toInt(),
                                     o.value(QStringLiteral("height")).toInt()));
    }
    return out;
}

// One marked description piece -> a run, by the command behind it.
DescriptionRun runForCommand(const QJsonObject &cmd, const QString &piece, const QString &videoId) {
    DescriptionRun r(RunKind::Text, s(piece));
    const QJsonObject watch = cmd.value(QStringLiteral("watchEndpoint")).toObject();
    if (!watch.isEmpty()) {
        const QString vid = watch.value(QStringLiteral("videoId")).toString();
        qint64 t = int64Field(watch.value(QStringLiteral("startTimeSeconds")));
        if (vid.isEmpty() || vid == videoId) {
            if (t < 0) t = parseTimestamp(s(piece.trimmed()));
            r.kind = RunKind::Timestamp;
            r.seekSeconds = t > 0 ? t : 0;
            return r;
        }
        r.kind = RunKind::Link;
        r.url = watchUrl(s(vid), t > 0 ? t : 0);
        return r;
    }
    const QJsonObject urlEndpoint = cmd.value(QStringLiteral("urlEndpoint")).toObject();
    if (!urlEndpoint.isEmpty()) {
        r.kind = RunKind::Link;
        r.url = unwrapRedirect(s(urlEndpoint.value(QStringLiteral("url")).toString()));
        return r;
    }
    const QJsonObject browse = cmd.value(QStringLiteral("browseEndpoint")).toObject();
    if (!browse.isEmpty()) {
        const QString trimmed = piece.trimmed();
        const QString browseId = browse.value(QStringLiteral("browseId")).toString();
        if (trimmed.startsWith(QLatin1Char('#')) || browseId.startsWith(QLatin1String(innertube::kBrowseIdHashtagPrefix))) {
            r.kind = RunKind::Hashtag;
            r.hashtag = s(trimmed.startsWith(QLatin1Char('#')) ? trimmed.mid(1) : trimmed);
            return r;
        }
        const QString base = browse.value(QStringLiteral("canonicalBaseUrl")).toString();
        r.kind = RunKind::Link;
        r.url = base.isEmpty() ? "https://www.youtube.com/channel/" + s(browseId) : unwrapRedirect(s(base));
        return r;
    }
    const QString web = json::at(cmd, QStringLiteral("commandMetadata.webCommandMetadata.url")).toString();
    if (!web.isEmpty()) {
        r.kind = RunKind::Link;
        r.url = unwrapRedirect(s(web));
    }
    return r;
}

void appendRuns(std::vector<DescriptionRun> &out, const std::vector<DescriptionRun> &more) {
    out.insert(out.end(), more.begin(), more.end());
}

// attributedDescription {content, commandRuns[{startIndex, length, onTap.innertubeCommand}]}.
// Indexes count UTF-16 code units, which is what QString::mid takes.
std::vector<DescriptionRun> attributedRuns(const QJsonObject &ad, const QString &videoId) {
    const QString content = ad.value(QStringLiteral("content")).toString();
    struct Marked { int start; int length; QJsonObject cmd; };
    std::vector<Marked> marks;
    for (const QJsonValue &v : ad.value(QStringLiteral("commandRuns")).toArray()) {
        const QJsonObject o = v.toObject();
        Marked m;
        m.start = o.value(QStringLiteral("startIndex")).toInt(-1);
        m.length = o.value(QStringLiteral("length")).toInt(0);
        m.cmd = json::at(o, QStringLiteral("onTap.innertubeCommand")).toObject();
        if (m.start >= 0 && m.length > 0 && m.start + m.length <= content.size()) marks.push_back(m);
    }
    std::stable_sort(marks.begin(), marks.end(), [](const Marked &a, const Marked &b) { return a.start < b.start; });
    std::vector<DescriptionRun> out;
    int pos = 0;
    for (const Marked &m : marks) {
        if (m.start < pos) continue;   // overlapping run
        appendRuns(out, descriptionRuns(s(content.mid(pos, m.start - pos))));
        out.push_back(runForCommand(m.cmd, content.mid(m.start, m.length), videoId));
        pos = m.start + m.length;
    }
    appendRuns(out, descriptionRuns(s(content.mid(pos))));
    return mergeTextRuns(out);
}

// Older shape: description {runs: [{text, navigationEndpoint}]}.
std::vector<DescriptionRun> legacyRuns(const QJsonArray &runs, const QString &videoId) {
    std::vector<DescriptionRun> out;
    for (const QJsonValue &v : runs) {
        const QJsonObject o = v.toObject();
        const QString t = o.value(QStringLiteral("text")).toString();
        const QJsonObject nav = o.value(QStringLiteral("navigationEndpoint")).toObject();
        if (nav.isEmpty()) appendRuns(out, descriptionRuns(s(t)));
        else out.push_back(runForCommand(nav, t, videoId));
    }
    return mergeTextRuns(out);
}

std::vector<ChapterMark> markersFromPlayerBar(const QJsonObject &json) {
    std::vector<ChapterMark> best;
    int bestRank = -1;
    json::forEachKey(json, QStringLiteral("multiMarkersPlayerBarRenderer"), [&](const QJsonObject &bar) {
        for (const QJsonValue &entry : bar.value(QStringLiteral("markersMap")).toArray()) {
            const QJsonObject e = entry.toObject();
            const QString key = e.value(QStringLiteral("key")).toString();
            const QJsonArray chapters = json::at(e, QStringLiteral("value.chapters")).toArray();
            if (chapters.isEmpty()) continue;
            const int rank = key == QLatin1String("DESCRIPTION_CHAPTERS") ? 2 : key == QLatin1String("AUTO_CHAPTERS") ? 1 : 0;
            if (rank <= bestRank) continue;
            std::vector<ChapterMark> list;
            for (const QJsonValue &c : chapters) {
                const QJsonObject r = c.toObject().value(QStringLiteral("chapterRenderer")).toObject();
                const qint64 ms = int64Field(r.value(QStringLiteral("timeRangeStartMillis")));
                if (r.isEmpty() || ms < 0) continue;
                list.push_back(ChapterMark(s(json::text(r.value(QStringLiteral("title")))), ms / 1000));
            }
            if (!list.empty()) { best = list; bestRank = rank; }
        }
        return true;
    });
    return best;
}

std::vector<ChapterMark> markersFromPanels(const QJsonObject &json) {
    std::vector<ChapterMark> out;
    json::forEachKey(json.value(QStringLiteral("engagementPanels")), QStringLiteral("macroMarkersListRenderer"),
                     [&](const QJsonObject &list) {
        for (const QJsonValue &v : list.value(QStringLiteral("contents")).toArray()) {
            const QJsonObject item = v.toObject().value(QStringLiteral("macroMarkersListItemRenderer")).toObject();
            if (item.isEmpty()) continue;
            qint64 start = int64Field(json::at(item, QStringLiteral("onTap.watchEndpoint.startTimeSeconds")));
            if (start < 0) {
                const QJsonValue time = item.value(QStringLiteral("timeDescription"));
                start = parseTimestamp(s(time.toObject().value(QStringLiteral("simpleText")).toString()));
                if (start < 0) start = parseDurationText(s(json::text(time)));
            }
            if (start < 0) continue;
            out.push_back(ChapterMark(s(json::text(item.value(QStringLiteral("title")))), start));
        }
        return out.empty();   // the first list with markers
    });
    return out;
}

QString commentsToken(const QJsonObject &two, const QJsonObject &json) {
    QString token;
    json::forEachKey(two.value(QStringLiteral("results")), QStringLiteral("itemSectionRenderer"), [&](const QJsonObject &sec) {
        if (sec.value(QStringLiteral("sectionIdentifier")).toString() != QLatin1String("comment-item-section")) return true;
        token = json::continuationToken(json::findFirst(sec, QStringLiteral("continuationItemRenderer")));
        return token.isEmpty();
    });
    if (!token.isEmpty()) return token;
    for (const QJsonValue &panel : json.value(QStringLiteral("engagementPanels")).toArray()) {
        const QJsonObject p = panel.toObject().value(QStringLiteral("engagementPanelSectionListRenderer")).toObject();
        if (p.value(QStringLiteral("panelIdentifier")).toString() != QLatin1String("engagement-panel-comments-section")) continue;
        token = json::continuationToken(json::findFirst(p.value(QStringLiteral("content")), QStringLiteral("continuationItemRenderer")));
        if (!token.isEmpty()) break;
    }
    return token;
}

}  // namespace

std::vector<ChapterMark> WatchPage::chapters(std::int64_t durationHint, bool *fromResponse) const {
    const std::int64_t duration = info.durationSeconds > 0 ? info.durationSeconds : durationHint;
    return chooseChapters(next.markers, info.description, duration, fromResponse);
}

WatchNextService::WatchNextService(InnerTubeClient *client, CatalogCounters *counters, QObject *parent,
                                   std::function<qint64()> nowMs)
    : QObject(parent), client_(client), counters_(counters), now_(nowMs ? nowMs : std::function<qint64()>(monotonicMs)),
      cache_([this] { return static_cast<std::int64_t>(now_()); }) {}

WatchNextService::~WatchNextService() {}

std::int64_t WatchNextService::isoDateEpoch(const QString &text) {
    static const QRegularExpression re(QStringLiteral("^\\s*(\\d{4})-(\\d{2})-(\\d{2})"));
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return -1;
    const QDate d(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
    if (!d.isValid()) return -1;
    return QDateTime(d, QTime(0, 0), Qt::UTC).toMSecsSinceEpoch() / 1000;
}

std::int64_t WatchNextService::isoInstantEpoch(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.size() < 19) return -1;   // a date alone is not an instant
    const QDateTime when = QDateTime::fromString(trimmed, Qt::ISODate);
    if (!when.isValid()) return -1;
    return when.toMSecsSinceEpoch() / 1000;
}

std::int64_t WatchNextService::largestNumber(const QString &text, const std::string &hl) {
    QString best;
    int bestDigits = 0;
    int i = 0;
    while (i < text.size()) {
        if (!text.at(i).isDigit()) { ++i; continue; }
        QString run;
        int digits = 0;
        while (i < text.size()) {
            const QChar c = text.at(i);
            if (c.isDigit()) { run += c; ++digits; ++i; continue; }
            const bool sep = c == QLatin1Char(',') || c == QLatin1Char('.') || c == QLatin1Char('\'') ||
                             c == QChar(0x00A0) || c == QChar(0x202F) || c == QChar(0x2009) || c == QLatin1Char(' ');
            if (sep && i + 1 < text.size() && text.at(i + 1).isDigit()) { run += c; ++i; continue; }
            break;
        }
        if (digits > bestDigits) { best = run; bestDigits = digits; }
    }
    if (best.isEmpty()) return -1;
    return parseCountText(s(best), hl);
}

NextParse WatchNextService::parseNext(const QJsonObject &json, const QString &hl, const QString &videoId) {
    NextParse out;
    const std::string hlS = s(hl);
    const QJsonObject two = json::at(json, QStringLiteral("contents.twoColumnWatchNextResults")).toObject();
    if (two.isEmpty()) {
        out.outcome = out.infoOutcome = out.relatedOutcome = Outcome::ShapeDrift;
        return out;
    }
    WatchInfoData &info = out.info;
    info.videoId = s(videoId);
    const QJsonValue results = two.value(QStringLiteral("results"));
    const QJsonObject primary = json::findFirst(results, QStringLiteral("videoPrimaryInfoRenderer"));
    const QJsonObject secondary = json::findFirst(results, QStringLiteral("videoSecondaryInfoRenderer"));
    if (!primary.isEmpty()) {
        info.title = s(json::text(primary.value(QStringLiteral("title"))));
        const QJsonObject views = json::at(primary, QStringLiteral("viewCount.videoViewCountRenderer")).toObject();
        info.viewText = s(json::text(views.value(QStringLiteral("viewCount"))));
        info.live = views.value(QStringLiteral("isLive")).toBool();
        info.dateText = s(json::text(primary.value(QStringLiteral("dateText"))));
        info.relativeDateText = s(json::text(primary.value(QStringLiteral("relativeDateText"))));
        // Like button: the accessibility label carries the exact count, the title a short one.
        const QJsonObject like = json::findFirst(primary, QStringLiteral("likeButtonViewModel"));
        const QJsonObject button = json::findFirst(like, QStringLiteral("buttonViewModel"));
        info.likeText = s(button.value(QStringLiteral("title")).toString());
        info.likeCount = largestNumber(button.value(QStringLiteral("accessibilityText")).toString(), hlS);
        if (info.likeCount < 0 && !info.likeText.empty()) info.likeCount = parseCountText(info.likeText, hlS);
    }
    // The structured-description panel repeats views and date (used when the primary lacks them).
    const QJsonObject header = json::findFirst(json.value(QStringLiteral("engagementPanels")), QStringLiteral("videoDescriptionHeaderRenderer"));
    if (!header.isEmpty()) {
        if (info.title.empty()) info.title = s(json::text(header.value(QStringLiteral("title"))));
        if (info.viewText.empty()) info.viewText = s(json::text(header.value(QStringLiteral("views"))));
        if (info.dateText.empty()) info.dateText = s(json::text(header.value(QStringLiteral("publishDate"))));
    }
    if (!info.viewText.empty()) info.viewCount = parseCountText(info.viewText, hlS);
    if (!info.dateText.empty()) info.publishedEpoch = parseAbsoluteDate(info.dateText, hlS);
    if (!info.relativeDateText.empty()) info.publishedAgeSeconds = parseAgeSeconds(info.relativeDateText, hlS);
    if (info.publishedEpoch < 0 && info.publishedAgeSeconds < 0 && !info.dateText.empty())
        info.publishedAgeSeconds = parseAgeSeconds(info.dateText, hlS);   // "Streamed 3 hours ago"

    if (!secondary.isEmpty()) {
        const QJsonObject owner = json::at(secondary, QStringLiteral("owner.videoOwnerRenderer")).toObject();
        info.ownerName = s(json::text(owner.value(QStringLiteral("title"))));
        QString ownerId = json::firstBrowseId(owner.value(QStringLiteral("navigationEndpoint")));
        if (ownerId.isEmpty()) ownerId = json::firstBrowseId(owner.value(QStringLiteral("title")));
        info.ownerId = s(ownerId);
        const QString base = json::findFirst(owner, QStringLiteral("browseEndpoint")).value(QStringLiteral("canonicalBaseUrl")).toString();
        if (base.startsWith(QLatin1String("/@"))) info.ownerHandle = s(base.mid(1));
        info.ownerAvatar = chooseAvatar(imageCandidates(owner.value(QStringLiteral("thumbnail"))));
        AvatarBook::instance().learn(info.ownerId, info.ownerAvatar);   // for this channel's rows in every list
        const QJsonObject subs = owner.value(QStringLiteral("subscriberCountText")).toObject();
        info.subscriberText = s(subs.value(QStringLiteral("simpleText")).toString());
        if (info.subscriberText.empty()) info.subscriberText = s(json::text(subs));
        if (!info.subscriberText.empty()) info.subscriberCount = parseSubscriberText(info.subscriberText, hlS);
        const QJsonObject attributed = secondary.value(QStringLiteral("attributedDescription")).toObject();
        if (!attributed.isEmpty()) info.descriptionRuns = attributedRuns(attributed, videoId);
        else info.descriptionRuns = legacyRuns(json::at(secondary, QStringLiteral("description.runs")).toArray(), videoId);
        info.description = runsText(info.descriptionRuns);
    }
    out.infoOutcome = (!primary.isEmpty() || !secondary.isEmpty()) ? Outcome::Ok : Outcome::ValidEmpty;

    out.markers = markersFromPlayerBar(json);
    if (out.markers.empty()) out.markers = markersFromPanels(json);
    out.commentsContinuation = commentsToken(two, json);

    if (!two.contains(QStringLiteral("secondaryResults"))) {
        // An answer without any info (removed video) may legitimately carry no related list.
        out.relatedOutcome = out.infoOutcome == Outcome::Ok ? Outcome::ShapeDrift : Outcome::ValidEmpty;
    } else {
        const ParsedItems parsed = RendererParser(hl).parse(two.value(QStringLiteral("secondaryResults")));
        for (const Item &item : parsed.items) if (isVideoLike(item)) out.related.push_back(item);
        out.relatedContinuation = parsed.continuation;
        out.unknownRenderers = parsed.unknown;
        out.relatedOutcome = out.related.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    }
    if (out.infoOutcome == Outcome::ShapeDrift || out.relatedOutcome == Outcome::ShapeDrift) out.outcome = Outcome::ShapeDrift;
    else if (out.infoOutcome == Outcome::Ok || out.relatedOutcome == Outcome::Ok) out.outcome = Outcome::Ok;
    else out.outcome = Outcome::ValidEmpty;
    return out;
}

PlayerParse WatchNextService::parsePlayer(const QJsonObject &json, const QString &hl) {
    Q_UNUSED(hl);
    PlayerParse p;
    const QJsonObject details = json.value(QStringLiteral("videoDetails")).toObject();
    const QJsonObject status = json.value(QStringLiteral("playabilityStatus")).toObject();
    const QJsonObject micro = json::at(json, QStringLiteral("microformat.playerMicroformatRenderer")).toObject();
    p.playabilityStatus = s(status.value(QStringLiteral("status")).toString());
    if (details.isEmpty()) {
        if (status.isEmpty()) p.outcome = Outcome::ShapeDrift;
        else p.outcome = p.playabilityStatus == "ERROR" ? Outcome::Unavailable : Outcome::ValidEmpty;
        return p;
    }
    p.outcome = Outcome::Ok;
    p.title = s(details.value(QStringLiteral("title")).toString());
    p.author = s(details.value(QStringLiteral("author")).toString());
    p.channelId = s(details.value(QStringLiteral("channelId")).toString());
    p.viewCount = int64Field(details.value(QStringLiteral("viewCount")));
    p.lengthSeconds = int64Field(details.value(QStringLiteral("lengthSeconds")));
    p.live = details.value(QStringLiteral("isLive")).toBool();
    p.shortDescription = s(details.value(QStringLiteral("shortDescription")).toString());
    if (!micro.isEmpty()) {
        p.likeCount = int64Field(micro.value(QStringLiteral("likeCount")));
        QString date = micro.value(QStringLiteral("publishDate")).toString();
        if (isoDateEpoch(date) < 0) date = micro.value(QStringLiteral("uploadDate")).toString();
        p.publishedEpoch = isoDateEpoch(date);
        if (p.publishedEpoch >= 0) p.publishDate = s(date.trimmed().left(10));
        if (json::at(micro, QStringLiteral("liveBroadcastDetails.isLiveNow")).toBool()) p.live = true;
        if (p.viewCount < 0) p.viewCount = int64Field(micro.value(QStringLiteral("viewCount")));
    }
    // RES-19: the scheduled start of a stream or premiere that has not begun. The offline slate
    // names it in seconds; the microformat's start time is the same instant as text. A stream
    // that is running or over keeps neither (its start time is then history, not a schedule).
    const std::int64_t slate = int64Field(json::at(status, QStringLiteral(
        "liveStreamability.liveStreamabilityRenderer.offlineSlate.liveStreamOfflineSlateRenderer.scheduledStartTime")));
    p.upcoming = !p.live && (details.value(QStringLiteral("isUpcoming")).toBool() || slate > 0);
    if (p.upcoming) {
        p.scheduledStartEpoch = slate > 0 ? slate
            : isoInstantEpoch(json::at(micro, QStringLiteral("liveBroadcastDetails.startTimestamp")).toString());
    }
    return p;
}

RelatedParse WatchNextService::parseRelatedPage(const QJsonObject &json, const QString &hl) {
    RelatedParse out;
    bool found = false;
    ParsedItems parsed;
    const RendererParser parser(hl);
    for (const QJsonValue &v : json.value(QStringLiteral("onResponseReceivedEndpoints")).toArray()) {
        const QJsonObject o = v.toObject();
        QJsonObject action = o.value(QStringLiteral("appendContinuationItemsAction")).toObject();
        if (action.isEmpty()) action = o.value(QStringLiteral("reloadContinuationItemsCommand")).toObject();
        if (action.isEmpty() || !action.contains(QStringLiteral("continuationItems"))) continue;
        const QString target = action.value(QStringLiteral("targetId")).toString();
        if (!target.isEmpty() && target != QLatin1String("watch-next-feed")) continue;   // e.g. comments
        found = true;
        const ParsedItems p = parser.parse(action);
        parsed.items.insert(parsed.items.end(), p.items.begin(), p.items.end());
        if (!p.continuation.isEmpty()) parsed.continuation = p.continuation;
        for (auto it = p.unknown.begin(); it != p.unknown.end(); ++it) parsed.unknown[it.key()] += it.value();
    }
    if (!found) {
        out.outcome = Outcome::ShapeDrift;
        return out;
    }
    for (const Item &item : parsed.items) if (isVideoLike(item)) out.items.push_back(item);
    out.continuation = parsed.continuation;
    out.unknownRenderers = parsed.unknown;
    out.outcome = out.items.empty() ? Outcome::ValidEmpty : Outcome::Ok;
    return out;
}

WatchInfoData WatchNextService::mergeInfo(const NextParse *next, const PlayerParse *player) {
    WatchInfoData i = next ? next->info : WatchInfoData();
    if (!player || player->outcome != Outcome::Ok) return i;
    if (player->viewCount >= 0) { i.viewCount = player->viewCount; i.viewsStructured = true; }
    if (player->likeCount >= 0) { i.likeCount = player->likeCount; i.likesStructured = true; }
    if (player->publishedEpoch >= 0) {
        i.publishedEpoch = player->publishedEpoch;
        i.dateStructured = true;
        if (i.dateText.empty()) i.dateText = player->publishDate;
    }
    if (player->lengthSeconds > 0) i.durationSeconds = player->lengthSeconds;
    if (player->live) i.live = true;
    i.upcoming = player->upcoming && !i.live;
    if (i.upcoming) i.scheduledStartEpoch = player->scheduledStartEpoch;
    if (i.title.empty()) i.title = player->title;
    if (i.ownerName.empty()) i.ownerName = player->author;
    if (i.ownerId.empty()) i.ownerId = player->channelId;
    if (i.description.empty() && !player->shortDescription.empty()) {
        i.descriptionRuns = descriptionRuns(player->shortDescription);
        i.description = player->shortDescription;
    }
    return i;
}

// --- service --------------------------------------------------------------------------------

std::shared_ptr<const WatchPage> WatchNextService::page(const QString &videoId) const {
    return cache_.peek(s(videoId));
}

QString WatchNextService::commentsContinuation(const QString &videoId) const {
    const std::shared_ptr<WatchPage> p = cache_.peek(s(videoId));
    return p ? p->commentsContinuation : QString();
}

void WatchNextService::forget(const QString &videoId) {
    cache_.setInFlight(s(videoId), false);
    cache_.erase(s(videoId));
}

bool WatchNextService::request(const QString &videoId) {
    const std::string key = s(videoId);
    if (!isVideoId(key)) return false;
    if (cache_.inFlight(key)) return true;           // single-flight: join the request in flight
    if (cache_.get(key)) return true;                // fresh
    std::shared_ptr<WatchPage> p = std::make_shared<WatchPage>();
    p->videoId = key;
    p->hl = s(client_->locale().hl);
    cache_.put(key, p);
    startNext(p);
    if (playerEnabled_) startPlayer(p);
    updatePin(key);
    return true;
}

void WatchNextService::startNext(const std::shared_ptr<WatchPage> &page) {
    page->nextPending = true;
    QJsonObject body;
    body.insert(QStringLiteral("videoId"), qs(page->videoId));
    body.insert(QStringLiteral("contentCheckOk"), true);
    body.insert(QStringLiteral("racyCheckOk"), true);
    const QString hl = qs(page->hl);
    const QString vid = qs(page->videoId);
    const std::string key = page->videoId;
    std::function<NextParse(const InnerTubeReply &)> parse = [hl, vid](const InnerTubeReply &r) {
        if (r.outcome != Outcome::Ok) {
            NextParse n;
            n.outcome = n.infoOutcome = n.relatedOutcome = r.outcome;
            return n;
        }
        return parseNext(r.json, hl, vid);
    };
    std::function<void(NextParse)> done = [this, key](NextParse n) { nextArrived(key, std::move(n)); };
    client_->post<NextParse>(QLatin1String(innertube::kEndpointNext), body, this, parse, done);
}

void WatchNextService::startPlayer(const std::shared_ptr<WatchPage> &page) {
    page->playerPending = true;
    QJsonObject body;
    body.insert(QStringLiteral("videoId"), qs(page->videoId));
    body.insert(QStringLiteral("contentCheckOk"), true);
    body.insert(QStringLiteral("racyCheckOk"), true);
    const QString hl = qs(page->hl);
    const std::string key = page->videoId;
    std::function<PlayerParse(const InnerTubeReply &)> parse = [hl](const InnerTubeReply &r) {
        if (r.outcome != Outcome::Ok) {
            PlayerParse p;
            p.outcome = r.outcome;
            return p;
        }
        return parsePlayer(r.json, hl);
    };
    std::function<void(PlayerParse)> done = [this, key](PlayerParse p) { playerArrived(key, std::move(p)); };
    client_->post<PlayerParse>(QLatin1String(innertube::kEndpointPlayer), body, this, parse, done);
}

void WatchNextService::nextArrived(const std::string &key, NextParse parse) {
    count(parse.outcome, parse.unknownRenderers);
    const std::shared_ptr<WatchPage> p = cache_.peek(key);
    if (!p) return;
    p->nextPending = false;
    p->nextDone = true;
    p->next = std::move(parse);
    p->info = mergeInfo(&p->next, p->playerDone ? &p->player : nullptr);
    p->commentsContinuation = p->next.commentsContinuation;
    p->relatedPageOutcome = p->next.relatedOutcome;
    if (!isFailure(p->next.relatedOutcome)) {
        appendRelated(*p, p->next.related);
        p->relatedContinuation = s(p->next.relatedContinuation);
        p->relatedPages = 1;
    }
    p->next.related.clear();
    p->next.related.shrink_to_fit();
    cache_.restamp(key);
    updatePin(key);
    const QString id = qs(key);
    emit pageChanged(id);
    emit relatedChanged(id);
    if (isFailure(p->next.outcome) && !p->loading()) cache_.erase(key);   // failures are not cached
}

void WatchNextService::playerArrived(const std::string &key, PlayerParse parse) {
    count(parse.outcome);
    const std::shared_ptr<WatchPage> p = cache_.peek(key);
    if (!p) return;
    p->playerPending = false;
    p->playerDone = true;
    p->player = std::move(parse);
    p->info = mergeInfo(p->nextDone ? &p->next : nullptr, &p->player);
    updatePin(key);
    emit pageChanged(qs(key));
    if (p->nextDone && isFailure(p->next.outcome) && !p->loading()) cache_.erase(key);
}

bool WatchNextService::loadMoreRelated(const QString &videoId) {
    const std::string key = s(videoId);
    const std::shared_ptr<WatchPage> p = cache_.peek(key);
    if (!p || p->relatedPending || p->relatedContinuation.empty()) return false;
    p->relatedPending = true;
    updatePin(key);
    QJsonObject body;
    body.insert(QStringLiteral("continuation"), qs(p->relatedContinuation));
    const QString hl = qs(p->hl);
    std::function<RelatedParse(const InnerTubeReply &)> parse = [hl](const InnerTubeReply &r) {
        if (r.outcome != Outcome::Ok) {
            RelatedParse rp;
            rp.outcome = r.outcome;
            return rp;
        }
        return parseRelatedPage(r.json, hl);
    };
    std::function<void(RelatedParse)> done = [this, key](RelatedParse rp) { relatedArrived(key, std::move(rp)); };
    client_->post<RelatedParse>(QLatin1String(innertube::kEndpointNext), body, this, parse, done);
    emit relatedChanged(videoId);
    return true;
}

void WatchNextService::relatedArrived(const std::string &key, RelatedParse parse) {
    count(parse.outcome, parse.unknownRenderers);
    const std::shared_ptr<WatchPage> p = cache_.peek(key);
    if (!p) return;
    p->relatedPending = false;
    p->relatedPageOutcome = parse.outcome;
    int added = 0;
    if (!isFailure(parse.outcome)) {
        const std::size_t before = p->related.size();
        appendRelated(*p, parse.items);
        added = static_cast<int>(p->related.size() - before);
        p->relatedContinuation = s(parse.continuation);   // empty = the end
        ++p->relatedPages;
    }
    updatePin(key);
    emit relatedChanged(qs(key));
    // A page that only repeated rows: follow the next token once more (bounded by the pages).
    if (!isFailure(parse.outcome) && added == 0 && !p->relatedContinuation.empty() && p->relatedPages < 10)
        loadMoreRelated(qs(key));
}

void WatchNextService::appendRelated(WatchPage &page, const std::vector<Item> &rows) {
    std::vector<Item> candidates;
    for (const Item &item : rows) {
        if (item.id == page.videoId) { ++page.seedsExcluded; continue; }
        candidates.push_back(item);
    }
    const RowFilterResult r = filterRows(candidates, filter_, &page.seen, shorts_);
    page.related.insert(page.related.end(), r.visible.begin(), r.visible.end());
    page.hiddenShorts += r.hiddenShorts;
    page.hiddenMembersOnly += r.hiddenMembersOnly;
    page.hiddenRestricted += r.hiddenRestricted;
    page.duplicates += r.duplicates;
}

void WatchNextService::updatePin(const std::string &key) {
    const std::shared_ptr<WatchPage> p = cache_.peek(key);
    cache_.setInFlight(key, p && (p->nextPending || p->playerPending || p->relatedPending));
}

void WatchNextService::count(Outcome o, const QMap<QString, int> &unknown) {
    if (!counters_) return;
    counters_->count(Area::WatchNext, o);
    for (auto it = unknown.begin(); it != unknown.end(); ++it) counters_->unknownRenderer(s(it.key()), it.value());
}

}}}
