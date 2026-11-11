// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Watch page data: DSC-13 (related + continuation), DSC-14 (info line, description runs, share
// URLs, details without playback), DSC-15 (chapters), I18N-02 (structured fields win), I18N-04
// (en/de/ja watch-next fixtures), DSC-01 (valid empty vs drift), cache and single-flight.

#include "tests/catalog_common/TestSupport.h"
#include "Core/Catalog/Domain/AvatarBook.h"
#include <functional>
#include "Core/Catalog/Domain/Chapters.h"
#include "Core/Catalog/Domain/Description.h"
#include "Core/Catalog/Domain/WatchCache.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/Json.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Core/Catalog/Infrastructure/DislikeService.h"
#include "Clients/Sailfish/Presentation/ChapterModel.h"
#include "Clients/Sailfish/Presentation/ContentFilters.h"
#include "Clients/Sailfish/Presentation/DislikeInfo.h"
#include "Clients/Sailfish/Presentation/RelatedModel.h"
#include "Clients/Sailfish/Presentation/WatchDislikes.h"
#include "Clients/Sailfish/Presentation/WatchInfo.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>
#include <QSettings>
#include <QTemporaryDir>
#include <QUrlQuery>
#include <set>

using namespace nordtile::video::catalog;
using nordtile::video::sailfish::ChapterModel;
using nordtile::video::sailfish::RelatedModel;
using nordtile::video::sailfish::WatchInfo;
using catalog_test::FakeTransport;

namespace {

const QString kSeed = QStringLiteral("9zCH37330f8");
const std::int64_t kDec12_2024 = 1733961600;   // 2024-12-12T00:00:00Z

QByteArray toBytes(const QJsonObject &o) { return QJsonDocument(o).toJson(QJsonDocument::Compact); }

// Sets (or, with an undefined value, removes) the member at a dotted object path.
QJsonObject withPath(QJsonObject root, const QStringList &path, const QJsonValue &value) {
    if (path.isEmpty()) return root;
    const QString key = path.first();
    if (path.size() == 1) {
        if (value.isUndefined()) root.remove(key);
        else root.insert(key, value);
        return root;
    }
    root.insert(key, withPath(root.value(key).toObject(), path.mid(1), value));
    return root;
}

int countKind(const std::vector<DescriptionRun> &runs, RunKind kind) {
    int n = 0;
    for (const DescriptionRun &r : runs) if (r.kind == kind) ++n;
    return n;
}

bool noDuplicates(const std::vector<Item> &rows) {
    std::set<std::string> ids;
    for (const Item &i : rows) ids.insert(i.id);
    return ids.size() == rows.size();
}

bool contains(const std::vector<Item> &rows, const QString &id) {
    for (const Item &i : rows) if (i.id == id.toStdString()) return true;
    return false;
}

// A fourth related page derived from the recorded second one (three pages are recorded):
// every row id is rotated so the rows are new, and the paging token is cleared (end of list).
QByteArray derivedThirdPage() {
    QByteArray raw = catalog_test::fixture(QStringLiteral("next_related_p2.json"));
    const RelatedParse p2 = WatchNextService::parseRelatedPage(QJsonDocument::fromJson(raw).object(), QStringLiteral("en"));
    if (!p2.continuation.isEmpty()) raw.replace(p2.continuation.toUtf8(), QByteArray());
    for (const Item &i : p2.items) {
        const QByteArray id = QByteArray::fromStdString(i.id);
        raw.replace(id, id.mid(1) + id.left(1));
    }
    return raw;
}

struct Rig {
    FakeTransport net;
    ClientIdentity identity;
    WorkLane lane;
    InnerTubeClient client;
    CatalogCounters counters;
    qint64 now = 1000;
    WatchNextService service;
    explicit Rig(const QString &hl = QStringLiteral("en"))
        : identity(nullptr, QString()), lane(QStringLiteral("watch-test"), 1),
          client(&net, &identity, &lane, locale(hl)), service(&client, &counters, nullptr, [this] { return now; }) {}
    static DiscoveryLocale locale(const QString &hl) { DiscoveryLocale l; l.hl = hl; return l; }
    void serveFixtures(int nextDelayMs = 0) {
        net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200,
               catalog_test::fixture(QStringLiteral("next_en.json")), nextDelayMs);
        net.on(QStringLiteral("/youtubei/v1/player"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("player_web.json")));
    }
    bool settled(const QString &id) {
        return catalog_test::waitFor([&] {
            const std::shared_ptr<const WatchPage> p = service.page(id);
            return p && !p->loading() && !p->relatedPending;
        }, 8000);
    }
};

}  // namespace

// --- DSC-15 chapters -------------------------------------------------------------------------

static void chapterRules() {
    CHECK_EQ(parseTimestamp("0:00"), 0);
    CHECK_EQ(parseTimestamp("1:23"), 83);
    CHECK_EQ(parseTimestamp("12:30"), 750);
    CHECK_EQ(parseTimestamp("1:02:03"), 3723);
    CHECK_EQ(parseTimestamp("75:00"), 4500);
    CHECK_EQ(parseTimestamp("1:2"), -1);
    CHECK_EQ(parseTimestamp("1:60"), -1);
    CHECK_EQ(parseTimestamp("1:60:00"), -1);
    CHECK_EQ(parseTimestamp("a:00"), -1);
    CHECK_EQ(formatTimestamp(3723), std::string("1:02:03"));
    CHECK_EQ(formatTimestamp(83), std::string("1:23"));

    // Valid list: >= 3, first 0:00, +10 s steps, last before the duration.
    ChapterRejection why = ChapterRejection::TooFew;
    std::vector<ChapterMark> c = chaptersFromDescription("Intro text\n0:00 Intro\n1:23 - Part one\n10:00 End\nthanks", 900, &why);
    CHECK(why == ChapterRejection::None);
    CHECK_EQ(static_cast<int>(c.size()), 3);
    if (c.size() == 3) {
        CHECK_EQ(c[0].title, std::string("Intro"));
        CHECK_EQ(c[1].title, std::string("Part one"));
        CHECK_EQ(c[1].startSeconds, 83);
        CHECK_EQ(c[0].endSeconds, 83);
        CHECK_EQ(c[2].endSeconds, 900);
    }

    // Decorations: brackets, dashes, emoji with VS16, emoji keycaps, timestamps at line end.
    const std::string decorated =
        "[0:00] Bracketed\n"
        "(1:00) Parenthesised\n"
        "1\xEF\xB8\x8F\xE2\x83\xA3 2:00 \xE2\x80\x93 Keycap one\n"     // 1️⃣ 2:00 – Keycap one
        "\xE2\x96\xB6\xEF\xB8\x8F 3:00 Play sign\n"                     // ▶️ 3:00
        "Trailing stamp - 4:00\n"
        "Closing (5:00)\n"
        "#\xEF\xB8\x8F\xE2\x83\xA3 6:00 Hash keycap\n"                  // #️⃣ 6:00
        "Emoji end 7:00 \xF0\x9F\x8E\xAC\n";                            // ... 7:00 🎬
    c = chaptersFromDescription(decorated, 3600, &why);
    CHECK(why == ChapterRejection::None);
    CHECK_EQ(static_cast<int>(c.size()), 8);
    if (c.size() == 8) {
        CHECK_EQ(c[0].title, std::string("Bracketed"));
        CHECK_EQ(c[1].title, std::string("Parenthesised"));
        CHECK_EQ(c[2].title, std::string("Keycap one"));
        CHECK_EQ(c[2].startSeconds, 120);
        CHECK_EQ(c[3].title, std::string("Play sign"));
        CHECK_EQ(c[4].title, std::string("Trailing stamp"));
        CHECK_EQ(c[5].title, std::string("Closing"));
        CHECK_EQ(c[5].startSeconds, 300);
        CHECK_EQ(c[6].title, std::string("Hash keycap"));
        CHECK_EQ(c[7].title, std::string("Emoji end"));
        CHECK_EQ(c[7].startSeconds, 420);
    }
    // More than 8 non-word characters between the timestamp and the line edge: not a chapter line.
    // (The space next to the timestamp counts; the line's own indentation does not.)
    CHECK(chapterCandidates("!!!!!!!! 0:00 Too far\n").empty());
    CHECK_EQ(static_cast<int>(chapterCandidates("!!!!!!! 0:00 Just in\n").size()), 1);
    CHECK(chapterCandidates("Too far 0:00 ########\n").empty());
    CHECK_EQ(static_cast<int>(chapterCandidates("Just in 0:00 #######\n").size()), 1);
    CHECK_EQ(static_cast<int>(chapterCandidates("            0:00 Indented  \n").size()), 1);
    CHECK(chapterCandidates("Meet at 10:30am tomorrow\n").empty());
    CHECK(chapterCandidates("The 0:00 mark is in the middle of the line\n").empty());
    CHECK(chapterCandidates("v1:23\n").empty());

    // Each rejection rule.
    CHECK(chaptersFromDescription("0:00 A\n1:00 B\n", 600, &why).empty());
    CHECK(why == ChapterRejection::TooFew);
    CHECK(chaptersFromDescription("0:05 A\n1:00 B\n2:00 C\n", 600, &why).empty());
    CHECK(why == ChapterRejection::FirstNotZero);
    CHECK(chaptersFromDescription("0:00 A\n2:00 B\n1:00 C\n", 600, &why).empty());
    CHECK(why == ChapterRejection::NotIncreasing);
    CHECK(chaptersFromDescription("0:00 A\n1:00 B\n1:00 C\n", 600, &why).empty());
    CHECK(why == ChapterRejection::NotIncreasing);
    CHECK(chaptersFromDescription("0:00 A\n0:09 B\n1:00 C\n", 600, &why).empty());
    CHECK(why == ChapterRejection::GapTooSmall);
    CHECK(!chaptersFromDescription("0:00 A\n0:10 B\n1:00 C\n", 600, &why).empty());   // exactly 10 s is fine
    CHECK(chaptersFromDescription("0:00 A\n1:00 B\n10:00 C\n", 600, &why).empty());
    CHECK(why == ChapterRejection::PastDuration);
    CHECK(!chaptersFromDescription("0:00 A\n1:00 B\n10:00 C\n", 601, &why).empty());
    CHECK(!chaptersFromDescription("0:00 A\n1:00 B\n10:00 C\n", -1, &why).empty());   // duration unknown

    // Response markers win over the description.
    std::vector<ChapterMark> markers;
    markers.push_back(ChapterMark("Marker B", 30));
    markers.push_back(ChapterMark("Marker A", 0));
    bool fromResponse = false;
    c = chooseChapters(markers, "0:00 A\n1:00 B\n2:00 C\n", 600, &fromResponse);
    CHECK(fromResponse);
    CHECK_EQ(static_cast<int>(c.size()), 2);
    if (c.size() == 2) { CHECK_EQ(c[0].title, std::string("Marker A")); CHECK_EQ(c[1].endSeconds, 600); }
    c = chooseChapters(std::vector<ChapterMark>(), "0:00 A\n1:00 B\n2:00 C\n", 600, &fromResponse);
    CHECK(!fromResponse);
    CHECK_EQ(static_cast<int>(c.size()), 3);

    // Chapter at a position.
    CHECK_EQ(chapterAt(c, 0), 0);
    CHECK_EQ(chapterAt(c, 59.9), 0);
    CHECK_EQ(chapterAt(c, 60), 1);
    CHECK_EQ(chapterAt(c, 5000), 2);
    CHECK_EQ(chapterAt(std::vector<ChapterMark>(), 10), -1);
}

