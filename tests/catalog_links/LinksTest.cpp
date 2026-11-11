// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Link handling (LNK-01..LNK-04, PLT-13): the classification table, t=/start= parsing, D-Bus
// argument normalisation, the cold-start queue, route-then-raise order, LNK-04 routing, the
// D-Bus adaptor (direct calls, then real s / as / v calls over the session bus when one is
// connected), and static checks of the desktop file, the service file and the packaging.
#include "tests/catalog_common/TestSupport.h"

#include "Core/Catalog/Domain/Links.h"
#include "Clients/Sailfish/Presentation/DBusLinkAdaptor.h"
#include "Clients/Sailfish/Presentation/LinkRouter.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusVariant>
#include <QFile>
#include <QStringList>

using namespace nordtile::video;
using catalog::LinkKind;
using catalog::LinkTarget;

namespace {

const char *const V = "9zCH37330f8";
const char *const V2 = "ROboUHfPUrU";
const char *const L = "PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG";
const char *const UC = "UCLA_DiR1FfKNvjuUpBHmylQ";

std::string str(const char *s) { return s ? std::string(s) : std::string(); }

const char *kindName(LinkKind k) {
    switch (k) {
    case LinkKind::Video: return "video";
    case LinkKind::Playlist: return "playlist";
    case LinkKind::Channel: return "channel";
    case LinkKind::Clip: return "clip";
    case LinkKind::Unsupported: break;
    }
    return "unsupported";
}

struct Row {
    std::string url;
    LinkKind kind;
    std::string videoId;
    int start;
    std::string playlist;
    int index;
    std::string channel;
    bool sailTune;
    bool embed;
};

Row video(const std::string &url, const char *id, int start = -1, const char *list = nullptr, int index = -1) {
    Row r = { url, LinkKind::Video, str(id), start, str(list), index, std::string(), false, false };
    return r;
}
Row playlist(const std::string &url, const char *list) {
    Row r = { url, LinkKind::Playlist, std::string(), -1, str(list), -1, std::string(), false, false };
    return r;
}
Row channel(const std::string &url, const std::string &ref) {
    Row r = { url, LinkKind::Channel, std::string(), -1, std::string(), -1, ref, false, false };
    return r;
}
Row none(const std::string &url) {
    Row r = { url, LinkKind::Unsupported, std::string(), -1, std::string(), -1, std::string(), false, false };
    return r;
}
Row embedded(Row r) { r.embed = true; return r; }
Row music(Row r) { r.sailTune = true; return r; }

void linkTable() {
    const std::string v = V, v2 = V2, l = L, uc = UC;
    std::vector<Row> rows;
    // 1. youtu.be/<id>
    rows.push_back(video("https://youtu.be/" + v, V));
    rows.push_back(video("https://youtu.be/" + v + "?t=90", V, 90));
    rows.push_back(video("https://youtu.be/" + v + "?si=AbCdEfGh123", V));          // tracking parameter
    rows.push_back(video("youtu.be/" + v, V));                                       // no scheme
    rows.push_back(video("http://youtu.be/" + v + "/", V));                          // http, trailing slash
    rows.push_back(video("  https://youtu.be/" + v + " \n", V));                     // whitespace around
    rows.push_back(video("https://youtu.be/" + v + "?list=" + l + "&t=5", V, 5, L)); // keeps the list
    // 2. v=
    rows.push_back(video("https://www.youtube.com/watch?v=" + v, V));
    rows.push_back(video("https://youtube.com/watch?v=" + v + "&feature=share", V));
    rows.push_back(video("https://www.youtube.com/watch?feature=youtu.be&v=" + v, V));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&utm_source=x&pp=ygUFaGVsbG8%3D&ab_channel=NASA", V));
    rows.push_back(video("HTTPS://WWW.YOUTUBE.COM/watch?v=" + v, V));               // case-insensitive scheme and host
    rows.push_back(video("https://www.youtube.com:443/watch?v=" + v, V));            // port
    rows.push_back(video("https://m.youtube.com/watch?v=" + v + "&t=1m30s", V, 90));
    // t= / start= forms
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=90s", V, 90));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=1h2m3s", V, 3723));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=1m30", V, 90));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=0", V, 0));    // explicit start over
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&start=42", V, 42));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "#t=1m30s", V, 90)); // fragment
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=10#t=20", V, 10)); // query over fragment
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=90&start=10", V, 90)); // t over start
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=abc", V, -1));  // malformed t: resume applies
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&t=30s1m", V, -1)); // units out of order
    // 3. /shorts/ /embed/ /live/ /v/
    rows.push_back(video("https://www.youtube.com/shorts/" + v, V));
    rows.push_back(video("https://youtube.com/shorts/" + v + "?feature=share", V));
    rows.push_back(embedded(video("https://www.youtube.com/embed/" + v + "?start=30", V, 30)));
    rows.push_back(embedded(video("https://www.youtube-nocookie.com/embed/" + v + "?rel=0", V)));
    rows.push_back(video("https://www.youtube.com/live/" + v2 + "?si=xyz", V2));
    rows.push_back(video("https://www.youtube.com/v/" + v, V));
    // 4. list=: a video link keeps it (and index=); otherwise a playlist
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&list=" + l + "&index=3", V, -1, L, 3));
    rows.push_back(video("https://www.youtube.com/watch?list=" + l + "&v=" + v + "&t=42s", V, 42, L));
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&list=x", V));                 // invalid list dropped
    rows.push_back(video("https://www.youtube.com/watch?v=" + v + "&index=3", V));                // index without list
    rows.push_back(playlist("https://www.youtube.com/playlist?list=" + l, L));
    rows.push_back(playlist("https://m.youtube.com/playlist?list=" + l + "&si=abc", L));
    rows.push_back(playlist("https://www.youtube.com/watch?list=" + l, L));
    rows.push_back(embedded(playlist("https://www.youtube.com/embed/videoseries?list=" + l, L)));
    // 5. /channel/UC...
    rows.push_back(channel("https://www.youtube.com/channel/" + uc, uc));
    rows.push_back(channel("https://www.youtube.com/channel/" + uc + "/videos", uc));
    // 6. /@handle, /c/, /user/
    rows.push_back(channel("https://www.youtube.com/@NASA", "@NASA"));
    rows.push_back(channel("https://m.youtube.com/@NASA/videos?si=1", "@NASA"));
    rows.push_back(channel("https://www.youtube.com/@%E6%97%A5%E6%9C%AC", "@\xE6\x97\xA5\xE6\x9C\xAC")); // UTF-8 handle
    rows.push_back(channel("https://www.youtube.com/c/NASA", "https://www.youtube.com/c/NASA"));
    rows.push_back(channel("https://www.youtube.com/user/NASAtelevision/featured", "https://www.youtube.com/user/NASAtelevision"));
    // precedence when several forms apply
    rows.push_back(video("https://youtu.be/" + v + "?v=" + v2, V));                    // 1 before 2
    rows.push_back(video("https://www.youtube.com/shorts/" + v + "?v=" + v2, V2));     // 2 before 3
    rows.push_back(playlist("https://www.youtube.com/channel/" + uc + "?list=" + l, L)); // 4 before 5
    rows.push_back(video("https://www.youtube.com/@NASA?v=" + v, V));                  // 2 before 6
    // music.youtube.com: recognised, flagged as SailTune's
    rows.push_back(music(video("https://music.youtube.com/watch?v=" + v + "&list=" + l, V, -1, L)));
    rows.push_back(music(playlist("https://music.youtube.com/playlist?list=OLAK5uy_abcdefghijk", "OLAK5uy_abcdefghijk")));
    rows.push_back(music(none("https://music.youtube.com/channel/" + uc)));
    // malformed ids and foreign hosts
    rows.push_back(none("https://www.youtube.com/watch?v=short"));
    rows.push_back(none("https://www.youtube.com/watch?v=" + v + "x"));               // 12 characters
    rows.push_back(none("https://www.youtube.com/watch?v=9zCH3733$f8"));              // bad character
    rows.push_back(none("https://www.youtube.com/shorts/tooShort"));
    rows.push_back(none("https://www.youtube.com/channel/UCbad"));
    rows.push_back(none("https://youtu.be/"));
    rows.push_back(none("https://www.youtube.com/"));
    rows.push_back(none("https://www.youtube.com/feed/subscriptions"));
    rows.push_back(none("https://example.com/watch?v=" + v));
    rows.push_back(none("https://www.youtube.com.evil.example/watch?v=" + v));
    rows.push_back(none("https://evil.example/?u=https://youtu.be/" + v));
    rows.push_back(none("ftp://youtu.be/" + v));
    rows.push_back(none("mailto:someone@youtube.com"));
    rows.push_back(none(""));

    CHECK(rows.size() >= 40);
    for (const Row &r : rows) {
        const LinkTarget t = catalog::classifyLink(r.url);
        const std::string label = "[" + r.url + "] ";
        CHECK_EQ(label + kindName(t.kind), label + kindName(r.kind));
        CHECK_EQ(label + t.videoId, label + r.videoId);
        CHECK_EQ(label + std::to_string(t.startSeconds), label + std::to_string(r.start));
        CHECK_EQ(label + t.playlistId, label + r.playlist);
        CHECK_EQ(label + std::to_string(t.index), label + std::to_string(r.index));
        CHECK_EQ(label + t.channelRef, label + r.channel);
        CHECK_EQ(label + (t.sailTuneHost ? "music" : "-"), label + (r.sailTune ? "music" : "-"));
        if (r.kind != LinkKind::Unsupported) CHECK_EQ(label + (t.embed ? "embed" : "-"), label + (r.embed ? "embed" : "-"));
    }

    // Clip (MAY): recognised, not routed.
    const LinkTarget clip = catalog::classifyLink("https://www.youtube.com/clip/UgkxABCdefGHIjklMNOpqrSTUvwxYZ012345");
    CHECK(clip.kind == LinkKind::Clip);
    CHECK_EQ(clip.clipId, std::string("UgkxABCdefGHIjklMNOpqrSTUvwxYZ012345"));
    const LinkTarget handle = catalog::classifyLink("https://www.youtube.com/@NASA");
    CHECK(handle.channelKind == catalog::ChannelRefKind::Handle);
    CHECK(catalog::classifyLink("https://www.youtube.com/channel/" + uc).channelKind == catalog::ChannelRefKind::ChannelId);
    CHECK(catalog::classifyLink("https://www.youtube.com/c/NASA").channelKind == catalog::ChannelRefKind::Custom);
    CHECK(catalog::classifyLink("https://www.youtube.com/user/NASA").channelKind == catalog::ChannelRefKind::User);
    CHECK_EQ(catalog::classifyLink("https://WWW.YouTube.com/watch?v=" + v).host, std::string("www.youtube.com"));
}

