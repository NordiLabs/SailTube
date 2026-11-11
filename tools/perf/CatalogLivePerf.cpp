// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Opt-in live latency harness for the catalog component's network paths. It talks to live YouTube
// (www.youtube.com, suggestqueries.google.com) through the real catalog classes and is never
// part of the offline gate. Built and run by tools/perf/catalog_live_perf.sh.
//
//   CatalogLivePerf [--runs N] [--query Q] [--prewarm] [--prefix P] [--channel UC...]
//                   [--video ID] [--playlist ID]
//
// Every scenario runs N times. Each sample prints "sailtube-timing perf <scenario> <ms>";
// failures print "# <scenario> run <i>: <outcome> ..." and are left out of the statistics.
// A Markdown table (scenario | runs | min | median | max | bytes | notes) closes the output.
//
// Network vs parse split: next to each service call the harness sends the same request raw
// through the transport (InnerTubeClient::buildRequest) and times the transport's own
// request-to-last-byte (".net"), the JSON decode (".json") and the service's own parser
// (".parse") separately, on the main thread. The library code is not modified.

#include "Core/Catalog/Domain/Comments.h"
#include "Core/Catalog/Domain/Outcome.h"
#include "Core/Catalog/Domain/SearchFilter.h"
#include "Core/Catalog/Infrastructure/ChannelService.h"
#include "Core/Catalog/Infrastructure/ClientIdentity.h"
#include "Core/Catalog/Infrastructure/CommentsService.h"
#include "Core/Catalog/Infrastructure/HttpTransport.h"
#include "Core/Catalog/Infrastructure/InnerTubeClient.h"
#include "Core/Catalog/Infrastructure/PlaylistService.h"
#include "Core/Catalog/Infrastructure/RssParser.h"
#include "Core/Catalog/Infrastructure/SearchService.h"
#include "Core/Catalog/Infrastructure/SuggestService.h"
#include "Core/Catalog/Infrastructure/WatchNextService.h"
#include "Core/Catalog/Infrastructure/WorkLane.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QPointer>
#include <QScopedPointer>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <memory>

using namespace nordtile::video::catalog;