static void chaptersFromFixture() {
    // 9zCH37330f8: the response carries markers (player bar + macro markers panel).
    const QJsonObject next = catalog_test::fixtureJson(QStringLiteral("next_en.json"));
    const NextParse parsed = WatchNextService::parseNext(next, QStringLiteral("en"), kSeed);
    const std::int64_t starts[] = {0, 30, 85, 334, 615, 672};
    CHECK_EQ(static_cast<int>(parsed.markers.size()), 6);
    for (std::size_t i = 0; i < parsed.markers.size() && i < 6; ++i) CHECK_EQ(parsed.markers[i].startSeconds, starts[i]);
    if (!parsed.markers.empty()) CHECK(!parsed.markers[0].title.empty());

    WatchPage page;
    page.next = parsed;
    page.info = parsed.info;
    page.info.durationSeconds = 750;
    bool fromResponse = false;
    std::vector<ChapterMark> c = page.chapters(-1, &fromResponse);
    CHECK(fromResponse);
    CHECK_EQ(static_cast<int>(c.size()), 6);
    if (c.size() == 6) CHECK_EQ(c[5].endSeconds, 750);

    // Variant derived in code: drop the player overlay and the panels -> no markers; the same
    // six chapters are rebuilt from the description text.
    QJsonObject bare = next;
    bare.remove(QStringLiteral("playerOverlays"));
    bare.remove(QStringLiteral("engagementPanels"));
    const NextParse noMarkers = WatchNextService::parseNext(bare, QStringLiteral("en"), kSeed);
    CHECK(noMarkers.markers.empty());
    page.next = noMarkers;
    page.info = noMarkers.info;
    page.info.durationSeconds = 750;
    c = page.chapters(-1, &fromResponse);
    CHECK(!fromResponse);
    CHECK_EQ(static_cast<int>(c.size()), 6);
    for (std::size_t i = 0; i < c.size() && i < 6; ++i) CHECK_EQ(c[i].startSeconds, starts[i]);
    // With a duration shorter than the last start the rebuild is refused.
    page.info.durationSeconds = 600;
    CHECK(page.chapters(-1, &fromResponse).empty());
}

// NFR-MNT-04 request shape: a description run whose command browses a hashtag page
// (browseId "FEhashtag...") is a hashtag even when its text has no '#'. Derived: the recorded
// watch-next answer with its attributedDescription replaced by one such command run.
static void hashtagBrowseRun() {
    QJsonObject next = catalog_test::fixtureJson(QStringLiteral("next_en.json"));
    QJsonObject cmd{{QStringLiteral("browseEndpoint"), QJsonObject{{QStringLiteral("browseId"), QStringLiteral("FEhashtag")},
                                                                   {QStringLiteral("params"), QStringLiteral("x")}}}};
    QJsonObject run{{QStringLiteral("startIndex"), 6}, {QStringLiteral("length"), 7},
                    {QStringLiteral("onTap"), QJsonObject{{QStringLiteral("innertubeCommand"), cmd}}}};
    const QJsonObject described{{QStringLiteral("content"), QStringLiteral("About science today")},
                                {QStringLiteral("commandRuns"), QJsonArray{run}}};
    // Put it wherever the recorded answer keeps its attributed description.
    std::function<bool(QJsonValue &)> replace = [&](QJsonValue &v) -> bool {
        if (v.isObject()) {
            QJsonObject o = v.toObject();
            bool changed = false;
            if (o.contains(QStringLiteral("attributedDescription"))) { o.insert(QStringLiteral("attributedDescription"), described); changed = true; }
            for (auto it = o.begin(); it != o.end(); ++it) { QJsonValue c = it.value(); if (replace(c)) { it.value() = c; changed = true; } }
            if (changed) v = o;
            return changed;
        }
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            bool changed = false;
            for (int i = 0; i < a.size(); ++i) { QJsonValue c = a.at(i); if (replace(c)) { a[i] = c; changed = true; } }
            if (changed) v = a;
            return changed;
        }
        return false;
    };
    QJsonValue root(next);
    CHECK(replace(root));
    const NextParse parsed = WatchNextService::parseNext(root.toObject(), QStringLiteral("en"), kSeed);
    CHECK_EQ(countKind(parsed.info.descriptionRuns, RunKind::Hashtag), 1);
    for (const DescriptionRun &x : parsed.info.descriptionRuns)
        if (x.kind == RunKind::Hashtag) CHECK_EQ(x.hashtag, std::string("science"));
    CHECK_EQ(std::string(innertube::kBrowseIdHashtagPrefix), std::string("FEhashtag"));
}

// --- DSC-14 description runs and URLs -----------------------------------------------------------

