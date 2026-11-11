// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Nemo.KeepAlive 1.2

// PLY-12 / PLT-11, FULL only: keep the display on while a video plays in the
// foreground. Loaded through a Loader so a missing plugin only logs a warning;
// the STORE package does not ship this file.
DisplayBlanking {
    property bool wanted: false
    preventBlanking: wanted
}