namespace {

const char *kYouTubeHost = "www.youtube.com";
const char *kSuggestHost = "suggestqueries.google.com";
const int kWaitMs = 25000;

void out(const QString &line) {
    std::fputs(line.toUtf8().constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// One scenario's samples.
class Stats {
public:
    explicit Stats(const QString &n) : name(n), attempts(0) {}
    QString name;
    QList<qint64> ms;
    QList<qint64> bytes;
    int attempts;
    QMap<QString, int> failures;
    QStringList notes;

    void addNote(const QString &note) { if (!notes.contains(note)) notes.append(note); }
};

class Report {
public:
    ~Report() { qDeleteAll(list_); }
    Stats *get(const QString &name) {
        for (Stats *s : list_) if (s->name == name) return s;
        Stats *s = new Stats(name);
        list_.append(s);
        return s;
    }
    void ok(const QString &name, qint64 ms, qint64 bytes = -1) {
        Stats *s = get(name);
        ++s->attempts;
        s->ms.append(ms);
        if (bytes >= 0) s->bytes.append(bytes);
        out(QStringLiteral("sailtube-timing perf %1 %2").arg(name).arg(ms));
    }
    void fail(const QString &name, int run, const QString &outcome, qint64 ms, const QString &detail = QString()) {
        Stats *s = get(name);
        ++s->attempts;
        ++s->failures[outcome];
        out(QStringLiteral("# %1 run %2: %3 after %4 ms%5").arg(name).arg(run).arg(outcome).arg(ms)
                .arg(detail.isEmpty() ? QString() : QStringLiteral(" (") + detail + QStringLiteral(")")));
    }
    void note(const QString &name, const QString &note) { get(name)->addNote(note); }
    int successes() const {
        int n = 0;
        for (const Stats *s : list_) n += s->ms.size();
        return n;
    }
    void table() const {
        out(QString());
        out(QStringLiteral("| scenario | runs | min | median | max | bytes | notes |"));
        out(QStringLiteral("|---|---|---|---|---|---|---|"));
        // Each scenario, then its ".net" / ".json" / ".parse" split rows.
        QList<const Stats *> ordered;
        for (const Stats *s : list_) {
            if (s->name.contains(QLatin1Char('.'))) continue;
            ordered.append(s);
            for (const Stats *sub : list_)
                if (sub->name.startsWith(s->name + QLatin1Char('.'))) ordered.append(sub);
        }
        for (const Stats *s : list_)
            if (!ordered.contains(s)) ordered.append(s);
        for (const Stats *s : ordered) {
            QStringList notes = s->notes;
            for (auto it = s->failures.constBegin(); it != s->failures.constEnd(); ++it)
                notes.prepend(QStringLiteral("%1x %2").arg(it.value()).arg(it.key()));
            const QString runs = QStringLiteral("%1/%2").arg(s->ms.size()).arg(s->attempts);
            QString mn = QStringLiteral("-"), md = QStringLiteral("-"), mx = QStringLiteral("-");
            if (!s->ms.isEmpty()) {
                QList<qint64> v = s->ms;
                std::sort(v.begin(), v.end());
                mn = QString::number(v.first());
                mx = QString::number(v.last());
                md = QString::number(median(v));
            }
            QString b = QStringLiteral("-");
            QList<qint64> bytes = s->bytes;
            if (bytes.isEmpty()) {   // a service row: the size its raw resend saw
                for (const Stats *sub : list_)
                    if (sub->name == s->name + QStringLiteral(".net")) bytes = sub->bytes;
            }
            if (!bytes.isEmpty()) {
                QList<qint64> v = bytes;
                std::sort(v.begin(), v.end());
                b = QString::number(median(v));
            }
            out(QStringLiteral("| %1 | %2 | %3 | %4 | %5 | %6 | %7 |").arg(s->name, runs, mn, md, mx, b,
                                                                       notes.join(QStringLiteral("; "))));
        }
    }
    static qint64 median(const QList<qint64> &sorted) {
        const int n = sorted.size();
        if (n == 0) return 0;
        if (n % 2) return sorted.at(n / 2);
        return (sorted.at(n / 2 - 1) + sorted.at(n / 2)) / 2;
    }
private:
    QList<Stats *> list_;
};

// Runs a nested event loop until finish() or the timeout.
class Waiter {
public:
    Waiter() : done_(false) {}
    void finish() { done_ = true; loop_.quit(); }
    bool wait(int ms = kWaitMs) {
        if (done_) return true;
        QTimer timer;
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, &loop_, &QEventLoop::quit);
        timer.start(ms);
        loop_.exec();
        return done_;
    }
private:
    bool done_;
    QEventLoop loop_;
};

void idle(int ms) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, SLOT(quit()));
    loop.exec();
}

// NetworkTransport::warm (connectToHostEncrypted) when this checkout has it; false otherwise.
template <typename T>
auto tryWarm(T *t, const QString &host, int) -> decltype(t->warm(host), bool()) {
    t->warm(host);
    return true;
}
template <typename T>
bool tryWarm(T *, const QString &, long) { return false; }

// The fallback pre-warm: a cheap GET on the same transport (opens TCP + TLS for the pool).
qint64 warmByRequest(HttpTransport *t, const QUrl &url, QObject *ctx, int *status) {
    HttpRequest req;
    req.url = url;
    req.timeoutMs = 10000;
    Waiter w;
    QElapsedTimer clock;
    clock.start();
    t->send(req, ctx, [&w, status](const HttpReply &r) { *status = r.status; w.finish(); });
    w.wait();
    return clock.elapsed();
}

struct RawResult {
    RawResult() : ok(false), netMs(-1), jsonMs(-1), parseMs(-1), bytes(-1), status(0) {}
    bool ok;
    qint64 netMs;
    qint64 jsonMs;
    qint64 parseMs;
    qint64 bytes;
    int status;
    QString outcome;
};

