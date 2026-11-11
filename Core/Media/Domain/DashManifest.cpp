// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "DashManifest.h"

#include <cmath>
#include <cstdio>
#include <sstream>

namespace nordtile { namespace video { namespace media {

namespace {
std::uint64_t be(const unsigned char *p, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v = (v << 8) | p[i];
    return v;
}
const std::int64_t maximumHeaderScan = 4 * 1024 * 1024;
}

Mp4Index parseMp4Index(const unsigned char *data, std::size_t size) {
    Mp4Index r;
    std::int64_t offset = 0;
    bool sawMoov = false;
    for (int boxes = 0; boxes < 64; ++boxes) {
        if (offset + 8 > static_cast<std::int64_t>(size)) {
            if (offset + 8 > maximumHeaderScan) return r;
            r.status = Mp4IndexStatus::NeedMoreData; r.bytesNeeded = offset + 16; return r;
        }
        const unsigned char *p = data + offset;
        std::uint64_t boxSize = be(p, 4);
        const std::string type(reinterpret_cast<const char *>(p + 4), 4);
        int header = 8;
        if (boxSize == 1) {
            if (offset + 16 > static_cast<std::int64_t>(size)) {
                r.status = Mp4IndexStatus::NeedMoreData; r.bytesNeeded = offset + 16; return r;
            }
            boxSize = be(p + 8, 8); header = 16;
        }
        if (boxSize == 0 || boxSize < static_cast<std::uint64_t>(header) || boxSize > (1ull << 40)) return r;
        const std::int64_t end = offset + static_cast<std::int64_t>(boxSize);
        if (type == "moov") { sawMoov = true; r.initializationEnd = end - 1; }
        if (type == "sidx") {
            if (!sawMoov) return r;
            r.indexStart = offset; r.indexEnd = end - 1; r.status = Mp4IndexStatus::Ok;
            return r;
        }
        if (type == "moof" || type == "mdat") return r; // no leading index: not on-demand DASH
        if (end > maximumHeaderScan) return r;
        offset = end;
    }
    return r;
}

std::string xmlEscape(const std::string &in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c;
        }
    }
    return out;
}

namespace {
std::string range(std::int64_t a, std::int64_t b) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%lld-%lld", static_cast<long long>(a), static_cast<long long>(b));
    return buffer;
}

void representation(std::ostringstream &x, const DashRepresentation &r, bool video) {
    x << "      <Representation id=\"" << xmlEscape(r.id) << "\" codecs=\"" << xmlEscape(r.codecs)
      << "\" bandwidth=\"" << (r.bandwidthBps > 0 ? r.bandwidthBps : 1) << "\"";
    if (video) {
        x << " width=\"" << r.width << "\" height=\"" << r.height << "\"";
        if (r.frameRate > 0) x << " frameRate=\"" << static_cast<int>(std::lround(r.frameRate)) << "\"";
    } else if (r.audioSampleRate > 0) {
        x << " audioSamplingRate=\"" << r.audioSampleRate << "\"";
    }
    x << ">\n        <BaseURL>" << xmlEscape(r.url) << "</BaseURL>\n"
      << "        <SegmentBase indexRange=\"" << range(r.indexStart, r.indexEnd) << "\">\n"
      << "          <Initialization range=\"" << range(0, r.initializationEnd) << "\"/>\n"
      << "        </SegmentBase>\n      </Representation>\n";
}
}

std::string buildOnDemandMpd(double durationSeconds, const DashRepresentation &video,
                             const DashRepresentation &audio) {
    char duration[48];
    std::snprintf(duration, sizeof duration, "PT%.3fS", durationSeconds > 0 ? durationSeconds : 0.0);
    std::ostringstream x;
    x << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\" "
      << "profiles=\"urn:mpeg:dash:profile:isoff-on-demand:2011\" minBufferTime=\"PT2S\" "
      << "mediaPresentationDuration=\"" << duration << "\">\n"
      << "  <Period id=\"0\" start=\"PT0S\">\n"
      << "    <AdaptationSet id=\"0\" contentType=\"video\" mimeType=\"" << xmlEscape(video.mimeType)
      << "\" segmentAlignment=\"true\" startWithSAP=\"1\">\n";
    representation(x, video, true);
    x << "    </AdaptationSet>\n"
      << "    <AdaptationSet id=\"1\" contentType=\"audio\" mimeType=\"" << xmlEscape(audio.mimeType)
      << "\" segmentAlignment=\"true\" startWithSAP=\"1\">\n";
    representation(x, audio, false);
    x << "    </AdaptationSet>\n  </Period>\n</MPD>\n";
    return x.str();
}

}}}
