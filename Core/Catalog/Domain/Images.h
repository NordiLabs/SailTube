// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// Image URL rules (DSC-19). Qt-free.
//  * Video thumbnails are derived from the id, never taken from the response:
//    i.ytimg.com/vi/<id>/mqdefault.jpg in lists (320x180), hqdefault.jpg on the details page.
//  * Avatars: square candidates only (sides within 2 px); the smallest at least 176 px, else
//    the largest; the URL's "=sNNN" (or "/sNNN-") size segment rewritten to 176; "https:" added
//    to protocol-relative URLs.

#include <string>
#include <vector>

namespace nordtile { namespace video { namespace catalog {

const int kAvatarSize = 176;

std::string listThumbnailUrl(const std::string &videoId);
std::string detailThumbnailUrl(const std::string &videoId);
// The 1280x720 picture for full-width cards (hq720.jpg). YouTube's card lists name it for nearly
// every video; it is missing for some (uploads below 720p). hqdefault.jpg (detailThumbnailUrl,
// 480x360) exists for every video and is the fallback: see ThumbnailBook.
std::string largeThumbnailUrl(const std::string &videoId);
// The video id of an id-derived thumbnail address (listThumbnailUrl / detailThumbnailUrl /
// largeThumbnailUrl), or "" for anything else (avatars, vertical frames, other hosts).
std::string thumbnailVideoId(const std::string &thumbnailUrl);
// A Short's vertical 9:16 frame (1080x1920), the image YouTube's own Shorts lockups use.
std::string shortsThumbnailUrl(const std::string &videoId);

struct ImageCandidate {
    ImageCandidate(const std::string &u = std::string(), int w = 0, int h = 0) : url(u), width(w), height(h) {}
    std::string url;
    int width;
    int height;
};

// Returns "" when no square candidate exists.
std::string chooseAvatar(const std::vector<ImageCandidate> &candidates, int size = kAvatarSize);
// Rewrites the size segment and makes the URL absolute; other URLs pass through unchanged.
std::string rewriteAvatarUrl(const std::string &url, int size = kAvatarSize);
std::string absoluteUrl(const std::string &url);

}}}