void startTimes() {
    CHECK_EQ(catalog::parseStartTime("90"), 90);
    CHECK_EQ(catalog::parseStartTime("90s"), 90);
    CHECK_EQ(catalog::parseStartTime("1m30s"), 90);
    CHECK_EQ(catalog::parseStartTime("1h2m3s"), 3723);
    CHECK_EQ(catalog::parseStartTime("2m"), 120);
    CHECK_EQ(catalog::parseStartTime("1h"), 3600);
    CHECK_EQ(catalog::parseStartTime("1H2M3S"), 3723);
    CHECK_EQ(catalog::parseStartTime("0"), 0);
    CHECK_EQ(catalog::parseStartTime(""), -1);
    CHECK_EQ(catalog::parseStartTime("-5"), -1);
    CHECK_EQ(catalog::parseStartTime("1.5"), -1);
    CHECK_EQ(catalog::parseStartTime("m"), -1);
    CHECK_EQ(catalog::parseStartTime("1m1m"), -1);
    CHECK_EQ(catalog::parseStartTime("99999999999"), -1);
    CHECK(catalog::isValidVideoId(V));
    CHECK(!catalog::isValidVideoId("9zCH37330f"));
    CHECK(catalog::isValidChannelId(UC));
    CHECK(!catalog::isValidChannelId("UXLA_DiR1FfKNvjuUpBHmylQ"));
    CHECK(catalog::isValidPlaylistId(L));
    CHECK(!catalog::isValidPlaylistId("P"));
}