static void descriptionRunsPlain() {
    std::vector<DescriptionRun> r =
        descriptionRuns("Watch https://example.com/a?b=1. At 1:23 and 1:02:03 see #science, #1 and www.test.org/x (or v1:23).");
    CHECK_EQ(countKind(r, RunKind::Link), 2);
    CHECK_EQ(countKind(r, RunKind::Timestamp), 2);
    CHECK_EQ(countKind(r, RunKind::Hashtag), 1);
    std::vector<std::string> urls;
    std::vector<std::int64_t> seeks;
    for (const DescriptionRun &x : r) {
        if (x.kind == RunKind::Link) urls.push_back(x.url);
        if (x.kind == RunKind::Timestamp) seeks.push_back(x.seekSeconds);
        if (x.kind == RunKind::Hashtag) CHECK_EQ(x.hashtag, std::string("science"));
    }
    if (urls.size() == 2) {
        CHECK_EQ(urls[0], std::string("https://example.com/a?b=1"));   // the sentence's full stop is not part of it
        CHECK_EQ(urls[1], std::string("https://www.test.org/x"));
    }
    if (seeks.size() == 2) { CHECK_EQ(seeks[0], 83); CHECK_EQ(seeks[1], 3723); }
    CHECK_EQ(runsText(r), std::string("Watch https://example.com/a?b=1. At 1:23 and 1:02:03 see #science, #1 and www.test.org/x (or v1:23)."));
    // Balanced parentheses stay in a URL.
    r = descriptionRuns("(see https://en.wikipedia.org/wiki/Foo_(bar))");
    CHECK_EQ(countKind(r, RunKind::Link), 1);
    for (const DescriptionRun &x : r) if (x.kind == RunKind::Link) CHECK_EQ(x.url, std::string("https://en.wikipedia.org/wiki/Foo_(bar)"));
    // Non-Latin text around timestamps and hashtags.
    r = descriptionRuns("\xE6\x97\xA5\xE6\x9C\xAC 0:30 \xE3\x83\x86\xE3\x82\xB9\xE3\x83\x88 #\xE6\x97\xA5\xE6\x9C\xAC");
    CHECK_EQ(countKind(r, RunKind::Timestamp), 1);
    CHECK_EQ(countKind(r, RunKind::Hashtag), 1);
    CHECK(descriptionRuns("Meet at 10:30am").size() == 1);

    // Collapsed to 5 lines.
    const std::vector<DescriptionRun> seven = descriptionRuns("l1\nl2 https://a.example/x\nl3\nl4 0:30\nl5\nl6\nl7");
    bool truncated = false;
    const std::vector<DescriptionRun> five = collapseLines(seven, kCollapsedDescriptionLines, &truncated);
    CHECK(truncated);
    CHECK_EQ(runsText(five), std::string("l1\nl2 https://a.example/x\nl3\nl4 0:30\nl5"));
    CHECK_EQ(countKind(five, RunKind::Link), 1);
    CHECK_EQ(countKind(five, RunKind::Timestamp), 1);
    collapseLines(descriptionRuns("a\nb\nc"), 5, &truncated);
    CHECK(!truncated);
    collapseLines(descriptionRuns("1\n2\n3\n4\n5\n\n  \n"), 5, &truncated);
    CHECK(!truncated);   // only blank lines after the cut

    // StyledText with anchors; text is escaped.
    std::vector<DescriptionRun> forStyle = descriptionRuns("a<b & c\n1:23 #tag ");
    DescriptionRun quoted(RunKind::Link, "quoted");
    quoted.url = "https://x.example/?a=1&b=\"2\"";
    forStyle.push_back(quoted);
    const std::string styled = runsToStyledText(forStyle);
    CHECK(styled.find("a&lt;b &amp; c<br>") != std::string::npos);
    CHECK(styled.find("<a href=\"seek:83\">1:23</a>") != std::string::npos);
    CHECK(styled.find("<a href=\"hashtag:tag\">#tag</a>") != std::string::npos);
    CHECK(styled.find("href=\"https://x.example/?a=1&b=%222%22\"") != std::string::npos);

    LinkAction a = parseLinkAnchor("seek:83");
    CHECK(a.kind == RunKind::Timestamp && a.seekSeconds == 83);
    a = parseLinkAnchor("hashtag:%E6%97%A5");
    CHECK(a.kind == RunKind::Hashtag && a.hashtag == "\xE6\x97\xA5");
    a = parseLinkAnchor("https://example.com");
    CHECK(a.kind == RunKind::Link && a.url == "https://example.com");
    CHECK(parseLinkAnchor("javascript:alert(1)").kind == RunKind::Text);
    CHECK(parseLinkAnchor("seek:12x").kind == RunKind::Text);

    CHECK_EQ(unwrapRedirect("https://www.youtube.com/redirect?event=video_description&redir_token=QUl&q=https%3A%2F%2Fwww.givewell.org%2FKurzgesagt&v=9zCH37330f8"),
             std::string("https://www.givewell.org/Kurzgesagt"));
    CHECK_EQ(unwrapRedirect("/@kurzgesagt"), std::string("https://www.youtube.com/@kurzgesagt"));
    CHECK_EQ(unwrapRedirect("https://example.com/x"), std::string("https://example.com/x"));

    // Share / copy-link / browser URLs.
    CHECK_EQ(shareUrl("9zCH37330f8"), std::string("https://youtu.be/9zCH37330f8"));
    CHECK_EQ(shareUrl("9zCH37330f8", 83), std::string("https://youtu.be/9zCH37330f8?t=83"));
    CHECK_EQ(shareUrl("9zCH37330f8", 0), std::string("https://youtu.be/9zCH37330f8"));
    CHECK(shareUrl("bad id").empty());
    CHECK(shareUrl("9zCH37330f8&x").empty());
    CHECK_EQ(watchUrl("9zCH37330f8", 83), std::string("https://www.youtube.com/watch?v=9zCH37330f8&t=83s"));
}

// --- DSC-14 / I18N-04: next_en/de/ja info fields ------------------------------------------------

static void infoFields() {
    const char *locales[] = {"en", "de", "ja"};
    for (const char *hl : locales) {
        const QString name = QStringLiteral("next_%1.json").arg(QLatin1String(hl));
        const NextParse p = WatchNextService::parseNext(catalog_test::fixtureJson(name), QLatin1String(hl), kSeed);
        std::printf("  %s: views=%lld likes=%lld date=%lld subs=%lld related=%d links=%d stamps=%d\n", hl,
                    static_cast<long long>(p.info.viewCount), static_cast<long long>(p.info.likeCount),
                    static_cast<long long>(p.info.publishedEpoch), static_cast<long long>(p.info.subscriberCount),
                    static_cast<int>(p.related.size()), countKind(p.info.descriptionRuns, RunKind::Link),
                    countKind(p.info.descriptionRuns, RunKind::Timestamp));
        CHECK(p.outcome == Outcome::Ok);
        CHECK(p.infoOutcome == Outcome::Ok);
        CHECK(!p.info.title.empty());
        CHECK_EQ(p.info.viewCount, 9039840);
        CHECK(p.info.likeCount >= 219000 && p.info.likeCount <= 220000);
        CHECK_EQ(p.info.publishedEpoch, kDec12_2024);
        CHECK(p.info.subscriberCount >= 25000000 && p.info.subscriberCount <= 26000000);
        CHECK(!p.info.subscriberText.empty());
        CHECK(!p.info.ownerName.empty());
        CHECK_EQ(p.info.ownerId.substr(0, 2), std::string("UC"));
        CHECK_EQ(p.info.ownerHandle, std::string("@kurzgesagt"));
        CHECK(p.info.ownerAvatar.compare(0, 8, "https://") == 0);
        CHECK(!p.info.description.empty());
        // Description runs from attributedDescription: links unwrapped, timestamps seek.
        CHECK(countKind(p.info.descriptionRuns, RunKind::Link) >= 20);
        std::vector<std::int64_t> seeks;
        for (const DescriptionRun &r : p.info.descriptionRuns) {
            if (r.kind == RunKind::Link) CHECK(r.url.find("youtube.com/redirect") == std::string::npos);
            if (r.kind == RunKind::Timestamp) seeks.push_back(r.seekSeconds);
        }
        CHECK_EQ(static_cast<int>(seeks.size()), 6);
        if (seeks.size() == 6) { CHECK_EQ(seeks[0], 0); CHECK_EQ(seeks[2], 85); CHECK_EQ(seeks[5], 672); }
        CHECK_EQ(runsText(p.info.descriptionRuns), p.info.description);
        // Related page 1, comments token, markers.
        CHECK(static_cast<int>(p.related.size()) >= 15);
        CHECK(noDuplicates(p.related));
        CHECK(!contains(p.related, kSeed));
        for (const Item &i : p.related) CHECK(isVideoId(i.id) && !i.title.empty());
        CHECK(!p.relatedContinuation.isEmpty());
        CHECK(!p.commentsContinuation.isEmpty());
        CHECK_EQ(static_cast<int>(p.markers.size()), 6);
    }
    // The URL the first commandRun points at (redirect unwrapped).
    const NextParse en = WatchNextService::parseNext(catalog_test::fixtureJson(QStringLiteral("next_en.json")), QStringLiteral("en"), kSeed);
    for (const DescriptionRun &r : en.info.descriptionRuns)
        if (r.kind == RunKind::Link) { CHECK_EQ(r.url, std::string("https://www.givewell.org/Kurzgesagt")); break; }

    // Further recorded locales (I18N-04 beyond the required three) must parse to the same counts.
    const char *more[] = {"es", "fr", "it", "pt", "ru", "zh", "ar"};
    for (const char *hl : more) {
        const QString name = QStringLiteral("next_%1.json").arg(QLatin1String(hl));
        if (!QFile::exists(catalog_test::fixturePath(name))) continue;
        const NextParse p = WatchNextService::parseNext(catalog_test::fixtureJson(name), QLatin1String(hl), kSeed);
        std::printf("  %s: views=%lld likes=%lld date=%lld subs=%lld related=%d\n", hl, static_cast<long long>(p.info.viewCount),
                    static_cast<long long>(p.info.likeCount), static_cast<long long>(p.info.publishedEpoch),
                    static_cast<long long>(p.info.subscriberCount), static_cast<int>(p.related.size()));
        CHECK(p.outcome == Outcome::Ok);
        CHECK(static_cast<int>(p.related.size()) >= 15);
        CHECK(!p.info.viewText.empty() && !p.info.dateText.empty());   // raw text kept even if a table misses
    }

    // I18N-02: results do not depend on the process locale.
    const QLocale saved;
    const char *processLocales[] = {"C", "de_DE", "ja_JP"};
    std::int64_t first = -2;
    for (const char *loc : processLocales) {
        QLocale::setDefault(QLocale(QLatin1String(loc)));
        const NextParse p = WatchNextService::parseNext(catalog_test::fixtureJson(QStringLiteral("next_de.json")), QStringLiteral("de"), kSeed);
        const std::int64_t sig = p.info.viewCount * 3 + p.info.likeCount * 5 + p.info.publishedEpoch + p.info.subscriberCount;
        if (first == -2) first = sig;
        CHECK_EQ(sig, first);
    }
    QLocale::setDefault(saved);
}

