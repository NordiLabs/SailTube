# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
# Builds the import fixtures into the directory given as argv[1]:
#   older.db / newer.db   NewPipe-format databases of an older and a newer app version
#   older.zip             newpipe.db (deflated) + preferences.settings (stored)
#   other-name.zip        notes.txt, data/pipepipe.db (newer layout), later.db (garbage)
#   no-db.zip             only a text file
#   truncated.zip         older.zip cut in half
#   partial.db            missing columns (stream_state.progress_time, playlists.name)
#   unrelated.db          an SQLite file without NewPipe tables
#   corrupt.db            SQLite magic followed by garbage
#   garbage.bin           neither ZIP nor SQLite
#   zipreader.zip         stored, deflated, traversal-named and CRC-corrupted entries
#   zip64.zip             EOCD patched with ZIP64 markers
#   takeout.zip           Takeout/YouTube and YouTube Music/subscriptions/subscriptions.csv
# Layouts follow the table and column names in FINTUBE_SPEC §5.13 / IMP-01.
import os
import sqlite3
import sys
import zipfile

out = sys.argv[1]
os.makedirs(out, exist_ok=True)

A = "UC" + "a" * 20 + "01"
B = "UC" + "b" * 20 + "02"
C = "UC" + "c" * 20 + "03"
D = "UC" + "d" * 20 + "04"


def vid(n):
    return "vid%08d" % n


def yt_channel(cid):
    return "https://www.youtube.com/channel/" + cid


def make_older(path):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.executescript("""
CREATE TABLE subscriptions (uid INTEGER PRIMARY KEY AUTOINCREMENT, service_id INTEGER NOT NULL DEFAULT 0,
  url TEXT, name TEXT, subscriber_count INTEGER);
CREATE TABLE streams (uid INTEGER PRIMARY KEY AUTOINCREMENT, service_id INTEGER, url TEXT, title TEXT,
  stream_type TEXT, duration INTEGER, uploader TEXT, uploader_url TEXT, thumbnail_url TEXT);
CREATE TABLE stream_history (stream_id INTEGER NOT NULL, access_date INTEGER NOT NULL,
  repeat_count INTEGER NOT NULL DEFAULT 1, PRIMARY KEY (stream_id, access_date));
CREATE TABLE stream_state (stream_id INTEGER PRIMARY KEY, progress_time INTEGER NOT NULL);
CREATE TABLE playlists (uid INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT);
CREATE TABLE playlist_stream_join (playlist_id INTEGER NOT NULL, stream_id INTEGER NOT NULL,
  join_index INTEGER NOT NULL, PRIMARY KEY (playlist_id, join_index));
""")
    # service_id is NOT NULL here (NULL rows are in streams and in the newer
    # layout). Rows: YouTube, SoundCloud (1), handle, /user/, junk.
    db.executemany("INSERT INTO subscriptions (uid, service_id, url, name) VALUES (?,?,?,?)", [
        (1, 0, yt_channel(A), "Alpha"),
        (2, 0, "https://youtube.com/channel/" + B + "/videos", "Beta"),
        (3, 1, "https://soundcloud.com/someone", "Cloud"),
        (4, 0, "https://www.youtube.com/@handlename", "Handle One"),
        (5, 0, "https://www.youtube.com/user/olduser", "Old User"),
        (6, 0, "not a url", "Bad"),
    ])
    db.executemany("INSERT INTO streams (uid, service_id, url, title, stream_type, duration, uploader,"
                   " uploader_url, thumbnail_url) VALUES (?,?,?,?,?,?,?,?,?)", [
        (1, 0, "https://www.youtube.com/watch?v=" + vid(1), "One", "VIDEO_STREAM", 600, "Alpha", yt_channel(A),
         "https://i.ytimg.com/vi/%s/hqdefault.jpg" % vid(1)),
        (2, None, "https://youtu.be/" + vid(2), "Two", "VIDEO_STREAM", 300, "Beta", yt_channel(B), ""),
        (3, 0, "https://www.youtube.com/shorts/" + vid(3), "Three", "VIDEO_STREAM", 45, "Alpha", yt_channel(A), ""),
        (4, 1, "https://soundcloud.com/x/track", "Cloud track", "AUDIO_STREAM", 200, "Cloud", "", ""),
        (5, 0, "https://www.youtube.com/embed/" + vid(5), "Five", "VIDEO_STREAM", 1200, "Beta", yt_channel(B), ""),
        (6, 0, "https://www.youtube.com/watch?v=" + vid(6) + "&t=10", "Six", "VIDEO_STREAM", 100, "Alpha", "", ""),
        (7, 0, "https://example.com/nothing", "Nothing", "VIDEO_STREAM", 10, "", "", ""),
    ])
    db.executemany("INSERT INTO stream_history VALUES (?,?,?)", [
        (1, 1000000, 2), (1, 3000000, 1), (1, 2000000, 4),
        (2, 5000000, 1),
        (3, 4000000, 3),
        (4, 6000000, 1),
        (6, 7000000, 1),
    ])
    db.executemany("INSERT INTO stream_state VALUES (?,?)", [
        (1, 120000), (3, 44000), (5, 900000), (6, 5000), (4, 100000),
    ])
    db.executemany("INSERT INTO playlists (uid, name) VALUES (?,?)", [(1, "Favourites"), (2, "MUSIC")])
    db.executemany("INSERT INTO playlist_stream_join VALUES (?,?,?)", [
        (1, 5, 2), (1, 1, 0), (1, 2, 1), (1, 4, 3), (1, 3, 4),
        (2, 6, 0), (2, 1, 1),
    ])
    db.commit()
    db.close()


