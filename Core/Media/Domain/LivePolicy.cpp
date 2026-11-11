// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "LivePolicy.h"

namespace nordtile { namespace video { namespace media {

int leadingItag(const std::string &formatId) {
    int value = 0;
    std::size_t i = 0;
    for (; i < formatId.size() && i < 6 && formatId[i] >= '0' && formatId[i] <= '9'; ++i)
        value = value * 10 + (formatId[i] - '0');
    if (i == 0 || (i < formatId.size() && formatId[i] != '-')) return 0;
    return value;
}

bool classicMuxedHlsItag(const std::string &formatId) {
    const int itag = leadingItag(formatId);
    return itag >= 91 && itag <= 96;
}

bool muxedHls(const MediaFormat &f) {
    return f.protocol == Protocol::Hls && f.hasVideo() && f.hasAudio()
        && f.videoCodec != VideoCodec::Unknown && f.audioCodec != AudioCodec::Unknown;
}

bool mayResolve(std::int64_t nowUnix, std::int64_t scheduledStartUnix) {
    return scheduledStartUnix <= 0 || nowUnix >= scheduledStartUnix;
}

bool stillProcessing(LiveStatus status, const std::vector<MediaFormat> &formats) {
    if (status != LiveStatus::PostLive && status != LiveStatus::WasLive) return false;
    bool segments = false;
    for (const MediaFormat &f : formats) {
        // Segment formats may carry no single URL (only fragments), so count them as offered.
        if (f.protocol == Protocol::Dash) segments = true;
        else if (!f.url.empty() && (f.protocol == Protocol::Https || f.protocol == Protocol::Hls)) return false;
    }
    return segments;
}

}}}
