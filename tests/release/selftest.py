#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""NFR-MNT-03 self-test of tools/release_check.py. Python 3.9 stdlib; run with `python3 -I`.

    selftest.py SCRATCH_DIR

  version  the real tree passes; a copy whose sailtube.pro disagrees fails.
  tag      in a temp git repo: matching tag on main passes as final (lightweight and annotated);
           a mismatching tag fails; a tag on a side-branch commit fails; v<ver>-rc1 is a
           prerelease; origin/main is used when there is no local main. Skipped without git.
  payload  synthetic newc archives: identical pass; a changed byte, a missing file, a changed
           mode and a changed symlink target fail; hard links share one digest. The same
           archives wrapped in minimal synthetic RPMs (gzip, xz) through the CLI.
"""
import gzip
import importlib.util
import io
import lzma
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
TOOL = os.path.join(ROOT, "tools", "release_check.py")

_spec = importlib.util.spec_from_file_location("release_check", TOOL)
rc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rc)

FAILURES = []


def check(cond, what):
    print("  %s: %s" % ("ok" if cond else "FAIL", what))
    if not cond:
        FAILURES.append(what)


def run_tool(*args, cwd=None, env=None):
    r = subprocess.run([sys.executable, "-I", TOOL] + list(args), stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, universal_newlines=True, cwd=cwd, env=env)
    return r.returncode, r.stdout


# ---------------------------------------------------------------------------- version


def test_version(scratch):
    print("version:")
    code, out = run_tool("version")
    spec_version = out.strip()
    check(code == 0 and rc.VERSION_SHAPE.match(spec_version), "real tree agrees (%s)" % spec_version)
    copy = os.path.join(scratch, "tree")
    os.makedirs(os.path.join(copy, "rpm"))
    shutil.copy(os.path.join(ROOT, rc.SPEC_PATH), os.path.join(copy, rc.SPEC_PATH))
    with open(os.path.join(ROOT, rc.PRO_PATH), encoding="utf-8") as fh:
        pro = fh.read()
    with open(os.path.join(copy, rc.PRO_PATH), "w", encoding="utf-8") as fh:
        fh.write(rc.PRO_VERSION.sub("VERSION = 9.9.9", pro))
    code, out = run_tool("--root", copy, "version")
    check(code == 1 and "9.9.9" in out, "mismatching sailtube.pro VERSION fails")
    os.remove(os.path.join(copy, rc.PRO_PATH))
    code, _ = run_tool("--root", copy, "version")
    check(code == 1, "missing sailtube.pro fails")
    return spec_version


# ---------------------------------------------------------------------------- tag


def test_tag(scratch, version):
    print("tag:")
    git = shutil.which("git")
    if not git:
        print("  skipped (no git)")
        return
    home = os.path.join(scratch, "home")
    os.makedirs(home)
    env = dict(os.environ, HOME=home, GIT_CONFIG_NOSYSTEM="1", GIT_TERMINAL_PROMPT="0",
               GIT_AUTHOR_NAME="t", GIT_AUTHOR_EMAIL="t@example.invalid",
               GIT_COMMITTER_NAME="t", GIT_COMMITTER_EMAIL="t@example.invalid")
    env.pop("GIT_DIR", None)
    env.pop("GIT_WORK_TREE", None)
    repo = os.path.join(scratch, "repo")
    os.makedirs(os.path.join(repo, "rpm"))

    def g(*args):
        subprocess.run([git, "-C", repo, "-c", "commit.gpgsign=false", "-c", "tag.gpgsign=false"]
                       + list(args), check=True, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def write(v):
        with open(os.path.join(ROOT, rc.SPEC_PATH), encoding="utf-8") as fh:
            spec = fh.read()
        with open(os.path.join(repo, rc.SPEC_PATH), "w", encoding="utf-8") as fh:
            fh.write(rc.SPEC_VERSION.sub("Version:    " + v, spec))
        with open(os.path.join(repo, rc.PRO_PATH), "w", encoding="utf-8") as fh:
            fh.write("TARGET = harbour-sailtube\nVERSION = %s\n" % v)

    def tag(name, *extra):
        r = subprocess.run([sys.executable, "-I", TOOL, "tag", name, "--repo", repo] + list(extra),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True, env=env)
        return r.returncode, r.stdout

    g("init", "-q")
    g("symbolic-ref", "HEAD", "refs/heads/main")
    write(version)
    g("add", "-A")
    g("commit", "-q", "-m", "release")
    g("tag", "v" + version)
    g("tag", "-a", "-m", "annotated", "v%s-rc1" % version)
    g("tag", "v0.0.1")
    g("checkout", "-q", "-b", "side")
    write(version)
    with open(os.path.join(repo, "side.txt"), "w") as fh:
        fh.write("side\n")
    g("add", "-A")
    g("commit", "-q", "-m", "side work")
    g("tag", "side-v" + version)  # not a version tag shape
    g("tag", "v%s-side" % version)
    g("checkout", "-q", "main")

    code, out = tag("v" + version)
    check(code == 0 and "release: final" in out, "matching tag on main passes as final")
    code, out = tag("refs/tags/v" + version)
    check(code == 0 and "release: final" in out, "refs/tags/ prefix accepted")
    code, out = tag("v%s-rc1" % version)
    check(code == 0 and "release: prerelease" in out, "annotated v%s-rc1 is a prerelease" % version)
    code, out = tag("v0.0.1")
    check(code == 1 and "0.0.1" in out and "release:" not in out, "tag disagreeing with the spec fails")
    code, out = tag("v%s-side" % version)
    check(code == 1 and "not on main" in out, "tag on a side-branch commit fails")
    code, out = tag("side-v" + version)
    check(code == 1 and "not v<version>" in out, "malformed tag name fails")
    code, out = tag("v9.9.9")
    check(code == 1 and "does not exist" in out, "absent tag fails")
    code, out = tag("v" + version, "--main-ref", "nosuchbranch")
    check(code == 1 and "does not resolve" in out, "unresolvable main ref fails")
    # A CI clone often has only origin/main: rename the local branch into a remote-tracking ref.
    g("checkout", "-q", "--detach")
    g("update-ref", "refs/remotes/origin/main", "refs/heads/main")
    g("branch", "-q", "-D", "main")
    code, out = tag("v" + version)
    check(code == 0 and "origin/main" in out and "release: final" in out, "falls back to origin/main")
    code, out = tag("v%s-side" % version)
    check(code == 1 and "not on origin/main" in out, "side commit still fails against origin/main")


# ---------------------------------------------------------------------------- payload


def newc(members):
    """Build a newc cpio archive. members: (name, mode, data, ino, nlink)."""
    out = io.BytesIO()

    def put(name, mode, data, ino, nlink):
        nb = name.encode() + b"\0"
        fields = [ino, mode, 0, 0, nlink, 1234567890, len(data), 8, 1, 0, 0, len(nb), 0]
        head = b"070701" + b"".join(b"%08X" % f for f in fields)
        out.write(head + nb)
        out.write(b"\0" * ((4 - (len(head) + len(nb)) % 4) % 4))
        out.write(data)
        out.write(b"\0" * ((4 - len(data) % 4) % 4))

    for m in members:
        put(*m)
    put("TRAILER!!!", 0, b"", 0, 1)
    return out.getvalue()


BASE = [
    ("./usr", 0o040755, b"", 1, 2),
    ("./usr/bin/harbour-sailtube", 0o100755, b"\x7fELF" + bytes(range(256)) * 9, 2, 1),
    ("./usr/share/harbour-sailtube/qml/main.qml", 0o100644, b"import QtQuick 2.0\n", 3, 1),
    ("./usr/share/harbour-sailtube/a.txt", 0o100644, b"", 4, 2),  # hard link, data on last
    ("./usr/share/harbour-sailtube/b.txt", 0o100644, b"linked\n", 4, 2),
    ("./usr/share/harbour-sailtube/link", 0o120777, b"qml/main.qml", 5, 1),
]


def variant(index, field, value):
    members = [list(m) for m in BASE]
    members[index][field] = value
    return [tuple(m) for m in members]


def rpm_header(tags):
    """A minimal RPM header structure with STRING tags."""
    index, store = b"", b""
    for tag, text in tags:
        index += struct.pack(">IIII", tag, 6, len(store), 1)
        store += text.encode() + b"\0"
    return b"\x8e\xad\xe8\x01\0\0\0\0" + struct.pack(">II", len(tags), len(store)) + index + store


def synthetic_rpm(path, cpio, compressor):
    lead = b"\xed\xab\xee\xdb\x03\x00" + bytes(90)
    sig = rpm_header([(1000, "s")])  # 34 bytes: exercises the 8-byte signature padding
    sig += b"\0" * ((8 - len(sig) % 8) % 8)
    hdr = rpm_header([(1000, "harbour-sailtube"), (rc.RPMTAG_PAYLOADCOMPRESSOR, compressor)])
    payload = gzip.compress(cpio, mtime=0) if compressor == "gzip" else lzma.compress(cpio)
    with open(path, "wb") as fh:
        fh.write(lead + sig + hdr + payload)


def test_payload(scratch):
    print("payload:")
    a = rc.read_newc_bytes(newc(BASE))
    b = rc.read_newc_bytes(newc(BASE))
    check(rc.compare_entries(a, b) == [], "identical archives compare equal (%d entries)" % len(a))
    check(a["/usr/share/harbour-sailtube/a.txt"][1] == a["/usr/share/harbour-sailtube/b.txt"][1] != "",
          "hard-linked members share the content digest")
    check(a["/usr/share/harbour-sailtube/link"][1] == "link:qml/main.qml", "symlink target recorded")
    binary = bytearray(BASE[1][2])
    binary[100] ^= 1
    d = rc.compare_entries(a, rc.read_newc_bytes(newc(variant(1, 2, bytes(binary)))))
    check(d == ["differs: /usr/bin/harbour-sailtube (content)"], "one changed byte is reported: %s" % d)
    d = rc.compare_entries(a, rc.read_newc_bytes(newc(BASE[:2] + BASE[3:])))
    check(d == ["only in A: /usr/share/harbour-sailtube/qml/main.qml"], "missing file is reported: %s" % d)
    d = rc.compare_entries(a, rc.read_newc_bytes(newc(variant(2, 1, 0o100755))))
    check(len(d) == 1 and "mode" in d[0], "changed mode is reported")
    d = rc.compare_entries(a, rc.read_newc_bytes(newc(variant(5, 2, b"qml/other.qml"))))
    check(d == ["differs: /usr/share/harbour-sailtube/link (content)"], "changed symlink target is reported")
    d = rc.compare_entries(a, rc.read_newc_bytes(newc([(m[0], m[1], m[2], m[3] + 100, m[4]) for m in BASE])))
    check(d == [], "inode numbers and mtimes are ignored")
    try:
        rc.read_newc_bytes(newc(BASE)[:300])
        check(False, "truncated archive rejected")
    except rc.CheckError:
        check(True, "truncated archive rejected")

    paths = {}
    for name, members, comp in (("a-gz", BASE, "gzip"), ("b-xz", BASE, "xz"),
                                ("c-xz", variant(1, 2, bytes(binary)), "xz")):
        paths[name] = os.path.join(scratch, name + ".rpm")
        synthetic_rpm(paths[name], newc(members), comp)
    code, out = run_tool("payload", paths["a-gz"], paths["b-xz"])
    check(code == 0 and "identical" in out, "CLI: synthetic RPMs (gzip vs xz payload) identical")
    code, out = run_tool("payload", paths["a-gz"], paths["c-xz"])
    check(code == 1 and "differs: /usr/bin/harbour-sailtube" in out, "CLI: changed byte in an RPM fails")
    code, out = run_tool("payload", paths["a-gz"], os.path.join(ROOT, rc.PRO_PATH))
    check(code == 1 and "not an RPM" in out, "CLI: a non-RPM input fails")


def main():
    scratch = sys.argv[1]
    version = test_version(scratch)
    test_tag(scratch, version)
    test_payload(scratch)
    if FAILURES:
        print("%d failure(s)" % len(FAILURES))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
