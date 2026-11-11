# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Unit tests of Core/Extractor/Runtime/verify_release.py and the worker's verify task (NFR-MNT-01,
NFR-SEC-04). Python 3.9+, standard library only; runs on the Mac and in the build container.

  python3 -I tests/extractor_update/test_verify_release.py [-v]

XUPD_REAL_ARCHIVE (or the build container's /builds/_vendor/python-runtime/runtime/yt-dlp.pyz) adds the
full verification of the real bundled 2026.10.26 archive."""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RUNTIME = os.path.join(ROOT, "Core", "Extractor", "Runtime")
VERIFIER = os.path.join(RUNTIME, "verify_release.py")
WORKER = os.path.join(ROOT, "Core", "Worker", "Runtime", "worker.py")
REAL_KEY = os.path.join(RUNTIME, "yt-dlp-release-key.asc")
REAL = os.path.join(HERE, "fixtures", "real")
FAKE = os.path.join(HERE, "fixtures", "fake")
BUNDLED_SHA256 = "e51e69e89489f199530f13d32a4342da3ddb2cf34842f364603a066db05afbf3"
REAL_FINGERPRINT = "AC0CBBE6848D6A873464AF4E57CF65933B5A7581"

sys.path.insert(0, RUNTIME)
import verify_release as vr  # noqa: E402

TEST_KEY = os.path.join(FAKE, "test-key.asc")
OTHER_KEY = os.path.join(FAKE, "other-key.asc")


def readb(path):
    with open(path, "rb") as handle:
        return handle.read()


TEST_FPR = readb(os.path.join(FAKE, "test-key.fpr")).decode().strip()
OTHER_FPR = readb(os.path.join(FAKE, "other-key.fpr")).decode().strip()


def real_archive():
    for path in (os.environ.get("XUPD_REAL_ARCHIVE", ""), "/builds/_vendor/python-runtime/runtime/yt-dlp.pyz"):
        if path and os.path.isfile(path):
            return path
    return None


def release(name):
    d = os.path.join(FAKE, name)
    return os.path.join(d, "SHA2-256SUMS"), os.path.join(d, "SHA2-256SUMS.sig"), os.path.join(d, "yt-dlp")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="xupd-py.")
        self.markers = os.path.join(self.tmp, "markers")
        os.mkdir(self.markers)

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def copy(self, path, name=None):
        target = os.path.join(self.tmp, name or os.path.basename(path))
        shutil.copyfile(path, target)
        return target

    def write(self, name, data):
        target = os.path.join(self.tmp, name)
        with open(target, "wb") as f:
            f.write(data)
        return target

    def fake(self, name, key=TEST_KEY, fpr=TEST_FPR, **kw):
        sums, sig, archive = release(name)
        return vr.verify(key, sums, sig, archive, "yt-dlp", fingerprint=fpr, **kw)

    def refused(self, result, label):
        self.assertFalse(result["ok"], result)
        self.assertEqual(result["error"], label, result)

    def no_markers(self):
        self.assertEqual(os.listdir(self.markers), [], "the archive was imported or run")


class RealRelease(Base):
    def test_shipped_key_matches_pin(self):
        key = vr.load_key(readb(REAL_KEY), vr.PINNED_FINGERPRINT)
        self.assertEqual(key.fingerprint, REAL_FINGERPRINT)
        self.assertEqual(vr.PINNED_FINGERPRINT, REAL_FINGERPRINT)
        self.assertEqual(key.n.bit_length(), 4096)

    def test_shipped_key_refused_under_another_pin(self):
        with self.assertRaises(vr.Refused) as ctx:
            vr.load_key(readb(REAL_KEY), TEST_FPR)
        self.assertEqual(ctx.exception.label, "key-pin")

    def test_real_sums_signature_verifies_with_pinned_key(self):
        key = vr.load_key(readb(REAL_KEY), vr.PINNED_FINGERPRINT)
        sums = readb(os.path.join(REAL, "SHA2-256SUMS"))
        vr.verify_signature(key, sums, readb(os.path.join(REAL, "SHA2-256SUMS.sig")))
        self.assertEqual(vr.sums_entry(sums, "yt-dlp"), BUNDLED_SHA256)

    def test_real_sums_tampered_refused(self):
        key = vr.load_key(readb(REAL_KEY), vr.PINNED_FINGERPRINT)
        sums = readb(os.path.join(REAL, "SHA2-256SUMS")).replace(b"e51e69e8", b"e51e69e9", 1)
        with self.assertRaises(vr.Refused) as ctx:
            vr.verify_signature(key, sums, readb(os.path.join(REAL, "SHA2-256SUMS.sig")))
        self.assertEqual(ctx.exception.label, "signature")

    def test_real_signature_under_test_key_refused(self):
        key = vr.load_key(readb(TEST_KEY), TEST_FPR)
        sums = readb(os.path.join(REAL, "SHA2-256SUMS"))
        with self.assertRaises(vr.Refused) as ctx:
            vr.verify_signature(key, sums, readb(os.path.join(REAL, "SHA2-256SUMS.sig")))
        self.assertEqual(ctx.exception.label, "signature-issuer")

    def test_real_bundled_archive_verifies(self):
        archive = real_archive()
        if not archive:
            self.skipTest("the real bundled archive is not available here")
        result = vr.verify(REAL_KEY, os.path.join(REAL, "SHA2-256SUMS"), os.path.join(REAL, "SHA2-256SUMS.sig"),
                           archive, "yt-dlp", expect_version="2026.10.26")
        self.assertTrue(result["ok"], result)
        self.assertEqual(result["sha256"], BUNDLED_SHA256)
        self.assertEqual(result["version"], "2026.10.26")


class FakeReleases(Base):
    def test_stable_binary_sha256_signature(self):
        r = self.fake("stable-2026.11.01", expect_version="2026.11.01")
        self.assertTrue(r["ok"], r)
        self.assertEqual(r["version"], "2026.11.01")
        self.assertEqual(len(r["sha256"]), 64)
        self.assertEqual(r["size"], os.path.getsize(release("stable-2026.11.01")[2]))

    def test_nightly_armoured_sha512_signature(self):
        self.assertTrue(readb(release("nightly-2026.11.04.232659")[1]).startswith(b"-----BEGIN PGP SIGNATURE"))
        r = self.fake("nightly-2026.11.04.232659", expect_version="2026.11.04.232659")
        self.assertTrue(r["ok"], r)

    def test_version_mismatch_refused(self):
        self.refused(self.fake("mismatch-2026.11.03", expect_version="2026.11.03"), "version-mismatch")

    def test_wrong_key_refused(self):
        self.refused(self.fake("wrongkey-2026.11.01"), "signature-issuer")

    def test_other_key_pinned_but_signature_by_test_key_refused(self):
        self.refused(self.fake("stable-2026.11.01", key=OTHER_KEY, fpr=OTHER_FPR), "signature-issuer")

    def test_key_file_not_matching_pin_refused(self):
        self.refused(self.fake("stable-2026.11.01", key=OTHER_KEY, fpr=TEST_FPR), "key-pin")

    def test_eddsa_signature_refused(self):
        self.refused(self.fake("ed25519-2026.11.01"), "signature-algorithm")

    def test_tampered_sums_refused(self):
        sums, sig, archive = release("stable-2026.11.01")
        bad = self.write("SHA2-256SUMS", readb(sums).replace(b"yt-dlp.exe", b"yt-dlp.exf"))
        self.refused(vr.verify(TEST_KEY, bad, sig, archive, "yt-dlp", fingerprint=TEST_FPR), "signature")

    def test_bad_signature_value_refused(self):
        sums, sig, archive = release("stable-2026.11.01")
        data = bytearray(readb(sig))
        data[-5] ^= 0x01
        bad = self.write("SHA2-256SUMS.sig", bytes(data))
        self.refused(vr.verify(TEST_KEY, sums, bad, archive, "yt-dlp", fingerprint=TEST_FPR), "signature")

    def test_bad_left16_refused(self):
        sums, sig, archive = release("stable-2026.11.01")
        data = readb(sig)
        body = vr.packets(data, "x")[0][1]
        hashed = int.from_bytes(body[4:6], "big")
        unhashed = int.from_bytes(body[6 + hashed:8 + hashed], "big")
        at = len(data) - len(body) + 8 + hashed + unhashed
        tampered = bytearray(data)
        tampered[at] ^= 0xFF
        bad = self.write("SHA2-256SUMS.sig", bytes(tampered))
        self.refused(vr.verify(TEST_KEY, sums, bad, archive, "yt-dlp", fingerprint=TEST_FPR), "signature")

    def test_armour_crc_refused(self):
        sums, sig, archive = release("nightly-2026.11.04.232659")
        lines = readb(sig).split(b"\n")
        crc = [i for i, line in enumerate(lines) if line.startswith(b"=")]
        if not crc:
            self.skipTest("gpg wrote no armour checksum")
        lines[crc[0]] = b"=AAAA"
        bad = self.write("SHA2-256SUMS.sig", b"\n".join(lines))
        self.refused(vr.verify(TEST_KEY, sums, bad, archive, "yt-dlp", fingerprint=TEST_FPR), "signature-format")

    def test_missing_signature_refused(self):
        sums, _sig, archive = release("stable-2026.11.01")
        self.refused(vr.verify(TEST_KEY, sums, os.path.join(self.tmp, "absent.sig"), archive, "yt-dlp",
                               fingerprint=TEST_FPR), "signature-missing")
        empty = self.write("empty.sig", b"")
        self.refused(vr.verify(TEST_KEY, sums, empty, archive, "yt-dlp", fingerprint=TEST_FPR), "signature-missing")

    def test_two_signatures_refused(self):
        sums, sig, archive = release("stable-2026.11.01")
        double = self.write("double.sig", readb(sig) * 2)
        self.refused(vr.verify(TEST_KEY, sums, double, archive, "yt-dlp", fingerprint=TEST_FPR), "signature-format")

    def test_wrong_sha256_refused(self):
        sums, sig, archive = release("stable-2026.11.01")
        data = bytearray(readb(archive))
        data[len(data) // 2] ^= 0x01
        bad = self.write("yt-dlp", bytes(data))
        self.refused(vr.verify(TEST_KEY, sums, sig, bad, "yt-dlp", fingerprint=TEST_FPR), "checksum")

    def test_other_release_archive_refused(self):
        sums, sig, _ = release("stable-2026.11.01")
        other = release("stable-2026.11.20")[2]
        self.refused(vr.verify(TEST_KEY, sums, sig, other, "yt-dlp", fingerprint=TEST_FPR), "checksum")

    def test_asset_name_exact(self):
        sums, sig, archive = release("stable-2026.11.01")
        self.refused(vr.verify(TEST_KEY, sums, sig, archive, "yt-dlp_macos", fingerprint=TEST_FPR), "sums-missing-asset")
        self.refused(vr.verify(TEST_KEY, sums, sig, archive, "../yt-dlp", fingerprint=TEST_FPR), "usage")
        self.assertEqual(vr.sums_entry(b"%s  yt-dlp_linux\n%s  yt-dlp\n" % (b"a" * 64, b"B" * 64), "yt-dlp"), "b" * 64)
        with self.assertRaises(vr.Refused) as ctx:
            vr.sums_entry(b"%s  yt-dlp\n%s *yt-dlp\n" % (b"a" * 64, b"b" * 64), "yt-dlp")
        self.assertEqual(ctx.exception.label, "sums-format")
        with self.assertRaises(vr.Refused) as ctx:
            vr.sums_entry(b"not-a-hash  yt-dlp\n", "yt-dlp")
        self.assertEqual(ctx.exception.label, "sums-format")

    def test_dotdot_entry_refused(self):
        self.refused(self.fake("dotdot-2026.11.02"), "archive-entry")
        self.no_markers()

    def test_absolute_entry_refused(self):
        self.refused(self.fake("absolute-2026.11.02"), "archive-entry")

    def test_missing_version_file_refused(self):
        self.refused(self.fake("noversion-2026.11.02"), "archive-content")

    def test_oversize_refused(self):
        size = os.path.getsize(release("stable-2026.11.01")[2])
        self.refused(self.fake("stable-2026.11.01", max_size=size - 1), "archive-size")
        self.assertTrue(self.fake("stable-2026.11.01", max_size=size)["ok"])

    def test_entry_names(self):
        for bad in ("/abs", "../x", "a/../b", "a/./b", "./a", "C:/x", "a\\b", "a//b", "a\x00b", ""):
            self.assertFalse(vr._plain_name(bad), bad)
        for good in ("yt_dlp/__init__.py", "yt_dlp/", "__main__.py", "a.b/c..d"):
            self.assertTrue(vr._plain_name(good), good)

    def crafted(self, mutate):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as z:
            z.writestr("yt_dlp/__init__.py", "")
            z.writestr("yt_dlp/version.py", "__version__ = '2026.11.01'\n")
            mutate(z)
        return self.write("crafted.zip", buffer.getvalue())

    def label(self, path):
        try:
            vr.archive_version(path)
        except vr.Refused as refused:
            return refused.label
        return None

    def test_symlink_entry_refused(self):
        def add(z):
            info = zipfile.ZipInfo("yt_dlp/link.py")
            info.create_system = 3
            info.external_attr = 0o120777 << 16
            z.writestr(info, "/etc/passwd")
        self.assertEqual(self.label(self.crafted(add)), "archive-entry")

    def test_encrypted_entry_refused(self):
        path = self.crafted(lambda z: z.writestr("yt_dlp/secret.py", "x"))
        data = bytearray(readb(path))
        # zipfile cannot write encrypted entries: set the "encrypted" flag of the entry in place
        # (central directory, offset 8 of the record; local header, offset 6).
        for signature, flag_offset in ((b"PK\x01\x02", 8), (b"PK\x03\x04", 6)):
            at = data.find(signature)
            while at >= 0:
                name_at = at + (46 if signature == b"PK\x01\x02" else 30)
                if data[name_at:name_at + 16] == b"yt_dlp/secret.py":
                    data[at + flag_offset] |= 0x01
                at = data.find(signature, at + 4)
        self.write("crafted.zip", bytes(data))
        self.assertEqual(self.label(path), "archive-entry")

    def test_duplicate_entry_refused(self):
        def add(z):
            import warnings
            with warnings.catch_warnings():
                warnings.simplefilter("ignore")
                z.writestr("yt_dlp/__init__.py", "again")
        self.assertEqual(self.label(self.crafted(add)), "archive-entry")

    def test_version_not_a_literal_refused(self):
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as z:
            z.writestr("yt_dlp/__init__.py", "")
            z.writestr("yt_dlp/version.py", "import os\n__version__ = os.environ['X']\n")
        self.assertEqual(self.label(self.write("v.zip", buffer.getvalue())), "version")
        buffer = io.BytesIO()
        with zipfile.ZipFile(buffer, "w") as z:
            z.writestr("yt_dlp/__init__.py", "")
            z.writestr("yt_dlp/version.py", "__version__ = 'latest'\n")
        self.assertEqual(self.label(self.write("w.zip", buffer.getvalue())), "version")

    def test_not_a_zip_refused(self):
        self.assertEqual(self.label(self.write("garbage", b"#!/bin/sh\n" + b"x" * 4096)), "archive-format")


class NeverRuns(Base):
    def run_cli(self, *args):
        env = dict(os.environ, XUPD_MARKER_DIR=self.markers)
        proc = subprocess.run([sys.executable, "-I", VERIFIER] + list(args), env=env, cwd=self.tmp,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        lines = proc.stdout.decode().strip().splitlines()
        self.assertEqual(len(lines), 1, proc.stdout)
        return proc.returncode, json.loads(lines[0])

    def test_cli_happy_path_never_imports_archive(self):
        sums, sig, archive = release("stable-2026.11.01")
        code, result = self.run_cli("--key", TEST_KEY, "--fingerprint", TEST_FPR, "--sums", sums, "--sig", sig,
                                    "--archive", archive, "--asset", "yt-dlp", "--expect-version", "2026.11.01")
        self.assertEqual(code, 0)
        self.assertTrue(result["ok"])
        self.assertEqual(sorted(result), ["error", "ok", "sha256", "size", "version"])
        self.no_markers()
        # Positive control: the markers do work when the archive really is imported.
        subprocess.run([sys.executable, "-I", "-c", "import sys; sys.path.insert(0, sys.argv[1]); import yt_dlp.version", archive],
                       env=dict(os.environ, XUPD_MARKER_DIR=self.markers), check=True, timeout=60)
        self.assertEqual(sorted(os.listdir(self.markers)), ["imported-init", "imported-version"])

    def test_cli_refusal_exit_code(self):
        sums, sig, archive = release("dotdot-2026.11.02")
        code, result = self.run_cli("--key", TEST_KEY, "--fingerprint", TEST_FPR, "--sums", sums, "--sig", sig,
                                    "--archive", archive, "--asset", "yt-dlp")
        self.assertEqual(code, 1)
        self.assertEqual(result["error"], "archive-entry")
        self.no_markers()

    def test_cli_usage(self):
        code, result = self.run_cli("--key", TEST_KEY)
        self.assertEqual((code, result["error"]), (1, "usage"))

    def worker_task(self, archive, with_verifier=True):
        runtime = tempfile.mkdtemp(prefix="runtime.", dir=self.tmp)
        shutil.copyfile(WORKER, os.path.join(runtime, "worker.py"))
        shutil.copyfile(REAL_KEY, os.path.join(runtime, "yt-dlp-release-key.asc"))
        if with_verifier:
            shutil.copyfile(VERIFIER, os.path.join(runtime, "verify_release.py"))
        # No resolver.py and no yt-dlp.pyz here: the verify task must not need either.
        env = dict(os.environ, XUPD_MARKER_DIR=self.markers, NORDTILE_WORKER_TASK="verify",
                   NORDTILE_VERIFY_SUMS=os.path.join(REAL, "SHA2-256SUMS"),
                   NORDTILE_VERIFY_SIG=os.path.join(REAL, "SHA2-256SUMS.sig"),
                   NORDTILE_VERIFY_ARCHIVE=archive, NORDTILE_VERIFY_ASSET="yt-dlp")
        proc = subprocess.run([sys.executable, "-I", os.path.join(runtime, "worker.py")], env=env, cwd=runtime,
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
        lines = proc.stdout.decode().strip().splitlines()
        self.assertEqual(len(lines), 1, (proc.stdout, proc.stderr))
        self.no_markers()
        return proc.returncode, json.loads(lines[0])

    def test_worker_task_uses_installed_key(self):
        archive = real_archive()
        if archive:
            code, result = self.worker_task(archive)
            self.assertEqual(code, 0, result)
            self.assertEqual((result["version"], result["sha256"]), ("2026.10.26", BUNDLED_SHA256))
        # A fake archive against the real signed sums: the signature verifies, the hash does not.
        code, result = self.worker_task(release("stable-2026.11.01")[2])
        self.assertEqual((code, result["error"]), (1, "checksum"), result)

    def test_worker_task_without_verifier(self):
        code, result = self.worker_task(release("stable-2026.11.01")[2], with_verifier=False)
        self.assertEqual((code, result["error"]), (2, "verifier-unavailable"))


if __name__ == "__main__":
    unittest.main()
