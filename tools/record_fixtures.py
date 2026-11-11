#!/usr/bin/env python3
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
"""Record discovery fixtures from YouTube's public endpoints (NFR-TST-01).

    tools/record_fixtures.py [--out tests/fixtures/catalog] [--only NAME[,NAME]] [--dry-run]
    tools/record_fixtures.py --locales es,pt,fr,it,ru,zh-CN,ar [--dry-run]

``--locales`` records only the per-locale set (I18N-04): ``search_<hl>_p1``, ``next_<hl>`` and
``channel_videos_<hl>`` for the same query, video and channel as the main set, with the region
of LOCALE_REGIONS as ``gl``. ``<hl>`` is the file-name-safe base language (zh-CN -> ``zh``).
Existing fixtures are not re-recorded; the new entries are merged into MANIFEST.json under
``fixtures`` and the run is described under ``locales``. Locale answers drop the subtrees no
parser reads (LOCALE_TRIM) to keep each file small.

Opt-in and manual: this is the only catalog tool that touches the network, it never runs from
tests/*/run.sh, and its output is reviewed before it is committed. Each answer is scrubbed before
it is saved and the tool refuses to save an answer whose scrub check fails:

  * googlevideo client IPs (``ip=``, ``/ip/<addr>/``) become ``203.0.113.1``;
  * ``sn-...`` cache node names are renamed in sequence (``sn-node1``, ``sn-node2``, ...);
  * the country inside ``visitorData`` (protobuf field 6.1) becomes ``ZZ``, and so does any
    ``cr=``/``gcr=`` URL parameter;
  * location-specific shelves (local news, "near you") are removed;
  * ``responseContext`` keeps only the scrubbed ``visitorData``; opaque tracking blobs
    (``trackingParams``, ``clickTrackingParams``, ``loggingContext``) are dropped.

After scrubbing, the serialized answer must not contain the recorder's own public address (taken
from the ``ip=`` parameters it scrubbed), its own country code in a visitor id or URL parameter, or
any ``sn-`` name outside the renamed sequence. Assertions in the tests check what the app needs
(counts, id shapes, tokens present), never exact values, so re-recording is always allowed.
"""
import argparse, base64, datetime, gzip, hashlib, json, os, re, sys, urllib.error, urllib.parse, urllib.request

UA = "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/130.0.0.0 Safari/537.36"
ORIGIN = "https://www.youtube.com"
SAFE_IP = "203.0.113.1"


# ---------------------------------------------------------------------------- transport
def http(url, data=None, headers=None, timeout=20):
    h = {"User-Agent": UA, "Accept-Encoding": "gzip"}
    h.update(headers or {})
    req = urllib.request.Request(url, data=data, headers=h)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as f:
            body = f.read()
            if f.headers.get("Content-Encoding") == "gzip":
                body = gzip.decompress(body)
            return f.status, body
    except urllib.error.HTTPError as e:
        body = e.read()
        if e.headers.get("Content-Encoding") == "gzip":
            body = gzip.decompress(body)
        return e.code, body


def scrape_client_version():
    """DSC-02: the WEB client version from the consent-free homepage."""
    status, body = http(ORIGIN + "/", headers={"Cookie": "SOCS=CAI", "Accept-Language": "en"})
    m = re.search(rb'"INNERTUBE_CLIENT_VERSION":"(\d+\.\d{8}[^"]*)"', body)
    if status != 200 or not m:
        sys.exit("could not scrape INNERTUBE_CLIENT_VERSION (HTTP %d)" % status)
    return m.group(1).decode()


# The JS-less player client of RES-03 (the playback component's fast path); captions come from its answer.
VISIONOS = {"clientName": "VISIONOS", "clientVersion": "1.02", "deviceMake": "Apple", "deviceModel": "RealityDevice17,1",
            "osName": "visionOS", "osVersion": "1.3.21O771", "hl": "en", "timeZone": "UTC", "utcOffsetMinutes": 0}
VISIONOS_UA = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15"


def visionos_post(endpoint, body, visitor=None):
    payload = dict(body)
    payload["context"] = {"client": VISIONOS}
    headers = {"Content-Type": "application/json", "Origin": ORIGIN, "User-Agent": VISIONOS_UA,
               "X-YouTube-Client-Name": "101", "X-YouTube-Client-Version": VISIONOS["clientVersion"]}
    if visitor:
        headers["X-Goog-Visitor-Id"] = visitor
    return http("%s/youtubei/v1/%s?prettyPrint=false" % (ORIGIN, endpoint), json.dumps(payload).encode(), headers)