// Records router output as "video:<id>@<start>|<list>#<index>", "channel:<ref>",
// "playlist:<id>", "unsupported:<url>" and "raise".
struct RouterLog {
    QStringList events;
    explicit RouterLog(sailfish::LinkRouter *router) {
        QObject::connect(router, &sailfish::LinkRouter::openVideo,
                         [this](const QString &id, int start, const QString &list, int index) {
            events << QStringLiteral("video:%1@%2|%3#%4").arg(id).arg(start).arg(list).arg(index);
        });
        QObject::connect(router, &sailfish::LinkRouter::openChannel, [this](const QString &ref) {
            events << QStringLiteral("channel:") + ref;
        });
        QObject::connect(router, &sailfish::LinkRouter::openPlaylist, [this](const QString &list) {
            events << QStringLiteral("playlist:") + list;
        });
        QObject::connect(router, &sailfish::LinkRouter::unsupported, [this](const QString &url) {
            events << QStringLiteral("unsupported:") + url;
        });
        QObject::connect(router, &sailfish::LinkRouter::raiseRequested, [this]() { events << QStringLiteral("raise"); });
    }
    QString joined() const { return events.join(QLatin1Char(' ')); }
};

const QString kVideoUrl = QStringLiteral("https://youtu.be/9zCH37330f8?t=90");
const QString kVideoEvent = QStringLiteral("video:9zCH37330f8@90|#-1");

