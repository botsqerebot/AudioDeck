"""
fpbuilder.py - builds the FLAC player's library.json and voice clips on a PC.

This is a line-by-line port of the firmware's scanner (tags.h, library.h,
textutil.h, voice.h), so the output is exactly what the ESP32 would make:
same JSON layout, same tag rules, same sort order and the same voice file
names. Because the names match, a later scan on the player itself reuses
these clips instead of making robotic ones.
"""
import json
import os
import re
import struct
import time
import calendar
import io
import wave
from pathlib import Path

LIBRARY_VERSION = 1
MUSIC_DIR = "/Music"
VOICE_MENU_DIR = "/Voices/Menu"
VOICE_LIB_DIR = "/Voices/Library"
LIBRARY_JSON = "/System/library.json"
VOICE_MAX_CHARS = 120
MANIFEST = "/Voices/piper_manifest.json"

# ---- menu texts: must match MENU_VOICES in library.h -------------------------
MENU_VOICES = [
    ("home_now_playing", "Now playing"),
    ("home_music", "Music"),
    ("home_settings", "Settings"),
    ("music_albums", "Albums"),
    ("music_artists", "Artists"),
    ("music_random", "Random album"),
    ("music_genre", "Genre"),
    ("music_all_songs", "All songs"),
    ("scan_confirm", "Scan library? Press play for a quick scan. Hold play for a full scan. Press home to cancel."),
    ("shuffle_on", "Shuffle on"),
    ("shuffle_off", "Shuffle off"),
    ("repeat_off", "Repeat off"),
    ("repeat_all", "Repeat all"),
    ("repeat_one", "Repeat one"),
    ("no_library", "No library. Go to settings and scan."),
    ("nothing_to_resume", "Nothing to resume"),
    ("empty", "Empty"),
    ("scan_quick", "Quick scan. Please wait."),
    ("scan_full", "Full scan. Please wait."),
    ("scan_done", "Scan complete."),
    ("scan_failed", "Scan failed. Check the SD card."),
    ("power_off", "Powering off."),
]