class InnerTube:
    def __init__(self, version):
        self.version = version

    def post(self, endpoint, body, hl, gl):
        payload = dict(body)
        payload["context"] = {"client": {"clientName": "WEB", "clientVersion": self.version, "hl": hl, "gl": gl}}
        status, raw = http("%s/youtubei/v1/%s?prettyPrint=false" % (ORIGIN, endpoint), json.dumps(payload).encode(), {
            "Content-Type": "application/json", "Origin": ORIGIN,
            "X-YouTube-Client-Name": "1", "X-YouTube-Client-Version": self.version})
        return status, raw


# ---------------------------------------------------------------------------- scrub
def _varint(buf, i):
    shift = value = 0
    while True:
        b = buf[i]; i += 1
        value |= (b & 0x7F) << shift
        if not b & 0x80:
            return value, i
        shift += 7


def _put_varint(v):
    out = bytearray()
    while True:
        b = v & 0x7F; v >>= 7
        out.append(b | (0x80 if v else 0))
        if not v:
            return bytes(out)


def _fields(buf):
    i, out = 0, []
    while i < len(buf):
        key, i = _varint(buf, i)
        num, wt = key >> 3, key & 7
        if wt == 0:
            v, j = _varint(buf, i); out.append((num, wt, buf[i:j])); i = j
        elif wt == 2:
            n, j = _varint(buf, i); out.append((num, wt, buf[j:j + n])); i = j + n
        elif wt == 1:
            out.append((num, wt, buf[i:i + 8])); i += 8
        elif wt == 5:
            out.append((num, wt, buf[i:i + 4])); i += 4
        else:
            raise ValueError("wire type %d" % wt)
    return out


def _encode(fields):
    out = bytearray()
    for num, wt, raw in fields:
        out += _put_varint(num << 3 | wt)
        out += _put_varint(len(raw)) + raw if wt == 2 else raw
    return bytes(out)


def scrub_visitor(value):
    """Returns (scrubbed, country) for a percent-encoded base64url visitor id."""
    raw = urllib.parse.unquote(value)
    padded = raw + "=" * (-len(raw) % 4)
    buf = base64.urlsafe_b64decode(padded)
    country = None
    fields = _fields(buf)
    for idx, (num, wt, payload) in enumerate(fields):
        if num == 6 and wt == 2:
            inner = _fields(payload)
            for jdx, (n2, w2, p2) in enumerate(inner):
                if n2 == 1 and w2 == 2 and re.fullmatch(rb"[A-Z]{2}", p2):
                    country = p2.decode()
                    inner[jdx] = (n2, w2, b"ZZ")
            fields[idx] = (num, wt, _encode(inner))
    if country is None:
        raise ValueError("visitor id without a country field")
    out = base64.urlsafe_b64encode(_encode(fields)).decode()
    return urllib.parse.quote(out, safe=""), country


LOCAL_SHELF = re.compile(r"near you|local news|in your area|cerca de ti|in deiner nähe|近くの", re.I)
DROP_KEYS = {"trackingParams", "clickTrackingParams", "loggingContext", "adSlotRenderer", "promotedSparklesWebRenderer",
             "themedPalette"}


class Scrubber:
    def __init__(self):
        self.ips, self.countries, self.nodes = set(), set(), {}
        self.report = {}

    def count(self, what, n=1):
        self.report[what] = self.report.get(what, 0) + n

    def _json(self, o):
        if isinstance(o, dict):
            out = {}
            for k, v in o.items():
                if k in DROP_KEYS:
                    self.count("dropped:" + k)
                    continue
                if k in ("shelfRenderer", "richShelfRenderer") and LOCAL_SHELF.search(json.dumps(v.get("title", ""), ensure_ascii=False)):
                    self.count("location-shelf")
                    continue
                out[k] = self._json(v)
            return out
        if isinstance(o, list):
            return [self._json(v) for v in o]
        return o

    def _node(self, m):
        name = m.group(0)
        if name not in self.nodes:
            self.nodes[name] = "sn-node%d" % (len(self.nodes) + 1)
        return self.nodes[name]

    def text(self, s):
        # Visitor ids anywhere in the text (JSON fields, URL parameters, tracking params).
        for vid in set(re.findall(r'"visitorData"\s*:\s*"([^"]+)"', s)):
            clean, country = scrub_visitor(vid)
            self.countries.add(country)
            s = s.replace(vid, clean); self.count("visitor-id")
        def ip(m):
            self.ips.add(urllib.parse.unquote(m.group(2))); self.count("ip"); return m.group(1) + SAFE_IP
        s = re.sub(r"((?:[?&]|\\u0026|%26)ip(?:=|%3D))([0-9A-Fa-f:.%]+?)(?=[&\"\\]|%26|$)", ip, s)
        s = re.sub(r"(/ip/)([0-9A-Fa-f:.%]+?)(?=/)", ip, s)
        for addr in list(self.ips):
            if addr != SAFE_IP and addr in s:
                s = s.replace(addr, SAFE_IP); self.count("ip-literal")
        s = re.sub(r"sn-[a-z0-9]{4,}(?:-[a-z0-9]{2,})?", self._node, s)
        s = re.sub(r"((?:[?&]|\\u0026|%26)g?cr(?:=|%3D))[A-Z]{2}(?=[&\"\\]|%26|$)", lambda m: m.group(1) + "ZZ", s)
        return s

    def json_body(self, raw):
        doc = json.loads(raw)
        if isinstance(doc, dict) and "responseContext" in doc:
            vd = doc["responseContext"].get("visitorData")
            doc["responseContext"] = {"visitorData": vd} if vd else {}
        doc = self._json(doc)
        return self.text(json.dumps(doc, ensure_ascii=False, separators=(",", ":")))

    def check(self, s):
        problems = []
        for addr in self.ips:
            if addr != SAFE_IP and addr in s:
                problems.append("public address left")
        for vid in re.findall(r'"visitorData"\s*:\s*"([^"]+)"', s):
            if scrub_visitor(vid)[1] != "ZZ":
                problems.append("visitor country left")
        for c in self.countries - {"ZZ"}:
            if re.search(r"(?:[?&]|\\u0026|%26)g?cr(?:=|%3D)" + c + r"\b", s):
                problems.append("cr= country left")
        if re.search(r"sn-(?!node\d)[a-z0-9]{4,}", s):
            problems.append("sn- node left")
        return problems


