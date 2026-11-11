#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Builds a fake yt-dlp zipimport archive for the warm-worker tests: main(argv) echoes its arguments and
obeys test verbs (--sleep S, --crash, --lazy, --code N, --big N). Usage: make_fake_archive.py OUT VERSION"""
import sys, zipfile
out, version = sys.argv[1], sys.argv[2]
main = '''
import os, sys, time
def main(argv=None):
    argv = list(argv or [])
    if "--crash" in argv:
        os._exit(9)
    if "--sleep" in argv:
        time.sleep(float(argv[argv.index("--sleep") + 1]))
    if "--lazy" in argv:
        from yt_dlp import lazy
        print("lazy=" + lazy.VALUE)
    if "--big" in argv:
        sys.stdout.write("x" * int(argv[argv.index("--big") + 1]))
    sys.stderr.write("diag\\n")
    print("args=" + " ".join(argv))
    code = int(argv[argv.index("--code") + 1]) if "--code" in argv else 0
    sys.exit(code)
'''
with zipfile.ZipFile(out, "w") as z:
    z.writestr("yt_dlp/__init__.py", main)
    z.writestr("yt_dlp/version.py", "__version__ = %r\n" % version)
    z.writestr("yt_dlp/lazy.py", "VALUE = %r\n" % version)