void normalisation() {
    // Domain level.
    CHECK_EQ(catalog::chooseLinkCandidate({ "https://youtu.be/9zCH37330f8" }), 0);
    CHECK_EQ(catalog::chooseLinkCandidate({ "garbage", "https://youtu.be/9zCH37330f8" }), 1);
    CHECK_EQ(catalog::chooseLinkCandidate({ "", "https://example.com/" }), 1); // nothing valid: first non-empty
    CHECK_EQ(catalog::chooseLinkCandidate({ "", " " }), -1);
    CHECK_EQ(catalog::chooseLinkCandidate(std::vector<std::string>()), -1);

    sailfish::LinkRouter router;
    router.setReady(true);
    RouterLog log(&router);
    // A plain string.
    router.openUrl(QVariant(kVideoUrl));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // A one-element array (warm start, FinTube issue #7).
    log.events.clear();
    router.openUrl(QVariant(QStringList() << kVideoUrl));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // An array whose first element is not valid: the first valid one wins.
    log.events.clear();
    router.openUrl(QVariant(QStringList() << QString() << QStringLiteral("not a link") << kVideoUrl
                                          << QStringLiteral("https://www.youtube.com/@NASA")));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // An empty array: reported, the window still comes up.
    log.events.clear();
    router.openUrl(QVariant(QStringList()));
    CHECK_EQ(log.joined(), QStringLiteral("unsupported: raise"));
    // A non-string argument.
    log.events.clear();
    router.openUrl(QVariant(42));
    CHECK_EQ(log.joined(), QStringLiteral("unsupported: raise"));
    // A variant list with a non-string first element.
    log.events.clear();
    router.openUrl(QVariant(QVariantList() << QVariant(7) << QVariant(kVideoUrl)));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // Nothing valid: the first non-empty string is reported.
    log.events.clear();
    router.openUrl(QVariant(QStringList() << QString() << QStringLiteral("https://example.com/x")));
    CHECK_EQ(log.joined(), QStringLiteral("unsupported:https://example.com/x raise"));
    CHECK_EQ(sailfish::LinkRouter::candidates(QVariant(QStringLiteral("a"))).size(), 1);
}