# ---------------------------------------------------------------------------- plan
def find(o, key):
    if isinstance(o, dict):
        if key in o:
            yield o[key]
        for v in o.values():
            yield from find(v, key)
    elif isinstance(o, list):
        for v in o:
            yield from find(v, key)


def first_token(doc):
    for c in find(doc, "continuationCommand"):
        if c.get("token"):
            return c["token"]
    return None


def tab_params(channel_doc):
    tabs = {}
    for tr in find(channel_doc, "tabRenderer"):
        url = tr.get("endpoint", {}).get("commandMetadata", {}).get("webCommandMetadata", {}).get("url", "")
        params = tr.get("endpoint", {}).get("browseEndpoint", {}).get("params")
        if params:
            tabs[url.rsplit("/", 1)[-1]] = urllib.parse.unquote(params)
    return tabs


class Recorder:
    def __init__(self, out, only, dry, trim=(), keep_existing=False, defer=False):
        self.out, self.only, self.dry, self.trim, self.keep_existing = out, only, dry, set(trim), keep_existing
        self.defer = defer  # locale mode: write the files only after the whole set passed
        self.manifest = {"recorded": datetime.date.today().isoformat(), "tool": "tools/record_fixtures.py", "fixtures": {}}
        self.it = None
        self.pending = []

    def save(self, name, status, raw, request, kind="json"):
        if self.only and name not in self.only:
            return
        path = os.path.join(self.out, name + (".json.gz" if kind == "json" else ".%s.gz" % kind))
        if self.keep_existing and os.path.exists(path):
            print("%-34s exists, not re-recorded" % name)
            return
        sc = Scrubber()
        if kind == "json" and raw.strip()[:1] in (b"{", b"[") and self.trim:
            doc = json.loads(raw)
            for k in sorted(self.trim & set(doc) if isinstance(doc, dict) else ()):
                del doc[k]
                sc.count("trimmed:" + k)
            raw = json.dumps(doc, ensure_ascii=False).encode()
        if kind == "json" and raw.strip()[:1] in (b"{", b"["):
            text = sc.json_body(raw)
        else:
            text = sc.text(raw.decode("utf-8", "replace"))
        problems = sc.check(text)
        if problems:
            sys.exit("refusing to save %s: scrub failed (%s)" % (name, ", ".join(sorted(set(problems)))))
        if self.keep_existing and status != 200:
            sys.exit("refusing to save %s: HTTP %d" % (name, status))
        entry = {"status": status, "request": request, "bytes": len(text), "sha256": hashlib.sha256(text.encode()).hexdigest()[:16], "scrub": sc.report}
        self.manifest["fixtures"][os.path.basename(path)] = entry
        print("%-34s HTTP %d %7d bytes %s" % (name, status, len(text), json.dumps(sc.report)))
        self.pending.append((path, text))
        if not self.defer:
            self.write()

    def write(self):
        """Writes the recorded files (in locale mode only after the whole set succeeded)."""
        if self.dry:
            self.pending = []
            return
        pending, self.pending = self.pending, []
        for path, text in pending:
            with gzip.GzipFile(path, "wb", mtime=0) as f:
                f.write(text.encode())

    def it_post(self, name, endpoint, body, hl="en", gl="US"):
        status, raw = self.it.post(endpoint, body, hl, gl)
        req = {"endpoint": endpoint, "body": {k: (v if k != "continuation" else "<token>") for k, v in body.items()}, "hl": hl, "gl": gl}
        self.save(name, status, raw, req)
        return json.loads(raw) if raw.strip() else {}

    def get(self, name, url, kind, headers=None):
        status, raw = http(url, headers=headers)
        if kind == "json" and status == 200:
            raw = json.dumps(json.loads(raw), ensure_ascii=False, separators=(",", ":")).encode()
        self.save(name, status, raw, {"get": re.sub(r"(q|videoId|channel_id)=[^&]+", r"\1=<arg>", url)}, kind)
        return status, raw


