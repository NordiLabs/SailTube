// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once
// The public request shape of the provider's player endpoint for a client that needs neither an
// API key, nor cookies, nor a PO token, nor player script (so no signature or "n" deciphering).
//
// Source: the public client table of yt-dlp (Unlicense), entry "visionos", and the behaviour
// described in docs/fin_analysis/INSTANT_PLAYBACK_SPEC.md section 2. The cold-start definition
// below is a default only: a host should refresh it from the bundled yt-dlp's table (the first
// entry of its JS-less default list) so that a rotation arrives with a yt-dlp update.
//
// The shape is deliberately minimal and pinned by tests: NO "gl", NO visitorData in the body (it
// travels in a header), no key, no cookies, no signatureTimestamp, no poToken. An earlier shape
// with "gl" and a body visitor produced addresses the media host refused.
#include <string>
#include <utility>
#include <vector>

namespace nordtile { namespace music { namespace sharedplayback { namespace domain {

struct ClientDefinition {
    std::string name;
    std::string version;
    int id = 0;
    std::string userAgent;
    std::string deviceMake;
    std::string deviceModel;
    std::string osName;
    std::string osVersion;

    bool usable() const { return !name.empty() && !version.empty() && id > 0 && !userAgent.empty(); }
};

// yt-dlp's "visionos" entry (client id 101).
inline ClientDefinition visionosClient() {
    ClientDefinition c;
    c.name = "VISIONOS";
    c.version = "1.02";
    c.id = 101;
    c.userAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
    c.deviceMake = "Apple";
    c.deviceModel = "RealityDevice17,1";
    c.osName = "visionOS";
    c.osVersion = "26.5.23O471";
    return c;
}

inline bool validVideoId(const std::string &id) {
    if (id.size() != 11) return false;
    for (std::size_t i = 0; i < id.size(); ++i) {
        const char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}

// Values that reach the request body: text with no quote, backslash or control character. A host
// feeding a table entry in must pass this; the cold-start default does.
inline bool plainJsonText(const std::string &text) {
    if (text.size() > 256) return false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == '"' || c == '\\' || c > 0x7e) return false;
    }
    return true;
}

inline bool plainClient(const ClientDefinition &c) {
    return c.usable() && plainJsonText(c.name) && plainJsonText(c.version) && plainJsonText(c.userAgent) &&
           plainJsonText(c.deviceMake) && plainJsonText(c.deviceModel) && plainJsonText(c.osName) && plainJsonText(c.osVersion);
}

// The JSON body of POST /youtubei/v1/player?prettyPrint=false. Empty for an invalid id or client.
inline std::string playerRequestBody(const ClientDefinition &client, const std::string &videoId) {
    if (!validVideoId(videoId) || !plainClient(client)) return std::string();
    std::string body = "{\"videoId\":\"" + videoId + "\",\"contentCheckOk\":true,\"racyCheckOk\":true,"
        "\"playbackContext\":{\"contentPlaybackContext\":{\"html5Preference\":\"HTML5_PREF_WANTS\"}},"
        "\"context\":{\"client\":{\"clientName\":\"" + client.name + "\",\"clientVersion\":\"" + client.version + "\"";
    if (!client.deviceMake.empty()) body += ",\"deviceMake\":\"" + client.deviceMake + "\"";
    if (!client.deviceModel.empty()) body += ",\"deviceModel\":\"" + client.deviceModel + "\"";
    body += ",\"userAgent\":\"" + client.userAgent + "\"";
    if (!client.osName.empty()) body += ",\"osName\":\"" + client.osName + "\"";
    if (!client.osVersion.empty()) body += ",\"osVersion\":\"" + client.osVersion + "\"";
    body += ",\"hl\":\"en\",\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0}}}";
    return body;
}

// POST /youtubei/v1/visitor_id?prettyPrint=false: the answer carries responseContext.visitorData.
inline std::string visitorRequestBody() {
    return "{\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"2.20260101.01.00\","
           "\"hl\":\"en\",\"timeZone\":\"UTC\",\"utcOffsetMinutes\":0}}}";
}

typedef std::pair<std::string, std::string> Header;

// Headers of the player request. `visitorData` may be empty (the header is then omitted).
inline std::vector<Header> playerRequestHeaders(const ClientDefinition &client, const std::string &visitorData) {
    std::vector<Header> headers;
    headers.push_back(Header("Content-Type", "application/json"));
    headers.push_back(Header("User-Agent", client.userAgent));
    headers.push_back(Header("Origin", "https://www.youtube.com"));
    headers.push_back(Header("X-YouTube-Client-Name", std::to_string(client.id)));
    headers.push_back(Header("X-YouTube-Client-Version", client.version));
    if (!visitorData.empty()) headers.push_back(Header("X-Goog-Visitor-Id", visitorData));
    headers.push_back(Header("Accept-Encoding", "identity"));
    return headers;
}

// The header set of every media request, the probe and the playback fetch alike: a bare request
// is refused at byte 0 and the address is bound to the user agent that obtained it.
inline std::vector<Header> mediaRequestHeaders(const std::string &userAgent) {
    std::vector<Header> headers;
    headers.push_back(Header("User-Agent", userAgent));
    headers.push_back(Header("Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"));
    headers.push_back(Header("Accept-Language", "en-us,en;q=0.5"));
    headers.push_back(Header("Sec-Fetch-Mode", "navigate"));
    headers.push_back(Header("Accept-Encoding", "identity"));
    return headers;
}

}}}}
