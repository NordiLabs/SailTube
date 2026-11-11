#!/bin/sh
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# SailTune's own tests for the pinned shared playback component, run unchanged from the pin
# (Shared/SailTune/tests/sharedplayback/run.sh): domain checks, fast path and media proxy against
# local TLS fakes. This wrapper only exists so the native tool finds it under tests/*/run.sh.
set -e
root=$(cd "$(dirname "$0")/../.." && pwd)
exec sh "$root/Shared/SailTune/tests/sharedplayback/run.sh"