def record_caption_extras(rec, vid):
    """SEL-06, the caption track list's two side answers: the visitor id the catalog asks for
    (its own WEB request shape) and the player answer to a request without one (refused)."""
    rec.it_post("visitor_id", "visitor_id", {})
    pbody = {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True,
             "playbackContext": {"contentPlaybackContext": {"html5Preference": "HTML5_PREF_WANTS"}}}
    status, raw = visionos_post("player", pbody)
    rec.save("player_refused", status, raw, {"endpoint": "player", "client": "VISIONOS", "videoId": "<arg>", "visitor": "none"})


def record_upcoming(rec):
    """RES-19: the WEB player answer of a stream that has not started (its scheduled start is in
    the offline slate and in the microformat). The video is the first search row that carries
    upcomingEventData; returns its id and start time, or (None, None) when no query finds one."""
    for query in ("upcoming live stream", "premiere", "launch live", "live countdown"):
        status, raw = rec.it.post("search", {"query": query}, "en", "US")
        if status != 200:
            continue
        for row in find(json.loads(raw), "videoRenderer"):
            start = row.get("upcomingEventData", {}).get("startTime")
            if row.get("videoId") and start:
                rec.it_post("player_upcoming", "player", {"videoId": row["videoId"], "contentCheckOk": True, "racyCheckOk": True})
                return row["videoId"], start
    return None, None


SHORTS_ITEM_TRIM = ("desktopTopbar", "topbar", "background", "tooltip", "frameworkUpdates")
SHORTS_NEXT_TRIM = ("topbar", "pageVisualEffects", "playerOverlays", "overlay")
SHORTS_ITEM_NAMES = ("shorts_item_en", "shorts_next_en", "shorts_item_remix_en", "shorts_item_nocomments_en", "shorts_item_hiddenlikes_en",
                     "shorts_item_description_es", "shorts_item_gone", "shorts_sequence_en_a", "shorts_sequence_en_a_p2",
                     "shorts_sequence_en_b")


def trim_top(raw, keys):
    """The answer without the given top-level members (page chrome the catalog never reads)."""
    doc = json.loads(raw)
    for k in keys:
        doc.pop(k, None)
    return json.dumps(doc, ensure_ascii=False).encode()


def trim_prefetch(raw):
    """A reel sequence without the player answers it prefetches for its first entries (stream
    addresses the catalog never reads; they would make the fixture ten times larger)."""
    doc = json.loads(raw)
    for e in doc.get("entries", []):
        ep = e.get("command", {}).get("reelWatchEndpoint", {})
        ep.pop("unserializedPrefetchData", None)
    return json.dumps(doc, ensure_ascii=False).encode()


def shorts_item_facts(doc):
    """What the catalog's Shorts item parser reads, found independently (recorder selection only)."""
    pv = next(find(doc, "reelPlayerOverlayViewModel"), {})
    bar = pv.get("actionBar", {}).get("reelActionBarViewModel", {}).get("buttonViewModels", [])
    comments = [b for b in bar if b.get("buttonViewModel", {}).get("accessibilityId") == "id.reel_comment_button"]
    like = next(find(pv, "defaultButtonViewModel"), {}).get("buttonViewModel", {}).get("title", "")
    chip = None
    for b in find(pv, "buttonViewModel"):
        if isinstance(b, dict) and b.get("accessibilityId") == "id.reel_multi_format_link":
            cmd = b.get("onTap", {}).get("innertubeCommand", {})
            chip = "reel" if "reelWatchEndpoint" in cmd else "watch" if "watchEndpoint" in cmd else "other"
    body = None
    for panel in doc.get("engagementPanels", []):
        for item in find(panel, "expandableVideoDescriptionBodyRenderer"):
            if item.get("descriptionBodyText", {}).get("runs") or item.get("attributedDescriptionBodyText"):
                body = True
    return {"hasPanel": bool(pv), "comments": bool(comments), "likeDigits": any(c.isdigit() for c in like),
            "chip": chip, "description": bool(body)}


