# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Deterministic fake yt-dlp release archives for tests/extractor_update (stored entries, fixed times).

Every archive is a zip with a shebang prefix, like the real `yt-dlp` release asset. Its __main__.py,
yt_dlp/__init__.py and yt_dlp/version.py each create a marker file in $XUPD_MARKER_DIR when run or
imported, so a test can prove the verifier never runs or imports anything from it.

  python3 -I make_archives.py OUT_DIR
"""
import os
import sys
import zipfile

SHEBANG = b"#!/usr/bin/env python3\n"
STAMP = (2026, 11, 1, 0, 0, 0)


def marker(name):
    return ("import os as _os\n"
            "if _os.environ.get('XUPD_MARKER_DIR'):\n"
            "    open(_os.path.join(_os.environ['XUPD_MARKER_DIR'], %r), 'w').close()\n" % name)


def entries(version):
    return [
        ("__main__.py", marker("executed-main") + "import yt_dlp\nyt_dlp.main()\n"),
        ("yt_dlp/", None),
        ("yt_dlp/__init__.py", marker("imported-init") + "def main(argv=None):\n    return 0\n"),
        ("yt_dlp/version.py", marker("imported-version") + "__version__ = %r\nRELEASE_GIT_HEAD = ''\n" % version),
        ("yt_dlp/extractor/", None),
        ("yt_dlp/extractor/_padding.py", "# padding\n" + "#" * 4000 + "\n"),
    ]


def write(path, items):
    tmp = path + ".tmp"
    with open(tmp, "wb") as raw:
        raw.write(SHEBANG)
        with zipfile.ZipFile(raw, "w", zipfile.ZIP_STORED) as z:
            for name, text in items:
                info = zipfile.ZipInfo(name, STAMP)
                info.create_system = 3
                info.external_attr = ((0o40755 if text is None else 0o100644) << 16) | (0x10 if text is None else 0)
                z.writestr(info, b"" if text is None else text.encode("utf-8"))
    os.replace(tmp, path)


def main(out):
    os.makedirs(out, exist_ok=True)
    write(os.path.join(out, "stable.pyz"), entries("2026.11.01"))
    write(os.path.join(out, "stable-next.pyz"), entries("2026.11.20"))
    write(os.path.join(out, "nightly.pyz"), entries("2026.11.04.232659"))
    write(os.path.join(out, "mismatch.pyz"), entries("2026.11.09"))
    write(os.path.join(out, "dotdot.pyz"), entries("2026.11.02") + [("../escape.py", marker("escaped"))])
    write(os.path.join(out, "absolute.pyz"), entries("2026.11.02") + [("/tmp/absolute.py", "x = 1\n")])
    write(os.path.join(out, "noversion.pyz"), [e for e in entries("2026.11.02") if e[0] != "yt_dlp/version.py"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