void coldStart() {
    sailfish::LinkRouter router;
    RouterLog log(&router);
    CHECK(!router.ready());
    router.openUrl(QVariant(kVideoUrl));
    router.openUrl(QVariant(QStringList() << QStringLiteral("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG")));
    CHECK(log.events.isEmpty());           // nothing routed or raised before ready
    CHECK_EQ(router.pending(), 2);         // both kept: no single overwritten slot
    router.setReady(true);
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise playlist:PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG raise"));
    CHECK_EQ(router.pending(), 0);
    // Warm from here on.
    log.events.clear();
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/@NASA")));
    CHECK_EQ(log.joined(), QStringLiteral("channel:@NASA raise"));
    // Not ready again (for example while the window is rebuilt): queued again.
    log.events.clear();
    router.setReady(false);
    router.openUrl(QVariant(kVideoUrl));
    CHECK(log.events.isEmpty());
    router.setReady(true);
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));

    // The queue is bounded and keeps the oldest links.
    catalog::LinkQueue queue;
    for (std::size_t i = 0; i < catalog::LinkQueue::kCapacity + 3; ++i) {
        LinkTarget t;
        t.url = std::to_string(i);
        CHECK(!queue.push(t));
    }
    CHECK_EQ(queue.pending(), catalog::LinkQueue::kCapacity);
    CHECK_EQ(queue.dropped(), static_cast<std::size_t>(3));
    const std::vector<LinkTarget> flushed = queue.setReady(true);
    CHECK_EQ(flushed.size(), catalog::LinkQueue::kCapacity);
    CHECK_EQ(flushed.front().url, std::string("0"));
    CHECK_EQ(flushed.back().url, std::to_string(catalog::LinkQueue::kCapacity - 1));
    LinkTarget late;
    CHECK(queue.push(late)); // ready: route now
}

void routing() {
    sailfish::LinkRouter router;
    router.setReady(true);
    RouterLog log(&router);
    // LNK-04: video (on top of the current page), video with list (queue seeded at the video),
    // playlist (preview), channel. Each route signal comes before the raise.
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/watch?v=9zCH37330f8")));
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/watch?v=9zCH37330f8&list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG&index=4")));
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG")));
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/channel/UCLA_DiR1FfKNvjuUpBHmylQ")));
    router.openUrl(QVariant(QStringLiteral("https://www.youtube.com/clip/UgkxABCdefGHIjklMNOpqrSTUvwxYZ012345")));
    const QStringList expected = QStringList()
        << QStringLiteral("video:9zCH37330f8@-1|#-1") << QStringLiteral("raise")
        << QStringLiteral("video:9zCH37330f8@-1|PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG#4") << QStringLiteral("raise")
        << QStringLiteral("playlist:PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG") << QStringLiteral("raise")
        << QStringLiteral("channel:UCLA_DiR1FfKNvjuUpBHmylQ") << QStringLiteral("raise")
        << QStringLiteral("unsupported:https://www.youtube.com/clip/UgkxABCdefGHIjklMNOpqrSTUvwxYZ012345") << QStringLiteral("raise");
    CHECK_EQ(log.joined(), expected.join(QLatin1Char(' ')));
    log.events.clear();
    router.raise();
    CHECK_EQ(log.joined(), QStringLiteral("raise"));
}

void adaptorDirect() {
    sailfish::LinkRouter router;
    router.setReady(true);
    RouterLog log(&router);
    sailfish::DBusLinkService service(&router);
    CHECK(!service.registered());
    service.linkAdaptor()->openUrl(kVideoUrl);                                              // s
    service.linkAdaptor()->openUrl(QStringList() << QStringLiteral("x") << kVideoUrl);      // as
    service.linkAdaptor()->openUrl(QDBusVariant(QVariant(QStringList() << kVideoUrl)));     // v holding as
    service.linkAdaptor()->openUrl(QDBusVariant(QVariant(kVideoUrl)));                      // v holding s
    service.applicationAdaptor()->Open(QStringList() << kVideoUrl, QVariantMap());
    CHECK_EQ(log.events.count(kVideoEvent), 5);
    CHECK_EQ(log.events.count(QStringLiteral("raise")), 5);
    log.events.clear();
    service.linkAdaptor()->activateWindow();
    service.applicationAdaptor()->Activate(QVariantMap());
    CHECK_EQ(log.joined(), QStringLiteral("raise raise"));
    // An unconnected bus is refused without crashing.
    CHECK(!service.registerOn(QDBusConnection(QStringLiteral("sailtube-no-such-connection"))));
    CHECK(!service.registered());
    CHECK(!service.errorText().isEmpty());
}

