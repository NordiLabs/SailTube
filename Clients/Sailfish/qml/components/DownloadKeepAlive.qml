// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
import QtQuick 2.6
import Nemo.KeepAlive 1.2

// DL-09 / PLT-11, FULL only: keep downloads running while the app is in the
// background. Loaded through a Loader so a missing plugin only logs a warning;
// the STORE package does not ship this file.
KeepAlive {
    enabled: downloads.keepAliveWanted
}
