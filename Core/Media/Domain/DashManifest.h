// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Builds a static, on-demand DASH manifest (ISO/IEC 23009-1, isoff-on-demand
// profile) for one video and one audio stream, so a single GStreamer pipeline
// can play adaptive formats with A/V sync owned by the pipeline. The byte
// ranges come from each file's own index ('sidx'), read by parseMp4Index().

#include <cstdint>
#include <string>

namespace nordtile { namespace video { namespace media {

enum class Mp4IndexStatus { Ok, NeedMoreData, Invalid };

struct Mp4Index {
    Mp4IndexStatus status = Mp4IndexStatus::Invalid;
    std::int64_t initializationEnd = 0; // inclusive end of 'moov'
    std::int64_t indexStart = 0;        // first byte of 'sidx'
    std::int64_t indexEnd = 0;          // inclusive end of 'sidx'
    std::int64_t bytesNeeded = 0;       // NeedMoreData: read at least this many bytes
};

// Walks top-level ISO BMFF boxes in the first bytes of a file. 'ftyp' and
// 'moov' must precede 'sidx' (the layout of on-demand DASH files).
Mp4Index parseMp4Index(const unsigned char *data, std::size_t size);

struct DashRepresentation {
    std::string id;
    std::string mimeType;   // video/mp4 or audio/mp4
    std::string codecs;     // RFC 6381, e.g. avc1.640028 / mp4a.40.2
    std::string url;        // absolute
    int bandwidthBps = 0;
    int width = 0, height = 0;
    double frameRate = 0;
    int audioSampleRate = 0;
    std::int64_t initializationEnd = 0;
    std::int64_t indexStart = 0, indexEnd = 0;
};

std::string buildOnDemandMpd(double durationSeconds, const DashRepresentation &video,
                             const DashRepresentation &audio);

std::string xmlEscape(const std::string &);

}}}
