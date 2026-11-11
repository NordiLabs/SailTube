// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// DL-01/DL-03: the top-level box layout of an ISO BMFF (MP4/M4A) file.
// QMediaPlayer on SFOS 5.2 cannot play a DASH-fragmented file from disk
// (ftyp "dash", sidx, moof/mdat pairs: "No valid frames decoded before end of
// stream"), so every MP4-family download must end up flat: one moov and one
// mdat, no sidx and no moof. Qt-free: the caller supplies a reader.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace nordtile { namespace video { namespace downloads {

struct Mp4Box {
    std::string type;        // four characters
    std::uint64_t offset = 0;
    std::uint64_t size = 0;  // whole box, header included
};

struct Mp4Layout {
    bool valid = false;      // a box chain that covers the file exactly
    std::vector<Mp4Box> boxes;
    std::string majorBrand;  // from ftyp ("dash", "mp42", "isom", "M4A ")
    int moov = 0, mdat = 0, moof = 0, sidx = 0;
    bool moovBeforeMdat = false;
};

// read(offset, buffer, length) -> bytes read (0 at the end or on error).
typedef std::function<std::size_t(std::uint64_t offset, char *buffer, std::size_t length)> Mp4Reader;

// Reads only box headers (8 or 16 bytes each, plus ftyp's brand); at most
// `maxBoxes` top-level boxes.
Mp4Layout scanMp4(const Mp4Reader &read, std::uint64_t fileSize, int maxBoxes = 100000);

// Flat = valid, exactly one moov and one mdat, no moof and no sidx.
bool isFlatMp4(const Mp4Layout &layout);
// Fragmented = valid and at least one moof or sidx (what DASH delivers).
bool isFragmentedMp4(const Mp4Layout &layout);
// The containers this applies to (the WebM/Matroska family is not ISO BMFF).
bool isMp4Family(const std::string &extension);

}}}