bool callAndWait(const QDBusConnection &client, const QDBusMessage &message, RouterLog &log, int expectedEvents) {
    QDBusPendingCall call = client.asyncCall(message, 5000);
    const bool done = catalog_test::waitFor([&]() { return call.isFinished() && log.events.size() >= expectedEvents; }, 6000);
    if (call.isError()) std::printf("  D-Bus error: %s\n", qPrintable(call.error().message()));
    return done && !call.isError();
}

// LNK-03 / L1, a real D-Bus activation: the client calls openUrl(as) while the name has no
// owner; the bus queues it and starts the activation entry; the service then registers (as the
// activated app does in main) and the queued call must reach the router.
void activationQueuedCall() {
    if (qgetenv("SAILTUBE_LINKS_ACTIVATION") != "1") {
        std::printf("catalog_links: NOTE no private bus with an activation entry, activation case skipped\n");
        return;
    }
    QDBusConnection bus = QDBusConnection::sessionBus();
    CHECK(bus.isConnected());
    const QString name = sailfish::DBusLinkService::serviceName();
    CHECK(!bus.interface()->isServiceRegistered(name).value());
    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("sailtube-links-activator"));
    CHECK(client.isConnected());
    QDBusMessage call = QDBusMessage::createMethodCall(name, QStringLiteral("/"), QStringLiteral("org.nordilabs.sailtube.SailTube"),
                                                       QStringLiteral("openUrl"));
    call << (QStringList() << kVideoUrl);
    QDBusPendingCall pending = client.asyncCall(call, 10000);
    // The bus holds the call while it "activates" (the entry only sleeps).
    catalog_test::waitFor([] { return false; }, 400);
    CHECK(!pending.isFinished());

    sailfish::LinkRouter router;
    RouterLog log(&router);
    sailfish::DBusLinkService service(&router);
    CHECK(service.registerOn(bus));
    CHECK(catalog_test::waitFor([&] { return pending.isFinished(); }, 5000));
    CHECK(!pending.isError());
    if (pending.isError()) std::printf("activation call error: %s\n", qPrintable(pending.error().message()));
    // Not ready yet (QML not loaded): queued, then replayed.
    CHECK(log.events.isEmpty());
    CHECK_EQ(router.pending(), 1);
    router.setReady(true);
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    service.registerOn(QDBusConnection(QStringLiteral("sailtube-no-such-connection")));
    QDBusConnection::disconnectFromBus(QStringLiteral("sailtube-links-activator"));
}

