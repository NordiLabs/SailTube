#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""SailTube's static network inventory (NFR-SEC-01): SailTune's checker, pinned verbatim at
Shared/SailTune/tests/privacy/check_network_inventory.py, run with SailTube's scan roots, build file
and allowlist (tests/privacy/network_allowlist.json). Arguments are the checker's own
(--root, --allowlist, --list, --strict), plus --variant store: the STORE (Harbour) run, where hosts
marked "variant": "FULL" in the allowlist are removed, so any code naming them fails.
Shipped code only: test trees under Shared/ are skipped."""
import importlib.util
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
PINNED = os.path.join(ROOT, "Shared", "SailTune", "tests", "privacy", "check_network_inventory.py")

spec = importlib.util.spec_from_file_location("check_network_inventory", PINNED)
inv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inv)

inv.SCAN_DIRS = ("Core", "Clients", "Shared")
inv.BUILD_FILES = ("sailtube.pro",)
_iter_files = inv.iter_files
inv.iter_files = lambda root: (f for f in _iter_files(root) if "/tests/" not in "/" + f[0])


def option(argv, name, default):
    """Removes --name VALUE / --name=VALUE from argv and returns the value."""
    for i, a in enumerate(argv):
        if a == name and i + 1 < len(argv):
            value = argv[i + 1]; del argv[i:i + 2]; return value
        if a.startswith(name + "="):
            del argv[i]; return a.split("=", 1)[1]
    return default


def main(argv=None):
    argv = list(sys.argv[1:] if argv is None else argv)
    root = option(argv, "--root", ROOT)
    allowlist = option(argv, "--allowlist", os.path.join(HERE, "network_allowlist.json"))
    variant = option(argv, "--variant", "full")
    if variant not in ("full", "store"):
        print("unknown --variant %r (full or store)" % variant, file=sys.stderr); return 2
    if variant == "full":
        return inv.main(argv + ["--root", root, "--allowlist", allowlist])
    with open(allowlist) as handle:
        allow = json.load(handle)
    allow["hosts"] = {h: e for h, e in allow.get("hosts", {}).items() if e.get("variant") != "FULL"}
    # Sources compiled only in `!sailtube_store { }` blocks are not in the STORE binary.
    spec_rd = importlib.util.spec_from_file_location("check_runtime_downloads", os.path.join(ROOT, "tools", "check_runtime_downloads.py"))
    rd = importlib.util.module_from_spec(spec_rd)
    spec_rd.loader.exec_module(rd)
    full_only = rd.store_excluded_sources(root)
    store_iter = inv.iter_files
    inv.iter_files = lambda r: (f for f in store_iter(r) if f[0] not in full_only)
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as handle:
        json.dump(allow, handle)
    try:
        print("STORE variant: FULL-only hosts removed from the allowlist")
        # Stale checks belong to the full run (a FULL host's files are not stale on STORE).
        return inv.main([a for a in argv if a != "--strict"] + ["--root", root, "--allowlist", handle.name])
    finally:
        os.unlink(handle.name)


if __name__ == "__main__":
    sys.exit(main())