// --- I18N-02 structured player fields -------------------------------------------------------------

static void structuredFields() {
    const QJsonObject playerJson = catalog_test::fixtureJson(QStringLiteral("player_web.json"));
    const PlayerParse player = WatchNextService::parsePlayer(playerJson, QStringLiteral("en"));
    CHECK(player.outcome == Outcome::Ok);              // UNPLAYABLE for WEB, but the fields are there
    CHECK_EQ(player.playabilityStatus, std::string("UNPLAYABLE"));
    CHECK_EQ(player.viewCount, 9039840);
    CHECK_EQ(player.lengthSeconds, 750);
    CHECK_EQ(player.likeCount, 219810);
    CHECK_EQ(player.publishedEpoch, kDec12_2024);
    CHECK_EQ(player.publishDate, std::string("2024-12-12"));
    CHECK_EQ(WatchNextService::isoDateEpoch(QStringLiteral("2024-12-12T07:00:04-08:00")), kDec12_2024);
    CHECK_EQ(WatchNextService::isoDateEpoch(QStringLiteral("Dec 12")), -1);
    CHECK_EQ(WatchNextService::largestNumber(QStringLiteral("like this video along with 219,810 other people"), "en"), 219810);
    CHECK_EQ(WatchNextService::largestNumber(QStringLiteral("bisher 219.810 positive Bewertungen"), "de"), 219810);

    // Variant derived in code: structured values that differ from the text, so the winner shows.
    QJsonObject variant = withPath(playerJson, QStringList() << QStringLiteral("videoDetails") << QStringLiteral("viewCount"),
                                   QStringLiteral("123456789"));
    variant = withPath(variant, QStringList() << QStringLiteral("microformat") << QStringLiteral("playerMicroformatRenderer")
                                              << QStringLiteral("likeCount"), QStringLiteral("4242"));
    variant = withPath(variant, QStringList() << QStringLiteral("microformat") << QStringLiteral("playerMicroformatRenderer")
                                              << QStringLiteral("publishDate"), QStringLiteral("2020-01-02T10:00:00+01:00"));
    const NextParse next = WatchNextService::parseNext(catalog_test::fixtureJson(QStringLiteral("next_en.json")), QStringLiteral("en"), kSeed);
    const PlayerParse pv = WatchNextService::parsePlayer(variant, QStringLiteral("en"));
    WatchInfoData merged = WatchNextService::mergeInfo(&next, &pv);
    CHECK_EQ(merged.viewCount, 123456789);
    CHECK(merged.viewsStructured);
    CHECK_EQ(merged.likeCount, 4242);
    CHECK(merged.likesStructured);
    CHECK_EQ(merged.publishedEpoch, 1577923200);        // 2020-01-02, not the text's Dec 12, 2024
    CHECK(merged.dateStructured);
    CHECK_EQ(merged.durationSeconds, 750);
    CHECK_EQ(merged.viewText, next.info.viewText);       // raw text kept
    // Text only (player failed): the parsed text values remain.
    PlayerParse failed;
    failed.outcome = Outcome::Http5xx;
    merged = WatchNextService::mergeInfo(&next, &failed);
    CHECK_EQ(merged.viewCount, 9039840);
    CHECK(!merged.viewsStructured);
    // Player only (watch-next missing): title, owner and description come from videoDetails.
    merged = WatchNextService::mergeInfo(nullptr, &player);
    CHECK(!merged.title.empty() && !merged.ownerName.empty() && !merged.description.empty());
    CHECK_EQ(merged.ownerId, std::string("UCsXVk37bltHxD1rDPwtNM8Q"));

    // Through the service: both requests run in parallel and the merged page prefers the player.
    Rig rig;
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200, catalog_test::fixture(QStringLiteral("next_en.json")));
    rig.net.on(QStringLiteral("/youtubei/v1/player"), QByteArray(), 200, toBytes(variant));
    CHECK(rig.service.request(kSeed));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 1);
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/player")), 1);   // sent together, before either answer
    CHECK(rig.settled(kSeed));
    const std::shared_ptr<const WatchPage> page = rig.service.page(kSeed);
    CHECK(page && page->info.viewCount == 123456789 && page->info.durationSeconds == 750);
    const QJsonObject sent = QJsonDocument::fromJson(rig.net.requests.value(1).body).object();
    CHECK_EQ(sent.value(QStringLiteral("videoId")).toString(), kSeed);
    CHECK(sent.value(QStringLiteral("contentCheckOk")).toBool() && sent.value(QStringLiteral("racyCheckOk")).toBool());
}

// --- DSC-13 related + continuation ----------------------------------------------------------------

