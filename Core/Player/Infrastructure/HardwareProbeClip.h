// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#pragma once

// PLY-20: the bundled 2 s H.264 clip the per-device hardware probe decodes (compiled in, so no
// packaging step can lose it). Qt Core only.

#include "Core/Player/Domain/TempDirPolicy.h"

#include <QByteArray>
#include <QString>

namespace nordtile { namespace player {

// The clip bytes (MP4, H.264 Constrained Baseline, 320x240, 60 frames at 30 fps).
QByteArray hardwareProbeClip();

// Writes the clip to <directory>/hardware-probe-h264.mp4 unless an identical file is there and
// returns its path; "" on failure or when `directory` is in a RAM-backed tree (PLT-09).
QString writeHardwareProbeClip(const QString &directory);

} } // namespace nordtile::player
