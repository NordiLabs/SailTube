#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Gate of the time-limited beta profile (docs/BETA.md): prints "ok" when DATE is a real YYYY-MM-DD that is
not in the past (local date) and not later than 2026-12-30; otherwise prints the reason and exits 1.
Called by sailtube.pro (CONFIG+=sailtube_beta) so the build fails; --today overrides the local date in tests.
  check_beta_date.py DATE [--today YYYY-MM-DD]"""
import datetime, re, sys

LATEST = datetime.date(2026, 12, 30)


def check(text, today):
    if not text:
        return "missing: set SAILTUBE_BETA_EXPIRES=YYYY-MM-DD"
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", text):
        return "unparsable %r (YYYY-MM-DD)" % text
    try:
        d = datetime.date(int(text[:4]), int(text[5:7]), int(text[8:]))
    except ValueError:
        return "not a calendar date: %s" % text
    if d < today:
        return "in the past: %s (today %s)" % (d, today)
    if d > LATEST:
        return "later than %s: %s" % (LATEST, d)
    return None


def main(argv):
    args = list(argv)
    if args == ["--build-ms"]:   # the build time the beta clock uses as its floor
        import time
        print(int(time.time() * 1000))
        return 0
    today = datetime.date.today()
    if "--today" in args:
        i = args.index("--today")
        today = datetime.date.fromisoformat(args[i + 1])
        del args[i:i + 2]
    problem = check(args[0] if args else "", today)
    print("ok" if problem is None else "error: " + problem)
    return 0 if problem is None else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