static void relatedPaging() {
    Rig rig;
    rig.service.setPlayerEnabled(false);
    const QByteArray p1 = catalog_test::fixture(QStringLiteral("next_en.json"));
    const QByteArray p2 = catalog_test::fixture(QStringLiteral("next_related_p2.json"));
    const QString token1 = WatchNextService::parseNext(QJsonDocument::fromJson(p1).object(), QStringLiteral("en"), kSeed).relatedContinuation;
    const QString token2 = WatchNextService::parseRelatedPage(QJsonDocument::fromJson(p2).object(), QStringLiteral("en")).continuation;
    CHECK(!token1.isEmpty() && !token2.isEmpty() && token1 != token2);
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200, p1);
    rig.net.on(QStringLiteral("/youtubei/v1/next"), token1.toUtf8(), 200, p2);
    const QByteArray p3 = catalog_test::fixture(QStringLiteral("next_related_p3.json"));
    const QString token3 = WatchNextService::parseRelatedPage(QJsonDocument::fromJson(p3).object(), QStringLiteral("en")).continuation;
    CHECK(!token3.isEmpty() && token3 != token2);
    rig.net.on(QStringLiteral("/youtubei/v1/next"), token2.toUtf8(), 200, p3);
    rig.net.on(QStringLiteral("/youtubei/v1/next"), token3.toUtf8(), 200, derivedThirdPage());

    CHECK(rig.service.request(kSeed));
    CHECK(rig.settled(kSeed));
    std::shared_ptr<const WatchPage> page = rig.service.page(kSeed);
    CHECK(page != nullptr);
    if (!page) return;
    const std::vector<Item> first = page->related;
    CHECK(static_cast<int>(first.size()) >= 15);
    CHECK(!contains(first, kSeed));

    CHECK(rig.service.loadMoreRelated(kSeed));
    CHECK(!rig.service.loadMoreRelated(kSeed));          // single-flight per video
    CHECK(rig.settled(kSeed));
    page = rig.service.page(kSeed);
    CHECK(static_cast<int>(page->related.size()) >= 30);
    CHECK(noDuplicates(page->related));
    for (std::size_t i = 0; i < first.size() && i < page->related.size(); ++i) CHECK_EQ(page->related[i].id, first[i].id);   // order kept
    const QJsonObject sent = QJsonDocument::fromJson(rig.net.requests.last().body).object();
    CHECK_EQ(sent.value(QStringLiteral("continuation")).toString(), token1);

    CHECK(rig.service.loadMoreRelated(kSeed));
    CHECK(rig.settled(kSeed));
    page = rig.service.page(kSeed);
    std::printf("  related after 3 recorded pages: %d (pages=%d)\n", static_cast<int>(page->related.size()), page->relatedPages);
    CHECK(static_cast<int>(page->related.size()) >= 50);
    CHECK(noDuplicates(page->related));
    CHECK(!page->relatedContinuation.empty());            // the recorded third page continues
    CHECK(rig.service.loadMoreRelated(kSeed));
    CHECK(rig.settled(kSeed));
    page = rig.service.page(kSeed);
    CHECK(static_cast<int>(page->related.size()) >= 60);   // DSC-13: at least 60 reachable
    CHECK(noDuplicates(page->related));
    CHECK(page->relatedContinuation.empty());             // the derived fourth page ends the list
    CHECK(!rig.service.loadMoreRelated(kSeed));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\"")), 1);
    CHECK_EQ(rig.counters.driftTotal(), 0);

    // Seed excluded and filters applied (variant derived in code: the first related row gets the
    // seed's id, the second is badged members-only).
    {
        QJsonObject next = QJsonDocument::fromJson(p1).object();
        const NextParse original = WatchNextService::parseNext(next, QStringLiteral("en"), kSeed);
        QByteArray raw = p1;
        if (!original.related.empty()) raw.replace(QByteArray::fromStdString(original.related[0].id), kSeed.toUtf8());
        Rig r2;
        r2.service.setPlayerEnabled(false);
        RowFilterSettings settings;
        settings.hideMembersOnly = true;
        r2.service.setRowFilterSettings(settings);
        r2.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, raw);
        CHECK(r2.service.request(kSeed));
        CHECK(r2.settled(kSeed));
        const std::shared_ptr<const WatchPage> p = r2.service.page(kSeed);
        CHECK(p && p->seedsExcluded == 1);
        CHECK(p && !contains(p->related, kSeed));
        CHECK(p && static_cast<int>(p->related.size()) == static_cast<int>(original.related.size()) - 1);
    }
}

// --- cache and single-flight -----------------------------------------------------------------------

static void cacheAndSingleFlight() {
    // Domain policy: TTL and LRU capacity.
    {
        std::int64_t now = 0;
        TtlLruCache<int> cache([&now] { return now; }, 1000, 2);
        cache.put("a", std::make_shared<int>(1));
        cache.put("b", std::make_shared<int>(2));
        CHECK(cache.get("a") != nullptr);                  // "a" now most recent
        cache.put("c", std::make_shared<int>(3));          // evicts "b"
        CHECK(cache.get("b") == nullptr);
        CHECK(cache.get("a") != nullptr && cache.get("c") != nullptr);
        cache.setInFlight("a", true);
        cache.put("d", std::make_shared<int>(4));          // "a" pinned: "c" goes
        CHECK(cache.peek("a") != nullptr);
        CHECK(cache.peek("c") == nullptr);
        now = 1000;
        CHECK(cache.get("d") == nullptr);                  // expired
        CHECK_EQ(kWatchCacheTtlMs, 600000LL);
        CHECK_EQ(static_cast<int>(kWatchCacheEntries), 32);
    }
    Rig rig;
    rig.serveFixtures(100);
    WatchInfo a(&rig.service);
    WatchInfo b(&rig.service);
    a.setVideoId(kSeed);
    b.setVideoId(kSeed);                                   // joins the request in flight
    CHECK(rig.service.request(kSeed));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 1);
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/player")), 1);
    CHECK(rig.settled(kSeed));
    CHECK(a.viewCount() > 0 && b.viewCount() > 0);
    // Cached for 10 minutes.
    rig.now += 9 * 60 * 1000;
    CHECK(rig.service.request(kSeed));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 1);
    rig.now += 61 * 1000;
    CHECK(rig.service.request(kSeed));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 2);
    CHECK(rig.settled(kSeed));
    CHECK(!rig.service.request(QStringLiteral("not-an-id")));

    // At most 32 videos are kept.
    Rig many;
    many.service.setPlayerEnabled(false);
    many.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("next_en.json")));
    for (int i = 0; i < 33; ++i) {
        const QString id = QStringLiteral("video%1").arg(i, 6, 10, QLatin1Char('0'));
        many.service.request(id);
        CHECK(many.settled(id));
    }
    CHECK(many.service.page(QStringLiteral("video000000")) == nullptr);
    CHECK(many.service.page(QStringLiteral("video000032")) != nullptr);

    // Failures are not cached: the next request tries again.
    Rig failing;
    failing.service.setPlayerEnabled(false);
    failing.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 503, QByteArray(), 0, 1);
    failing.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("next_en.json")));
    WatchInfo info(&failing.service);
    info.setVideoId(kSeed);
    CHECK(catalog_test::waitFor([&] { return !info.errorText().isEmpty(); }));
    CHECK(failing.service.page(kSeed) == nullptr);
    info.reload();
    CHECK(catalog_test::waitFor([&] { return info.loaded(); }));
    CHECK(info.errorText().isEmpty());
    CHECK_EQ(failing.net.count(QStringLiteral("/youtubei/v1/next")), 2);
    CHECK_EQ(failing.counters.value(Area::WatchNext, Outcome::Http5xx), 1);
    CHECK_EQ(failing.counters.driftTotal(), 0);
}

// --- DSC-01 valid empty vs drift -------------------------------------------------------------------

static void validEmptyVsDrift() {
    const QJsonObject next = catalog_test::fixtureJson(QStringLiteral("next_en.json"));
    const QStringList secondary = QStringList() << QStringLiteral("contents") << QStringLiteral("twoColumnWatchNextResults")
                                                << QStringLiteral("secondaryResults");
    // No watch-next container at all: drift.
    NextParse p = WatchNextService::parseNext(QJsonObject(), QStringLiteral("en"), kSeed);
    CHECK(p.outcome == Outcome::ShapeDrift && p.infoOutcome == Outcome::ShapeDrift && p.relatedOutcome == Outcome::ShapeDrift);
    // Related list present but empty (variant derived in code): valid empty, the page is fine.
    QJsonObject emptied = withPath(next, QStringList(secondary) << QStringLiteral("secondaryResults") << QStringLiteral("results"), QJsonArray());
    p = WatchNextService::parseNext(emptied, QStringLiteral("en"), kSeed);
    CHECK(p.relatedOutcome == Outcome::ValidEmpty);
    CHECK(p.outcome == Outcome::Ok);
    CHECK(p.related.empty());
    // Related container missing while the info is there: drift.
    QJsonObject missing = withPath(next, secondary, QJsonValue(QJsonValue::Undefined));
    p = WatchNextService::parseNext(missing, QStringLiteral("en"), kSeed);
    CHECK(p.relatedOutcome == Outcome::ShapeDrift);
    CHECK(p.outcome == Outcome::ShapeDrift);
    CHECK(p.infoOutcome == Outcome::Ok);
    // Results container with nothing in it (removed video): valid empty, not drift.
    const QJsonObject bare = QJsonDocument::fromJson(
        "{\"contents\":{\"twoColumnWatchNextResults\":{\"results\":{\"results\":{\"contents\":[]}}}}}").object();
    p = WatchNextService::parseNext(bare, QStringLiteral("en"), kSeed);
    CHECK(p.outcome == Outcome::ValidEmpty && p.infoOutcome == Outcome::ValidEmpty && p.relatedOutcome == Outcome::ValidEmpty);

    // Continuation pages.
    CHECK(WatchNextService::parseRelatedPage(QJsonObject(), QStringLiteral("en")).outcome == Outcome::ShapeDrift);
    const QJsonObject emptyPage = QJsonDocument::fromJson(
        "{\"onResponseReceivedEndpoints\":[{\"appendContinuationItemsAction\":{\"continuationItems\":[],\"targetId\":\"watch-next-feed\"}}]}").object();
    CHECK(WatchNextService::parseRelatedPage(emptyPage, QStringLiteral("en")).outcome == Outcome::ValidEmpty);
    CHECK(WatchNextService::parseRelatedPage(catalog_test::fixtureJson(QStringLiteral("next_related_p2.json")), QStringLiteral("en")).outcome == Outcome::Ok);

    // Player answers.
    CHECK(WatchNextService::parsePlayer(QJsonObject(), QStringLiteral("en")).outcome == Outcome::ShapeDrift);
    CHECK(WatchNextService::parsePlayer(QJsonDocument::fromJson("{\"playabilityStatus\":{\"status\":\"ERROR\"}}").object(),
                                        QStringLiteral("en")).outcome == Outcome::Unavailable);
    CHECK(WatchNextService::parsePlayer(QJsonDocument::fromJson("{\"playabilityStatus\":{\"status\":\"LOGIN_REQUIRED\"}}").object(),
                                        QStringLiteral("en")).outcome == Outcome::ValidEmpty);

    // HTTP failures are counted as what they are, never as drift.
    Rig rig;
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 429, QByteArray());
    rig.net.on(QStringLiteral("/youtubei/v1/player"), QByteArray(), 200, QByteArray("<html>"));
    rig.service.request(kSeed);
    CHECK(catalog_test::waitFor([&] { return rig.counters.value(Area::WatchNext, Outcome::RateLimited) == 1 &&
                                             rig.counters.value(Area::WatchNext, Outcome::ShapeDrift) == 1; }));
    // (The non-JSON 200 player answer is drift by InnerTubeClient's rules; the 429 is not.)
    CHECK_EQ(rig.counters.value(Area::WatchNext, Outcome::Ok), 0);
}

