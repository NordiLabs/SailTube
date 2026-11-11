// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "InnerTubeClient.h"
#include "Json.h"
#include "Shared/SailTune/Core/SharedPlayback/Domain/InnerTubeClient.h"

#include <QUrl>

namespace nordtile { namespace video { namespace catalog {

namespace innertube {
const char kEndpointBrowse[] = "browse";
const char kEndpointNext[] = "next";
const char kEndpointPlayer[] = "player";
const char kEndpointSearch[] = "search";
const char kEndpointResolveUrl[] = "navigation/resolve_url";
const char kEndpointReelSequence[] = "reel/reel_watch_sequence";
const char kEndpointReelItemWatch[] = "reel/reel_item_watch";
const char kEndpointVisitorId[] = "visitor_id";
const char kVisitorDataPath[] = "responseContext.visitorData";
const char kBrowseIdHome[] = "FEwhat_to_watch";
const char kBrowseIdHashtagPrefix[] = "FEhashtag";
const char kTabParamsVideos[] = "EgZ2aWRlb3PyBgQKAjoA";
const char kTabParamsShorts[] = "EgZzaG9ydHPyBgUKA5oBAA==";
const char kTabParamsStreams[] = "EgdzdHJlYW1z8gYECgJ6AA==";
const char kTabParamsPlaylists[] = "EglwbGF5bGlzdHPyBgoKCEIGCgIQaCIA";
const char kTabParamsFeatured[] = "EghmZWF0dXJlZPIGBAoCMgA=";
const char kSearchParamsShorts[] = "EgIQCQ==";
const char kWebClientVersion[] = "2.20261105.05.01";
const char kClientVersionKey[] = "INNERTUBE_CLIENT_VERSION";
const char kSuggestQueryPrefix[] = "client=firefox&ds=yt&hl=";

QString channelTabParams(const QString &tab) {
    if (tab == QLatin1String("videos")) return QLatin1String(kTabParamsVideos);
    if (tab == QLatin1String("shorts")) return QLatin1String(kTabParamsShorts);
    if (tab == QLatin1String("streams")) return QLatin1String(kTabParamsStreams);
    if (tab == QLatin1String("playlists")) return QLatin1String(kTabParamsPlaylists);
    if (tab == QLatin1String("featured")) return QLatin1String(kTabParamsFeatured);
    return QString();
}
}

namespace {
const char kServiceBase[] = "https://www.youtube.com/youtubei/v1/";
const char kServiceQuery[] = "?prettyPrint=false";

// A visitor id travels in a request header: printable ASCII of sane length, nothing else.
bool plainHeaderText(const QString &text) {
    if (text.isEmpty() || text.size() > 512) return false;
    for (const QChar c : text)
        if (c.unicode() <= 0x20 || c.unicode() > 0x7e) return false;
    return true;
}
}  // namespace

InnerTubeClient::InnerTubeClient(HttpTransport *transport, ClientIdentity *identity, WorkLane *lane,
                                 const DiscoveryLocale &locale, QObject *parent)
    : QObject(parent), transport_(transport), identity_(identity), lane_(lane), locale_(locale),
      visitor_(std::make_shared<VisitorMemory>()) {}

QJsonObject InnerTubeClient::context() const {
    QJsonObject client;
    client.insert(QStringLiteral("clientName"), QStringLiteral("WEB"));
    client.insert(QStringLiteral("clientVersion"), identity_ ? identity_->clientVersion()
                                                             : QString::fromLatin1(ClientIdentity::builtInVersion()));
    client.insert(QStringLiteral("hl"), locale_.hl);
    client.insert(QStringLiteral("gl"), locale_.gl);
    QJsonObject ctx;
    ctx.insert(QStringLiteral("client"), client);
    return ctx;
}

HttpRequest InnerTubeClient::buildRequest(const QString &endpoint, const QJsonObject &body, int timeoutMs) const {
    QJsonObject payload = body;
    payload.insert(QStringLiteral("context"), context());
    HttpRequest req;
    req.url = QUrl(QLatin1String(kServiceBase) + endpoint + QLatin1String(kServiceQuery));
    req.method = "POST";
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.timeoutMs = timeoutMs;
    const QByteArray version = (identity_ ? identity_->clientVersion() : QString::fromLatin1(ClientIdentity::builtInVersion())).toLatin1();
    req.headers.append(qMakePair(QByteArray("Content-Type"), QByteArray("application/json")));
    req.headers.append(qMakePair(QByteArray("Origin"), QByteArray("https://www.youtube.com")));
    req.headers.append(qMakePair(QByteArray("X-YouTube-Client-Name"), QByteArray("1")));
    req.headers.append(qMakePair(QByteArray("X-YouTube-Client-Version"), version));
    return req;
}

HttpRequest InnerTubeClient::buildTrackListRequest(const QString &videoId, const QString &visitorData, int timeoutMs) const {
    namespace shared = nordtile::music::sharedplayback::domain;
    const shared::ClientDefinition def = shared::visionosClient();
    auto text = [](const std::string &value) { return QString::fromStdString(value); };
    // The pinned request shape (playerRequestBody) with the device language, so that track
    // names arrive localised; like it, no gl, no visitor id in the body, no key, no cookies.
    QJsonObject client;
    client.insert(QStringLiteral("clientName"), text(def.name));
    client.insert(QStringLiteral("clientVersion"), text(def.version));
    if (!def.deviceMake.empty()) client.insert(QStringLiteral("deviceMake"), text(def.deviceMake));
    if (!def.deviceModel.empty()) client.insert(QStringLiteral("deviceModel"), text(def.deviceModel));
    client.insert(QStringLiteral("userAgent"), text(def.userAgent));
    if (!def.osName.empty()) client.insert(QStringLiteral("osName"), text(def.osName));
    if (!def.osVersion.empty()) client.insert(QStringLiteral("osVersion"), text(def.osVersion));
    client.insert(QStringLiteral("hl"), locale_.hl.isEmpty() ? QStringLiteral("en") : locale_.hl);
    client.insert(QStringLiteral("timeZone"), QStringLiteral("UTC"));
    client.insert(QStringLiteral("utcOffsetMinutes"), 0);
    QJsonObject ctx;
    ctx.insert(QStringLiteral("client"), client);
    QJsonObject content;
    content.insert(QStringLiteral("html5Preference"), QStringLiteral("HTML5_PREF_WANTS"));
    QJsonObject playback;
    playback.insert(QStringLiteral("contentPlaybackContext"), content);
    QJsonObject payload;
    payload.insert(QStringLiteral("videoId"), videoId);
    payload.insert(QStringLiteral("contentCheckOk"), true);
    payload.insert(QStringLiteral("racyCheckOk"), true);
    payload.insert(QStringLiteral("playbackContext"), playback);
    payload.insert(QStringLiteral("context"), ctx);

    HttpRequest req;
    req.url = QUrl(QLatin1String(kServiceBase) + QLatin1String(innertube::kEndpointPlayer) + QLatin1String(kServiceQuery));
    req.method = "POST";
    req.body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    req.timeoutMs = timeoutMs;
    // The pinned header set, minus its Accept-Encoding: the transport negotiates compression
    // itself (the answer is about 100 KB of JSON).
    const std::vector<shared::Header> headers =
        shared::playerRequestHeaders(def, plainHeaderText(visitorData) ? visitorData.toStdString() : std::string());
    for (const shared::Header &h : headers) {
        if (h.first == "Accept-Encoding") continue;
        req.headers.append(qMakePair(QByteArray::fromStdString(h.first), QByteArray::fromStdString(h.second)));
    }
    return req;
}

QString InnerTubeClient::visitorOf(const QJsonObject &answer) {
    const QString value = json::at(answer, QLatin1String(innertube::kVisitorDataPath)).toString();
    return plainHeaderText(value) ? value : QString();
}

void InnerTubeClient::remember(const std::shared_ptr<VisitorMemory> &memory, const QJsonObject &answer) {
    if (!memory) return;
    const QString value = visitorOf(answer);
    if (value.isEmpty()) return;
    std::lock_guard<std::mutex> lock(memory->mutex);
    memory->value = value;
}

QString InnerTubeClient::visitorData() const {
    std::lock_guard<std::mutex> lock(visitor_->mutex);
    return visitor_->value;
}

void InnerTubeClient::forgetVisitor() {
    std::lock_guard<std::mutex> lock(visitor_->mutex);
    visitor_->value.clear();
}

void InnerTubeClient::ensureVisitor(QObject *context, std::function<void()> then) {
    if (!visitorData().isEmpty()) {
        if (then) then();
        return;
    }
    // The answer is remembered by send() before `done` runs; nothing else is read from it.
    std::function<bool(const InnerTubeReply &)> parse = [](const InnerTubeReply &r) { return r.outcome == Outcome::Ok; };
    std::function<void(bool)> done = [then](bool) { if (then) then(); };
    post<bool>(QLatin1String(innertube::kEndpointVisitorId), QJsonObject(), context, parse, done);
}

InnerTubeReply InnerTubeClient::toReply(const HttpReply &http) {
    InnerTubeReply r;
    r.httpStatus = http.status;
    r.elapsedMs = http.elapsedMs;
    r.errorText = http.errorText;
    r.outcome = outcomeForHttp(http.status);
    if (r.outcome != Outcome::Ok) return r;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(http.body, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) {
        r.outcome = Outcome::ShapeDrift;
        r.errorText = QStringLiteral("not a JSON object");
        return r;
    }
    r.json = doc.object();
    return r;
}

void InnerTubeClient::logStage(const QByteArray &endpoint, qint64 netMs, qint64 parseMs, qint64 readyMs, int bytes, int status) {
    qInfo("sailtube-timing innertube %s net=%lld parse=%lld ready=%lld bytes=%d status=%d", endpoint.constData(),
          static_cast<long long>(netMs), static_cast<long long>(parseMs), static_cast<long long>(readyMs), bytes, status);
}

}}}