// The same InnerTube request sent raw: transport time, JSON decode and the service parser timed
// apart. `parse` returns the outcome of the service's parser.
RawResult rawInnerTube(InnerTubeClient *client, const QString &endpoint, const QJsonObject &body, QObject *ctx,
                       std::function<Outcome(const InnerTubeReply &)> parse) {
    RawResult res;
    HttpReply http;
    Waiter w;
    client->transport()->send(client->buildRequest(endpoint, body, 10000), ctx, [&w, &http](const HttpReply &r) {
        http = r;
        w.finish();
    });
    if (!w.wait()) { res.outcome = QStringLiteral("harness-timeout"); return res; }
    res.netMs = http.elapsedMs;
    res.bytes = http.body.size();
    res.status = http.status;
    QElapsedTimer t;
    t.start();
    const InnerTubeReply reply = InnerTubeClient::toReply(http);
    res.jsonMs = t.elapsed();
    if (reply.outcome != Outcome::Ok) {
        res.outcome = QString::fromLatin1(outcomeName(reply.outcome));
        return res;
    }
    t.restart();
    const Outcome o = parse(reply);
    res.parseMs = t.elapsed();
    res.outcome = QString::fromLatin1(outcomeName(o));
    res.ok = !isFailure(o);
    return res;
}

void reportRaw(Report &rep, const QString &scenario, int run, const RawResult &r) {
    if (r.netMs < 0) { rep.fail(scenario + QStringLiteral(".net"), run, r.outcome, kWaitMs); return; }
    if (r.status != 200) {
        rep.fail(scenario + QStringLiteral(".net"), run, r.outcome, r.netMs, QStringLiteral("HTTP %1").arg(r.status));
        return;
    }
    rep.ok(scenario + QStringLiteral(".net"), r.netMs, r.bytes);
    rep.ok(scenario + QStringLiteral(".json"), r.jsonMs, r.bytes);
    if (r.parseMs >= 0) {
        if (r.ok) rep.ok(scenario + QStringLiteral(".parse"), r.parseMs, r.bytes);
        else rep.fail(scenario + QStringLiteral(".parse"), run, r.outcome, r.parseMs);
    }
}

QString variant(const QString &base, const QStringList &words, int i) {
    QString w = words.at(i % words.size());
    if (i >= words.size()) w += QStringLiteral(" %1").arg(i);
    return base + QLatin1Char(' ') + w;
}

struct Options {
    Options() : runs(5), prewarm(false), query(QStringLiteral("big buck bunny 4k")), prefix(QStringLiteral("big buck")),
                channel(QStringLiteral("UCLA_DiR1FfKNvjuUpBHmylQ")), video(QStringLiteral("9zCH37330f8")),
                playlist(QStringLiteral("PLF98rxtslw6s")) {}
    int runs;
    bool prewarm;
    QString query, prefix, channel, video, playlist;
};