def record_shorts_items(rec, args):
    """Shorts details (reel/reel_item_watch, one Short per request) and the anonymous Shorts
    sequence. The Shorts are the first popular ones of a Shorts-filtered search for "#shorts"
    (en/US); one of each kind is kept. Nothing is saved for a kind that is not found."""
    if rec.only and not (rec.only & set(SHORTS_ITEM_NAMES)):
        return
    status, raw = rec.it.post("search", {"query": "#shorts", "params": "EgIQCQ=="}, "en", "US")
    if status != 200:
        print("WARN: Shorts search for the item answers: HTTP %d" % status)
        return
    endpoints, seen = [], set()
    doc = json.loads(raw)
    for page in range(2):   # page 1, then the first continuation
        for e in find(doc, "reelWatchEndpoint"):
            if e.get("videoId") and e["videoId"] not in seen:
                seen.add(e["videoId"])
                endpoints.append(e)
        tok = first_token(doc)
        if page == 0 and tok:
            st2, raw2 = rec.it.post("search", {"continuation": tok}, "en", "US")
            doc = json.loads(raw2) if st2 == 200 else {}

    def item(video, hl, gl):
        body = {"playerRequest": {"videoId": video}, "disablePlayerResponse": True}
        return rec.it.post("reel/reel_item_watch", body, hl, gl)

    want = {"shorts_item_en": lambda f: f["hasPanel"] and f["comments"] and f["likeDigits"] and f["chip"] == "watch",
            "shorts_item_remix_en": lambda f: f["hasPanel"] and f["chip"] == "reel",
            "shorts_item_nocomments_en": lambda f: f["hasPanel"] and not f["comments"] and f["likeDigits"],
            "shorts_item_hiddenlikes_en": lambda f: f["hasPanel"] and f["comments"] and not f["likeDigits"]}
    for e in endpoints[:60]:
        if not want:
            break
        st, body = item(e["videoId"], "en", "US")
        if st != 200:
            continue
        facts = shorts_item_facts(json.loads(body))
        for name in list(want):
            if want[name](facts):
                rec.save(name, st, trim_top(body, SHORTS_ITEM_TRIM), {"endpoint": "reel/reel_item_watch", "hl": "en", "gl": "US",
                         "body": {"playerRequest": {"videoId": "<id>"}, "disablePlayerResponse": True}})
                if name == "shorts_item_en":
                    # The watch-next answer for the same Short: what a per-Short watch info would be built from.
                    stn, nbody = rec.it.post("next", {"videoId": e["videoId"], "contentCheckOk": True, "racyCheckOk": True}, "en", "US")
                    if stn == 200:
                        rec.save("shorts_next_en", stn, trim_top(nbody, SHORTS_NEXT_TRIM), {"endpoint": "next", "hl": "en", "gl": "US",
                                 "body": {"videoId": "<same Short as shorts_item_en>", "contentCheckOk": True, "racyCheckOk": True}})
                del want[name]
                break
    for name in want:
        print("WARN: no Short found for %s" % name)
    # A Short that has a description of its own (rare: most Shorts have only a title), in es/MX.
    st, body = item(args.shorts_description_video, "es", "MX")
    if st == 200 and shorts_item_facts(json.loads(body))["description"]:
        rec.save("shorts_item_description_es", st, trim_top(body, SHORTS_ITEM_TRIM), {"endpoint": "reel/reel_item_watch",
                 "hl": "es", "gl": "MX", "body": {"playerRequest": {"videoId": "<id>"}, "disablePlayerResponse": True}})
    else:
        print("WARN: %s has no description now; shorts_item_description_es not recorded" % args.shorts_description_video)
    # An id that does not exist: the answer is a success status without an overlay.
    st, body = item("AAAAAAAAAAA", "en", "US")
    rec.save("shorts_item_gone", st, trim_top(body, SHORTS_ITEM_TRIM), {"endpoint": "reel/reel_item_watch", "hl": "en", "gl": "US",
             "body": {"playerRequest": {"videoId": "<missing>"}, "disablePlayerResponse": True}})
    # The anonymous sequence of two seeds (first pages), and the first continuation of the first.
    seeds = [e["sequenceParams"] for e in endpoints if e.get("sequenceParams")]
    first = None
    for i, seed in enumerate(seeds[:8]):
        st, body = rec.it.post("reel/reel_watch_sequence", {"sequenceParams": seed}, "en", "US")
        if st != 200:
            continue
        doc = json.loads(body)
        if len(doc.get("entries", [])) < 10:
            continue
        req = {"endpoint": "reel/reel_watch_sequence", "hl": "en", "gl": "US", "body": {"sequenceParams": "<seed>"}}
        if first is None:
            first = doc
            rec.save("shorts_sequence_en_a", st, trim_prefetch(body), req)
            tok = next(find(doc.get("continuationEndpoint", {}), "token"), None)
            if tok:
                st2, body2 = rec.it.post("reel/reel_watch_sequence", {"continuation": tok}, "en", "US")
                if st2 == 200:
                    rec.save("shorts_sequence_en_a_p2", st2, trim_prefetch(body2),
                             {"endpoint": "reel/reel_watch_sequence", "hl": "en", "gl": "US", "body": {"continuation": "<token>"}})
        else:
            rec.save("shorts_sequence_en_b", st, trim_prefetch(body), req)
            break


