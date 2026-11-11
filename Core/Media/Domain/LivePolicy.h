// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Pure live/upcoming policy (RES-19) and the post-live "still processing"
// rule (RES-09). Time is passed in as Unix seconds; nothing reads a clock.

#include "MediaFormat.h"
#include <cstdint>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace media {

// RES-19: the leading itag of a format id ("95" or "95-1" -> 95), 0 if none.
int leadingItag(const std::string &formatId);

// RES-19: classic muxed HLS variants (itags 91-96, served by the ANDROID_VR
// client). Other clients' split-audio HLS is not joinable.
bool classicMuxedHlsItag(const std::string &formatId);

// RES-19: a muxed (video + audio) HLS format, the only kind a running
// livestream is played from. HLS is never probed or proxied.
bool muxedHls(const MediaFormat &);

// RES-19: whether a video may be resolved now. An upcoming video is not
// resolved again before its scheduled start; scheduledStartUnix <= 0 (not
// known) leaves the decision to the caller's normal error policy (RES-09
// treats "upcoming" as final, so there is no automatic retry).
bool mayResolve(std::int64_t nowUnix, std::int64_t scheduledStartUnix);

// RES-09: a post-live (or was-live) recording that offers only segment
// formats (DASH fragments) and no direct or HLS format is still being
// processed by the provider.
bool stillProcessing(LiveStatus, const std::vector<MediaFormat> &);

}}}
