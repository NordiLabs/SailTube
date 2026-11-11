// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// Import and export (IMP-01, IMP-03, IMP-04): ZipReader, the NewPipe/PipePipe
// parser on two layouts, Takeout CSV / OPML, the SailTube backup, and end to
// end through LibraryStore::applyImport and ImportJob. Build: tests/library_import/run.sh
#include "Core/Library/Infrastructure/ImportJob.h"
#include "Core/Library/Infrastructure/LibraryStore.h"
#include "Core/Library/Infrastructure/NewPipeImporter.h"
#include "Core/Library/Infrastructure/SailTubeBackup.h"
#include "Core/Library/Infrastructure/TakeoutImporter.h"
#include "Core/Library/Infrastructure/ZipReader.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QRegExp>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace nordtile::video;
using namespace nordtile::video::library;

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

static const qint64 NOW = 1700000000000LL;
static QString fixtures;
static QString scratch;

static const std::string A = "UCaaaaaaaaaaaaaaaaaaaa01";
static const std::string B = "UCbbbbbbbbbbbbbbbbbbbb02";
static const std::string C = "UCcccccccccccccccccccc03";
static const std::string D = "UCdddddddddddddddddddd04";

static std::string vid(int n) {
    char b[16];
    std::snprintf(b, sizeof b, "vid%08d", n);
    return b;
}
static QString qvid(int n) { return QString::fromStdString(vid(n)); }
static QString fx(const char *name) { return QDir(fixtures).filePath(QLatin1String(name)); }
static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }
static QByteArray slurp(const QString &path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
static QStringList entriesOf(const QString &dir) {
    return QDir(dir).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
}
static QString freshDir(const char *name) {
    const QString d = QDir(scratch).filePath(QLatin1String(name));
    QDir(d).removeRecursively();
    QDir().mkpath(d);
    return d;
}
static bool privateMode(const QString &path) {
    return QFileInfo(path).permissions() == (QFileDevice::ReadOwner | QFileDevice::WriteOwner
                                             | QFileDevice::ReadUser | QFileDevice::WriteUser);
}
static bool hasSection(const ImportBatch &b, const char *s) {
    for (const std::string &x : b.skippedSections) if (x == s) return true;
    return false;
}
static std::vector<std::string> ids(const std::vector<VideoEntry> &items) {
    std::vector<std::string> out;
    for (const VideoEntry &v : items) out.push_back(v.videoId);
    return out;
}

// ---------------------------------------------------------------- ZipReader
static void zipReaderTests() {
    const QString out = freshDir("zip");
    ZipReader zip(fx("zipreader.zip"));
    CHECK(zip.open());
    CHECK(zip.entries().size() == 6);
    CHECK(zip.indexOf(QStringLiteral("dir/")) >= 0 && zip.entries().at(zip.indexOf(QStringLiteral("dir/"))).isDirectory());

    const QString stored = out + QStringLiteral("/stored.out");
    CHECK(zip.extract(zip.indexOf(QStringLiteral("stored.txt")), stored));
    CHECK(slurp(stored) == QByteArray("stored entry body\n"));
    CHECK(privateMode(stored));
    CHECK(!QFile::exists(stored + QStringLiteral(".part-import")));

    const int di = zip.indexOf(QStringLiteral("dir/deflated.txt"));
    CHECK(di >= 0 && zip.entries().at(di).method == 8 && zip.entries().at(di).compressedSize < zip.entries().at(di).uncompressedSize);
    QByteArray expected;
    for (int i = 0; i < 2000; ++i) {
        char line[32];
        std::snprintf(line, sizeof line, "deflated line %05d\n", i);
        expected += line;
    }
    const QString deflated = out + QStringLiteral("/deflated.out");
    CHECK(zip.extract(di, deflated));
    CHECK(slurp(deflated) == expected);
    QByteArray mem;
    CHECK(zip.read(di, &mem, 1 << 20) && mem == expected);
    CHECK(!zip.read(di, &mem, 100) && zip.error() == ZipReader::Error::TooLarge);

    // A traversal name goes exactly where the caller says, never to "../evil.txt".
    QDir().mkpath(out + QStringLiteral("/sub"));
    const QString evilTarget = out + QStringLiteral("/sub/evil.out");
    CHECK(zip.extract(zip.indexOf(QStringLiteral("../evil.txt")), evilTarget));
    CHECK(slurp(evilTarget) == QByteArray("must never land outside the target\n"));
    CHECK(!QFile::exists(out + QStringLiteral("/evil.txt")));
    CHECK(!QFile::exists(out + QStringLiteral("/sub/../evil.txt")));
    CHECK(!QFile::exists(QFileInfo(fixtures).absolutePath() + QStringLiteral("/evil.txt")));
    CHECK(!QFile::exists(QDir::current().absoluteFilePath(QStringLiteral("../evil.txt"))));

    const QString crc = out + QStringLiteral("/crc.out");
    CHECK(!zip.extract(zip.indexOf(QStringLiteral("crc.bin")), crc));
    CHECK(zip.error() == ZipReader::Error::CrcMismatch);
    CHECK(!QFile::exists(crc) && !QFile::exists(crc + QStringLiteral(".part-import")));

    QByteArray empty("x");
    CHECK(zip.read(zip.indexOf(QStringLiteral("empty.txt")), &empty, 10) && empty.isEmpty());
    CHECK(!zip.extract(99, crc) && zip.error() == ZipReader::Error::NoSuchEntry);

    ZipReader z64(fx("zip64.zip"));
    CHECK(!z64.open() && z64.error() == ZipReader::Error::Zip64Unsupported);
    ZipReader cut(fx("truncated.zip"));
    CHECK(!cut.open() && cut.error() == ZipReader::Error::NotZip);
    ZipReader none(fx("does-not-exist.zip"));
    CHECK(!none.open() && none.error() == ZipReader::Error::OpenFailed);
    // Only the requested targets exist; the failed CRC extraction left nothing behind.
    CHECK(entriesOf(out) == (QStringList() << QStringLiteral("deflated.out") << QStringLiteral("stored.out")
                                           << QStringLiteral("sub")));
    CHECK(entriesOf(out + QStringLiteral("/sub")) == QStringList(QStringLiteral("evil.out")));
}

// ---------------------------------------------------------------- NewPipe parse
static void checkOlder(const ImportBatch &b) {
    CHECK(b.source == "newpipe");
    CHECK(b.subscriptions.size() == 2);
    if (b.subscriptions.size() == 2) {
        CHECK(b.subscriptions[0].channelId == A && b.subscriptions[0].name == "Alpha");
        CHECK(b.subscriptions[0].avatarUrl.empty() && b.subscriptions[0].addedMs == NOW);
        CHECK(b.subscriptions[1].channelId == B && b.subscriptions[1].name == "Beta");
    }
    CHECK(b.pendingHandles.size() == 2);
    if (b.pendingHandles.size() == 2) {
        CHECK(b.pendingHandles[0].url == "https://www.youtube.com/@handlename" && b.pendingHandles[0].name == "Handle One");
        CHECK(b.pendingHandles[1].url == "https://www.youtube.com/user/olduser");
    }
    CHECK(b.skippedRows == 4);             // soundcloud + junk subscription, soundcloud + non-video stream
    CHECK(b.skippedSections.size() == 1 && hasSection(b, "remote_playlists"));
    CHECK(b.history.size() == 4);
    if (b.history.size() == 4) {
        const HistoryRecord &six = b.history[0], &two = b.history[1], &three = b.history[2], &one = b.history[3];
        CHECK(six.video.videoId == vid(6) && six.watchedAtMs == 7000000 && six.positionMs == 5000);
        CHECK(six.durationMs == 100000 && near(six.fraction, 0.05) && !six.watched && six.playCount == 1);
        CHECK(six.video.title == "Six" && six.video.channel == "Alpha" && six.video.channelId.empty());
        // In history, no stream_state row: fully watched.
        CHECK(two.video.videoId == vid(2) && two.watched && near(two.fraction, 1) && two.positionMs == 300000);
        CHECK(two.video.channelId == B && two.watchedAtMs == 5000000);
        // 44 s of 45 s, progress in ms: watched.
        CHECK(three.video.videoId == vid(3) && three.watched && three.positionMs == 44000 && three.playCount == 3);
        CHECK(near(three.fraction, 44.0 / 45.0) && three.video.durationSeconds == 45);
        // Latest access kept, repeats summed over three rows.
        CHECK(one.video.videoId == vid(1) && one.watchedAtMs == 3000000 && one.playCount == 7);
        CHECK(one.positionMs == 120000 && one.durationMs == 600000 && near(one.fraction, 0.2) && !one.watched);
        CHECK(one.video.thumbnailUrl == "https://i.ytimg.com/vi/vid00000001/hqdefault.jpg");
        CHECK(one.video.channelId == A && one.video.durationSeconds == 600);
    }
    CHECK(b.resume.size() == 2);           // under 10 s and within 15 s of the end are dropped
    if (b.resume.size() == 2) {
        CHECK(b.resume[0].videoId == vid(1) && b.resume[0].positionMs == 120000 && b.resume[0].durationMs == 600000);
        CHECK(b.resume[0].touchedMs == NOW);
        CHECK(b.resume[1].videoId == vid(5) && b.resume[1].positionMs == 900000); // state without history
    }
    CHECK(b.playlists.size() == 2);
    if (b.playlists.size() == 2) {
        CHECK(b.playlists[0].title == "Favourites");
        CHECK(ids(b.playlists[0].items) == (std::vector<std::string>{vid(1), vid(2), vid(5), vid(3)}));
        CHECK(b.playlists[1].title == "MUSIC");
        CHECK(ids(b.playlists[1].items) == (std::vector<std::string>{vid(6), vid(1)}));
        CHECK(!b.playlists[0].items.empty() && b.playlists[0].items[0].title == "One");
    }
    CHECK(b.remotePlaylists.empty());
}

static void checkNewer(const ImportBatch &b) {
    CHECK(b.source == "newpipe");
    CHECK(b.subscriptions.size() == 3);
    if (b.subscriptions.size() == 3) {
        CHECK(b.subscriptions[0].channelId == C && b.subscriptions[0].avatarUrl == "https://yt3.example/gamma.jpg");
        CHECK(b.subscriptions[1].channelId == A && b.subscriptions[1].name == "Alpha (new name)");
        CHECK(b.subscriptions[2].channelId == D && b.subscriptions[2].avatarUrl.empty()); // m.youtube.com, NULL avatar
    }
    CHECK(b.pendingHandles.size() == 1 && b.pendingHandles[0].url == "https://www.youtube.com/c/SomeCustom");
    CHECK(b.skippedRows == 5);             // soundcloud sub, soundcloud stream, orphan history, 2 remote
    CHECK(b.skippedSections.empty());
    CHECK(b.history.size() == 4);
    if (b.history.size() == 4) {
        const HistoryRecord &seven = b.history[0], &eight = b.history[1], &ten = b.history[2], &nine = b.history[3];
        // Two stream rows of one video: latest access, repeats summed across both.
        CHECK(seven.video.videoId == vid(7) && seven.watchedAtMs == 9500000 && seven.playCount == 4);
        CHECK(seven.positionMs == 3000000 && seven.durationMs == 3600000 && seven.watched);
        CHECK(near(seven.fraction, 3000.0 / 3600.0) && seven.video.title == "Seven" && seven.video.channelId == C);
        CHECK(eight.video.videoId == vid(8) && eight.watched && eight.positionMs == 239000); // youtu.be with ?si=
        CHECK(ten.video.videoId == vid(10) && !ten.watched && ten.positionMs == 9999 && near(ten.fraction, 0.009999));
        CHECK(nine.video.videoId == vid(9) && nine.watched && near(nine.fraction, 1) && nine.playCount == 2);
    }
    CHECK(b.resume.size() == 2);
    if (b.resume.size() == 2) {
        CHECK(b.resume[0].videoId == vid(7) && b.resume[0].positionMs == 3000000);
        CHECK(b.resume[1].videoId == vid(11) && b.resume[1].positionMs == 250000 && b.resume[1].durationMs == 500000);
    }
    CHECK(b.playlists.size() == 2);        // display_index order
    if (b.playlists.size() == 2) {
        CHECK(b.playlists[0].title == "favourites");
        CHECK(ids(b.playlists[0].items) == (std::vector<std::string>{vid(9)}));
        CHECK(b.playlists[1].title == "Road trip");
        CHECK(ids(b.playlists[1].items) == (std::vector<std::string>{vid(8), vid(11), vid(7)}));
    }
    CHECK(b.remotePlaylists.size() == 2);
    if (b.remotePlaylists.size() == 2) {
        const RemotePlaylistRef &l = b.remotePlaylists[0], &m = b.remotePlaylists[1];
        CHECK(l.remoteId == "PLabcdefghijklmnopqrstuvwxyz012345" && l.title == "Lectures" && l.channel == "Prof");
        CHECK(l.thumbnailUrl == "https://i.ytimg.com/lectures.jpg" && l.itemCountHint == 42);
        CHECK(m.remoteId == "PLzyxwvutsrqponmlkjihgfedcba987654" && m.title == "Mix" && m.itemCountHint == 10);
    }
}

static void newPipeParseTests() {
    const QString priv = freshDir("private");

    NewPipeImportResult r = importNewPipeBackup(fx("older.zip"), priv, NOW);
    CHECK(r.ok());
    checkOlder(r.batch);
    CHECK(entriesOf(priv).isEmpty());      // extracted copy removed

    r = importNewPipeBackup(fx("other-name.zip"), priv, NOW); // first ".db" entry, not later.db
    CHECK(r.ok());
    checkNewer(r.batch);
    CHECK(entriesOf(priv).isEmpty());

    r = importNewPipeBackup(QStringLiteral("file://") + fx("newer.db"), priv, NOW); // raw .db as file:// URL
    CHECK(r.ok());
    checkNewer(r.batch);
    CHECK(entriesOf(priv).isEmpty());

    const QByteArray home = qgetenv("HOME");
    qputenv("HOME", fixtures.toUtf8());
    r = importNewPipeBackup(QStringLiteral("~/older.db"), priv, NOW);
    qputenv("HOME", home);
    CHECK(r.ok());
    checkOlder(r.batch);

    r = parseNewPipeDatabase(fx("partial.db"), NOW);
    CHECK(r.ok());
    CHECK(r.batch.subscriptions.size() == 1);
    CHECK(r.batch.history.size() == 1 && r.batch.history[0].playCount == 2 && !r.batch.history[0].watched);
    CHECK(r.batch.resume.empty() && r.batch.playlists.empty());
    CHECK(r.batch.skippedSections == (std::vector<std::string>{"stream_state", "playlists", "remote_playlists"}));

    r = importNewPipeBackup(fx("corrupt.db"), priv, NOW);
    CHECK(r.error == NewPipeImportError::DatabaseOpenFailed && !r.errorString.isEmpty());
    CHECK(entriesOf(priv).isEmpty());      // removed on failure too
    r = importNewPipeBackup(fx("garbage.bin"), priv, NOW);
    CHECK(r.error == NewPipeImportError::UnrecognizedFormat);
    r = importNewPipeBackup(fx("truncated.zip"), priv, NOW);
    CHECK(r.error == NewPipeImportError::ZipError);
    r = importNewPipeBackup(fx("no-db.zip"), priv, NOW);
    CHECK(r.error == NewPipeImportError::NoDatabaseInZip);
    r = importNewPipeBackup(fx("missing.zip"), priv, NOW);
    CHECK(r.error == NewPipeImportError::FileNotFound);
    r = importNewPipeBackup(fx("unrelated.db"), priv, NOW);
    CHECK(r.error == NewPipeImportError::NotNewPipeDatabase);
    r = importNewPipeBackup(fx("older.zip"), QString(), NOW);
    CHECK(r.error == NewPipeImportError::NoPrivateDirectory);
    CHECK(entriesOf(priv).isEmpty());
    // The input is never modified.
    CHECK(QFileInfo(fx("newer.db")).size() > 0 && !QFile::exists(fx("newer.db-journal")));
}

// ---------------------------------------------------------------- Takeout / OPML
static const char *const takeoutCsv =
    "\xEF\xBB\xBF" "Channel Id,Channel Url,Channel Title\r\n"
    "UCaaaaaaaaaaaaaaaaaaaa01,http://www.youtube.com/channel/UCaaaaaaaaaaaaaaaaaaaa01,\"Alpha, the first\"\r\n"
    "UCbbbbbbbbbbbbbbbbbbbb02,http://www.youtube.com/channel/UCbbbbbbbbbbbbbbbbbbbb02,\"Beta \"\"quoted\"\"\"\r\n"
    "UCcccccccccccccccccccc03,http://www.youtube.com/channel/UCcccccccccccccccccccc03,Gamma\r\n"
    "UCaaaaaaaaaaaaaaaaaaaa01,http://www.youtube.com/channel/UCaaaaaaaaaaaaaaaaaaaa01,\"Alpha, the first\"\r\n"
    "not-an-id,http://www.youtube.com/feed/trending,Broken\r\n"
    "\r\n";

// Localized headers, columns in another order, LF only, a title with a line break.
static const char *const localizedCsv =
    "Titre de la cha\xC3\xAEne,URL de la cha\xC3\xAEne,ID de la cha\xC3\xAEne\n"
    "\"Alpha, the first\",http://www.youtube.com/channel/UCaaaaaaaaaaaaaaaaaaaa01,UCaaaaaaaaaaaaaaaaaaaa01\n"
    "\"Beta\nsecond line\",http://www.youtube.com/channel/UCbbbbbbbbbbbbbbbbbbbb02,UCbbbbbbbbbbbbbbbbbbbb02\n"
    "Gamma,http://www.youtube.com/channel/UCcccccccccccccccccccc03,UCcccccccccccccccccccc03";

static const char *const opml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<opml version=\"1.1\"><head><title>Subscriptions</title></head><body>\n"
    " <outline text=\"YouTube Subscriptions\" title=\"YouTube Subscriptions\">\n"
    "  <outline text=\"Alpha, the first\" title=\"Alpha, the first\" type=\"rss\"\n"
    "   xmlUrl=\"https://www.youtube.com/feeds/videos.xml?channel_id=UCaaaaaaaaaaaaaaaaaaaa01\"/>\n"
    "  <outline text=\"Music\">\n"
    "   <outline text=\"Beta\" type=\"rss\" htmlUrl=\"https://www.youtube.com/channel/UCbbbbbbbbbbbbbbbbbbbb02\"/>\n"
    "   <outline text=\"Gamma\" type=\"rss\" xmlUrl=\"https://www.youtube.com/feeds/videos.xml?channel_id=UCcccccccccccccccccccc03\"\n"
    "    htmlUrl=\"https://www.youtube.com/channel/UCcccccccccccccccccccc03\"/>\n"
    "  </outline>\n"
    "  <outline text=\"Handle\" type=\"rss\" htmlUrl=\"https://www.youtube.com/@someone\"/>\n"
    "  <outline text=\"A playlist\" type=\"rss\" xmlUrl=\"https://www.youtube.com/feeds/videos.xml?playlist_id=PLabc\"/>\n"
    " </outline>\n"
    "</body></opml>\n";

static std::vector<std::string> channelIds(const ImportBatch &b) {
    std::vector<std::string> out;
    for (const ChannelRecord &c : b.subscriptions) out.push_back(c.channelId);
    return out;
}

static void takeoutTests() {
    const std::vector<std::string> abc{A, B, C};
    TextImportResult r = parseTakeoutCsv(QByteArray(takeoutCsv), NOW);
    CHECK(r.ok() && r.batch.source == "takeout");
    CHECK(channelIds(r.batch) == abc);
    if (r.batch.subscriptions.size() == 3) {
        CHECK(r.batch.subscriptions[0].name == "Alpha, the first");
        CHECK(r.batch.subscriptions[1].name == "Beta \"quoted\"");
        CHECK(r.batch.subscriptions[2].name == "Gamma" && r.batch.subscriptions[2].addedMs == NOW);
    }
    CHECK(r.batch.skippedRows == 1);

    r = parseTakeoutCsv(QByteArray(localizedCsv), NOW);
    CHECK(r.ok() && channelIds(r.batch) == abc && r.batch.skippedRows == 0);
    if (r.batch.subscriptions.size() == 3) {
        CHECK(r.batch.subscriptions[0].name == "Alpha, the first");
        CHECK(r.batch.subscriptions[1].name == "Beta\nsecond line");
    }

    r = parseTakeoutCsv(QByteArray("Channel Id,Channel Url,Channel Title\r\n"), NOW);
    CHECK(r.ok() && r.batch.subscriptions.empty());
    r = parseTakeoutCsv(QByteArray("a,b,c\n1,2,3\n"), NOW);
    CHECK(r.error == TextImportError::UnrecognizedFormat);

    r = importTakeoutFile(fx("takeout.zip"), NOW);
    CHECK(r.ok() && channelIds(r.batch) == abc);
    r = importTakeoutFile(fx("no-db.zip"), NOW);
    CHECK(r.error == TextImportError::NoSubscriptionsCsv);
    r = importTakeoutFile(fx("missing.csv"), NOW);
    CHECK(r.error == TextImportError::FileNotFound);

    r = parseOpml(QByteArray(opml), NOW);
    CHECK(r.ok() && r.batch.source == "opml");
    CHECK(channelIds(r.batch) == abc);     // the same set as the CSV
    if (r.batch.subscriptions.size() == 3) CHECK(r.batch.subscriptions[0].name == "Alpha, the first");
    CHECK(r.batch.pendingHandles.size() == 1 && r.batch.pendingHandles[0].url == "https://www.youtube.com/@someone");
    CHECK(r.batch.skippedRows == 1);
    r = parseOpml(QByteArray("<?xml version=\"1.0\"?><!DOCTYPE opml [<!ENTITY a \"aaaa\">]><opml><body>"
                             "<outline text=\"&a;\"/></body></opml>"), NOW);
    CHECK(r.error == TextImportError::MalformedXml);
    r = parseOpml(QByteArray("<opml><body><outline text=\"x\"></body>"), NOW);
    CHECK(r.error == TextImportError::MalformedXml);
    r = parseOpml(QByteArray("<rss><channel/></rss>"), NOW);
    CHECK(r.error == TextImportError::UnrecognizedFormat);
}

// ---------------------------------------------------------------- SailTube backup (file level)
static const char *const secretNames[] = {
    "__Secure-3PAPISID", "SAPISID", "__Secure-1PAPISID", "__Secure-3PSID", "__Secure-1PSID", "HSID", "SSID",
    "APISID", "SIDCC", "LOGIN_INFO", "VISITOR_INFO1_LIVE", "__Secure-3PSIDTS", "SAPISIDHASH", "secret-token-value",
    "hunter2", "ya29."};

// IMP-04: no ACC-06 cookie name, no credential-like key and no token in an exported file.
static void scanForSecrets(const QByteArray &json) {
    for (const char *name : secretNames) CHECK(!json.contains(name));
    const QString text = QString::fromUtf8(json);
    for (const char *word : {"cookie", "token", "secret", "password", "auth"})
        CHECK(!text.contains(QLatin1String(word), Qt::CaseInsensitive));
    CHECK(QRegExp(QStringLiteral("\\bSID\\b")).indexIn(text) < 0);
}

static QString writeIni(const QString &dir) {
    const QString path = dir + QStringLiteral("/sailtube.conf");
    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(QStringLiteral("playback/resumeBehaviour"), 1);
    ini.setValue(QStringLiteral("playback/codec"), QStringLiteral("vp9"));
    ini.setValue(QStringLiteral("library/historyCap"), 800);
    ini.setValue(QStringLiteral("content/region"), QStringLiteral("DE"));
    ini.setValue(QStringLiteral("downloads/directory"), QStringLiteral("/home/defaultuser/Videos"));
    ini.setValue(QStringLiteral("ui/languages"), QStringList() << QStringLiteral("en") << QStringLiteral("de"));
    ini.setValue(QStringLiteral("ui/showDislikes"), true);
    // Never exported:
    ini.setValue(QStringLiteral("account/cookieJar"), QStringLiteral("SAPISID=abc; SID=def"));
    ini.setValue(QStringLiteral("playback/authUser"), 2);
    ini.setValue(QStringLiteral("content/apiToken"), QStringLiteral("secret-token-value"));
    ini.setValue(QStringLiteral("ui/Password"), QStringLiteral("hunter2"));
    ini.setValue(QStringLiteral("downloads/oauthRefresh"), QStringLiteral("ya29.x"));
    ini.setValue(QStringLiteral("library/lastHeaders"), QStringLiteral("Cookie: __Secure-3PAPISID=x; LOGIN_INFO=y"));
    ini.setValue(QStringLiteral("network/proxy"), QStringLiteral("socks5://127.0.0.1"));
    ini.sync();
    return path;
}

static ImportBatch sampleLibrary() {
    ImportBatch b;
    ChannelRecord c; c.channelId = A; c.name = "Alpha"; c.avatarUrl = "https://yt3.example/a.jpg";
    c.latestUploadMs = NOW - 5000; c.addedMs = NOW - 90000;
    b.subscriptions.push_back(c);
    PendingHandle h; h.url = "https://www.youtube.com/@pending"; h.name = "Pending"; h.addedMs = NOW - 1;
    b.pendingHandles.push_back(h);
    HistoryRecord r; r.video.videoId = vid(1); r.video.title = "One \xE2\x80\x94 \"quoted\""; r.video.channel = "Alpha";
    r.video.channelId = A; r.video.durationSeconds = 600; r.video.thumbnailUrl = "https://i.ytimg.com/1.jpg";
    r.positionMs = 120000; r.durationMs = 600000; r.fraction = 0.2; r.watchedAtMs = NOW - 100; r.playCount = 3;
    b.history.push_back(r);
    ResumeRecord s; s.videoId = vid(1); s.positionMs = 120000; s.durationMs = 600000; s.touchedMs = NOW - 100;
    b.resume.push_back(s);
    ImportedPlaylist p; p.title = "Road trip"; p.items.push_back(r.video);
    VideoEntry two; two.videoId = vid(2); two.title = "Two"; p.items.push_back(two);
    b.playlists.push_back(p);
    RemotePlaylistRef m; m.remoteId = "PLabcdefghijklmnopqrstuvwxyz012345"; m.title = "Lectures"; m.channel = "Prof";
    m.itemCountHint = 42;
    b.remotePlaylists.push_back(m);
    return b;
}

static void backupFileTests() {
    CHECK(backupSettingAllowed(QStringLiteral("playback/codec")));
    CHECK(backupSettingAllowed(QStringLiteral("ui/theme")));
    CHECK(!backupSettingAllowed(QStringLiteral("account/name")));
    CHECK(!backupSettingAllowed(QStringLiteral("playback/AUTHUser")));
    CHECK(!backupSettingAllowed(QStringLiteral("library/SapisidCache")));
    CHECK(!backupSettingAllowed(QStringLiteral("playbackx/codec")));
    CHECK(!backupValueAllowed(QStringLiteral("a=b; __Secure-1PSID=c")));
    CHECK(backupValueAllowed(QStringLiteral("vp9")));

    const QString dir = freshDir("backup-file");
    const QString ini = writeIni(dir);
    const QVariantMap settings = backupSettingsFromIni(ini);
    CHECK(settings.size() == 7);
    CHECK(!settings.contains(QStringLiteral("library/lastHeaders")) && !settings.contains(QStringLiteral("network/proxy")));

    const QString target = dir + QStringLiteral("/sailtube-backup.json");
    const ImportBatch lib = sampleLibrary();
    QVariantMap meta;
    meta.insert(QStringLiteral("history_cap"), QStringLiteral("800"));
    meta.insert(QStringLiteral("hide_watched"), QStringLiteral("1"));
    meta.insert(QStringLiteral("auth_cache"), QStringLiteral("dropped: not a library setting"));
    QString error;
    CHECK(writeSailTubeBackup(target, lib, settings, meta, NOW, &error));
    CHECK(error.isEmpty());
    CHECK(privateMode(target));
    CHECK(entriesOf(dir) == (QStringList() << QStringLiteral("sailtube-backup.json") << QStringLiteral("sailtube.conf")));
    const QByteArray json = slurp(target);
    scanForSecrets(json);
    // Unfiltered settings handed to the writer are filtered again.
    QVariantMap raw = settings;
    raw.insert(QStringLiteral("account/cookieJar"), QStringLiteral("SAPISID=abc"));
    CHECK(writeSailTubeBackup(target, lib, raw, meta, NOW, &error));
    CHECK(!slurp(target).contains("SAPISID") && privateMode(target));
    CHECK(entriesOf(dir).size() == 2);
    // Missing directory: an error, nothing written.
    CHECK(!writeSailTubeBackup(dir + QStringLiteral("/nope/x.json"), lib, settings, meta, NOW, &error) && !error.isEmpty());

    BackupRestoreResult r = readSailTubeBackup(QStringLiteral("file://") + target);
    CHECK(r.ok() && r.exportedMs == NOW && r.batch.source == "sailtube");
    CHECK(r.settings == settings);
    CHECK(r.librarySettings.size() == 2 && r.librarySettings.value(QStringLiteral("history_cap")).toString() == QLatin1String("800"));
    CHECK(r.settings.value(QStringLiteral("ui/languages")).toStringList()
          == (QStringList() << QStringLiteral("en") << QStringLiteral("de")));
    const ImportBatch &b = r.batch;
    CHECK(b.subscriptions.size() == 1 && b.subscriptions[0].channelId == A && b.subscriptions[0].latestUploadMs == NOW - 5000);
    CHECK(b.subscriptions.size() == 1 && b.subscriptions[0].addedMs == NOW - 90000 && b.subscriptions[0].avatarUrl == lib.subscriptions[0].avatarUrl);
    CHECK(b.pendingHandles.size() == 1 && b.pendingHandles[0].url == "https://www.youtube.com/@pending");
    CHECK(b.history.size() == 1);
    if (b.history.size() == 1) {
        const HistoryRecord &h = b.history[0];
        CHECK(h.video.title == lib.history[0].video.title && h.video.durationSeconds == 600 && h.video.channelId == A);
        CHECK(h.positionMs == 120000 && h.durationMs == 600000 && near(h.fraction, 0.2) && !h.watched);
        CHECK(h.watchedAtMs == NOW - 100 && h.playCount == 3 && h.video.thumbnailUrl == "https://i.ytimg.com/1.jpg");
    }
    CHECK(b.resume.size() == 1 && b.resume[0].positionMs == 120000 && b.resume[0].touchedMs == NOW - 100);
    CHECK(b.playlists.size() == 1 && b.playlists[0].title == "Road trip");
    CHECK(b.playlists.size() == 1 && ids(b.playlists[0].items) == (std::vector<std::string>{vid(1), vid(2)}));
    CHECK(b.remotePlaylists.size() == 1 && b.remotePlaylists[0].remoteId == lib.remotePlaylists[0].remoteId);
    CHECK(b.remotePlaylists.size() == 1 && b.remotePlaylists[0].itemCountHint == 42);
    CHECK(b.skippedRows == 0);

    CHECK(!parseSailTubeBackup("{\"format\":\"other\",\"version\":1}").ok());
    CHECK(!parseSailTubeBackup("{\"format\":\"sailtube-backup\",\"version\":2}").ok());
    CHECK(!parseSailTubeBackup("{\"format\":\"sailtube-backup\"}").ok());
    CHECK(!parseSailTubeBackup("not json").ok());
    r = parseSailTubeBackup("{\"format\":\"sailtube-backup\",\"version\":1,\"subscriptions\":[{\"channelId\":\"x\"}],"
                            "\"settings\":{\"account/token\":\"t\",\"ui/theme\":\"dark\",\"ui/authKey\":\"k\"}}");
    CHECK(r.ok() && r.batch.subscriptions.empty() && r.batch.skippedRows == 1);
    CHECK(r.settings.size() == 1 && r.settings.value(QStringLiteral("ui/theme")).toString() == QLatin1String("dark"));
}

// ---------------------------------------------------------------- end to end
static qint64 g_now = NOW;
static qint64 testClock() { return g_now; }

struct JobResult {
    QVariantMap summary;
    QString error;
    bool done = false;
};

static JobResult runJob(ImportJob &job) {
    JobResult r;
    QEventLoop loop;
    const QMetaObject::Connection c = QObject::connect(&job, &ImportJob::finished,
                                                       [&](const QVariantMap &m, const QString &e) {
        r.summary = m;
        r.error = e;
        r.done = true;
        loop.quit();
    });
    QTimer::singleShot(30000, &loop, SLOT(quit()));
    if (job.isRunning()) loop.exec();
    QObject::disconnect(c);
    return r;
}

static int num(const JobResult &r, const char *key) { return r.summary.value(QLatin1String(key)).toInt(); }

static VideoEntry entry(int n, const char *title) {
    VideoEntry v;
    v.videoId = vid(n);
    v.title = title;
    v.durationSeconds = 600;
    return v;
}

static QString primaryDb;   // populated by endToEndTests, exported by backupRoundTripTests

static void endToEndTests() {
    const QString dir = freshDir("e2e");
    const QString priv = freshDir("e2e-private");
    primaryDb = dir + QStringLiteral("/library.db");
    LibraryStore store(primaryDb, QStringLiteral("primary"));
    CHECK(store.open());

    // Local data that must win: a subscription, a list named like an imported one,
    // a history record with a resume point.
    ChannelRecord local;
    local.channelId = A;
    local.name = "Local Alpha";
    CHECK(store.subscribe(local));
    CHECK(!store.createPlaylist(QStringLiteral("Music")).isEmpty());
    PlaybackProgress p;
    p.video = entry(1, "Local One");
    p.positionMs = 300000;
    p.durationMs = 600000;
    store.recordProgress(p, true);
    CHECK(store.resumePositionMs(qvid(1)) == 300000);

    const NewPipeImportResult older = importNewPipeBackup(fx("older.zip"), priv, testClock());
    CHECK(older.ok());
    ImportSummary s = store.applyImport(older.batch);
    CHECK(s.subscriptionsAdded == 1 && s.handlesPending == 2);
    CHECK(s.historyAdded == 3 && s.historyUpdated == 1);   // vid1 is local: only its play count grows
    CHECK(s.resumeAdded == 1);                             // vid5; the local vid1 point is kept
    CHECK(s.playlistsAdded == 1 && s.playlistsSkipped == 1 && s.playlistItemsAdded == 4); // MUSIC ~ Music
    CHECK(s.remotePlaylistsAdded == 0 && s.skippedRows == 4);
    CHECK(s.skippedSections == (std::vector<std::string>{"remote_playlists"}));
    CHECK(!s.nothingNew());
    std::vector<ChannelRecord> subs = store.subscriptions(SubscriptionSort::Name);
    for (const ChannelRecord &c : subs) if (c.channelId == A) CHECK(c.name == "Local Alpha");
    HistoryRecord one;
    CHECK(store.historyRecord(qvid(1), &one) && one.positionMs == 300000 && one.video.title == "Local One");
    CHECK(store.resumePositionMs(qvid(1)) == 300000 && store.resumePositionMs(qvid(5)) == 900000);
    CHECK(store.resumePositionMs(qvid(3)) < 0 && store.resumePositionMs(qvid(6)) < 0);
    CHECK(store.watchState(qvid(2)).watched && store.watchState(qvid(3)).watched && !store.watchState(qvid(6)).watched);
    const std::vector<HistoryRecord> newest = store.history(0, 1);
    CHECK(newest.size() == 1 && newest[0].video.videoId == vid(1));  // local history stays newest
    CHECK(store.pendingHandles().size() == 2);
    CHECK(entriesOf(priv).isEmpty());

    s = store.applyImport(importNewPipeBackup(fx("older.zip"), priv, testClock()).batch);
    CHECK(s.nothingNew() && s.playlistsSkipped == 2);       // idempotent

    // ImportJob: the worker writes through its own store; the primary reloads.
    ImportJob job(primaryDb, priv);
    CHECK(job.start(ImportJob::Kind::NewPipe, fx("other-name.zip")));
    CHECK(job.isRunning() && !job.start(ImportJob::Kind::Opml, fx("x.opml")));
    JobResult r = runJob(job);
    CHECK(r.done && r.error.isEmpty() && !job.isRunning());
    CHECK(r.summary.value(QStringLiteral("source")).toString() == QLatin1String("newpipe"));
    CHECK(num(r, "subscriptionsAdded") == 2 && num(r, "handlesPending") == 1);
    CHECK(num(r, "historyAdded") == 4 && num(r, "historyUpdated") == 0 && num(r, "resumeAdded") == 2);
    CHECK(num(r, "playlistsAdded") == 1 && num(r, "playlistsSkipped") == 1 && num(r, "playlistItemsAdded") == 3);
    CHECK(num(r, "remotePlaylistsAdded") == 2 && num(r, "skippedRows") == 5);
    CHECK(!r.summary.value(QStringLiteral("nothingNew")).toBool());
    CHECK(!store.isSubscribed(QString::fromStdString(C)));  // until reload
    store.reload();
    CHECK(store.isSubscribed(QString::fromStdString(C)) && store.isSubscribed(QString::fromStdString(D)));
    CHECK(store.watchState(qvid(9)).watched && store.resumePositionMs(qvid(11)) == 250000);
    CHECK(!store.mirrorIdForRemote(QStringLiteral("PLabcdefghijklmnopqrstuvwxyz012345")).isEmpty());
    CHECK(entriesOf(priv).isEmpty());
    CHECK(job.start(ImportJob::Kind::NewPipe, fx("newer.db")));
    r = runJob(job);
    CHECK(r.done && r.error.isEmpty() && r.summary.value(QStringLiteral("nothingNew")).toBool());
    CHECK(job.start(ImportJob::Kind::NewPipe, fx("corrupt.db")));
    r = runJob(job);
    CHECK(r.done && !r.error.isEmpty() && entriesOf(priv).isEmpty());
    CHECK(!job.start(ImportJob::Kind::Batch, QString()));

    // IMP-03 into a fresh library: every id once, a re-import adds nothing, OPML gives the same set.
    const QString csvPath = dir + QStringLiteral("/subscriptions.csv");
    {
        QFile f(csvPath);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(takeoutCsv) > 0);
    }
    const QString opmlPath = dir + QStringLiteral("/subs.opml");
    {
        QFile f(opmlPath);
        CHECK(f.open(QIODevice::WriteOnly) && f.write(opml) > 0);
    }
    const QString takeoutDb = dir + QStringLiteral("/takeout.db");
    ImportJob takeout(takeoutDb, priv);
    CHECK(takeout.start(ImportJob::Kind::TakeoutCsv, csvPath));
    r = runJob(takeout);
    CHECK(r.done && r.error.isEmpty() && num(r, "subscriptionsAdded") == 3 && num(r, "skippedRows") == 1);
    CHECK(takeout.start(ImportJob::Kind::TakeoutCsv, fx("takeout.zip")));
    r = runJob(takeout);
    CHECK(r.done && r.summary.value(QStringLiteral("nothingNew")).toBool());
    ImportJob opmlJob(dir + QStringLiteral("/opml.db"), priv);
    CHECK(opmlJob.start(ImportJob::Kind::Opml, opmlPath));
    r = runJob(opmlJob);
    CHECK(r.done && r.error.isEmpty() && num(r, "subscriptionsAdded") == 3 && num(r, "handlesPending") == 1);
    {
        LibraryStore a(takeoutDb, QStringLiteral("takeout-check"));
        LibraryStore b(dir + QStringLiteral("/opml.db"), QStringLiteral("opml-check"));
        CHECK(a.open() && b.open());
        QStringList x = a.subscribedChannelIds(), y = b.subscribedChannelIds();
        x.sort();
        y.sort();
        CHECK(x.size() == 3 && x == y);
    }

    // The account importer's entry point (IMP-02 builds this batch online).
    ImportBatch account;
    account.source = "account";
    ChannelRecord d;
    d.channelId = D;
    d.name = "Delta";
    account.subscriptions.push_back(d);
    RemotePlaylistRef mix;
    mix.remoteId = "PLzyxwvutsrqponmlkjihgfedcba987654";
    mix.title = "Mix";
    account.remotePlaylists.push_back(mix);
    CHECK(takeout.startBatch(account));
    r = runJob(takeout);
    CHECK(r.done && r.error.isEmpty() && num(r, "subscriptionsAdded") == 1 && num(r, "remotePlaylistsAdded") == 1);
    CHECK(r.summary.value(QStringLiteral("source")).toString() == QLatin1String("account"));
    CHECK(takeout.startBatch(account));
    r = runJob(takeout);
    CHECK(r.done && r.summary.value(QStringLiteral("nothingNew")).toBool());
}