def run(rec, args):
    version = scrape_client_version()
    rec.manifest["clientVersion"] = version
    rec.it = InnerTube(version)
    vid, channel, sb_video = args.video, args.channel, args.sponsor_video

    # Home and search (DSC-04, DSC-08, DSC-09, DSC-01 valid-empty).
    rec.it_post("home_anonymous", "browse", {"browseId": "FEwhat_to_watch"})
    # DSC-04a (user-approved 2026-11-08): signed-out Home = most viewed this week, videos only, in
    # the request's language and region. "CAMSBAgDEAE=" = sort by views, uploaded this week, video.
    # Shorts section (user-requested 2026-11-08): popular Shorts = the search for "#shorts" with
    # the Shorts type filter ("EgIQCQ=="), then YouTube's own Shorts sequence seeded by the first
    # lockup's sequenceParams (the signed-in surface uses the same call over the signed client).
    sh = rec.it_post("shorts_popular_es", "search", {"query": "#shorts", "params": "EgIQCQ=="}, "es", "MX")
    stok = first_token(sh) if sh else None
    if stok:
        rec.it_post("shorts_popular_es_p2", "search", {"continuation": stok}, "es", "MX")
    seq = None
    for c in find(sh or {}, "reelWatchEndpoint"):
        if c.get("sequenceParams"):
            seq = c["sequenceParams"]
            break
    if seq:
        rec.it_post("shorts_sequence_es", "reel/reel_watch_sequence", {"sequenceParams": seq}, "es", "MX")
    record_shorts_items(rec, args)
    pop = rec.it_post("home_popular_es", "search", {"query": "", "params": "CAMSBAgDEAE="}, "es", "ES")
    tok = first_token(pop) if pop else None
    if tok:
        rec.it_post("home_popular_es_p2", "search", {"continuation": tok}, "es", "ES")
    s1 = rec.it_post("search_en_p1", "search", {"query": args.query})
    tok = first_token(s1)
    s2 = rec.it_post("search_en_p2", "search", {"continuation": tok}) if tok else {}
    tok = first_token(s2)
    if tok:
        rec.it_post("search_en_p3", "search", {"continuation": tok})
    rec.it_post("search_de_p1", "search", {"query": args.query}, "de", "DE")
    rec.it_post("search_ja_p1", "search", {"query": args.query}, "ja", "JP")
    rec.it_post("search_filter_channel", "search", {"query": args.query, "params": "EgIQAg=="})
    rec.it_post("search_filter_playlist", "search", {"query": args.query, "params": "EgIQAw=="})
    rec.it_post("search_filter_live", "search", {"query": "news", "params": "EgJAAQ=="})
    rec.it_post("search_filter_hour_video", "search", {"query": "news", "params": "EgQIARAB"})
    rec.it_post("search_empty", "search", {"query": "qzxvwk jjvqpl xqzwvk fixture sailtube", "params": "EgQIARAD"})

    # Channel (DSC-12): resolve, header, tabs with their own params, a Videos continuation.
    r = rec.it_post("resolve_handle", "navigation/resolve_url", {"url": "https://www.youtube.com/" + channel})
    ucid = r["endpoint"]["browseEndpoint"]["browseId"]
    home = rec.it_post("channel_home", "browse", {"browseId": ucid})
    tabs = tab_params(home)
    videos = rec.it_post("channel_videos", "browse", {"browseId": ucid, "params": tabs["videos"]})
    tok = first_token(videos)
    if tok:
        rec.it_post("channel_videos_p2", "browse", {"continuation": tok})
    rec.it_post("channel_shorts", "browse", {"browseId": ucid, "params": tabs["shorts"]})
    live = rec.it_post("channel_live", "browse", {"browseId": ucid, "params": tabs["streams"]})
    pls = rec.it_post("channel_playlists", "browse", {"browseId": ucid, "params": tabs["playlists"]})
    rec.it_post("channel_videos_de", "browse", {"browseId": ucid, "params": tabs["videos"]}, "de", "DE")
    rec.it_post("channel_videos_ja", "browse", {"browseId": ucid, "params": tabs["videos"]}, "ja", "JP")

    # A playlist (LIB-03 shape, LNK-04 preview).
    plid = next((p for p in find(pls, "playlistId")), None)
    if plid:
        rec.it_post("playlist_browse", "browse", {"browseId": "VL" + plid})

    # Watch-next, related continuation, comments with replies (DSC-13..16).
    nxt = rec.it_post("next_en", "next", {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True})
    rec.it_post("next_de", "next", {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True}, "de", "DE")
    rec.it_post("next_ja", "next", {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True}, "ja", "JP")
    related_tok = None
    for sr in find(nxt, "secondaryResults"):
        related_tok = first_token(sr)
    if related_tok:
        r2 = rec.it_post("next_related_p2", "next", {"continuation": related_tok})
        # The page's own continuation is the last continuationItemRenderer of the appended items.
        p3_tok = None
        for cir in find(r2, "continuationItemRenderer"):
            p3_tok = first_token(cir) or p3_tok
        if p3_tok:
            rec.it_post("next_related_p3", "next", {"continuation": p3_tok})
    # Comments token of the main results (what the app uses), recorded as comments_section_p1;
    # comments_p1 below keeps the token found last (the engagement panel's).
    for isr in find(nxt.get("contents", {}), "itemSectionRenderer"):
        if isr.get("sectionIdentifier") == "comment-item-section" or isr.get("targetId") == "comments-section":
            section_tok = first_token(isr)
            if section_tok:
                rec.it_post("comments_section_p1", "next", {"continuation": section_tok})
            break
    comments_tok = None
    for isr in find(nxt, "itemSectionRenderer"):
        if isr.get("sectionIdentifier") == "comment-item-section" or isr.get("targetId") == "comments-section":
            comments_tok = first_token(isr)
    if comments_tok:
        c1 = rec.it_post("comments_p1", "next", {"continuation": comments_tok})
        tok = None
        for ep in find(c1, "onResponseReceivedEndpoints"):
            for e in ep:
                items = (e.get("reloadContinuationItemsCommand") or e.get("appendContinuationItemsAction") or {}).get("continuationItems", [])
                for it in items:  # the page's own continuation is the last top-level item
                    if "continuationItemRenderer" in it:
                        tok = first_token(it["continuationItemRenderer"]) or tok
        if tok:
            rec.it_post("comments_p2", "next", {"continuation": tok})
        reply_tok = None
        for th in find(c1, "commentThreadRenderer"):
            rt = first_token(th.get("replies", {}))
            if rt:
                reply_tok = rt
                break
        if reply_tok:
            rp = rec.it_post("comments_replies_p1", "next", {"continuation": reply_tok})
            tok = first_token(rp)
            if tok:
                rec.it_post("comments_replies_p2", "next", {"continuation": tok})

    # Player answers: WEB (details only, usually unplayable), VISIONOS (captions, SEL-06) and a live one.
    rec.it_post("player_web", "player", {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True})
    record_caption_extras(rec, vid)
    record_upcoming(rec)
    status, raw = visionos_post("visitor_id", {})
    visitor = json.loads(raw)["responseContext"]["visitorData"]
    pbody = {"videoId": vid, "contentCheckOk": True, "racyCheckOk": True,
             "playbackContext": {"contentPlaybackContext": {"html5Preference": "HTML5_PREF_WANTS"}}}
    status, raw = visionos_post("player", pbody, visitor)
    rec.save("player_visionos", status, raw, {"endpoint": "player", "client": "VISIONOS", "videoId": "<arg>"})
    player = json.loads(raw)
    live_id = args.live_video
    for lv in find(live, "lockupViewModel"):
        if not live_id and "THUMBNAIL_OVERLAY_BADGE_STYLE_LIVE" in json.dumps(lv.get("contentImage", "")):
            live_id = lv.get("contentId")
    if live_id:
        status, raw = visionos_post("player", dict(pbody, videoId=live_id), visitor)
        rec.save("player_live", status, raw, {"endpoint": "player", "client": "VISIONOS", "videoId": "<live>"})
    tracks = next(find(player, "captionTracks"), [])
    picked = {}
    for t in tracks:
        if t.get("languageCode", "").startswith("en"):
            picked.setdefault("asr" if t.get("kind") == "asr" else "manual", t)
    for kind, t in picked.items():
        url = re.sub(r"&fmt=[^&]*", "", t["baseUrl"]) + "&fmt=json3"
        rec.get("timedtext_%s_json3" % kind, url if url.startswith("http") else ORIGIN + url, "json")

    # RSS (DSC-05) including a dead channel, suggestions (DSC-11), SponsorBlock (SB-01), RYD.
    for attempt in range(8):  # RSS is fast but unreliable (DSC-05): intermittent 404/500
        status, raw = http("%s/feeds/videos.xml?channel_id=%s" % (ORIGIN, ucid))
        if status == 200:
            break
    rec.save("rss_channel", status, raw, {"get": "/feeds/videos.xml?channel_id=<arg>", "attempts": attempt + 1}, "xml")
    rec.get("rss_dead_channel", "%s/feeds/videos.xml?channel_id=UC%s" % (ORIGIN, "0" * 22), "xml")
    for hl in ("en", "de", "ja"):
        rec.get("suggest_" + hl, "https://suggestqueries.google.com/complete/search?client=firefox&ds=yt&hl=%s&q=%s" % (hl, urllib.parse.quote(args.query[:5])), "json")
    prefix = hashlib.sha256(sb_video.encode()).hexdigest()[:4]
    rec.get("sponsorblock_prefix", "https://sponsor.ajay.app/api/skipSegments/%s?categories=%s" % (
        prefix, urllib.parse.quote('["sponsor","selfpromo","interaction","intro","outro","preview","filler","music_offtopic"]')), "json")
    rec.get("ryd_votes", "https://returnyoutubedislikeapi.com/votes?videoId=" + vid, "json")
    rec.manifest["arguments"] = {"query": args.query, "channel": channel, "video": vid, "sponsorVideo": sb_video, "liveVideo": live_id}