void adaptorOverBus() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        std::printf("catalog_links: NOTE session bus not connected, real D-Bus calls skipped (direct calls tested)\n");
        return;
    }
    std::printf("catalog_links: real session bus at %s\n", qPrintable(bus.baseService()));
    const QString name = sailfish::DBusLinkService::serviceName();
    const QString iface = QStringLiteral("org.nordilabs.sailtube.SailTube");
    sailfish::LinkRouter router;
    router.setReady(true);
    RouterLog log(&router);
    sailfish::DBusLinkService service(&router);
    CHECK(service.registerOn(bus));
    CHECK(service.registered());

    // A second instance: the name is taken, registration fails cleanly, the first keeps it.
    QDBusConnection second = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("sailtube-links-second"));
    CHECK(second.isConnected());
    {
        sailfish::LinkRouter otherRouter;
        sailfish::DBusLinkService other(&otherRouter);
        CHECK(!other.registerOn(second));
        CHECK(!other.registered());
        CHECK(other.errorText().contains(QStringLiteral("another instance")));
    }
    CHECK_EQ(bus.interface()->serviceOwner(name).value(), bus.baseService());

    QDBusConnection client = QDBusConnection::connectToBus(QDBusConnection::SessionBus, QStringLiteral("sailtube-links-client"));
    CHECK(client.isConnected());
    // s
    QDBusMessage m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << kVideoUrl;
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // s holding an array written as text (what `gdbus call ... "['url']"` sends, device run 5b)
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << (QStringLiteral("['") + kVideoUrl + QStringLiteral("']"));
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << QStringLiteral("[\"\", \"https://www.youtube.com/@NASA\"]");
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), QStringLiteral("channel:@NASA raise"));
    // as, invalid first element
    log.events.clear();
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << (QStringList() << QStringLiteral("") << QStringLiteral("https://www.youtube.com/@NASA"));
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), QStringLiteral("channel:@NASA raise"));
    // v holding as
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << QVariant::fromValue(QDBusVariant(QVariant(QStringList() << kVideoUrl)));
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // v holding av (arrives as a QDBusArgument)
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    m << QVariant::fromValue(QDBusVariant(QVariant(QVariantList() << QVariant(kVideoUrl))));
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise"));
    // org.freedesktop.Application.Open(as, a{sv}) on the DBusActivatable path
    log.events.clear();
    m = QDBusMessage::createMethodCall(name, sailfish::DBusLinkService::applicationObjectPath(),
                                       QStringLiteral("org.freedesktop.Application"), QStringLiteral("Open"));
    m << (QStringList() << QStringLiteral("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG")) << QVariantMap();
    CHECK(callAndWait(client, m, log, 2));
    CHECK_EQ(log.joined(), QStringLiteral("playlist:PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG raise"));
    // Two links at once: both routed, in order.
    log.events.clear();
    QDBusMessage a = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    a << (QStringList() << kVideoUrl);
    QDBusMessage b = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
    b << QStringLiteral("https://www.youtube.com/@NASA");
    QDBusPendingCall ca = client.asyncCall(a);
    QDBusPendingCall cb = client.asyncCall(b);
    CHECK(catalog_test::waitFor([&]() { return ca.isFinished() && cb.isFinished() && log.events.size() >= 4; }));
    CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise channel:@NASA raise"));

    // Cold start (L1): links that arrive over the bus before the shell is ready, in every
    // signature, are kept and replayed into openVideo when it becomes ready.
    {
        router.setReady(false);
        log.events.clear();
        QDBusMessage s1 = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
        s1 << (QStringList() << kVideoUrl);   // as, what the launcher sends
        QDBusMessage s2 = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
        s2 << QStringLiteral("https://www.youtube.com/@NASA");
        QDBusMessage s3 = QDBusMessage::createMethodCall(name, QStringLiteral("/"), iface, QStringLiteral("openUrl"));
        s3 << QVariant::fromValue(QDBusVariant(QVariant(QStringList() << QStringLiteral("https://www.youtube.com/playlist?list=PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG"))));
        QDBusPendingCall c1 = client.asyncCall(s1);
        QDBusPendingCall c2 = client.asyncCall(s2);
        QDBusPendingCall c3 = client.asyncCall(s3);
        CHECK(catalog_test::waitFor([&]() { return c1.isFinished() && c2.isFinished() && c3.isFinished(); }));
        CHECK(!c1.isError() && !c2.isError() && !c3.isError());
        CHECK(log.events.isEmpty());                 // nothing routed or raised before ready
        CHECK_EQ(router.pending(), 3);
        router.setReady(true);
        CHECK_EQ(log.joined(), kVideoEvent + QStringLiteral(" raise channel:@NASA raise playlist:PLx0sYbCqOb8TBPRdmBHs5Iftvv9TPboYG raise"));
        CHECK_EQ(router.pending(), 0);
    }

    // Unregistering releases the name.
    service.registerOn(QDBusConnection(QStringLiteral("sailtube-no-such-connection")));
    CHECK(!bus.interface()->isServiceRegistered(name).value());
    QDBusConnection::disconnectFromBus(QStringLiteral("sailtube-links-second"));
    QDBusConnection::disconnectFromBus(QStringLiteral("sailtube-links-client"));
}