def make_newer(path):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.executescript("""
CREATE TABLE subscriptions (uid INTEGER PRIMARY KEY AUTOINCREMENT, service_id INTEGER, url TEXT, name TEXT,
  avatar_url TEXT, subscriber_count INTEGER, description TEXT, notification_mode INTEGER NOT NULL DEFAULT 0,
  future_col TEXT);
CREATE TABLE streams (uid INTEGER PRIMARY KEY AUTOINCREMENT, service_id INTEGER, url TEXT, title TEXT,
  stream_type TEXT, duration INTEGER, uploader TEXT, uploader_url TEXT, thumbnail_url TEXT, view_count INTEGER,
  textual_upload_date TEXT, upload_date INTEGER, is_upload_date_approximation INTEGER, future_flag INTEGER);
CREATE TABLE stream_history (stream_id INTEGER NOT NULL, access_date INTEGER NOT NULL,
  repeat_count INTEGER NOT NULL DEFAULT 1, PRIMARY KEY (stream_id, access_date));
CREATE TABLE stream_state (stream_id INTEGER PRIMARY KEY, progress_time INTEGER NOT NULL, extra INTEGER);
CREATE TABLE playlists (uid INTEGER PRIMARY KEY AUTOINCREMENT, name TEXT, is_thumbnail_permanent INTEGER,
  thumbnail_stream_id INTEGER, display_index INTEGER NOT NULL DEFAULT -1);
CREATE TABLE playlist_stream_join (playlist_id INTEGER NOT NULL, stream_id INTEGER NOT NULL,
  join_index INTEGER NOT NULL, PRIMARY KEY (playlist_id, join_index));
CREATE TABLE remote_playlists (uid INTEGER PRIMARY KEY AUTOINCREMENT, service_id INTEGER, name TEXT, url TEXT,
  thumbnail_url TEXT, uploader TEXT, display_index INTEGER NOT NULL DEFAULT -1, stream_count INTEGER);
CREATE TABLE feed_group (uid INTEGER PRIMARY KEY, name TEXT);
""")
    db.executemany("INSERT INTO subscriptions (uid, service_id, url, name, avatar_url) VALUES (?,?,?,?,?)", [
        (1, 0, yt_channel(C), "Gamma", "https://yt3.example/gamma.jpg"),
        (2, 0, yt_channel(A), "Alpha (new name)", "https://yt3.example/alpha.jpg"),
        (3, 1, "https://soundcloud.com/other", "Cloud", ""),
        (4, None, "https://www.youtube.com/c/SomeCustom", "Custom", ""),
        (5, 0, "https://m.youtube.com/channel/" + D + "?feature=x", "Delta", None),
    ])
    db.executemany("INSERT INTO streams (uid, service_id, url, title, stream_type, duration, uploader,"
                   " uploader_url, thumbnail_url, future_flag) VALUES (?,?,?,?,?,?,?,?,?,?)", [
        (1, 0, "https://www.youtube.com/watch?v=" + vid(7), "Seven", "VIDEO_STREAM", 3600, "Gamma", yt_channel(C),
         "https://i.ytimg.com/vi/%s/hqdefault.jpg" % vid(7), 1),
        (2, 0, "https://youtu.be/%s?si=abc" % vid(8), "Eight", "VIDEO_STREAM", 240, "Gamma", yt_channel(C), "", 0),
        (3, 0, "https://www.youtube.com/shorts/" + vid(9), "Nine", "VIDEO_STREAM", 30, "Delta", yt_channel(D), "", 0),
        (4, 1, "https://soundcloud.com/x/other", "Cloud", "AUDIO_STREAM", 100, "", "", "", 0),
        (5, None, "https://www.youtube.com/watch?v=" + vid(10), "Ten", "VIDEO_STREAM", 1000, "Alpha",
         yt_channel(A), "", 0),
        (6, 0, "https://m.youtube.com/watch?v=" + vid(11), "Eleven", "VIDEO_STREAM", 500, "Alpha", "", "", 0),
        (7, 0, "https://www.youtube.com/watch?v=%s&list=PLx" % vid(7), "Seven (list)", "VIDEO_STREAM", 3600,
         "Gamma", yt_channel(C), "", 0),
    ])
    db.executemany("INSERT INTO stream_history VALUES (?,?,?)", [
        (1, 8000000, 2), (7, 9500000, 1), (1, 9000000, 1),
        (2, 8500000, 1),
        (3, 8100000, 2),
        (4, 9900000, 5),
        (5, 8200000, 1),
        (99, 8300000, 1),
    ])
    db.executemany("INSERT INTO stream_state (stream_id, progress_time) VALUES (?,?)", [
        (1, 3000000), (2, 239000), (5, 9999), (6, 250000), (4, 50000),
    ])
    db.executemany("INSERT INTO playlists (uid, name, is_thumbnail_permanent, thumbnail_stream_id, display_index)"
                   " VALUES (?,?,?,?,?)", [(1, "Road trip", 0, 2, 1), (2, "favourites", 0, 3, 0)])
    db.executemany("INSERT INTO playlist_stream_join VALUES (?,?,?)", [
        (1, 6, 1), (1, 2, 0), (1, 1, 2), (1, 7, 3),
        (2, 3, 0), (2, 4, 1),
    ])
    db.executemany("INSERT INTO remote_playlists (uid, service_id, name, url, thumbnail_url, uploader,"
                   " display_index, stream_count) VALUES (?,?,?,?,?,?,?,?)", [
        (1, 0, "Lectures", "https://www.youtube.com/playlist?list=PLabcdefghijklmnopqrstuvwxyz012345",
         "https://i.ytimg.com/lectures.jpg", "Prof", 0, 42),
        (2, 1, "Cloud set", "https://soundcloud.com/x/sets/y", "", "", 1, 3),
        (3, None, "Mix", "https://www.youtube.com/watch?v=%s&list=PLzyxwvutsrqponmlkjihgfedcba987654" % vid(1),
         "", "Someone", 2, 10),
        (4, 0, "Broken", "https://www.youtube.com/playlist", "", "", 3, 1),
    ])
    db.execute("INSERT INTO feed_group VALUES (1, 'ignored')")
    db.commit()
    db.close()