static void backupRoundTripTests() {
    const QString dir = freshDir("roundtrip");
    LibraryStore source(primaryDb, QStringLiteral("roundtrip-source"));
    CHECK(source.open());
    VideoEntry later = entry(12, "Later");
    CHECK(source.addToPlaylist(QString::fromLatin1(watchLaterPlaylistId), later));
    source.setHistoryCap(800);
    source.setMetaValue(QStringLiteral("resume_behaviour"), QStringLiteral("1"));
    source.setMetaValue(QStringLiteral("hide_watched"), QStringLiteral("1"));
    source.setMetaValue(QStringLiteral("hide_watched_search"), QStringLiteral("0"));
    source.setHistoryPaused(false);
    const QString ini = writeIni(dir);
    const QString target = dir + QStringLiteral("/sailtube-backup.json");

    ImportJob exporter(primaryDb, dir);
    CHECK(exporter.startExport(target, ini));
    JobResult r = runJob(exporter);
    CHECK(r.done && r.error.isEmpty() && r.summary.value(QStringLiteral("path")).toString() == target);
    CHECK(num(r, "subscriptions") == 4 && num(r, "history") == 8 && num(r, "remotePlaylists") == 2);
    CHECK(privateMode(target));
    CHECK(entriesOf(dir) == (QStringList() << QStringLiteral("sailtube-backup.json") << QStringLiteral("sailtube.conf")));
    const QByteArray json = slurp(target);
    scanForSecrets(json);
    CHECK(json.contains("\"librarySettings\"") && json.contains("\"history_cap\": \"800\""));

    // Wipe: a fresh library restores the file.
    const QString wipedDb = freshDir("roundtrip-wiped") + QStringLiteral("/library.db");
    LibraryStore wiped(wipedDb, QStringLiteral("roundtrip-wiped"));
    CHECK(wiped.open());
    ImportJob restore(wipedDb, dir);
    CHECK(restore.start(ImportJob::Kind::SailTubeBackup, target));
    r = runJob(restore);
    CHECK(r.done && r.error.isEmpty() && r.summary.value(QStringLiteral("source")).toString() == QLatin1String("sailtube"));
    CHECK(num(r, "subscriptionsAdded") == 4 && num(r, "historyAdded") == 8 && num(r, "playlistsSkipped") == 0);
    CHECK(r.summary.value(QStringLiteral("settings")).toMap() == backupSettingsFromIni(ini));
    ImportJob::applyLibrarySettings(wiped, r.summary.value(QStringLiteral("librarySettings")).toMap());
    wiped.reload();
    for (const QString &key : backupLibraryMetaKeys()) CHECK(wiped.metaValue(key) == source.metaValue(key));
    CHECK(wiped.historyCap() == 800 && !wiped.historyPaused());

    const ImportBatch a = source.snapshot(), b = wiped.snapshot();
    CHECK(a.subscriptions.size() == b.subscriptions.size());
    for (std::size_t i = 0; i < a.subscriptions.size() && i < b.subscriptions.size(); ++i) {
        CHECK(a.subscriptions[i].channelId == b.subscriptions[i].channelId && a.subscriptions[i].name == b.subscriptions[i].name);
        CHECK(a.subscriptions[i].avatarUrl == b.subscriptions[i].avatarUrl && a.subscriptions[i].addedMs == b.subscriptions[i].addedMs);
        CHECK(a.subscriptions[i].latestUploadMs == b.subscriptions[i].latestUploadMs);
    }
    CHECK(a.pendingHandles.size() == b.pendingHandles.size() && a.pendingHandles.size() == 3);
    for (std::size_t i = 0; i < a.pendingHandles.size() && i < b.pendingHandles.size(); ++i)
        CHECK(a.pendingHandles[i].url == b.pendingHandles[i].url && a.pendingHandles[i].name == b.pendingHandles[i].name);
    // History: identical records per video (the restore orders new records by access time).
    CHECK(a.history.size() == b.history.size() && a.history.size() == 8);
    for (const HistoryRecord &x : a.history) {
        bool found = false;
        for (const HistoryRecord &y : b.history) {
            if (y.video.videoId != x.video.videoId) continue;
            found = true;
            CHECK(y.video.title == x.video.title && y.video.channel == x.video.channel && y.video.channelId == x.video.channelId);
            CHECK(y.video.durationSeconds == x.video.durationSeconds && y.video.thumbnailUrl == x.video.thumbnailUrl);
            CHECK(y.positionMs == x.positionMs && y.durationMs == x.durationMs && near(y.fraction, x.fraction));
            CHECK(y.watched == x.watched && y.watchedAtMs == x.watchedAtMs && y.playCount == x.playCount);
        }
        CHECK(found);
    }
    CHECK(a.resume.size() == b.resume.size() && a.resume.size() == 4);
    for (const ResumeRecord &x : a.resume) CHECK(wiped.resumePositionMs(QString::fromStdString(x.videoId)) == x.positionMs);
    CHECK(a.playlists.size() == b.playlists.size() && a.playlists.size() == 4); // watch later, Road trip, Favourites, Music
    for (std::size_t i = 0; i < a.playlists.size() && i < b.playlists.size(); ++i) {
        CHECK(a.playlists[i].title == b.playlists[i].title);
        CHECK(ids(a.playlists[i].items) == ids(b.playlists[i].items));
    }
    CHECK(wiped.playlistContains(QString::fromLatin1(watchLaterPlaylistId), qvid(12)));
    CHECK(a.remotePlaylists.size() == b.remotePlaylists.size());
    for (std::size_t i = 0; i < a.remotePlaylists.size() && i < b.remotePlaylists.size(); ++i) {
        CHECK(a.remotePlaylists[i].remoteId == b.remotePlaylists[i].remoteId && a.remotePlaylists[i].title == b.remotePlaylists[i].title);
        CHECK(a.remotePlaylists[i].channel == b.remotePlaylists[i].channel && a.remotePlaylists[i].itemCountHint == b.remotePlaylists[i].itemCountHint);
    }

    CHECK(restore.start(ImportJob::Kind::SailTubeBackup, target));
    r = runJob(restore);
    CHECK(r.done && r.summary.value(QStringLiteral("nothingNew")).toBool());  // idempotent
    QFile::remove(target);
    CHECK(restore.start(ImportJob::Kind::SailTubeBackup, target));
    r = runJob(restore);
    CHECK(r.done && !r.error.isEmpty());
}

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    if (argc < 3) {
        std::printf("usage: %s <fixtures dir> <scratch dir>\n", argv[0]);
        return 2;
    }
    fixtures = QString::fromLocal8Bit(argv[1]);
    scratch = QString::fromLocal8Bit(argv[2]);
    zipReaderTests();
    newPipeParseTests();
    takeoutTests();
    backupFileTests();
    setLibraryClockForTests(&testClock);
    endToEndTests();
    backupRoundTripTests();
    if (failures) {
        std::printf("library import tests: FAIL (%d failures)\n", failures);
        return 1;
    }
    std::printf("library import tests: PASS (0 failures)\n");
    return 0;
}
