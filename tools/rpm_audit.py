#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""RPM content gates, run on every built RPM by tools/rpm_build.sh (inside the build container).

  rpm_audit.py --rpm FILE [--release]                 inspect a real RPM (needs the rpm tool)
  rpm_audit.py --requires FILE --dump FILE [--release]  same checks on saved `rpm -qp --requires`
                                                        and `rpm -qp --dump` output (tests)
  rpm_audit.py --probe-floor LIB...                   ceilings defined by target libraries; fails
                                                        if packaging/platform_floor.json claims more

Checks:
  glibc       PLT-02: no requirement names a GLIBC_/GLIBCXX_/CXXABI_ version above the ceilings
              of the oldest supported target (packaging/platform_floor.json).
  components  NFR-MNT-02: every payload file is app-owned or belongs to a component of
              packaging/components.json, and each component file has the listed size and SHA-256.
  licences    NFR-SEC-06: every component has a licence and a review state; with --release an
              incomplete review fails (a development build only warns).
  helpers     NFR-SEC-06: no runtime helper program (Deno, Node, ffmpeg, PO-token provider...)
              is shipped inside the RPM.
Options --floor and --components override the default files. Exit: 0 ok, 1 a check failed,
2 usage or tool error.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SYMBOL_RE = re.compile(r"\((GLIBC|GLIBCXX|CXXABI)_([0-9][0-9.]*)\)")


def version_key(text):
    return tuple(int(p) for p in text.split(".") if p.isdigit())


def load(path):
    with open(path) as handle:
        return json.load(handle)


def check_glibc(requires, floor):
    errors, highest = [], {}
    ceilings = floor["ceilings"]
    for line in requires:
        for family, version in SYMBOL_RE.findall(line):
            if version_key(version) > version_key(highest.get(family, "0")):
                highest[family] = version
            limit = ceilings.get(family)
            if limit is None:
                errors.append("glibc: no ceiling for %s in the floor file (%s)" % (family, line.strip()))
            elif version_key(version) > version_key(limit):
                errors.append("glibc: %s needs %s_%s, above the %s ceiling %s of %s" % (
                    line.strip(), family, version, family, limit, floor.get("oldest_supported_os", "the target")))
    summary = ", ".join("%s_%s (ceiling %s)" % (f, v, ceilings.get(f, "?")) for f, v in sorted(highest.items()))
    return errors, "glibc: highest required " + (summary or "none")


def parse_dump(lines):
    """`rpm -qp --dump`: path size mtime digest mode owner group isconfig isdoc rdev symlink."""
    files = {}
    for line in lines:
        parts = line.split()
        if len(parts) < 5:
            continue
        mode = int(parts[4], 8)
        files[parts[0]] = {"size": int(parts[1]), "sha256": parts[3], "dir": (mode & 0o170000) == 0o040000,
                           "exec": bool(mode & 0o111) and (mode & 0o170000) == 0o100000}
    return files


def owned(path, patterns):
    for pattern in patterns:
        if pattern.endswith("/**") and path.startswith(pattern[:-2]):
            return True
        if fnmatch.fnmatchcase(path, pattern):
            return True
    return False


def check_components(files, manifest):
    errors = []
    listed = {}
    for comp in manifest["components"]:
        for f in comp.get("files", []):
            listed[f["path"]] = (comp, f)
    for path, info in sorted(files.items()):
        if info["dir"] or owned(path, manifest.get("app_owned", [])):
            continue
        if path not in listed:
            errors.append("components: shipped file %s is neither app-owned nor in packaging/components.json" % path)
            continue
        comp, f = listed[path]
        if info["size"] != f["size"] or info["sha256"] != f["sha256"]:
            errors.append("components: %s (%s %s) ships %d bytes sha256 %s, manifest says %d bytes %s" % (
                path, comp["name"], comp["version"], info["size"], info["sha256"], f["size"], f["sha256"]))
    for path, (comp, _) in sorted(listed.items()):
        if path not in files:
            errors.append("components: %s of %s is in the manifest but not in the RPM" % (path, comp["name"]))
    for comp in manifest["components"]:
        for key in ("name", "version", "files"):
            if not comp.get(key):
                errors.append("components: an entry lacks %r" % key)
    return errors


