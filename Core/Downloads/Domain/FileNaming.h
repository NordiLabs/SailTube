// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-05 / NFR-SEC-05: download file names. Output is always one literal path
// component: no separator, never "." or "..", no NUL or control character, no
// leading '-' or '.', nothing a FAT/exFAT SD card refuses, at most 255 bytes
// including the longest suffix the engine appends. Nothing is ever expanded:
// "%(title)s" or "${x}" in a title stay as those literal characters.
//
// Pattern: "<title> [<id>].<ext>". The tracks of a video download use
// "<title> [<id>].video.<ext>" / ".audio.<ext>" until remuxed; the in-progress
// file is "<name>.part"; the saved thumbnail is "<title> [<id>].jpg".

#include "DownloadTypes.h"
#include <cstddef>
#include <string>

namespace nordtile { namespace video { namespace downloads {

const std::size_t maxTitleBytes = 150;
const std::size_t maxIdBytes = 64;
const std::size_t maxNameBytes = 255;
// ".video.webm.part": the longest suffix appended to a base name.
const std::size_t maxSuffixBytes = 16;

// [A-Za-z0-9_-] only, at most 64 bytes; "unknown" when nothing is left.
std::string sanitizeVideoId(const std::string &id);

// UTF-8 in, UTF-8 out, at most maxBytes (cut on a code point boundary).
// Invalid UTF-8, control and bidi-override characters are dropped; path and
// FAT-reserved characters become spaces; whitespace collapses; dots, spaces and
// '-' are trimmed at the start, dots and spaces at the end. Empty -> "video".
std::string sanitizeTitle(const std::string &title, std::size_t maxBytes = maxTitleBytes);

// m4a, webm, mp4, mkv, opus, jpg.
bool allowedExtension(const std::string &ext);

// "<title> [<id>]", title shrunk further (still on a code point boundary) so
// that base + maxSuffixBytes fits in maxNameBytes.
std::string baseName(const std::string &title, const std::string &videoId);

// base + "." + ext; empty when ext is not allowed or base is not a safe name.
std::string fileName(const std::string &base, const std::string &ext);
// base + ".video." / ".audio." + ext; empty on a bad ext.
std::string trackFileName(const std::string &base, Track track, const std::string &ext);
std::string partFileName(const std::string &name);       // name + ".part"
std::string thumbnailFileName(const std::string &base);  // base + ".jpg"

// One safe path component (the check every name above passes).
bool safeFileName(const std::string &name);

// Number of bytes of the longest prefix of s that ends on a UTF-8 code point
// boundary and is at most maxBytes.
std::size_t utf8Prefix(const std::string &s, std::size_t maxBytes);

}}}