bool parseArgs(const QStringList &args, Options *o) {
    for (int i = 1; i < args.size(); ++i) {
        const QString a = args.at(i);
        const bool hasValue = i + 1 < args.size();
        if (a == QLatin1String("--prewarm")) o->prewarm = true;
        else if (a == QLatin1String("--runs") && hasValue) o->runs = qMax(1, args.at(++i).toInt());
        else if (a == QLatin1String("--query") && hasValue) o->query = args.at(++i);
        else if (a == QLatin1String("--prefix") && hasValue) o->prefix = args.at(++i);
        else if (a == QLatin1String("--channel") && hasValue) o->channel = args.at(++i);
        else if (a == QLatin1String("--video") && hasValue) o->video = args.at(++i);
        else if (a == QLatin1String("--playlist") && hasValue) o->playlist = args.at(++i);
        else {
            std::fprintf(stderr, "usage: %s [--runs N] [--query Q] [--prewarm] [--prefix P] [--channel UC..] "
                                 "[--video ID] [--playlist ID]\n", qPrintable(args.at(0)));
            return false;
        }
    }
    return true;
}

}   // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    Options opt;
    if (!parseArgs(app.arguments(), &opt)) return 2;

    QObject ctx;
    Report rep;
    const DiscoveryLocale locale;   // en / US: comparable across machines
    const QString hl = locale.hl;
    const int N = opt.runs;

    // Composition as in CatalogComposition: two transports, two lanes, one identity. The
    // identity scrape gets its own transport so it does not pre-open the measured connections.
    QTemporaryDir tmp;
    NetworkTransport identityTransport(QStringLiteral("perf-identity"));
    NetworkTransport interactive(QStringLiteral("discovery-interactive"));
    NetworkTransport background(QStringLiteral("discovery-background"));
    WorkLane interactiveLane(QStringLiteral("discovery-interactive"), 2);
    WorkLane backgroundLane(QStringLiteral("discovery-background"), 1);
    ClientIdentity identity(&identityTransport, tmp.path() + QStringLiteral("/client.json"));
    {
        QElapsedTimer clock;
        clock.start();
        identity.start();
        while (identity.scrapeInFlight() && clock.elapsed() < 15000) idle(50);
        out(QStringLiteral("# client identity: %1 (%2), %3 ms").arg(identity.clientVersion(), identity.source())
                .arg(clock.elapsed()));
    }
    InnerTubeClient interactiveClient(&interactive, &identity, &interactiveLane, locale);
    InnerTubeClient backgroundClient(&background, &identity, &backgroundLane, locale);
    out(QStringLiteral("# runs=%1 query=\"%2\" prewarm=%3").arg(N).arg(opt.query).arg(opt.prewarm ? "yes" : "no"));

    const SearchFilter filter;

    // a. search_cold: a new transport + client per run (new TCP + TLS).
    const QStringList coldWords = QStringList() << QStringLiteral("trailer") << QStringLiteral("blender")
                                                << QStringLiteral("60fps") << QStringLiteral("open movie")
                                                << QStringLiteral("animation");
    rep.note(QStringLiteral("search_cold"), QStringLiteral("new transport per run; query + suffix word"));
    for (int i = 0; i < N; ++i) {
        QScopedPointer<NetworkTransport> t(new NetworkTransport(QStringLiteral("perf-cold")));
        QScopedPointer<InnerTubeClient> c(new InnerTubeClient(t.data(), &identity, &interactiveLane, locale));
        QScopedPointer<SearchService> s(new SearchService(c.data()));
        const QString q = variant(opt.query, coldWords, i);
        Waiter w;
        SearchResult result;
        QElapsedTimer clock;
        clock.start();
        s->firstPage(q, filter, [&](const SearchResult &r) { result = r; w.finish(); });
        const bool arrived = w.wait();
        const qint64 ms = clock.elapsed();
        if (!arrived) rep.fail(QStringLiteral("search_cold"), i, QStringLiteral("harness-timeout"), ms);
        else if (isFailure(result.outcome))
            rep.fail(QStringLiteral("search_cold"), i, QString::fromLatin1(outcomeName(result.outcome)), ms,
                     QStringLiteral("HTTP %1").arg(result.httpStatus));
        else rep.ok(QStringLiteral("search_cold"), ms);
        interactiveLane.drain();
        idle(10);   // let the deleted reply objects go before the transport
    }

    // Pre-warm the shared interactive transport.
    if (opt.prewarm) {
        QElapsedTimer clock;
        clock.start();
        if (tryWarm(&interactive, QString::fromLatin1(kYouTubeHost), 0)) {
            tryWarm(&interactive, QString::fromLatin1(kSuggestHost), 0);
            idle(1500);
            out(QStringLiteral("# prewarm: NetworkTransport::warm (connectToHostEncrypted) for %1 and %2, waited %3 ms")
                    .arg(kYouTubeHost).arg(kSuggestHost).arg(clock.elapsed()));
            rep.note(QStringLiteral("search_warm"), QStringLiteral("prewarmed with NetworkTransport::warm"));
        } else {
            int s1 = 0, s2 = 0;
            const qint64 a = warmByRequest(&interactive, QUrl(QStringLiteral("https://www.youtube.com/generate_204")), &ctx, &s1);
            const qint64 b = warmByRequest(&interactive,
                                           SuggestService::buildRequest(QStringLiteral("a"), hl).url, &ctx, &s2);
            out(QStringLiteral("# prewarm: no NetworkTransport::warm in this checkout; cheap GETs instead: "
                               "generate_204 %1 ms (HTTP %2), suggest %3 ms (HTTP %4)").arg(a).arg(s1).arg(b).arg(s2));
            rep.note(QStringLiteral("search_warm"), QStringLiteral("prewarmed with a cheap GET"));
        }
    } else {
        rep.note(QStringLiteral("search_warm"), QStringLiteral("no --prewarm: run 0 opens the connection"));
    }

    // b. search_warm + c. search_page2 on the shared interactive transport.
    const QStringList warmWords = QStringList() << QStringLiteral("short film") << QStringLiteral("full movie")
                                                << QStringLiteral("8k") << QStringLiteral("soundtrack")
                                                << QStringLiteral("making of");
    SearchService search(&interactiveClient);
    for (int i = 0; i < N; ++i) {
        const QString q = variant(opt.query, warmWords, i);
        SearchResult first;
        {
            Waiter w;
            QElapsedTimer clock;
            clock.start();
            search.firstPage(q, filter, [&](const SearchResult &r) { first = r; w.finish(); });
            const bool arrived = w.wait();
            const qint64 ms = clock.elapsed();
            if (!arrived) rep.fail(QStringLiteral("search_warm"), i, QStringLiteral("harness-timeout"), ms);
            else if (isFailure(first.outcome))
                rep.fail(QStringLiteral("search_warm"), i, QString::fromLatin1(outcomeName(first.outcome)), ms,
                         QStringLiteral("HTTP %1").arg(first.httpStatus));
            else rep.ok(QStringLiteral("search_warm"), ms);
            if (arrived && i == 0) rep.note(QStringLiteral("search_warm"), QStringLiteral("%1 rows").arg(first.rows.size()));
        }
        if (!first.continuation.isEmpty()) {
            Waiter w;
            SearchResult next;
            QElapsedTimer clock;
            clock.start();
            search.nextPage(q, filter, first.continuation, int(first.rows.size()),
                            [&](const SearchResult &r) { next = r; w.finish(); });
            const bool arrived = w.wait();
            const qint64 ms = clock.elapsed();
            if (!arrived) rep.fail(QStringLiteral("search_page2"), i, QStringLiteral("harness-timeout"), ms);
            else if (isFailure(next.outcome))
                rep.fail(QStringLiteral("search_page2"), i, QString::fromLatin1(outcomeName(next.outcome)), ms,
                         QStringLiteral("HTTP %1").arg(next.httpStatus));
            else rep.ok(QStringLiteral("search_page2"), ms);
            if (arrived && i == 0) rep.note(QStringLiteral("search_page2"), QStringLiteral("%1 rows").arg(next.rows.size()));
            // Raw split of a continuation (same token: the server answers it again).
            QJsonObject body;
            body.insert(QStringLiteral("continuation"), first.continuation);
            reportRaw(rep, QStringLiteral("search_page2"), i,
                      rawInnerTube(&interactiveClient, QStringLiteral("search"), body, &ctx,
                                   [hl](const InnerTubeReply &r) { return SearchService::parseContinuation(r.json, hl).outcome; }));
        } else if (first.outcome == Outcome::Ok) {
            rep.fail(QStringLiteral("search_page2"), i, QStringLiteral("no-continuation"), 0);
        }
        // Raw split of page 1 with its own distinct query (no server-side repeat).
        reportRaw(rep, QStringLiteral("search_warm"), i,
                  rawInnerTube(&interactiveClient, QStringLiteral("search"),
                               SearchService::firstPageBody(q + QStringLiteral(" hd"), filter), &ctx,
                               [hl](const InnerTubeReply &r) { return SearchService::parseFirstPage(r.json, hl).outcome; }));
    }
    rep.note(QStringLiteral("search_warm.net"), QStringLiteral("raw POST /search, query + \" hd\""));

    // d. suggest: distinct prefixes (the service caches exact queries for 10 min).
    {
        SuggestService suggest(&interactive, locale);
        const QStringList tails = QStringList() << QString() << QStringLiteral(" b") << QStringLiteral(" bu")
                                                << QStringLiteral(" bun") << QStringLiteral(" bunn");
        for (int i = 0; i < N; ++i) {
            QString prefix = opt.prefix + tails.at(i % tails.size());
            if (i >= tails.size()) prefix += QString::number(i);
            Waiter w;
            SuggestResult result;
            QElapsedTimer clock;
            clock.start();
            suggest.fetch(prefix, [&](const SuggestResult &r) { result = r; w.finish(); });
            const bool arrived = w.wait();
            const qint64 ms = clock.elapsed();
            if (!arrived) rep.fail(QStringLiteral("suggest"), i, QStringLiteral("harness-timeout"), ms);
            else if (isFailure(result.outcome))
                rep.fail(QStringLiteral("suggest"), i, QString::fromLatin1(outcomeName(result.outcome)), ms);
            else rep.ok(QStringLiteral("suggest"), ms);
            if (arrived && i == 0) rep.note(QStringLiteral("suggest"), QStringLiteral("%1 entries").arg(result.suggestions.size()));
            // Raw split (same prefix, transport only).
            HttpReply http;
            Waiter rw;
            interactive.send(SuggestService::buildRequest(prefix + QStringLiteral("x"), hl), &ctx,
                             [&](const HttpReply &r) { http = r; rw.finish(); });
            if (rw.wait() && http.status == 200) {
                rep.ok(QStringLiteral("suggest.net"), http.elapsedMs, http.body.size());
                QElapsedTimer t;
                t.start();
                SuggestService::parse(http.body);
                rep.ok(QStringLiteral("suggest.parse"), t.elapsed(), http.body.size());
            } else {
                rep.fail(QStringLiteral("suggest.net"), i, QStringLiteral("http"), http.elapsedMs,
                         QStringLiteral("HTTP %1").arg(http.status));
            }
        }
        rep.note(QStringLiteral("suggest"), QStringLiteral("host %1").arg(kSuggestHost));
    }

    // e. channel_videos: a fresh service per run (no learnt tab params, as on a first open).
    for (int i = 0; i < N; ++i) {
        ChannelService channels(&interactiveClient);
        Waiter w;
        ChannelPage page;
        QElapsedTimer clock;
        clock.start();
        channels.browseTab(opt.channel, QStringLiteral("videos"), &ctx, [&](ChannelPage p) { page = p; w.finish(); });
        const bool arrived = w.wait();
        const qint64 ms = clock.elapsed();
        if (!arrived) rep.fail(QStringLiteral("channel_videos"), i, QStringLiteral("harness-timeout"), ms);
        else if (isFailure(page.outcome))
            rep.fail(QStringLiteral("channel_videos"), i, QString::fromLatin1(outcomeName(page.outcome)), ms,
                     QStringLiteral("HTTP %1").arg(page.httpStatus));
        else rep.ok(QStringLiteral("channel_videos"), ms);
        if (arrived && i == 0) {
            rep.note(QStringLiteral("channel_videos"), QStringLiteral("%1 rows, tab %2%3").arg(page.items.size())
                         .arg(page.selectedTab).arg(page.tabMissing ? QStringLiteral(" (videos missing)") : QString()));
            if (channels.tabParams(opt.channel, QStringLiteral("videos")) != ChannelService::fallbackTabParams(QStringLiteral("videos")))
                rep.note(QStringLiteral("channel_videos"), QStringLiteral("channel params differ from recorded ones (may cost a retry)"));
        }
        QJsonObject body;
        body.insert(QStringLiteral("browseId"), opt.channel);
        body.insert(QStringLiteral("params"), ChannelService::fallbackTabParams(QStringLiteral("videos")));
        const QString channelId = opt.channel;
        reportRaw(rep, QStringLiteral("channel_videos"), i,
                  rawInnerTube(&interactiveClient, QStringLiteral("browse"), body, &ctx, [channelId, hl](const InnerTubeReply &r) {
                      return ChannelService::parseBrowse(r, channelId, QStringLiteral("videos"), hl).outcome;
                  }));
    }

    // f. watch_next (POST /next) and watch_player (WEB POST /player), started together.
    rep.note(QStringLiteral("watch_next"), QStringLiteral("fresh service per run; /next and /player in parallel"));
    QString commentsToken;
    for (int i = 0; i < N; ++i) {
        WatchNextService watch(&interactiveClient);
        Waiter w;
        QElapsedTimer clock;
        qint64 nextMs = -1, playerMs = -1;
        const QString vid = opt.video;
        QObject::connect(&watch, &WatchNextService::pageChanged, &ctx, [&](const QString &id) {
            if (id != vid) return;
            std::shared_ptr<const WatchPage> p = watch.page(vid);
            if (!p) return;
            if (p->nextDone && nextMs < 0) nextMs = clock.elapsed();
            if (p->playerDone && playerMs < 0) playerMs = clock.elapsed();
            if (p->nextDone && p->playerDone) w.finish();
        });
        clock.start();
        if (!watch.request(vid)) { rep.fail(QStringLiteral("watch_next"), i, QStringLiteral("invalid-id"), 0); break; }
        w.wait();
        std::shared_ptr<const WatchPage> p = watch.page(vid);
        if (!p || nextMs < 0) rep.fail(QStringLiteral("watch_next"), i, QStringLiteral("harness-timeout"), clock.elapsed());
        else if (isFailure(p->next.outcome))
            rep.fail(QStringLiteral("watch_next"), i, QString::fromLatin1(outcomeName(p->next.outcome)), nextMs);
        else rep.ok(QStringLiteral("watch_next"), nextMs);
        if (!p || playerMs < 0) rep.fail(QStringLiteral("watch_player"), i, QStringLiteral("harness-timeout"), clock.elapsed());
        else if (isFailure(p->player.outcome))
            rep.fail(QStringLiteral("watch_player"), i, QString::fromLatin1(outcomeName(p->player.outcome)), playerMs);
        else rep.ok(QStringLiteral("watch_player"), playerMs);
        if (p && i == 0) {
            rep.note(QStringLiteral("watch_next"), QStringLiteral("%1 related").arg(p->related.size()));
            rep.note(QStringLiteral("watch_player"), QStringLiteral("playability %1")
                         .arg(QString::fromStdString(p->player.playabilityStatus)));
        }
        if (p && commentsToken.isEmpty()) commentsToken = p->commentsContinuation;
        QJsonObject body;
        body.insert(QStringLiteral("videoId"), vid);
        body.insert(QStringLiteral("contentCheckOk"), true);
        body.insert(QStringLiteral("racyCheckOk"), true);
        reportRaw(rep, QStringLiteral("watch_next"), i,
                  rawInnerTube(&interactiveClient, QStringLiteral("next"), body, &ctx, [hl, vid](const InnerTubeReply &r) {
                      return WatchNextService::parseNext(r.json, hl, vid).outcome;
                  }));
        interactiveLane.drain();
        idle(10);
    }

    // g. comments on the background client: the entry (POST /next {videoId}) then page 1.
    {
        CommentsService comments(&backgroundClient);
        const std::string chl = comments.hl();
        for (int i = 0; i < N; ++i) {
            CommentsEntry entry;
            {
                Waiter w;
                QElapsedTimer clock;
                clock.start();
                comments.fetchEntry(opt.video, &ctx, [&](const CommentsEntry &e) { entry = e; w.finish(); });
                const bool arrived = w.wait();
                const qint64 ms = clock.elapsed();
                if (!arrived) rep.fail(QStringLiteral("comments_entry"), i, QStringLiteral("harness-timeout"), ms);
                else if (isFailure(entry.outcome))
                    rep.fail(QStringLiteral("comments_entry"), i, QString::fromLatin1(outcomeName(entry.outcome)), ms);
                else rep.ok(QStringLiteral("comments_entry"), ms);
                if (arrived && i == 0)
                    rep.note(QStringLiteral("comments_entry"), QStringLiteral("total %1%2")
                                 .arg(QString::fromStdString(entry.totalText))
                                 .arg(entry.commentsOff ? QStringLiteral(" (comments off)") : QString()));
            }
            QString token = QString::fromStdString(entry.token);
            if (token.isEmpty()) token = commentsToken;   // watch-next's copy
            if (token.isEmpty()) {
                rep.fail(QStringLiteral("comments_p1"), i, QStringLiteral("no-token"), 0);
                continue;
            }
            Waiter w;
            CommentsPage page;
            QElapsedTimer clock;
            clock.start();
            comments.fetchThreads(token, &ctx, [&](const CommentsPage &p) { page = p; w.finish(); });
            const bool arrived = w.wait();
            const qint64 ms = clock.elapsed();
            if (!arrived) rep.fail(QStringLiteral("comments_p1"), i, QStringLiteral("harness-timeout"), ms);
            else if (isFailure(page.outcome))
                rep.fail(QStringLiteral("comments_p1"), i, QString::fromLatin1(outcomeName(page.outcome)), ms);
            else rep.ok(QStringLiteral("comments_p1"), ms);
            if (arrived && i == 0)
                rep.note(QStringLiteral("comments_p1"), QStringLiteral("%1 threads, %2 missing bodies")
                             .arg(page.comments.size()).arg(page.missingBodies));
            QJsonObject body;
            body.insert(QStringLiteral("continuation"), token);
            reportRaw(rep, QStringLiteral("comments_p1"), i,
                      rawInnerTube(&backgroundClient, QStringLiteral("next"), body, &ctx, [chl](const InnerTubeReply &r) {
                          return CommentsService::parsePage(r.json, chl, false).outcome;
                      }));
        }
        rep.note(QStringLiteral("comments_entry"), QStringLiteral("background transport"));
    }

    // h. rss: one GET of the channel feed through the interactive transport, parsed here.
    for (int i = 0; i < N; ++i) {
        HttpRequest req;
        req.url = RssParser::feedUrl(opt.channel);
        req.timeoutMs = 10000;
        HttpReply http;
        Waiter w;
        QElapsedTimer clock;
        clock.start();
        interactive.send(req, &ctx, [&](const HttpReply &r) { http = r; w.finish(); });
        const bool arrived = w.wait();
        QElapsedTimer pt;
        pt.start();
        const RssFeed feed = RssParser::parse(http.body, http.status);
        const qint64 parseMs = pt.elapsed();
        const qint64 ms = clock.elapsed();
        if (!arrived) { rep.fail(QStringLiteral("rss"), i, QStringLiteral("harness-timeout"), ms); continue; }
        if (isFailure(feed.outcome)) {
            rep.fail(QStringLiteral("rss"), i, QString::fromLatin1(outcomeName(feed.outcome)), ms,
                     QStringLiteral("HTTP %1").arg(http.status));
            continue;
        }
        rep.ok(QStringLiteral("rss"), ms, http.body.size());
        rep.ok(QStringLiteral("rss.net"), http.elapsedMs, http.body.size());
        rep.ok(QStringLiteral("rss.parse"), parseMs, http.body.size());
        if (i == 0) rep.note(QStringLiteral("rss"), QStringLiteral("%1 entries").arg(feed.items.size()));
    }

    // i. playlist_p1.
    {
        PlaylistService playlists(&interactiveClient);
        const QString listId = PlaylistService::normalizeListId(opt.playlist);
        for (int i = 0; i < N; ++i) {
            Waiter w;
            PlaylistPage page;
            QElapsedTimer clock;
            clock.start();
            playlists.open(opt.playlist, &ctx, [&](PlaylistPage p) { page = p; w.finish(); });
            const bool arrived = w.wait();
            const qint64 ms = clock.elapsed();
            if (!arrived) rep.fail(QStringLiteral("playlist_p1"), i, QStringLiteral("harness-timeout"), ms);
            else if (isFailure(page.outcome))
                rep.fail(QStringLiteral("playlist_p1"), i, QString::fromLatin1(outcomeName(page.outcome)), ms,
                         QStringLiteral("HTTP %1").arg(page.httpStatus));
            else rep.ok(QStringLiteral("playlist_p1"), ms);
            if (arrived && i == 0 && !isFailure(page.outcome))
                rep.note(QStringLiteral("playlist_p1"), QStringLiteral("%1 rows").arg(page.items.size()));
            QJsonObject body;
            body.insert(QStringLiteral("browseId"), QStringLiteral("VL") + listId);
            reportRaw(rep, QStringLiteral("playlist_p1"), i,
                      rawInnerTube(&interactiveClient, QStringLiteral("browse"), body, &ctx, [listId, hl](const InnerTubeReply &r) {
                          return PlaylistService::parseBrowse(r, listId, hl).outcome;
                      }));
        }
        rep.note(QStringLiteral("playlist_p1"), QStringLiteral("list %1").arg(listId));
    }

    interactiveLane.drain();
    backgroundLane.drain();

    QStringList hosts = interactive.contactedHosts() + background.contactedHosts() + identityTransport.contactedHosts();
    hosts.removeDuplicates();
    out(QStringLiteral("# hosts contacted: %1").arg(hosts.join(QStringLiteral(", "))));
    out(QStringLiteral("# .net = transport request to last byte (raw resend), .json = JSON decode, "
                       ".parse = service parser; times in ms, bytes = median body size"));
    rep.table();
    return rep.successes() > 0 ? 0 : 1;
}
