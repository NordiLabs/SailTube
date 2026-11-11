// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#include "Mp4Layout.h"

namespace nordtile { namespace video { namespace downloads {

namespace {

std::uint64_t be(const unsigned char *p, int n) {
    std::uint64_t v = 0;
    for (int i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

} // namespace

Mp4Layout scanMp4(const Mp4Reader &read, std::uint64_t fileSize, int maxBoxes) {
    Mp4Layout layout;
    std::uint64_t offset = 0;
    bool sawMdat = false;
    while (offset < fileSize) {
        if (static_cast<int>(layout.boxes.size()) >= maxBoxes) return layout;
        unsigned char head[16];
        if (fileSize - offset < 8 || read(offset, reinterpret_cast<char *>(head), 8) != 8) return layout;
        std::uint64_t size = be(head, 4);
        const std::string type(reinterpret_cast<const char *>(head + 4), 4);
        std::uint64_t header = 8;
        if (size == 1) {
            if (fileSize - offset < 16 || read(offset + 8, reinterpret_cast<char *>(head + 8), 8) != 8) return layout;
            size = be(head + 8, 8);
            header = 16;
        } else if (size == 0) {
            size = fileSize - offset; // to the end of the file
        }
        if (size < header || size > fileSize - offset) return layout;
        Mp4Box box;
        box.type = type;
        box.offset = offset;
        box.size = size;
        layout.boxes.push_back(box);
        if (type == "ftyp" && size >= header + 4) {
            char brand[4];
            if (read(offset + header, brand, 4) == 4) layout.majorBrand.assign(brand, 4);
        } else if (type == "moov") {
            ++layout.moov;
            if (!sawMdat) layout.moovBeforeMdat = true;
        } else if (type == "mdat") {
            ++layout.mdat;
            sawMdat = true;
        } else if (type == "moof") {
            ++layout.moof;
        } else if (type == "sidx") {
            ++layout.sidx;
        }
        offset += size;
    }
    layout.valid = offset == fileSize && !layout.boxes.empty();
    return layout;
}

bool isFlatMp4(const Mp4Layout &l) {
    return l.valid && l.moov == 1 && l.mdat == 1 && l.moof == 0 && l.sidx == 0;
}

bool isFragmentedMp4(const Mp4Layout &l) { return l.valid && (l.moof > 0 || l.sidx > 0); }

bool isMp4Family(const std::string &e) { return e == "mp4" || e == "m4a" || e == "m4v" || e == "mov"; }

}}}