def check_licences(manifest, release):
    errors, warnings = [], []
    for comp in manifest["components"]:
        name = comp.get("name", "?")
        if not str(comp.get("licence", "")).strip():
            errors.append("licences: %s has no licence" % name)
        review = comp.get("review")
        if not isinstance(review, dict) or not isinstance(review.get("complete"), bool):
            errors.append("licences: %s has no review state (review.complete true/false)" % name)
        elif not review["complete"]:
            (errors if release else warnings).append("licences: %s licence review is not complete (%s)" % (
                name, review.get("note", "")))
    return errors, warnings


def check_helpers(files, manifest):
    errors = []
    names = set(n.lower() for n in manifest.get("forbidden_helpers", []))
    for path, info in sorted(files.items()):
        base = os.path.basename(path).lower()
        stem = base.split(".")[0]
        if base in names or stem in names:
            errors.append("helpers: runtime helper %s is inside the RPM (helpers stay separate programs)" % path)
        elif info["exec"] and path not in ("/usr/bin/harbour-sailtube",) and not path.endswith(".so") \
                and ".so." not in base:
            errors.append("helpers: unexpected executable %s in the RPM" % path)
    return errors


def rpm_query(rpm, *args):
    try:
        return subprocess.check_output(["rpm", "-qp"] + list(args) + [rpm], universal_newlines=True).splitlines()
    except (OSError, subprocess.CalledProcessError) as exc:
        raise SystemExit("rpm_audit: cannot query %s: %s" % (rpm, exc))


def defined_versions(lib):
    out = subprocess.check_output(["readelf", "-V", lib], universal_newlines=True)
    found = {}
    for family, version in re.findall(r"Name: (GLIBC|GLIBCXX|CXXABI)_([0-9][0-9.]*)", out):
        if version_key(version) > version_key(found.get(family, "0")):
            found[family] = version
    return found


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rpm")
    ap.add_argument("--requires")
    ap.add_argument("--dump")
    ap.add_argument("--release", action="store_true", help="an incomplete licence review fails")
    ap.add_argument("--floor", default=os.path.join(ROOT, "packaging", "platform_floor.json"))
    ap.add_argument("--components", default=os.path.join(ROOT, "packaging", "components.json"))
    ap.add_argument("--probe-floor", nargs="+", metavar="LIB")
    a = ap.parse_args(argv)
    floor = load(a.floor)
    if a.probe_floor:
        provided = {}
        for lib in a.probe_floor:
            for family, version in defined_versions(lib).items():
                if version_key(version) > version_key(provided.get(family, "0")):
                    provided[family] = version
        bad = 0
        for family, limit in sorted(floor["ceilings"].items()):
            have = provided.get(family)
            print("floor: %s ceiling %s, build target defines up to %s" % (family, limit, have))
            if have is None or version_key(limit) > version_key(have):
                print("ERROR: the floor claims %s_%s but this target does not define it" % (family, limit)); bad += 1
        return 1 if bad else 0
    if a.rpm:
        requires, dump = rpm_query(a.rpm, "--requires"), rpm_query(a.rpm, "--dump")
    elif a.requires and a.dump:
        with open(a.requires) as r, open(a.dump) as d:
            requires, dump = r.read().splitlines(), d.read().splitlines()
    else:
        ap.print_usage(); return 2
    manifest = load(a.components)
    files = parse_dump(dump)
    errors, summary = check_glibc(requires, floor)
    print(summary)
    errors += check_components(files, manifest)
    lic_errors, warnings = check_licences(manifest, a.release)
    errors += lic_errors + check_helpers(files, manifest)
    for w in warnings:
        print("WARN: " + w)
    for e in errors:
        print("ERROR: " + e)
    print("rpm audit %s (%d file(s), %d component(s), %d warning(s))" % (
        "FAILED" if errors else "OK", len(files), len(manifest["components"]), len(warnings)))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