def make_partial(path):
    # stream_state without progress_time and playlists without name: only those sections skip.
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.executescript("""
CREATE TABLE subscriptions (uid INTEGER PRIMARY KEY, service_id INTEGER, url TEXT, name TEXT);
CREATE TABLE streams (uid INTEGER PRIMARY KEY, service_id INTEGER, url TEXT, title TEXT, duration INTEGER);
CREATE TABLE stream_history (stream_id INTEGER, access_date INTEGER);
CREATE TABLE stream_state (stream_id INTEGER, progress INTEGER);
CREATE TABLE playlists (uid INTEGER PRIMARY KEY, title TEXT);
""")
    db.execute("INSERT INTO subscriptions VALUES (1, 0, ?, 'Alpha')", (yt_channel(A),))
    db.execute("INSERT INTO streams VALUES (1, 0, ?, 'One', 600)", ("https://www.youtube.com/watch?v=" + vid(1),))
    db.executemany("INSERT INTO stream_history VALUES (?,?)", [(1, 100), (1, 200)])
    db.execute("INSERT INTO stream_state VALUES (1, 590000)")
    db.execute("INSERT INTO playlists VALUES (1, 'x')")
    db.commit()
    db.close()


def make_unrelated(path):
    if os.path.exists(path):
        os.remove(path)
    db = sqlite3.connect(path)
    db.execute("CREATE TABLE notes (x TEXT)")
    db.commit()
    db.close()