# Locale set (I18N-04): request hl -> gl. zh-CN is answered in Simplified Chinese with gl=TW
# (YouTube serves no CN region); ar uses Egypt, the largest Arabic-language region YouTube serves.
LOCALE_REGIONS = {"es": "ES", "pt": "BR", "fr": "FR", "it": "IT", "ru": "RU", "zh-CN": "TW", "ar": "EG",
                  "en": "US", "de": "DE", "ja": "JP"}
LOCALE_TRIM = ("playerOverlays", "topbar")


def file_hl(hl):
    """File-name-safe language: zh-CN -> zh, pt-BR -> pt."""
    return re.split(r"[-_]", hl)[0].lower()


def run_locales(rec, args, locales):
    version = scrape_client_version()
    rec.it = InnerTube(version)
    # The channel id and its Videos tab params come from unsaved helper calls (en).
    status, raw = rec.it.post("navigation/resolve_url", {"url": "https://www.youtube.com/" + args.channel}, "en", "US")
    ucid = json.loads(raw)["endpoint"]["browseEndpoint"]["browseId"]
    status, raw = rec.it.post("browse", {"browseId": ucid}, "en", "US")
    videos_params = tab_params(json.loads(raw))["videos"]
    plan = {}
    for hl in locales:
        gl = LOCALE_REGIONS.get(hl)
        if not gl:
            sys.exit("no region for locale %s (add it to LOCALE_REGIONS)" % hl)
        tag = file_hl(hl)
        plan[hl] = gl
        rec.it_post("search_%s_p1" % tag, "search", {"query": args.query}, hl, gl)
        rec.it_post("next_%s" % tag, "next", {"videoId": args.video, "contentCheckOk": True, "racyCheckOk": True}, hl, gl)
        rec.it_post("channel_videos_%s" % tag, "browse", {"browseId": ucid, "params": videos_params}, hl, gl)
    rec.manifest["locales"] = {"recorded": datetime.date.today().isoformat(), "clientVersion": version, "regions": plan,
                               "trimmed": list(LOCALE_TRIM),
                               "arguments": {"query": args.query, "channel": args.channel, "video": args.video}}