// NFR-PERF: watch-next waits for the player, so related and comments never compete with the
// first video bytes; the grace timer releases it if playback never starts; a cached answer
// still shows at once; a page opened on a running video loads at once.
static void holdUntilPlaying() {
    Rig rig;
    rig.serveFixtures(0);
    WatchInfo info(&rig.service);
    info.setWaitForPlayback(true);
    info.setVideoId(kSeed);
    CHECK(info.deferred());
    catalog_test::waitFor([] { return false; }, 200);
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 0);
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/player")), 0);
    CHECK(!info.loaded());
    info.setPlaybackRunning(true);
    CHECK(!info.deferred());
    CHECK(catalog_test::waitFor([&] { return info.loaded(); }, 6000));
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 1);
    // Back to the same video: the cached answer is used, no wait, no request.
    info.setPlaybackRunning(false);
    info.setVideoId(QStringLiteral("aaaaaaaaaaa"));
    CHECK(info.deferred());
    info.setVideoId(kSeed);
    CHECK(!info.deferred());
    CHECK(info.loaded());
    CHECK_EQ(rig.net.count(QStringLiteral("/youtubei/v1/next")), 1);

    // Playback never starts: the grace timer releases it.
    Rig slow;
    slow.serveFixtures(0);
    WatchInfo stalled(&slow.service);
    stalled.setWaitForPlayback(true);
    stalled.setGraceMs(150);
    stalled.setVideoId(kSeed);
    CHECK(stalled.deferred());
    CHECK(catalog_test::waitFor([&] { return !stalled.deferred(); }, 2000));
    CHECK(catalog_test::waitFor([&] { return stalled.loaded(); }, 6000));

    // DL-07: a download plays offline: no watch-next request, not even after the grace; back
    // online the deferred load starts.
    Rig local;
    local.serveFixtures(0);
    WatchInfo offline(&local.service);
    offline.setWaitForPlayback(true);
    offline.setGraceMs(50);
    offline.setOffline(true);
    offline.setVideoId(kSeed);
    offline.setPlaybackRunning(true);
    catalog_test::waitFor([] { return false; }, 200);
    CHECK(offline.deferred());
    CHECK_EQ(local.net.count(QStringLiteral("/youtubei/v1/next")), 0);
    offline.setOffline(false);
    CHECK(catalog_test::waitFor([&] { return offline.loaded(); }, 6000));
    CHECK_EQ(local.net.count(QStringLiteral("/youtubei/v1/next")), 1);

    // The page opens on a video that is already playing: no wait.
    Rig running;
    running.serveFixtures(0);
    WatchInfo onPlaying(&running.service);
    onPlaying.setWaitForPlayback(true);
    onPlaying.setPlaybackRunning(true);
    onPlaying.setVideoId(kSeed);
    CHECK(!onPlaying.deferred());
    CHECK(catalog_test::waitFor([&] { return onPlaying.loaded(); }, 6000));
}

// --- presentation: WatchInfo, RelatedModel, ChapterModel --------------------------------------------

static void presentation() {
    Rig rig;
    rig.serveFixtures(3000);   // DSC-14: a slow watch-next reply delays nothing
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"continuation\""), 200,
               catalog_test::fixture(QStringLiteral("next_related_p2.json")));
    WatchInfo info(&rig.service);
    QList<double> seeks;
    QStringList tags, urls;
    QObject::connect(&info, &WatchInfo::seekRequested, [&seeks](double s) { seeks.append(s); });
    QObject::connect(&info, &WatchInfo::hashtagRequested, [&tags](const QString &t) { tags.append(t); });
    QObject::connect(&info, &WatchInfo::openUrlRequested, [&urls](const QString &u) { urls.append(u); });
    QElapsedTimer clock;
    clock.start();
    info.setVideoId(kSeed);
    CHECK(clock.elapsed() < 50);                          // returns at once
    CHECK(info.loading());
    CHECK(!info.loaded());
    CHECK(catalog_test::waitFor([&] { return info.loaded(); }, 6000));
    CHECK(clock.elapsed() >= 2800);                       // coarse timers may fire up to 5 % early
    CHECK_EQ(info.viewCount(), 9039840.0);
    CHECK(info.likeCount() > 0);
    CHECK(!info.dateText().isEmpty());
    CHECK_EQ(info.publishedEpoch(), static_cast<double>(kDec12_2024));
    CHECK(info.subscriberCount() > 0 && !info.subscriberText().isEmpty());
    CHECK(!info.owner().isEmpty() && info.ownerId().startsWith(QLatin1String("UC")) && !info.avatar().isEmpty());
    CHECK_EQ(info.durationSeconds(), 750.0);
    CHECK(info.thumbnail().contains(QLatin1String("hqdefault.jpg")));
    CHECK(!info.commentsContinuation().isEmpty());

    // Description: collapsed to 5 lines by default, anchors for links / timestamps.
    CHECK(info.collapsed());
    CHECK(info.descriptionTruncated());
    CHECK_EQ(info.shownDescription(), info.collapsedDescriptionStyled());
    CHECK(info.collapsedDescriptionStyled().count(QStringLiteral("<br>")) <= 4);
    CHECK(info.descriptionStyled().contains(QLatin1String("<a href=\"seek:30\">0:30</a>")));
    CHECK(info.descriptionStyled().contains(QLatin1String("<a href=\"https://www.givewell.org/Kurzgesagt\">")));
    info.toggleCollapsed();
    CHECK(!info.collapsed());
    CHECK_EQ(info.shownDescription(), info.descriptionStyled());
    CHECK(info.descriptionRuns().size() > 20);

    info.activateLink(QStringLiteral("seek:85"));
    CHECK_EQ(seeks.count(), 1);
    if (seeks.count()) CHECK_EQ(seeks.at(0), 85.0);
    info.activateLink(QStringLiteral("hashtag:science"));
    CHECK_EQ(tags.count(), 1);
    info.activateLink(QStringLiteral("https://kgs.link/patreon"));
    CHECK_EQ(urls.count(), 1);
    info.activateLink(QStringLiteral("file:///etc/passwd"));
    CHECK_EQ(urls.count(), 1);

    CHECK_EQ(info.shareUrl(), QStringLiteral("https://youtu.be/9zCH37330f8"));
    CHECK_EQ(info.shareUrl(true, 83.7), QStringLiteral("https://youtu.be/9zCH37330f8?t=83"));
    CHECK_EQ(info.shareUrl(false, 83.7), QStringLiteral("https://youtu.be/9zCH37330f8"));
    CHECK_EQ(info.openInBrowser(true, 10), QStringLiteral("https://www.youtube.com/watch?v=9zCH37330f8&t=10s"));
    CHECK_EQ(urls.count(), 2);

    // Chapters: markers from the response, jump-to and the current chapter.
    ChapterModel *chapters = info.chapterModel();
    QList<double> jumps;
    QObject::connect(chapters, &ChapterModel::jumpRequested, [&jumps](double s) { jumps.append(s); });
    CHECK_EQ(chapters->count(), 6);
    CHECK(chapters->fromResponse());
    CHECK_EQ(chapters->indexAt(100), 2);
    chapters->jumpTo(3);
    CHECK_EQ(jumps.count(), 1);
    if (jumps.count()) CHECK_EQ(jumps.at(0), 334.0);
    chapters->setPosition(700);
    CHECK_EQ(chapters->currentIndex(), 5);
    CHECK_EQ(chapters->data(chapters->index(1, 0), ChapterModel::StartTextRole).toString(), QStringLiteral("0:30"));
    CHECK_EQ(chapters->get(5).value(QStringLiteral("end")).toDouble(), 750.0);

    // Related rows and loadMore through the continuation.
    RelatedModel *related = info.relatedModel();
    CHECK(related->count() >= 15);
    CHECK(related->canLoadMore());
    const QVariantMap row = related->get(0);
    CHECK(isVideoId(row.value(QStringLiteral("videoId")).toString().toStdString()));
    CHECK(row.value(QStringLiteral("thumbnail")).toString().contains(QLatin1String("mqdefault.jpg")));
    related->loadMore();
    CHECK(related->busy());
    CHECK(catalog_test::waitFor([&] { return related->count() >= 30 && !related->busy(); }));

    // A second WatchInfo (the details-only view) for another video leaves the first untouched.
    WatchInfo details(&rig.service);
    details.setVideoId(QStringLiteral("Zpq1GbPqhy4"));
    CHECK_EQ(info.videoId(), kSeed);
    CHECK_EQ(info.viewCount(), 9039840.0);

    // Fallback chapters (resolver's list) when the answer has none.
    WatchInfo empty(&rig.service);
    QVariantList fallback;
    for (int i = 0; i < 3; ++i) {
        QVariantMap m;
        m.insert(QStringLiteral("title"), QStringLiteral("Part %1").arg(i));
        m.insert(QStringLiteral("start"), i * 60000.0);
        fallback.append(m);
    }
    empty.setFallbackChapters(fallback);
    CHECK_EQ(empty.chapterModel()->count(), 3);
}

