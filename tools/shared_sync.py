#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Check (or refresh) the pinned SailTune snapshot under Shared/.

  tools/shared_sync.py --check                         local files match Shared/SHARED_MANIFEST.json
  tools/shared_sync.py --check --upstream DIR          ... and report drift against a SailTune checkout
  tools/shared_sync.py --check --upstream DIR --ref R  ... against commit R of that checkout (git objects,
                                                       not the working tree)
  tools/shared_sync.py --check --upstream DIR --pinned ... each entry against its own upstream_commit
  tools/shared_sync.py --refresh-verbatim DIR [--ref R] [--only PREFIX]
                                                       copy changed verbatim files and re-pin
  ... --ref R --restamp                                also record R as the commit of the verbatim entries in scope
                                                       whose upstream content at R equals their pin (a pinned
                                                       commit that no longer exists upstream, e.g. after a
                                                       history rewrite)
  ... --ref R --add UPSTREAM_PATH [--note TEXT]        pin a new verbatim file from commit R at
                                                       Shared/SailTune/<UPSTREAM_PATH> (repeatable; not limited
                                                       by --only)

Each entry may carry its own "upstream_commit"; the top-level one is the default for entries without it.
Mode "local" marks a shared file authored in SailTube (no upstream copy yet); only its pin is checked.
Patched and extracted files are never overwritten automatically: drift in their upstream
source is reported so a person can re-apply the documented patch (docs/SHARING.md).
Exit codes: 0 clean, 1 drift or mismatch found, 2 usage error.
"""
import argparse, hashlib, json, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "Shared" / "SHARED_MANIFEST.json"


def digest(data):
    return hashlib.sha256(data).hexdigest()


def git(upstream, *args):
    return subprocess.check_output(["git", "-C", str(upstream)] + list(args))


def upstream_bytes(upstream, ref, path):
    """Bytes of path in the upstream checkout (working tree when ref is None), or None if absent."""
    if ref is None:
        src = upstream / path
        return src.read_bytes() if src.is_file() else None
    try:
        return git(upstream, "show", "%s:%s" % (ref, path))
    except subprocess.CalledProcessError:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--upstream")
    ap.add_argument("--refresh-verbatim")
    ap.add_argument("--ref", help="compare against this upstream commit instead of the working tree")
    ap.add_argument("--pinned", action="store_true", help="compare each entry against its own upstream_commit")
    ap.add_argument("--only", help="limit to manifest paths starting with this prefix")
    ap.add_argument("--restamp", action="store_true",
                    help="with --refresh-verbatim --ref: record the commit for unchanged verbatim entries in scope too")
    ap.add_argument("--add", action="append", default=[], metavar="UPSTREAM_PATH",
                    help="with --refresh-verbatim --ref: pin this upstream file as a new verbatim entry")
    ap.add_argument("--note", default="", help="note of the entries created by --add")
    a = ap.parse_args()
    if not (a.check or a.refresh_verbatim) or (a.ref and a.pinned):
        ap.print_usage(); return 2
    if (a.restamp or a.add) and not (a.refresh_verbatim and a.ref):
        ap.print_usage(); return 2
    data = json.loads(MANIFEST.read_text())
    bad = 0
    upstream = Path(a.upstream or a.refresh_verbatim) if (a.upstream or a.refresh_verbatim) else None
    if (a.ref or a.pinned) and upstream is None:
        ap.print_usage(); return 2
    resolved = None
    if upstream is not None and a.ref:
        resolved = git(upstream, "rev-parse", a.ref).decode().strip()
    for e in data["files"]:
        if a.only and not e["path"].startswith(a.only):
            continue
        local = ROOT / e["path"]
        if not local.is_file():
            print("MISSING  %s" % e["path"]); bad += 1; continue
        if digest(local.read_bytes()) != e["local_sha256"]:
            print("EDITED   %s (local copy differs from its pin; edit upstream or re-pin deliberately)" % e["path"]); bad += 1
        if upstream is None or e["mode"] == "local":
            # "local": authored here for both apps (no upstream copy yet).
            continue
        ref = resolved
        if a.pinned:
            ref = e.get("upstream_commit", data["upstream_commit"])
        src = upstream_bytes(upstream, ref, e["upstream_path"])
        if src is None:
            print("GONE     %s (upstream %s no longer exists)" % (e["path"], e["upstream_path"])); bad += 1; continue
        if digest(src) == e["upstream_sha256"]:
            # Unchanged at this commit: with --restamp the entry is recorded against it.
            if a.restamp and e["mode"] == "verbatim" and e.get("upstream_commit") != resolved:
                e["upstream_commit"] = resolved
                print("RESTAMP  %s" % e["path"])
            continue
        if a.refresh_verbatim and e["mode"] == "verbatim":
            local.write_bytes(src)
            e["upstream_sha256"] = e["local_sha256"] = digest(src)
            if resolved:
                e["upstream_commit"] = resolved
            print("UPDATED  %s" % e["path"])
        else:
            print("DRIFT    %s (%s upstream changed: %s)" % (e["path"], e["mode"], e["upstream_path"])); bad += 1
    for upstream_path in a.add:
        # A new verbatim pin, with SailTune's own relative layout under Shared/SailTune/.
        rel = "Shared/SailTune/" + upstream_path
        local = ROOT / rel
        parts = Path(upstream_path).parts
        if Path(upstream_path).is_absolute() or ".." in parts or not parts:
            print("REFUSED  %s (not a path inside the upstream tree)" % upstream_path); bad += 1; continue
        if any(e["path"] == rel for e in data["files"]) or local.exists():
            print("EXISTS   %s (already pinned or present; use --refresh-verbatim without --add)" % rel); bad += 1; continue
        src = upstream_bytes(upstream, resolved, upstream_path)
        if src is None:
            print("GONE     %s (upstream %s does not exist at %s)" % (rel, upstream_path, a.ref)); bad += 1; continue
        local.parent.mkdir(parents=True, exist_ok=True)
        local.write_bytes(src)
        data["files"].append({"path": rel, "upstream_path": upstream_path, "mode": "verbatim", "note": a.note,
                              "upstream_commit": resolved, "upstream_sha256": digest(src), "local_sha256": digest(src)})
        print("ADDED    %s" % rel)
    if a.refresh_verbatim:
        if not resolved and not a.only:
            try:
                data["upstream_commit"] = git(upstream, "rev-parse", "HEAD").decode().strip()
            except (OSError, subprocess.CalledProcessError):
                pass
        MANIFEST.write_text(json.dumps(data, indent=2) + "\n")
    print("shared snapshot: %s" % ("clean" if not bad else "%d problem(s)" % bad))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
