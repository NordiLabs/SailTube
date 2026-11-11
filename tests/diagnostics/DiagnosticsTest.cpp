// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// DIA-01 (one fixture per failure class, drift-only hint) and DIA-02 (report sections, 10 timelines
// with five phases, no secrets or full media URLs). Plain C++11.
#include "../../Core/Diagnostics/Domain/DiagnosticsReport.h"
#include "../../Core/Diagnostics/Domain/ErrorCounters.h"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace nordtile::diagnostics;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static Outcome http(int status, Outcome::Parse parse = Outcome::Parse::Ok) { Outcome o; o.httpStatus = status; o.parse = parse; return o; }

static void classes() {
    // One fixture per class.
    CHECK(classify(http(200, Outcome::Parse::ShapeMissing)) == FailureClass::ShapeDrift);
    CHECK(classify(http(404)) == FailureClass::Http4xx);
    CHECK(classify(http(403)) == FailureClass::Http4xx);
    CHECK(classify(http(429)) == FailureClass::Http429);
    CHECK(classify(http(503)) == FailureClass::Http5xx);
    Outcome net; net.networkError = true;
    CHECK(classify(net) == FailureClass::Network);
    Outcome final_ = http(200); final_.unplayable = true;
    CHECK(classify(final_) == FailureClass::Unplayable);
    CHECK(classify(http(200, Outcome::Parse::Empty)) == FailureClass::EmptyValid);
    CHECK(classify(http(200)) == FailureClass::None);
    // A 429 or 5xx whose body also lacks the expected shape is still not drift.
    CHECK(classify(http(429, Outcome::Parse::ShapeMissing)) == FailureClass::Http429);
    CHECK(classify(http(502, Outcome::Parse::ShapeMissing)) == FailureClass::Http5xx);
    Outcome netShape; netShape.networkError = true; netShape.parse = Outcome::Parse::ShapeMissing;
    CHECK(classify(netShape) == FailureClass::Network);
    CHECK(classify(http(302)) == FailureClass::Http4xx);  // an unfollowed redirect is a request failure, not drift
}

static void hintOnlyFromDrift() {
    ErrorCounters c;
    for (int i = 0; i < 10; ++i) {
        c.record(Area::Channel, http(429));
        c.record(Area::Channel, http(503));
        Outcome net; net.networkError = true;
        c.record(Area::Channel, net);
        c.record(Area::Channel, http(200, Outcome::Parse::Empty));   // "no Videos tab" is valid-empty, not drift
    }
    CHECK(!c.parserUpdateHint(Area::Channel));
    CHECK(!c.anyParserUpdateHint());
    CHECK(c.count(Area::Channel, FailureClass::Http429) == 10);
    CHECK(c.count(Area::Channel, FailureClass::EmptyValid) == 10);
    c.record(Area::Search, http(200, Outcome::Parse::ShapeMissing));
    c.record(Area::Search, http(200, Outcome::Parse::ShapeMissing));
    CHECK(!c.parserUpdateHint(Area::Search));
    c.record(Area::Search, http(200, Outcome::Parse::ShapeMissing));
    CHECK(c.parserUpdateHint(Area::Search));
    CHECK(!c.parserUpdateHint(Area::Channel));
    CHECK(c.total(FailureClass::ShapeDrift) == 3);
    CHECK(c.record(Area::Feed, http(200)) == FailureClass::None);
    CHECK(c.total(FailureClass::None) == 0);
}

static bool contains(const std::string &h, const std::string &n) { return h.find(n) != std::string::npos; }