def build_zip(path, entries):
    with zipfile.ZipFile(path, "w") as z:
        for name, data, method in entries:
            info = zipfile.ZipInfo(name, date_time=(2024, 1, 2, 3, 4, 6))
            info.compress_type = method
            info.external_attr = 0o644 << 16
            z.writestr(info, data)


def read(path):
    with open(path, "rb") as f:
        return f.read()


older = os.path.join(out, "older.db")
newer = os.path.join(out, "newer.db")
make_older(older)
make_newer(newer)
make_partial(os.path.join(out, "partial.db"))
make_unrelated(os.path.join(out, "unrelated.db"))

build_zip(os.path.join(out, "older.zip"), [
    ("newpipe.db", read(older), zipfile.ZIP_DEFLATED),
    ("preferences.settings", b"\xac\xed\x00\x05serialized-prefs", zipfile.ZIP_STORED),
])
build_zip(os.path.join(out, "other-name.zip"), [
    ("notes.txt", b"exported by a fork\n", zipfile.ZIP_DEFLATED),
    ("data/pipepipe.db", read(newer), zipfile.ZIP_DEFLATED),
    ("later.db", b"not the first database", zipfile.ZIP_STORED),
])
build_zip(os.path.join(out, "no-db.zip"), [("readme.txt", b"nothing here\n", zipfile.ZIP_DEFLATED)])
z = read(os.path.join(out, "older.zip"))
with open(os.path.join(out, "truncated.zip"), "wb") as f:
    f.write(z[: len(z) // 2])
with open(os.path.join(out, "corrupt.db"), "wb") as f:
    f.write(b"SQLite format 3\x00" + b"\x13\x37garbage page " * 400)
with open(os.path.join(out, "garbage.bin"), "wb") as f:
    f.write(b"\x00\x01\x02 this is neither a zip nor a database " * 10)

# ZipReader fixture: deflated text is repetitive so it really compresses.
deflated = b"".join(b"deflated line %05d\n" % i for i in range(2000))
crc_payload = b"CRC-PAYLOAD-0123456789-abcdefghij"
zr = os.path.join(out, "zipreader.zip")
build_zip(zr, [
    ("stored.txt", b"stored entry body\n", zipfile.ZIP_STORED),
    ("dir/", b"", zipfile.ZIP_STORED),
    ("dir/deflated.txt", deflated, zipfile.ZIP_DEFLATED),
    ("../evil.txt", b"must never land outside the target\n", zipfile.ZIP_DEFLATED),
    ("crc.bin", crc_payload, zipfile.ZIP_STORED),
    ("empty.txt", b"", zipfile.ZIP_DEFLATED),
])
data = bytearray(read(zr))
at = data.find(crc_payload)
assert at > 0 and data.find(crc_payload, at + 1) < 0
data[at + 4] ^= 0x01  # same size, different bytes: only the CRC can tell
with open(zr, "wb") as f:
    f.write(bytes(data))

# ZIP64 markers in the end-of-central-directory record.
z64 = bytearray(read(os.path.join(out, "no-db.zip")))
eocd = z64.rfind(b"PK\x05\x06")
z64[eocd + 8:eocd + 12] = b"\xff\xff\xff\xff"
z64[eocd + 16:eocd + 20] = b"\xff\xff\xff\xff"
with open(os.path.join(out, "zip64.zip"), "wb") as f:
    f.write(bytes(z64))

takeout_csv = ("﻿Channel Id,Channel Url,Channel Title\r\n"
               "%s,http://www.youtube.com/channel/%s,\"Alpha, the first\"\r\n"
               "%s,http://www.youtube.com/channel/%s,\"Beta \"\"quoted\"\"\"\r\n"
               "%s,http://www.youtube.com/channel/%s,Gamma\r\n" % (A, A, B, B, C, C)).encode("utf-8")
build_zip(os.path.join(out, "takeout.zip"), [
    ("Takeout/archive_browser.html", b"<html></html>", zipfile.ZIP_DEFLATED),
    ("Takeout/YouTube and YouTube Music/subscriptions/subscriptions.csv", takeout_csv, zipfile.ZIP_DEFLATED),
])
print("fixtures written to", out)
