# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""NFR-TST-01: the recorder's scrub rules, its refusal to save, and the committed fixtures.

Offline: imports tools/record_fixtures.py as a module and never calls its network functions.
"""
import base64, glob, gzip, importlib.util, json, os, re, sys, tempfile, unittest, urllib.parse

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
spec = importlib.util.spec_from_file_location("record_fixtures", os.path.join(ROOT, "tools", "record_fixtures.py"))
rf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(rf)


def visitor(country):
    inner = rf._encode([(1, 2, country.encode()), (2, 2, b"\x1a\x00 ]")])
    raw = rf._encode([(1, 2, b"ywxWOmjSajo"), (5, 0, rf._put_varint(1794000000)), (6, 2, inner)])
    return urllib.parse.quote(base64.urlsafe_b64encode(raw).decode(), safe="")


def decode_country(value):
    raw = urllib.parse.unquote(value)
    buf = base64.urlsafe_b64decode(raw + "=" * (-len(raw) % 4))
    for num, wt, payload in rf._fields(buf):
        if num == 6:
            for n2, w2, p2 in rf._fields(payload):
                if n2 == 1:
                    return p2.decode()
    return None


class ScrubRules(unittest.TestCase):
    def test_visitor_country(self):
        clean, country = rf.scrub_visitor(visitor("MX"))
        self.assertEqual(country, "MX")
        self.assertEqual(decode_country(clean), "ZZ")
        with self.assertRaises(ValueError):
            rf.scrub_visitor(urllib.parse.quote(base64.urlsafe_b64encode(rf._encode([(1, 2, b"abc")])).decode()))

    def test_ip_node_and_country_params(self):
        sc = rf.Scrubber()
        text = ('{"url":"https://rr3---sn-a5mekn6d.googlevideo.com/videoplayback?ip=198.51.100.7&id=1",'
                '"b":"https://rr1---sn-ab5l6nrz.googlevideo.com/x?mn=sn-a5mekn6d%2Csn-ab5l6nrz\\u0026ip=2001:db8::5\\u0026cr=MX",'
                '"c":"/videoplayback/ip/198.51.100.7/id/2","d":"seen at 198.51.100.7 earlier",'
                '"visitorData":"' + visitor("MX") + '"}')
        out = sc.text(text)
        self.assertNotIn("198.51.100.7", out)
        self.assertNotIn("2001:db8::5", out)
        self.assertEqual(out.count(rf.SAFE_IP), 4)
        self.assertNotIn("sn-a5mekn6d", out)
        self.assertIn("sn-node1", out)
        self.assertIn("sn-node2", out)
        self.assertNotIn("cr=MX", out)
        self.assertEqual(sc.check(out), [])

    def test_json_body_drops_tracking_and_location_shelves(self):
        sc = rf.Scrubber()
        doc = {"responseContext": {"visitorData": visitor("DE"), "serviceTrackingParams": [{"x": 1}]},
               "contents": [{"shelfRenderer": {"title": {"simpleText": "Local news"}, "items": [1]}},
                            {"videoRenderer": {"videoId": "abcdefghijk", "trackingParams": "AAAA"}}]}
        out = json.loads(sc.json_body(json.dumps(doc)))
        self.assertEqual(list(out["responseContext"].keys()), ["visitorData"])
        self.assertEqual(decode_country(out["responseContext"]["visitorData"]), "ZZ")
        self.assertNotIn("Local news", json.dumps(out))         # the location shelf is gone
        self.assertNotIn("trackingParams", out["contents"][1]["videoRenderer"])

    def test_refuses_to_save_when_scrub_fails(self):
        with tempfile.TemporaryDirectory() as d:
            rec = rf.Recorder(d, set(), False)
            original = rf.Scrubber.text
            try:
                rf.Scrubber.text = lambda self, s: s          # a scrub that silently does nothing
                payload = json.dumps({"visitorData": visitor("MX"), "u": "https://rr1---sn-ab5l6nrz.googlevideo.com/"})
                with self.assertRaises(SystemExit):
                    rec.save("leaky", 200, payload.encode(), {"endpoint": "test"})
            finally:
                rf.Scrubber.text = original
            self.assertEqual(os.listdir(d), [])

    def test_saves_clean_answers(self):
        with tempfile.TemporaryDirectory() as d:
            rec = rf.Recorder(d, set(), False)
            rec.save("clean", 200, json.dumps({"responseContext": {"visitorData": visitor("MX")}, "x": 1}).encode(), {})
            self.assertEqual(os.listdir(d), ["clean.json.gz"])
            body = gzip.open(os.path.join(d, "clean.json.gz")).read().decode()
            self.assertEqual(decode_country(json.loads(body)["responseContext"]["visitorData"]), "ZZ")


class CommittedFixtures(unittest.TestCase):
    def test_every_fixture_is_scrubbed(self):
        files = sorted(glob.glob(os.path.join(ROOT, "tests", "fixtures", "catalog", "*.gz")))
        self.assertGreaterEqual(len(files), 40)
        manifest = json.load(open(os.path.join(ROOT, "tests", "fixtures", "catalog", "MANIFEST.json")))
        for path in files:
            name = os.path.basename(path)
            self.assertIn(name, manifest["fixtures"], name)
            text = gzip.open(path).read().decode("utf-8")
            for value in re.findall(r"(?:[?&]|\\u0026|%26)ip(?:=|%3D)([0-9A-Fa-f:.]+)", text):
                self.assertEqual(value, rf.SAFE_IP, name)
            for value in re.findall(r"/ip/([0-9A-Fa-f:.]+)/", text):
                self.assertEqual(value, rf.SAFE_IP, name)
            self.assertIsNone(re.search(r"sn-(?!node\d)[a-z0-9]{4,}", text), name)
            for vd in re.findall(r'"visitorData"\s*:\s*"([^"]+)"', text):
                self.assertEqual(decode_country(vd), "ZZ", name)
            self.assertIsNone(re.search(r"(?:[?&]|\\u0026|%26)g?cr(?:=|%3D)(?!ZZ)[A-Z]{2}\b", text), name)


if __name__ == "__main__":
    result = unittest.main(exit=False, verbosity=1).result
    print("catalog_fixture_scrub: %d tests, %d failures, %d errors" % (result.testsRun, len(result.failures), len(result.errors)))
    sys.exit(0 if result.wasSuccessful() else 1)