static void report() {
    const std::string cookie = "SAPISID=AbCdEfGhIjKlMnOp1234/xyz";
    const std::string cookieValue = "AbCdEfGhIjKlMnOp1234/xyz";
    const std::string token = "MnRpb2tlbi12YWx1ZS1mb3ItcG8tdG9rZW4";
    const std::string mediaUrl = "https://rr3---sn-abc.googlevideo.com/videoplayback?expire=1&sig=SECRETSIG&pot=" + token;
    ReportInput in;
    in.versions = {{"app", "0.1.0"}, {"extractor", "yt-dlp 2026.10.26"}, {"client definition", "VISIONOS 101"},
                   {"po helper", "bgutil 1.2 (loopback, " + mediaUrl + ")"}};
    in.versionSkew = {"extractor archive on disk 2026.11.01, loaded 2026.10.26 until relaunch"};
    in.breakerState = "closed (strikes 1/2)";
    in.poProviderState = "running, last token for session " + cookie;
    in.counters.record(Area::Player, http(403));
    in.counters.record(Area::Account, http(401));
    for (int i = 0; i < 12; ++i) {
        ResolveTimeline t;
        t.path = i % 3 == 0 ? "extractor" : "fast";
        t.queueWaitMs = i; t.postMs = 300 + i; t.probeMs = 80; t.prerollMs = 400; t.firstFrameMs = 900 + i;
        if (i == 11) t.path = "fast " + mediaUrl;   // a careless label
        in.timelines.push_back(t);
    }
    in.hardwareDecodeProbe = "gstreamer 1.26 loaded=1 h264hw=1 vp9hw=0";
    in.networkFamily = "ipv4";
    in.secrets = {cookieValue, token};
    const std::string r = buildReport(in);

    for (const std::string &section : reportSections()) CHECK(contains(r, section));
    CHECK(contains(r, "skew: extractor archive"));
    CHECK(contains(r, "player http-4xx: 1"));
    CHECK(contains(r, "account http-4xx: 1"));
    // Exactly the last 10 timelines, each with five phase columns.
    std::istringstream lines(r.substr(r.find("[resolves]")));
    std::string line;
    std::getline(lines, line); std::getline(lines, line);  // header + column names
    int rows = 0;
    while (std::getline(lines, line) && !line.empty()) {
        ++rows;
        std::istringstream cols(line);
        std::vector<std::string> v; std::string w;
        while (cols >> w) v.push_back(w);
        CHECK(v.size() >= 6);
        if (v.size() >= 6) for (std::size_t k = v.size() - 5; k < v.size(); ++k) CHECK(v[k] != "-");
    }
    CHECK(rows == 10);
    CHECK(!contains(r, "queue_ms post_ms probe_ms preroll_ms first_frame_ms\nextractor 0 "));  // the oldest two are cut
    // No secrets, tokens, signatures or full media URLs.
    CHECK(!contains(r, cookieValue));
    CHECK(!contains(r, token));
    CHECK(!contains(r, "SECRETSIG"));
    CHECK(!contains(r, "googlevideo.com/videoplayback"));
    CHECK(!contains(r, "pot=M"));
    CHECK(!contains(r, "sig=S"));
    CHECK(contains(r, "[media-url]"));
}

static void helpers() {
    CHECK(formatMs(-1) == "-");
    CHECK(formatMs(1234) == "1234");
    CHECK(detail::startsWithAt("xhttps://a", 1, "https://"));
    CHECK(!detail::startsWithAt("http", 0, "https://"));
    CHECK(detail::isUrlChar('a') && !detail::isUrlChar(' ') && !detail::isUrlChar('"'));
    CHECK(std::string(areaLabel(Area::WatchNext)) == "watch-next");
    CHECK(std::string(areaLabel(Area::Count)) == "unknown");
    CHECK(std::string(classLabel(FailureClass::Http429)) == "http-429");
    CHECK(std::string(classLabel(FailureClass::ShapeDrift)) == "shape-drift");
    ErrorCounters c;
    c.record(Area::Feed, http(503));
    c.reset();
    CHECK(c.total(FailureClass::Http5xx) == 0);
}

static void redaction() {
    CHECK(redact("see https://x.googlevideo.com/videoplayback?sig=1 now") == "see [media-url] now");
    CHECK(redact("https://www.youtube.com/watch?v=abc&pot=TOKEN&x=1") == "https://www.youtube.com/watch?v=abc&pot=[redacted]&x=1");
    CHECK(redact("token=abc", {"abc"}) == "token=abc");          // secrets shorter than 4 are not blanked
    CHECK(redact("cookie LONGSECRET here", {"LONGSECRET"}) == "cookie [redacted] here");
    CHECK(redact("plain text n=5 ok") == "plain text n=[redacted] ok");
}

int main() {
    classes();
    hintOnlyFromDrift();
    report();
    redaction();
    helpers();
    if (failures) { std::printf("diagnostics: %d failure(s)\n", failures); return 1; }
    std::printf("diagnostics DiagnosticsTest: all checks passed\n");
    return 0;
}