def write_manifest(rec, out, merge_top=False):
    mpath = os.path.join(out, "MANIFEST.json")
    old = json.load(open(mpath)) if os.path.exists(mpath) and (rec.only or merge_top) else {"fixtures": {}}
    old.setdefault("fixtures", {}).update(rec.manifest["fixtures"])
    if merge_top:
        # Keep the main set's description; only add the locale run's own entry.
        old["locales"] = rec.manifest["locales"]
        manifest = old
    else:
        rec.manifest["fixtures"] = old["fixtures"]
        if "locales" in old:
            rec.manifest["locales"] = old["locales"]
        manifest = rec.manifest
    with open(mpath, "w") as f:
        json.dump(manifest, f, indent=1, sort_keys=True)
        f.write("\n")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(os.path.dirname(__file__), "..", "tests", "fixtures", "catalog"))
    ap.add_argument("--only", default="")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--query", default="kurzgesagt")
    ap.add_argument("--channel", default="@NASA")
    ap.add_argument("--video", default="9zCH37330f8")
    ap.add_argument("--sponsor-video", default="9zCH37330f8")
    ap.add_argument("--shorts-description-video", default="xWUJ0y1QRZM",
                    help="a Short whose creator wrote a description (the only kind that has one)")
    ap.add_argument("--live-video", default="")
    ap.add_argument("--locales", default="", help="record only the per-locale set, e.g. es,pt,fr,it,ru,zh-CN,ar")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    only = set(filter(None, args.only.split(",")))
    locales = [l for l in args.locales.split(",") if l]
    if locales:
        rec = Recorder(args.out, only, args.dry_run, LOCALE_TRIM, keep_existing=True, defer=True)
        run_locales(rec, args, locales)
    else:
        rec = Recorder(args.out, only, args.dry_run)
        run(rec, args)
    if not args.dry_run:
        rec.write()
        write_manifest(rec, args.out, merge_top=bool(locales))


if __name__ == "__main__":
    main()