// YouTube-style cards: related rows carry their owner's avatar, and the watch page's owner
// teaches the book, so the same channel's rows elsewhere have it too.
static void channelAvatars() {
    AvatarBook::instance().clear();
    Rig rig;
    rig.serveFixtures(0);
    WatchInfo info(&rig.service);
    info.setVideoId(kSeed);
    CHECK(catalog_test::waitFor([&] { return info.loaded(); }, 6000));
    RelatedModel *related = info.relatedModel();
    CHECK(catalog_test::waitFor([&] { return related->count() > 0; }, 6000));
    CHECK(related->roleNames().values().contains("channelAvatar"));
    int with = 0;
    for (int i = 0; i < related->count(); ++i)
        if (related->get(i).value(QStringLiteral("channelAvatar")).toString().startsWith(QLatin1String("https://"))) ++with;
    CHECK(with >= related->count() - 2);          // nearly every related row names its owner's avatar
    CHECK(!info.ownerId().isEmpty() && !info.avatar().isEmpty());
    CHECK(!AvatarBook::instance().lookup(info.ownerId().toStdString()).empty());
    AvatarBook::instance().clear();
}

// --- RES-19 upcoming streams ----------------------------------------------------------------------

// The WEB player answer of a stream that has not started (recorded 2026-11-10, scheduled for
// 2026-11-11T02:00:00Z) carries its start twice: in the offline slate and in the microformat.
static void upcomingStream() {
    const std::int64_t kStart = 1794362400;   // 2026-11-11T02:00:00Z
    const QJsonObject json = catalog_test::fixtureJson(QStringLiteral("player_upcoming.json"));
    const PlayerParse p = WatchNextService::parsePlayer(json, QStringLiteral("en"));
    CHECK(p.outcome == Outcome::Ok);
    CHECK_EQ(p.playabilityStatus, std::string("LIVE_STREAM_OFFLINE"));
    CHECK(p.upcoming);
    CHECK(!p.live);
    CHECK_EQ(p.scheduledStartEpoch, kStart);

    CHECK_EQ(WatchNextService::isoInstantEpoch(QStringLiteral("2026-11-11T02:00:00+00:00")), kStart);
    CHECK_EQ(WatchNextService::isoInstantEpoch(QStringLiteral("2026-11-10T18:00:00-08:00")), kStart);
    CHECK_EQ(WatchNextService::isoInstantEpoch(QStringLiteral(" 2026-11-11T02:00:00Z ")), kStart);
    CHECK_EQ(WatchNextService::isoInstantEpoch(QStringLiteral("2026-11-11")), -1);   // a day is not an instant
    CHECK_EQ(WatchNextService::isoInstantEpoch(QStringLiteral("soon")), -1);
    CHECK_EQ(WatchNextService::isoInstantEpoch(QString()), -1);

    // Derived: without the offline slate, the microformat's start time gives the same instant.
    const QStringList slate = QStringList() << QStringLiteral("playabilityStatus") << QStringLiteral("liveStreamability");
    const QJsonObject noSlate = withPath(json, slate, QJsonValue(QJsonValue::Undefined));
    const PlayerParse fromMicro = WatchNextService::parsePlayer(noSlate, QStringLiteral("en"));
    CHECK(fromMicro.upcoming);
    CHECK_EQ(fromMicro.scheduledStartEpoch, kStart);
    // Derived: neither names it: still upcoming, start unknown.
    const QJsonObject neither = withPath(noSlate, QStringList() << QStringLiteral("microformat") << QStringLiteral("playerMicroformatRenderer")
                                                                << QStringLiteral("liveBroadcastDetails"), QJsonValue(QJsonValue::Undefined));
    const PlayerParse unknown = WatchNextService::parsePlayer(neither, QStringLiteral("en"));
    CHECK(unknown.upcoming);
    CHECK_EQ(unknown.scheduledStartEpoch, -1);
    // Derived: the stream has gone live. Its start time is history then, not a schedule.
    const QJsonObject running = withPath(noSlate, QStringList() << QStringLiteral("videoDetails") << QStringLiteral("isLive"), true);
    const PlayerParse live = WatchNextService::parsePlayer(running, QStringLiteral("en"));
    CHECK(live.live);
    CHECK(!live.upcoming);
    CHECK_EQ(live.scheduledStartEpoch, -1);
    // An ordinary video.
    const PlayerParse plain = WatchNextService::parsePlayer(catalog_test::fixtureJson(QStringLiteral("player_web.json")), QStringLiteral("en"));
    CHECK(!plain.upcoming);
    CHECK_EQ(plain.scheduledStartEpoch, -1);

    // Merged into the watch info, with or without the watch-next answer.
    const NextParse next = WatchNextService::parseNext(catalog_test::fixtureJson(QStringLiteral("next_en.json")), QStringLiteral("en"), kSeed);
    WatchInfoData merged = WatchNextService::mergeInfo(&next, &p);
    CHECK(merged.upcoming);
    CHECK_EQ(merged.scheduledStartEpoch, kStart);
    merged = WatchNextService::mergeInfo(nullptr, &p);
    CHECK(merged.upcoming && merged.scheduledStartEpoch == kStart);
    merged = WatchNextService::mergeInfo(&next, &plain);
    CHECK(!merged.upcoming);
    CHECK_EQ(merged.scheduledStartEpoch, -1);

    // On the watch page: milliseconds, the unit of playback.scheduledStart; 0 when unknown.
    Rig rig;
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray("\"videoId\""), 200, catalog_test::fixture(QStringLiteral("next_en.json")));
    rig.net.on(QStringLiteral("/youtubei/v1/player"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("player_upcoming.json")));
    WatchInfo info(&rig.service);
    CHECK(!info.upcoming());
    CHECK_EQ(info.scheduledStart(), 0.0);
    info.setVideoId(kSeed);
    CHECK(rig.settled(kSeed));
    CHECK(catalog_test::waitFor([&info] { return info.upcoming(); }));
    CHECK_EQ(info.scheduledStart(), static_cast<double>(kStart) * 1000.0);
    CHECK(!info.live());

    Rig ordinary;
    ordinary.serveFixtures();
    WatchInfo other(&ordinary.service);
    other.setVideoId(kSeed);
    CHECK(ordinary.settled(kSeed));
    CHECK(!other.upcoming());
    CHECK_EQ(other.scheduledStart(), 0.0);
}

// --- SET-01 Hide Shorts on related ---------------------------------------------------------------