def read_library_h(path):
    """Read MENU_VOICES from the firmware's library.h, if available."""
    try:
        src = Path(path).read_text(encoding="utf-8")
    except OSError:
        return None
    block = re.search(r"MENU_VOICES\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not block:
        return None
    items = re.findall(r'\{\s*"(\w+)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\}', block.group(1))
    return [(k, bytes(t, "utf-8").decode("unicode_escape")) for k, t in items] or None


# ---- textutil.h ---------------------------------------------------------------
FOLD = {
    0xE6: "ae", 0xC6: "AE", 0xF8: "o", 0xD8: "O", 0xE5: "aa", 0xC5: "AA",
    0xE4: "a", 0xC4: "A", 0xF6: "o", 0xD6: "O", 0xFC: "u", 0xDC: "U", 0xDF: "ss",
    0xF1: "n", 0xD1: "N", 0xE7: "c", 0xC7: "C", 0xFD: "y", 0xFF: "y",
    0x2018: "'", 0x2019: "'", 0x201C: '"', 0x201D: '"', 0x2013: "-", 0x2014: "-", 0x2026: "...",
}
for _cps, _f in (((0xE9, 0xE8, 0xEA, 0xEB), "e"), ((0xC9, 0xC8, 0xCA, 0xCB), "E"),
                 ((0xE1, 0xE0, 0xE2, 0xE3), "a"), ((0xC1, 0xC0, 0xC2, 0xC3), "A"),
                 ((0xED, 0xEC, 0xEE, 0xEF), "i"), ((0xCD, 0xCC, 0xCE, 0xCF), "I"),
                 ((0xF3, 0xF2, 0xF4, 0xF5), "o"), ((0xD3, 0xD2, 0xD4, 0xD5), "O"),
                 ((0xFA, 0xF9, 0xFB), "u"), ((0xDA, 0xD9, 0xDB), "U")):
    for _cp in _cps:
        FOLD[_cp] = _f

SPACES = " \t\n\v\f\r"


def sam_text(s):
    out = []
    for ch in s:
        cp = ord(ch)
        f = FOLD.get(cp)
        if f:
            out.append(f)
        elif ch in "&+":
            out.append(" AND ")
        elif cp < 0x80 and (ch.isalnum() or ch in " .,?!'-"):
            out.append(ch)
        else:
            out.append(" ")
    clean, space = [], True
    for c in "".join(out):
        if c == " ":
            if not space:
                clean.append(" ")
            space = True
        else:
            clean.append(c.upper() if "a" <= c <= "z" else c)
            space = False
    clean = "".join(clean).strip(SPACES)
    if len(clean) > VOICE_MAX_CHARS:
        cut = clean.rfind(" ", 0, VOICE_MAX_CHARS + 1)
        clean = clean[:cut] if cut > 20 else clean[:VOICE_MAX_CHARS]
    return clean or "UNKNOWN"


def sort_key(s):
    out = []
    for ch in s:
        cp = ord(ch)
        f = FOLD.get(cp)
        if f:
            out.append(f.lower())
        elif cp < 0x80:
            out.append(ch.lower() if "A" <= ch <= "Z" else ch)
        else:
            out.append("~")
    r = "".join(out)
    return r[4:] if r.startswith("the ") else r


def fold_key(s):
    """Key equivalent to foldCmp() in library.h."""
    if len(s) >= 4 and s[:3].lower() == "the" and s[3] == " ":
        s = s[4:]
    out = []
    for ch in s:
        cp = ord(ch)
        f = FOLD.get(cp)
        out.append(f if f else (ch if cp < 0x80 else "~"))
    return "".join(c.lower() if "A" <= c <= "Z" else c for c in "".join(out))


def fnv64(data):
    h = 1469598103934665603
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def library_voice_path(text):
    h = f"{fnv64(sam_text(text).encode('utf-8')):016x}"
    return f"{VOICE_LIB_DIR}/{h[0]}/{h}.wav"


def menu_voice_path(key, text):
    return f"{VOICE_MENU_DIR}/{key}_{fnv64(text.encode('utf-8')):016x}"[: len(VOICE_MENU_DIR) + len(key) + 8] + ".wav"


def to_int(s):
    """Arduino String::toInt (atol)."""
    m = re.match(r"\s*([+-]?\d+)", s)
    return int(m.group(1)) if m else 0


def path_base(p):
    i = p.rfind("/")
    return p[i + 1:] if i >= 0 else p


def path_dir(p):
    i = p.rfind("/")
    return p[:i] if i > 0 else "/"


def path_stem(p):
    b = path_base(p)
    i = b.rfind(".")
    return b[:i] if i > 0 else b


# ---- tags.h -------------------------------------------------------------------
ID3_GENRES = [
    "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk", "Grunge", "Hip-Hop", "Jazz", "Metal",
    "New Age", "Oldies", "Other", "Pop", "R&B", "Rap", "Reggae", "Rock", "Techno", "Industrial",
    "Alternative", "Ska", "Death Metal", "Pranks", "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House", "Game", "Sound Clip", "Gospel", "Noise",
    "Alternative Rock", "Bass", "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock", "Ethnic", "Gothic",
    "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk", "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret", "New Wave", "Psychedelic", "Rave", "Showtunes",
    "Trailer", "Lo-Fi", "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical", "Rock & Roll", "Hard Rock",
]


class Tags:
    def __init__(self):
        self.title = self.artist = self.album = self.album_artist = self.genre = self.year = ""
        self.track = self.disc = 0
        self.duration = self.data_start = self.sample_rate = 0
        self.bits = self.channels = 0


def genre_name(g):
    g = g.strip(SPACES)
    if g.startswith("("):
        close = g.find(")")
        if close > 1:
            rest = g[close + 1:].strip(SPACES)
            if rest:
                return rest
            g = g[1:close]
    if g and all(c in "0123456789" for c in g):
        n = int(g)
        return ID3_GENRES[n] if 0 <= n < len(ID3_GENRES) else ""
    return g


def be32(b, o=0):
    return struct.unpack_from(">I", b, o)[0]


def le32(b, o=0):
    return struct.unpack_from("<I", b, o)[0]


def syncsafe(b, o=0):
    return ((b[o] & 0x7F) << 21) | ((b[o + 1] & 0x7F) << 14) | ((b[o + 2] & 0x7F) << 7) | (b[o + 3] & 0x7F)


def _cut_nul(d):
    i = d.find(b"\0")
    return d if i < 0 else d[:i]


def latin1(d):
    return _cut_nul(d).decode("latin-1")


def raw_utf8(d):
    return _cut_nul(d).decode("utf-8", "replace")


def id3_text(d):
    if len(d) < 2:
        return ""
    enc, d = d[0], d[1:]
    if enc == 0:
        s = latin1(d)
    elif enc == 3:
        s = raw_utf8(d)
    else:
        le = False
        if enc == 1 and len(d) >= 2:
            if d[0] == 0xFF and d[1] == 0xFE:
                le, d = True, d[2:]
            elif d[0] == 0xFE and d[1] == 0xFF:
                d = d[2:]
        units = []
        for i in range(0, len(d) - 1, 2):
            u = (d[i] | (d[i + 1] << 8)) if le else ((d[i] << 8) | d[i + 1])
            if u == 0:
                break
            units.append(u)
        out, i = [], 0
        while i < len(units):
            u = units[i]
            if 0xD800 <= u <= 0xDBFF and i + 1 < len(units):
                u = 0x10000 + ((u - 0xD800) << 10) + (units[i + 1] - 0xDC00)
                i += 1
            try:
                out.append(chr(u))
            except ValueError:
                out.append("?")
            i += 1
        s = "".join(out)
    return s.strip(SPACES)


ID3_MAP = {
    "TIT2": "title", "TT2": "title", "TPE1": "artist", "TP1": "artist", "TALB": "album", "TAL": "album",
    "TPE2": "album_artist", "TP2": "album_artist", "TCON": "genre", "TCO": "genre",
    "TRCK": "track", "TRK": "track", "TPOS": "disc", "TPA": "disc",
    "TDRC": "year", "TYER": "year", "TYE": "year",
}


def id3_assign(t, fid, v):
    if not v:
        return
    k = ID3_MAP[fid]
    if k == "genre":
        t.genre = genre_name(v)
    elif k in ("track", "disc"):
        setattr(t, k, to_int(v))
    elif k == "year":
        if not t.year:
            t.year = v[:4]
    else:
        setattr(t, k, v)


def parse_id3v2(f, t):
    f.seek(0)
    h = f.read(10)
    if len(h) != 10 or h[:3] != b"ID3":
        return 0
    ver, flags = h[3], h[5]
    size = syncsafe(h, 6)
    end = 10 + size
    tag_end = end + (10 if flags & 0x10 else 0)
    pos = 10
    if (flags & 0x40) and ver >= 3:
        e = f.read(4)
        if len(e) == 4:
            pos += syncsafe(e) if ver == 4 else be32(e) + 4
    while pos < end:
        f.seek(pos)
        if ver == 2:
            if pos + 6 > end:
                break
            fh = f.read(6)
            if len(fh) != 6 or fh[0] == 0:
                break
            fid = fh[:3].decode("latin-1")
            fs = (fh[3] << 16) | (fh[4] << 8) | fh[5]
            fflags = 0
            pos += 6
        else:
            if pos + 10 > end:
                break
            fh = f.read(10)
            if len(fh) != 10 or fh[0] == 0:
                break
            fid = fh[:4].decode("latin-1")
            fs = syncsafe(fh, 4) if ver == 4 else be32(fh, 4)
            fflags = (fh[8] << 8) | fh[9]
            pos += 10
        if fs == 0 or pos + fs > end:
            break
        skip = (ver == 3 and (fflags & 0x00C0)) or (ver == 4 and (fflags & 0x000C))
        if not skip and fid in ID3_MAP and fs <= 512:
            off = 4 if (ver == 4 and (fflags & 0x0001)) else 0
            buf = f.read(fs)
            if len(buf) == fs and fs > off:
                id3_assign(t, fid, id3_text(buf[off:]))
        pos += fs
    return tag_end


def parse_id3v1(f, t, size):
    if size < 128:
        return False
    f.seek(size - 128)
    b = f.read(128)
    if len(b) != 128 or b[:3] != b"TAG":
        return False

    def field(o, n):
        return latin1(b[o:o + n]).strip(SPACES)

    if not t.title:
        t.title = field(3, 30)
    if not t.artist:
        t.artist = field(33, 30)
    if not t.album:
        t.album = field(63, 30)
    if not t.year:
        t.year = field(93, 4)
    if not t.track and b[125] == 0 and b[126]:
        t.track = b[126]
    if not t.genre and b[127] < len(ID3_GENRES):
        t.genre = ID3_GENRES[b[127]]
    return True


BR_V1 = [0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0]
BR_V2 = [0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0]
SR_TAB = [[11025, 12000, 8000], [22050, 24000, 16000], [44100, 48000, 32000]]


def mp3_header(b, i):
    if b[i] != 0xFF or (b[i + 1] & 0xE0) != 0xE0:
        return None
    vbits, layer = (b[i + 1] >> 3) & 3, (b[i + 1] >> 1) & 3
    if vbits == 1 or layer != 1:
        return None
    bri, sri, pad = b[i + 2] >> 4, (b[i + 2] >> 2) & 3, (b[i + 2] >> 1) & 1
    if bri in (0, 15) or sri == 3:
        return None
    ver = 1 if vbits == 3 else 2
    sr = SR_TAB[2 if vbits == 3 else (1 if vbits == 2 else 0)][sri]
    br = (BR_V1 if ver == 1 else BR_V2)[bri]
    spf = 1152 if ver == 1 else 576
    ln = (144000 if ver == 1 else 72000) * br // sr + pad
    mono = (b[i + 3] >> 6) == 3
    return (ver, sr, br, ln, spf, mono) if ln > 4 else None


def mp3_duration(f, start, has_v1, t, size):
    f.seek(start)
    buf = f.read(4096)
    n = len(buf)
    i = 0
    while i + 4 < n:
        h = mp3_header(buf, i)
        if not h:
            i += 1
            continue
        ver, sr, br, ln, spf, mono = h
        if i + ln + 4 < n and not mp3_header(buf, i + ln):
            i += 1
            continue
        t.data_start = start + i
        t.sample_rate, t.channels, t.bits = sr, (1 if mono else 2), 16
        xo = (21 if mono else 36) if ver == 1 else (13 if mono else 21)
        frames = 0
        if i + xo + 12 < n and buf[i + xo:i + xo + 4] in (b"Xing", b"Info"):
            if be32(buf, i + xo + 4) & 1:
                frames = be32(buf, i + xo + 8)
        elif i + 36 + 18 < n and buf[i + 36:i + 40] == b"VBRI":
            frames = be32(buf, i + 36 + 14)
        if frames:
            t.duration = frames * spf // sr
        else:
            audio = (size - t.data_start - (128 if has_v1 else 0)) & 0xFFFFFFFF
            t.duration = (audio * 8 // (br * 1000)) & 0xFFFFFFFF
        return
    t.data_start = start


def read_mp3(f, t, size):
    start = parse_id3v2(f, t)
    v1 = parse_id3v1(f, t, size)
    mp3_duration(f, start, v1, t, size)
    return True


def vorbis_assign(t, kv):
    eq = kv.find("=")
    if eq <= 0:
        return
    k, v = kv[:eq].upper(), kv[eq + 1:].strip(SPACES)
    if not v:
        return
    if k == "TITLE":
        t.title = t.title or v
    elif k == "ARTIST":
        t.artist = t.artist or v
    elif k == "ALBUM":
        t.album = t.album or v
    elif k in ("ALBUMARTIST", "ALBUM ARTIST"):
        t.album_artist = t.album_artist or v
    elif k == "GENRE":
        t.genre = t.genre or genre_name(v)
    elif k == "TRACKNUMBER":
        t.track = to_int(v)
    elif k == "DISCNUMBER":
        t.disc = to_int(v)
    elif k in ("DATE", "YEAR"):
        t.year = t.year or v[:4]


def read_flac(f, t, size):
    pos = parse_id3v2(f, Tags())
    f.seek(pos)
    if f.read(4) != b"fLaC":
        return False
    pos += 4
    while True:
        f.seek(pos)
        h = f.read(4)
        if len(h) != 4:
            return False
        last, typ = h[0] & 0x80, h[0] & 0x7F
        ln = (h[1] << 16) | (h[2] << 8) | h[3]
        pos += 4
        if pos + ln > size:
            return False
        if typ == 0 and ln >= 34:
            b = f.read(34)
            t.sample_rate = (b[10] << 12) | (b[11] << 4) | (b[12] >> 4)
            t.channels = ((b[12] >> 1) & 7) + 1
            t.bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1
            total = ((b[13] & 0x0F) << 32) | be32(b, 14)
            if t.sample_rate:
                t.duration = total // t.sample_rate
        elif typ == 4:
            l4 = f.read(4)
            p = pos + 4 + le32(l4)
            f.seek(p)
            count = le32(f.read(4))
            p += 4
            i = 0
            while i < count and p + 4 <= pos + ln:
                f.seek(p)
                el = le32(f.read(4))
                p += 4
                if el < 512:
                    d = f.read(el)
                    if len(d) == el:
                        vorbis_assign(t, raw_utf8(d))
                p += el
                i += 1
        pos += ln
        if last:
            break
    t.data_start = pos
    return True


def apply_fallbacks(t, path):
    if not t.title:
        t.title = path_stem(path)
    d = path_dir(path)
    if not t.album:
        t.album = "Unknown Album" if d == MUSIC_DIR else path_base(d)
    if not t.artist:
        up = path_dir(d)
        t.artist = "Unknown Artist" if (d == MUSIC_DIR or up == MUSIC_DIR or up == "/") else path_base(up)
    if not t.album_artist:
        t.album_artist = t.artist
    if not t.genre:
        t.genre = "Unknown Genre"


def fat_mtime(st_mtime):
    """What the ESP32 reports for this file: FAT local time read as UTC, 2 s steps."""
    return (calendar.timegm(time.localtime(int(st_mtime))) // 2 * 2) & 0xFFFFFFFF


# ---- library.h scan -------------------------------------------------------------
class Cancelled(Exception):
    pass


def scan_library(root, quick=True, progress=None, cancel=None):
    """Walk <root>/Music and return the library dict (without voices)."""
    root = Path(root)
    music = root / "Music"
    old = {}
    if quick:
        try:
            with open(root / LIBRARY_JSON.lstrip("/"), encoding="utf-8") as fh:
                prev = json.load(fh)
            if prev.get("version") == LIBRARY_VERSION:
                old = {t["path"]: t for t in prev.get("tracks", [])}
        except (OSError, ValueError):
            pass

    tracks, reused = [], []
    found = 0
    for dirpath, dirnames, filenames in os.walk(music):
        dirnames[:] = sorted(d for d in dirnames if not d.startswith("."))
        for name in sorted(filenames):
            if cancel and cancel():
                raise Cancelled()
            if name.startswith("."):
                continue
            low = name.lower()
            flac, mp3 = low.endswith(".flac"), low.endswith(".mp3")
            if not (flac or mp3):
                continue
            full = Path(dirpath) / name
            rel = "/" + full.relative_to(root).as_posix()
            st = full.stat()
            size, mtime = st.st_size, fat_mtime(st.st_mtime)
            o = old.get(rel)
            if o and o.get("size") == size and o.get("mtime") == mtime:
                tracks.append(dict(o))
                reused.append(True)
            else:
                t = Tags()
                try:
                    with open(full, "rb") as f:
                        ok = read_flac(f, t, size) if flac else read_mp3(f, t, size)
                except OSError:
                    ok = False
                if ok:
                    apply_fallbacks(t, rel)
                    tracks.append({
                        "path": rel, "title": t.title, "artist": t.artist, "album": t.album,
                        "albumArtist": t.album_artist, "genre": t.genre, "year": t.year,
                        "track": t.track, "disc": t.disc, "duration": t.duration,
                        "format": "flac" if flac else "mp3", "sampleRate": t.sample_rate,
                        "bits": t.bits, "channels": t.channels, "size": size, "mtime": mtime,
                        "dataStart": t.data_start,
                    })
                    reused.append(False)
            found += 1
            if progress:
                progress("Scanning", found, 0, name)
            if len(tracks) >= 65000:
                break

    T = tracks
    if progress:
        progress("Sorting", 0, 0, "")

    album_map, artist_map, genre_map = {}, {}, {}
    album_items, artist_items, genre_items = [], [], []
    album_year, album_first, album_artist_of, genre_first = [], [], [], []
    track_genre = []

    def group(m, items, key):
        if key in m:
            return m[key]
        m[key] = len(items)
        items.append([])
        return m[key]

    for i, t in enumerate(T):
        aa, al = t["albumArtist"], t["album"]
        a = group(album_map, album_items, sort_key(aa) + "\x1f" + sort_key(al))
        if len(album_first) <= a:
            album_first.append(i)
            album_year.append(9999)
            r = group(artist_map, artist_items, sort_key(aa))
            album_artist_of.append(r)
            artist_items[r].append(a)
        album_items[a].append(i)
        y = to_int(t.get("year", ""))
        if 0 < y < album_year[a]:
            album_year[a] = y
        g = group(genre_map, genre_items, sort_key(t["genre"]))
        track_genre.append(g)
        if len(genre_first) <= g:
            genre_first.append(i)
    genre_albums = [[] for _ in genre_items]
    for a, items in enumerate(album_items):
        seen = []
        for ti in items:
            g = track_genre[ti]
            if g not in seen:
                seen.append(g)
                genre_albums[g].append(a)

    for items in album_items:
        items.sort(key=lambda x: (T[x].get("disc", 0), T[x].get("track", 0), fold_key(T[x]["title"])))

    def album_name(a):
        return T[album_first[a]]["album"]

    def artist_name(r):
        return T[album_first[artist_items[r][0]]]["albumArtist"]

    def genre_label(g):
        return T[genre_first[g]]["genre"]

    album_order = sorted(range(len(album_items)), key=lambda a: (fold_key(album_name(a)), fold_key(artist_name(album_artist_of[a]))))
    artist_order = sorted(range(len(artist_items)), key=lambda r: fold_key(artist_name(r)))
    genre_order = sorted(range(len(genre_items)), key=lambda g: fold_key(genre_label(g)))
    album_rank = {a: i for i, a in enumerate(album_order)}
    artist_rank = {r: i for i, r in enumerate(artist_order)}
    for items in artist_items:
        items.sort(key=lambda a: (album_year[a], fold_key(album_name(a))))
    for items in genre_albums:
        items.sort(key=lambda a: album_rank[a])

    albums = []
    for a in album_order:
        dur = 0
        for ti in album_items[a]:
            dur += T[ti].get("duration", 0)
            T[ti]["albumId"] = album_rank[a]
        albums.append({
            "name": album_name(a), "artist": artist_name(album_artist_of[a]),
            "artistId": artist_rank[album_artist_of[a]], "genre": T[album_items[a][0]]["genre"],
            "year": 0 if album_year[a] == 9999 else album_year[a],
            "tracks": list(album_items[a]), "duration": dur,
        })
    artists = [{"name": artist_name(r), "albums": [album_rank[a] for a in artist_items[r]]} for r in artist_order]
    genres = [{"name": genre_label(g), "albums": [album_rank[a] for a in genre_albums[g]]} for g in genre_order]
    all_songs = sorted(range(len(T)), key=lambda x: fold_key(T[x]["title"]))

    lib = {
        "version": LIBRARY_VERSION, "tracks": T, "albums": albums, "artists": artists,
        "genres": genres, "allSongs": all_songs,
        "counts": {"tracks": len(T), "albums": len(albums), "artists": len(artists), "genres": len(genres)},
    }
    return lib, reused


# ---- voices -----------------------------------------------------------------------
class PiperTTS:
    def __init__(self, voice_name, voices_dir, speed=1.0):
        from piper import PiperVoice, SynthesisConfig
        from piper.download_voices import download_voice
        d = Path(voices_dir)
        d.mkdir(parents=True, exist_ok=True)
        model = d / f"{voice_name}.onnx"
        if not model.exists():
            download_voice(voice_name, d)
        self.voice = PiperVoice.load(str(model))
        self.cfg = SynthesisConfig(length_scale=speed, normalize_audio=True, volume=0.9)
        self.tag = f"{voice_name}@{speed}"

    def wav_bytes(self, text):
        buf = io.BytesIO()
        with wave.open(buf, "wb") as w:
            self.voice.synthesize_wav(text, w, syn_config=self.cfg)
        buf.seek(0)
        with wave.open(buf, "rb") as r:
            pcm, rate, ch, sw = r.readframes(r.getnframes()), r.getframerate(), r.getnchannels(), r.getsampwidth()
        if ch != 1 or sw != 2:
            raise RuntimeError("Piper returned an unexpected format")
        out = io.BytesIO()
        with wave.open(out, "wb") as w:     # plain 16-bit mono PCM, what the player decodes
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(rate)
            w.writeframes(pcm)
        return out.getvalue()


def _write_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_bytes(data)
    os.replace(tmp, path)


def build(root, tts, quick=True, menu_items=None, regen=False, cleanup=False, menu_only=False,
          progress=None, log=print, cancel=None):
    """Full job: scan, voices, save library.json. Returns a summary dict."""
    root = Path(root)
    menu_items = menu_items or MENU_VOICES
    man_path = root / MANIFEST.lstrip("/")
    try:
        manifest = json.loads(man_path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        manifest = {}

    made = kept = 0

    def voice(text, rel):
        nonlocal made, kept
        if cancel and cancel():
            raise Cancelled()
        p = root / rel.lstrip("/")
        if p.exists() and not regen and manifest.get(rel) == tts.tag:
            kept += 1
            return
        _write_atomic(p, tts.wav_bytes(text))
        manifest[rel] = tts.tag
        made += 1

    try:
        menu_voices = {}
        for i, (key, text) in enumerate(menu_items):
            rel = menu_voice_path(key, text)
            voice(text, rel)
            menu_voices[key] = rel
            if progress:
                progress("Menu voices", i + 1, len(menu_items), text)
        # remove menu clips left over from older menu texts
        mdir = root / VOICE_MENU_DIR.lstrip("/")
        for key, text in menu_items:
            keep = path_base(menu_voice_path(key, text))
            for oldf in mdir.glob(f"{key}_??????.wav"):
                if oldf.name != keep:
                    oldf.unlink()
        log(f"Menu: {len(menu_items)} clips")
        if menu_only:
            return {"menu": len(menu_items), "made": made, "kept": kept}

        if not (root / "Music").is_dir():
            raise RuntimeError(f"No Music folder in {root}")
        lib, _ = scan_library(root, quick=quick, progress=progress, cancel=cancel)
        log(f"Found {lib['counts']['tracks']} songs, {lib['counts']['albums']} albums, "
            f"{lib['counts']['artists']} artists, {lib['counts']['genres']} genres")
        lib["menuVoices"] = menu_voices

        jobs = [(t, t["title"]) for t in lib["tracks"]] + [(o, o["name"]) for o in lib["albums"] + lib["artists"] + lib["genres"]]
        for i, (obj, text) in enumerate(jobs):
            rel = library_voice_path(text)
            voice(text, rel)
            obj["voice"] = rel
            if progress:
                progress("Voices", i + 1, len(jobs), text)

        if cleanup:
            used = {t["voice"] for t in lib["tracks"]} | {o["voice"] for o in lib["albums"] + lib["artists"] + lib["genres"]}
            removed = 0
            ldir = root / VOICE_LIB_DIR.lstrip("/")
            for f in ldir.rglob("*.wav") if ldir.exists() else []:
                rel = "/" + f.relative_to(root).as_posix()
                if rel not in used:
                    f.unlink()
                    manifest.pop(rel, None)
                    removed += 1
            log(f"Removed {removed} unused clips")

        if progress:
            progress("Saving", 0, 0, "")
        data = json.dumps(lib, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        _write_atomic(root / LIBRARY_JSON.lstrip("/"), data)
        log(f"Saved {LIBRARY_JSON} ({len(data) // 1024} KB)")
        return {"songs": lib["counts"]["tracks"], "albums": lib["counts"]["albums"], "made": made, "kept": kept}
    finally:
        _write_atomic(man_path, json.dumps(manifest, indent=0).encode("utf-8"))
