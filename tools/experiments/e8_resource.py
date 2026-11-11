#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""E8 helper: play a silent stream with a media.role, gated by the Sailfish audio resource.

Mirrors the PLY-23 order: acquire the resource, go to PLAYING only when it is granted, pause
when it is lost, and log how long the pause took. Needs python3-gobject (GLib, Gst 1.0) and
libaudioresource.so.1 (public C API: audioresource_init/acquire/release/free, types
AUDIO_RESOURCE_GAME = 1 and AUDIO_RESOURCE_MEDIA = 2). Run with `python3 -I`.

    python3 -I e8_resource.py --role x-maemo --resource media|game|none --name TAG --hold 90

Each event is one line: `e8: t=<monotonic ms> event=<name> ...`. The stream is silent
(wave=silence) and at volume 0; the caller has already muted every sink.
"""
import argparse
import ctypes
import signal
import sys
import time

import gi

gi.require_version("Gst", "1.0")
from gi.repository import GLib, Gst  # noqa: E402

TYPES = {"game": 1, "media": 2}
CALLBACK = ctypes.CFUNCTYPE(None, ctypes.c_void_p, ctypes.c_bool, ctypes.c_void_p)


def log(event, **fields):
    extra = " ".join("%s=%s" % item for item in sorted(fields.items()))
    print("e8: t=%d event=%s %s" % (int(time.monotonic() * 1000), event, extra), flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--role", required=True)
    parser.add_argument("--resource", choices=["none", "game", "media"], default="media")
    parser.add_argument("--name", default="sailtube-e8")
    parser.add_argument("--hold", type=int, default=90)
    args = parser.parse_args()

    Gst.init(None)
    pipeline = Gst.parse_launch(
        "audiotestsrc wave=silence is-live=true ! audioconvert ! audioresample ! "
        "pulsesink name=sink volume=0.0")
    sink = pipeline.get_by_name("sink")
    sink.set_property("client-name", args.name)
    sink.set_property("stream-properties", Gst.Structure.new_from_string(
        "props,media.role=(string)%s,media.name=(string)%s" % (args.role, args.name)))
    loop = GLib.MainLoop()

    def set_state(state, reason):
        start = time.monotonic()
        pipeline.set_state(state)
        result, current, _pending = pipeline.get_state(2 * Gst.SECOND)
        log("state", target=state.value_nick, reached=current.value_nick, result=result.value_nick,
            ms=int((time.monotonic() - start) * 1000), reason=reason)

    pipeline.set_state(Gst.State.PAUSED)
    library = None
    resource = None
    if args.resource == "none":
        set_state(Gst.State.PLAYING, "no-resource")
    else:
        try:
            library = ctypes.CDLL("libaudioresource.so.1")
        except OSError as error:
            log("resource-library-missing", error=str(error).replace(" ", "_"))
            return 4
        library.audioresource_init.restype = ctypes.c_void_p
        library.audioresource_init.argtypes = [ctypes.c_int, CALLBACK, ctypes.c_void_p]
        for name in ("audioresource_acquire", "audioresource_release", "audioresource_free"):
            getattr(library, name).argtypes = [ctypes.c_void_p]

        def on_acquired(_resource, acquired, _data):
            log("resource-callback", acquired=int(bool(acquired)))
            if acquired:
                set_state(Gst.State.PLAYING, "acquired")
            else:
                set_state(Gst.State.PAUSED, "lost")

        callback = CALLBACK(on_acquired)  # kept alive for the whole run
        resource = library.audioresource_init(TYPES[args.resource], callback, None)
        log("resource-init", type=args.resource, ok=int(bool(resource)))
        library.audioresource_acquire(resource)
        log("resource-acquire-requested")

    bus = pipeline.get_bus()
    bus.add_signal_watch()
    bus.connect("message::error", lambda _b, m: log("gst-error", message=m.parse_error()[0].message.replace(" ", "_")))
    GLib.timeout_add_seconds(args.hold, loop.quit)
    # The experiment scripts stop helpers with SIGINT/SIGTERM; end cleanly so the resource is
    # released and the stream paused. (Background jobs of sh start with SIGINT ignored.)
    for number in (signal.SIGINT, signal.SIGTERM):
        GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, number, lambda: (log("signal"), loop.quit(), False)[-1])
    log("hold", seconds=args.hold, role=args.role, resource=args.resource)
    try:
        loop.run()
    except KeyboardInterrupt:
        log("interrupted")
    set_state(Gst.State.PAUSED, "end")
    if resource:
        library.audioresource_release(resource)
        context = GLib.MainContext.default()
        deadline = time.monotonic() + 1.0
        while time.monotonic() < deadline:
            context.iteration(False)
            time.sleep(0.02)
        library.audioresource_free(resource)
        log("resource-freed")
    pipeline.set_state(Gst.State.NULL)
    return 0


if __name__ == "__main__":
    sys.exit(main())