QString repoFile(const char *relative) {
    QFile f(QString::fromLocal8Bit(qgetenv("SAILTUBE_REPO_ROOT")) + QLatin1Char('/') + QLatin1String(relative));
    if (!f.open(QIODevice::ReadOnly)) {
        std::printf("FAIL cannot read %s\n", relative);
        ++catalog_test::failures();
        return QString();
    }
    return QString::fromUtf8(f.readAll());
}

QString keyValue(const QString &text, const QString &key) {
    const QStringList lines = text.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        if (line.startsWith(key + QLatin1Char('='))) return line.mid(key.size() + 1).trimmed();
    }
    return QString();
}

void packaging() {
    const QString desktop = repoFile("harbour-sailtube.desktop");
    const QStringList mimes = keyValue(desktop, QStringLiteral("MimeType")).split(QLatin1Char(';'), QString::SkipEmptyParts);
    const char *hosts[] = { "youtube.com", "www.youtube.com", "m.youtube.com", "youtu.be", "www.youtube-nocookie.com" };
    for (const char *host : hosts) {
        CHECK(mimes.contains(QStringLiteral("x-url-handler/") + QLatin1String(host)));
        CHECK(catalog::isYouTubeHost(host));
    }
    // LNK-01: music.youtube.com is SailTune's; not in the main desktop file.
    CHECK(!desktop.contains(QStringLiteral("music.youtube.com")));
    const QString org = keyValue(desktop, QStringLiteral("OrganizationName"));
    const QString app = keyValue(desktop, QStringLiteral("ApplicationName"));
    const QString sailjailName = org + QLatin1Char('.') + app;
    CHECK_EQ(sailjailName, sailfish::DBusLinkService::serviceName());
    CHECK_EQ(keyValue(desktop, QStringLiteral("X-Maemo-Service")), sailfish::DBusLinkService::serviceName());
    CHECK_EQ(keyValue(desktop, QStringLiteral("X-Maemo-Object-Path")), sailfish::DBusLinkService::objectPath());
    CHECK_EQ(keyValue(desktop, QStringLiteral("X-Maemo-Method")), sailjailName + QStringLiteral(".openUrl"));
    // Harbour's validator wants Exec=<name> exactly.
    CHECK_EQ(keyValue(desktop, QStringLiteral("Exec")), QStringLiteral("harbour-sailtube"));

    const QString service = repoFile("dbus/org.nordilabs.sailtube.SailTube.service");
    CHECK(service.contains(QStringLiteral("[D-BUS Service]")));
    CHECK_EQ(keyValue(service, QStringLiteral("Name")), sailfish::DBusLinkService::serviceName());
    const QString exec = keyValue(service, QStringLiteral("Exec"));
    CHECK(exec.startsWith(QStringLiteral("/usr/bin/invoker ")));
    CHECK(exec.contains(QStringLiteral("--type=silica-qt5")));
    CHECK(exec.contains(QStringLiteral("--single-instance")));
    CHECK(exec.endsWith(QStringLiteral("/usr/bin/harbour-sailtube")));

    CHECK(repoFile("rpm/harbour-sailtube.spec").contains(QStringLiteral("%{_datadir}/dbus-1/services/org.nordilabs.sailtube.SailTube.service")));
    const QString pri = repoFile("Core/Catalog/pri/links.pri");
    CHECK(pri.contains(QStringLiteral("QT += dbus")));
    CHECK(pri.contains(QStringLiteral("/usr/share/dbus-1/services")));
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    linkTable();
    startTimes();
    normalisation();
    coldStart();
    routing();
    adaptorDirect();
    adaptorOverBus();
    activationQueuedCall();
    packaging();
    return catalog_test::finish("catalog_links");
}