static void relatedHidesShorts() {
    const NextParse original = WatchNextService::parseNext(catalog_test::fixtureJson(QStringLiteral("next_en.json")), QStringLiteral("en"), kSeed);
    // The recorded related rows hold no Short: the Shorts knowledge says one of the videos is one.
    std::string known;
    for (const Item &row : original.related)
        if (isVideoLike(row) && !row.id.empty() && row.id != kSeed.toStdString()) { known = row.id; break; }
    CHECK(!known.empty());
    if (known.empty()) return;
    Rig rig;
    rig.service.setPlayerEnabled(false);
    rig.service.setShortsLookup([known](const std::string &id) { return id == known ? ShortEvidence::ShortsTab : ShortEvidence::None; });
    rig.net.on(QStringLiteral("/youtubei/v1/next"), QByteArray(), 200, catalog_test::fixture(QStringLiteral("next_en.json")));
    nordtile::video::sailfish::ContentFilters filters;
    WatchNextService *service = &rig.service;
    filters.addList([service] { return service->rowFilterSettings(); },
                    [service](const RowFilterSettings &f) { service->setRowFilterSettings(f); });
    CHECK(!rig.service.rowFilterSettings().hideShorts);

    CHECK(rig.service.request(kSeed));
    CHECK(rig.settled(kSeed));
    std::shared_ptr<const WatchPage> page = rig.service.page(kSeed);
    CHECK(page && contains(page->related, QString::fromStdString(known)));
    CHECK(page && page->hiddenShorts == 0);
    const int shown = page ? static_cast<int>(page->related.size()) : 0;

    filters.setHideShorts(true);
    CHECK(rig.service.rowFilterSettings().hideShorts);
    // The page already loaded keeps its rows; the next page loaded is filtered.
    page = rig.service.page(kSeed);
    CHECK(page && contains(page->related, QString::fromStdString(known)));
    rig.service.forget(kSeed);
    CHECK(rig.service.request(kSeed));
    CHECK(rig.settled(kSeed));
    page = rig.service.page(kSeed);
    CHECK(page && !contains(page->related, QString::fromStdString(known)));
    CHECK(page && page->hiddenShorts == 1);
    CHECK(page && static_cast<int>(page->related.size()) == shown - 1);

    filters.setHideShorts(false);
    CHECK(!rig.service.rowFilterSettings().hideShorts);
}

// --- RYD-01 the dislike estimate on the watch info ------------------------------------------------

static void dislikeEstimate() {
    using nordtile::video::sailfish::DislikeInfo;
    using nordtile::video::sailfish::WatchDislikes;
    const QString kVotesHost = QStringLiteral("returnyoutubedislikeapi.com");
    const QString kOtherVideo = QStringLiteral("ROboUHfPUrU");

    Rig rig;
    rig.serveFixtures();
    rig.net.on(kVotesHost, QByteArray(), 200, catalog_test::fixture(QStringLiteral("ryd_votes.json")));
    QTemporaryDir dir;
    QSettings settings(dir.path() + QStringLiteral("/ryd.conf"), QSettings::IniFormat);
    DislikeService votes(&rig.net);
    DislikeInfo dislikes(&votes, &settings);
    WatchInfo info(&rig.service);
    WatchDislikes glue(&info, &dislikes);
    int changes = 0;
    QObject::connect(&info, &WatchInfo::changed, [&changes] { ++changes; });

    // The setting is off by default: the watch page opens and nothing is asked of the service.
    CHECK(!dislikes.enabled());
    info.setVideoId(kSeed);
    CHECK(rig.settled(kSeed));
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(rig.net.count(kVotesHost), 0);
    CHECK_EQ(info.dislikeCount(), -1.0);
    CHECK(info.dislikeText().isEmpty());
    CHECK_EQ(dislikes.videoId(), kSeed);              // remembered, so the switch can act on it

    // Turned on while the video is open: one request for that video, the number on the watch info.
    dislikes.setEnabled(true);
    CHECK(catalog_test::waitFor([&info] { return info.dislikeCount() >= 0; }));
    CHECK_EQ(info.dislikeCount(), 1533.0);
    CHECK_EQ(info.dislikeText(), QStringLiteral("1.5K"));
    CHECK_EQ(rig.net.count(kVotesHost), 1);
    CHECK_EQ(QUrlQuery(rig.net.requests.last().url).queryItemValue(QStringLiteral("videoId")), kSeed);
    CHECK(changes > 0);
    // Asking for the watch data again does not ask the service again.
    info.reload();
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(rig.net.count(kVotesHost), 1);
    CHECK_EQ(info.dislikeCount(), 1533.0);

    // The next video: the previous number is gone at once; its own arrives with its watch data.
    info.setVideoId(kOtherVideo);
    CHECK_EQ(info.dislikeCount(), -1.0);
    CHECK(info.dislikeText().isEmpty());
    CHECK(catalog_test::waitFor([&info] { return info.dislikeCount() >= 0; }));
    CHECK_EQ(rig.net.count(kVotesHost), 2);
    CHECK_EQ(QUrlQuery(rig.net.requests.last().url).queryItemValue(QStringLiteral("videoId")), kOtherVideo);

    // Turned off: the number goes away and nothing more is asked.
    dislikes.setEnabled(false);
    CHECK_EQ(info.dislikeCount(), -1.0);
    CHECK(info.dislikeText().isEmpty());
    info.setVideoId(kSeed);
    CHECK(rig.settled(kSeed));
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(rig.net.count(kVotesHost), 2);
    CHECK_EQ(info.dislikeCount(), -1.0);

    // With the setting on from the start, the request waits with the watch data until the video
    // plays, and is not made for a video that plays offline.
    Rig held;
    held.serveFixtures();
    held.net.on(kVotesHost, QByteArray(), 200, catalog_test::fixture(QStringLiteral("ryd_votes.json")));
    settings.setValue(QLatin1String(DislikeInfo::kSettingsKey), true);
    DislikeService heldVotes(&held.net);
    DislikeInfo heldDislikes(&heldVotes, &settings);
    CHECK(heldDislikes.enabled());
    WatchInfo heldInfo(&held.service);
    WatchDislikes heldGlue(&heldInfo, &heldDislikes);
    heldInfo.setWaitForPlayback(true);
    heldInfo.setVideoId(kSeed);
    catalog_test::waitFor([] { return false; }, 50);
    CHECK(heldInfo.deferred());
    CHECK_EQ(held.net.requests.size(), 0);
    heldInfo.setPlaybackRunning(true);
    CHECK(catalog_test::waitFor([&heldInfo] { return heldInfo.dislikeCount() >= 0; }));
    CHECK_EQ(held.net.count(kVotesHost), 1);

    heldInfo.setVideoId(QString());
    heldInfo.setOffline(true);
    heldInfo.setVideoId(kSeed);                       // its watch data is still cached: shown at once
    CHECK(!heldInfo.deferred());
    catalog_test::waitFor([] { return false; }, 50);
    CHECK_EQ(heldInfo.dislikeCount(), -1.0);
    CHECK_EQ(held.net.count(kVotesHost), 1);
    heldInfo.setOffline(false);                       // played from the network instead
    CHECK(catalog_test::waitFor([&heldInfo] { return heldInfo.dislikeCount() >= 0; }));
    CHECK_EQ(held.net.count(kVotesHost), 1);          // answered from the service's ten-minute cache

    // setDislikeEstimate itself: unknown is -1 with no text, and an unchanged value is silent.
    WatchInfo plain(&rig.service);
    int plainChanges = 0;
    QObject::connect(&plain, &WatchInfo::changed, [&plainChanges] { ++plainChanges; });
    plain.setDislikeEstimate(12, QStringLiteral("12"));
    CHECK_EQ(plain.dislikeCount(), 12.0);
    CHECK_EQ(plain.dislikeText(), QStringLiteral("12"));
    CHECK_EQ(plainChanges, 1);
    plain.setDislikeEstimate(12, QStringLiteral("12"));
    CHECK_EQ(plainChanges, 1);
    plain.setDislikeEstimate(-5, QStringLiteral("ignored"));
    CHECK_EQ(plain.dislikeCount(), -1.0);
    CHECK(plain.dislikeText().isEmpty());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    chapterRules();
    chaptersFromFixture();
    descriptionRunsPlain();
    infoFields();
    structuredFields();
    relatedPaging();
    cacheAndSingleFlight();
    validEmptyVsDrift();
    presentation();
    holdUntilPlaying();
    hashtagBrowseRun();
    channelAvatars();
    upcomingStream();
    relatedHidesShorts();
    dislikeEstimate();
    return catalog_test::finish("catalog_watch");
}
