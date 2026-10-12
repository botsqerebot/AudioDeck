// tags.h — reads ID3v2/ID3v1 (MP3) and Vorbis comments (FLAC), plus duration
#pragma once
#include <Arduino.h>
#include <FS.h>
#include "textutil.h"

struct TrackTags {
  String title, artist, album, albumArtist, genre, year;
  int track = 0, disc = 0;
  uint32_t duration = 0;    // seconds
  uint32_t dataStart = 0;   // first audio byte (after tags/metadata/cover art)
  uint32_t sampleRate = 0;
  uint8_t bits = 0, channels = 0;
};

static const char *const ID3_GENRES[] = {
  "Blues","Classic Rock","Country","Dance","Disco","Funk","Grunge","Hip-Hop","Jazz","Metal",
  "New Age","Oldies","Other","Pop","R&B","Rap","Reggae","Rock","Techno","Industrial",
  "Alternative","Ska","Death Metal","Pranks","Soundtrack","Euro-Techno","Ambient","Trip-Hop","Vocal","Jazz+Funk",
  "Fusion","Trance","Classical","Instrumental","Acid","House","Game","Sound Clip","Gospel","Noise",
  "Alternative Rock","Bass","Soul","Punk","Space","Meditative","Instrumental Pop","Instrumental Rock","Ethnic","Gothic",
  "Darkwave","Techno-Industrial","Electronic","Pop-Folk","Eurodance","Dream","Southern Rock","Comedy","Cult","Gangsta",
  "Top 40","Christian Rap","Pop/Funk","Jungle","Native American","Cabaret","New Wave","Psychedelic","Rave","Showtunes",
  "Trailer","Lo-Fi","Tribal","Acid Punk","Acid Jazz","Polka","Retro","Musical","Rock & Roll","Hard Rock"
};
static const int ID3_GENRE_COUNT = sizeof(ID3_GENRES) / sizeof(ID3_GENRES[0]);

static String genreName(String g) {
  g.trim();
  // "(17)", "(17)Rock", "17"
  if (g.startsWith("(")) {
    int close = g.indexOf(')');
    if (close > 1) {
      String rest = g.substring(close + 1); rest.trim();
      if (rest.length()) return rest;
      g = g.substring(1, close);
    }
  }
  bool numeric = g.length() > 0;
  for (size_t i = 0; i < g.length(); i++) if (!isdigit((unsigned char)g[i])) numeric = false;
  if (numeric) {
    int n = g.toInt();
    return (n >= 0 && n < ID3_GENRE_COUNT) ? String(ID3_GENRES[n]) : String();
  }
  return g;
}

static uint32_t be32(const uint8_t *b) { return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3]; }
static uint32_t le32(const uint8_t *b) { return ((uint32_t)b[3] << 24) | ((uint32_t)b[2] << 16) | ((uint32_t)b[1] << 8) | b[0]; }
static uint32_t syncsafe(const uint8_t *b) { return ((uint32_t)(b[0] & 0x7F) << 21) | ((uint32_t)(b[1] & 0x7F) << 14) | ((uint32_t)(b[2] & 0x7F) << 7) | (b[3] & 0x7F); }

// ---------- ID3v2 ----------
static String id3Text(const uint8_t *d, size_t n) {
  if (n < 2) return String();
  uint8_t enc = d[0]; d++; n--;
  String s;
  if (enc == 0) s = latin1ToUtf8(d, n);
  else if (enc == 3) s = rawUtf8(d, n);
  else {
    bool le = false;
    if (enc == 1 && n >= 2) {
      if (d[0] == 0xFF && d[1] == 0xFE) { le = true; d += 2; n -= 2; }
      else if (d[0] == 0xFE && d[1] == 0xFF) { d += 2; n -= 2; }
    }
    for (size_t i = 0; i + 1 < n; i += 2) {
      uint32_t u = le ? (d[i] | (d[i + 1] << 8)) : ((d[i] << 8) | d[i + 1]);
      if (u == 0) break;
      if (u >= 0xD800 && u <= 0xDBFF && i + 3 < n) {
        uint32_t lo = le ? (d[i + 2] | (d[i + 3] << 8)) : ((d[i + 2] << 8) | d[i + 3]);
        u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
        i += 2;
      }
      appendUtf8(s, u);
    }
  }
  s.trim();
  return s;
}

static void id3Assign(TrackTags &t, const char *id, const String &v) {
  if (!v.length()) return;
  if (!strcmp(id, "TIT2") || !strcmp(id, "TT2")) t.title = v;
  else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1")) t.artist = v;
  else if (!strcmp(id, "TALB") || !strcmp(id, "TAL")) t.album = v;
  else if (!strcmp(id, "TPE2") || !strcmp(id, "TP2")) t.albumArtist = v;
  else if (!strcmp(id, "TCON") || !strcmp(id, "TCO")) t.genre = genreName(v);
  else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK")) t.track = v.toInt();
  else if (!strcmp(id, "TPOS") || !strcmp(id, "TPA")) t.disc = v.toInt();
  else if (!strcmp(id, "TDRC") || !strcmp(id, "TYER") || !strcmp(id, "TYE")) { if (!t.year.length()) t.year = v.substring(0, 4); }
}

static bool id3Wanted(const char *id) {
  static const char *const W[] = {"TIT2","TPE1","TALB","TPE2","TCON","TRCK","TPOS","TDRC","TYER",
                                  "TT2","TP1","TAL","TP2","TCO","TRK","TPA","TYE"};
  for (auto w : W) if (!strcmp(id, w)) return true;
  return false;
}

// Returns the offset just past the ID3v2 tag (0 if none). Cover art is skipped, never read.
static uint32_t parseID3v2(fs::File &f, TrackTags &t) {
  uint8_t h[10];
  f.seek(0);
  if (f.read(h, 10) != 10 || memcmp(h, "ID3", 3) != 0) return 0;
  uint8_t ver = h[3], flags = h[5];
  uint32_t size = syncsafe(h + 6);
  uint32_t end = 10 + size;
  uint32_t tagEnd = end + ((flags & 0x10) ? 10 : 0);
  uint32_t pos = 10;
  if ((flags & 0x40) && ver >= 3) {          // extended header
    uint8_t e[4]; f.read(e, 4);
    pos += (ver == 4) ? syncsafe(e) : be32(e) + 4;
  }
  uint8_t buf[512];
  while (pos < end) {
    f.seek(pos);
    char id[5] = {0};
    uint32_t fs; uint16_t fflags = 0;
    if (ver == 2) {
      uint8_t fh[6];
      if (pos + 6 > end || f.read(fh, 6) != 6 || fh[0] == 0) break;
      memcpy(id, fh, 3);
      fs = ((uint32_t)fh[3] << 16) | (fh[4] << 8) | fh[5];
      pos += 6;
    } else {
      uint8_t fh[10];
      if (pos + 10 > end || f.read(fh, 10) != 10 || fh[0] == 0) break;
      memcpy(id, fh, 4);
      fs = (ver == 4) ? syncsafe(fh + 4) : be32(fh + 4);
      fflags = (fh[8] << 8) | fh[9];
      pos += 10;
    }
    if (fs == 0 || pos + fs > end) break;
    bool skip = (ver == 3 && (fflags & 0x00C0)) || (ver == 4 && (fflags & 0x000C));  // compressed/encrypted
    if (!skip && id3Wanted(id) && fs <= sizeof(buf)) {
      uint32_t off = (ver == 4 && (fflags & 0x0001)) ? 4 : 0;   // data length indicator
      if (fs > off && f.read(buf, fs) == (int)fs) id3Assign(t, id, id3Text(buf + off, fs - off));
    }
    pos += fs;
  }
  return tagEnd;
}

// ID3v1 at the end of the file; only fills fields that are still empty
static bool parseID3v1(fs::File &f, TrackTags &t) {
  uint32_t sz = f.size();
  if (sz < 128) return false;
  uint8_t b[128];
  f.seek(sz - 128);
  if (f.read(b, 128) != 128 || memcmp(b, "TAG", 3) != 0) return false;
  auto field = [&](int off, int len) {
    uint8_t tmp[31]; memcpy(tmp, b + off, len); tmp[len] = 0;
    String s = latin1ToUtf8(tmp, len); s.trim(); return s;
  };
  if (!t.title.length()) t.title = field(3, 30);
  if (!t.artist.length()) t.artist = field(33, 30);
  if (!t.album.length()) t.album = field(63, 30);
  if (!t.year.length()) t.year = field(93, 4);
  if (!t.track && b[125] == 0 && b[126]) t.track = b[126];
  if (!t.genre.length() && b[127] < ID3_GENRE_COUNT) t.genre = ID3_GENRES[b[127]];
  return true;
}

// ---------- MP3 frame header / duration ----------
static const uint16_t MP3_BR_V1[16] = {0,32,40,48,56,64,80,96,112,128,160,192,224,256,320,0};
static const uint16_t MP3_BR_V2[16] = {0,8,16,24,32,40,48,56,64,80,96,112,128,144,160,0};

struct Mp3Hdr { int ver; uint32_t sr, br, len, spf; bool mono; };   // ver: 1, 2 (also 2.5)

static bool mp3Header(const uint8_t *b, Mp3Hdr &h) {
  if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) return false;
  int vbits = (b[1] >> 3) & 3, layer = (b[1] >> 1) & 3;
  if (vbits == 1 || layer != 1) return false;              // Layer III only
  int bri = b[2] >> 4, sri = (b[2] >> 2) & 3, pad = (b[2] >> 1) & 1;
  if (bri == 0 || bri == 15 || sri == 3) return false;
  static const uint32_t SR[3][3] = {{11025,12000,8000},{22050,24000,16000},{44100,48000,32000}};
  h.ver = (vbits == 3) ? 1 : 2;
  h.sr = SR[vbits == 3 ? 2 : (vbits == 2 ? 1 : 0)][sri];
  h.br = (h.ver == 1 ? MP3_BR_V1 : MP3_BR_V2)[bri];
  h.spf = (h.ver == 1) ? 1152 : 576;
  h.len = (h.ver == 1 ? 144000 : 72000) * h.br / h.sr + pad;
  h.mono = (b[3] >> 6) == 3;
  return h.len > 4;
}

static void mp3Duration(fs::File &f, uint32_t start, bool hasV1, TrackTags &t) {
  static uint8_t buf[4096];
  f.seek(start);
  int n = f.read(buf, sizeof(buf));
  for (int i = 0; i + 4 < n; i++) {
    Mp3Hdr h;
    if (!mp3Header(buf + i, h)) continue;
    // confirm with the next frame header so we don't lock onto a false sync
    if (i + (int)h.len + 4 < n) { Mp3Hdr h2; if (!mp3Header(buf + i + h.len, h2)) continue; }
    t.dataStart = start + i;
    t.sampleRate = h.sr; t.channels = h.mono ? 1 : 2; t.bits = 16;
    int xo = (h.ver == 1) ? (h.mono ? 21 : 36) : (h.mono ? 13 : 21);
    uint32_t frames = 0;
    if (i + xo + 12 < n && (!memcmp(buf + i + xo, "Xing", 4) || !memcmp(buf + i + xo, "Info", 4))) {
      if (be32(buf + i + xo + 4) & 1) frames = be32(buf + i + xo + 8);
    } else if (i + 36 + 18 < n && !memcmp(buf + i + 36, "VBRI", 4)) {
      frames = be32(buf + i + 36 + 14);
    }
    if (frames) t.duration = (uint32_t)((uint64_t)frames * h.spf / h.sr);
    else {
      uint32_t audio = f.size() - t.dataStart - (hasV1 ? 128 : 0);
      t.duration = (uint32_t)((uint64_t)audio * 8 / (h.br * 1000));
    }
    return;
  }
  t.dataStart = start;
}

static bool readMp3Tags(fs::File &f, TrackTags &t) {
  uint32_t start = parseID3v2(f, t);
  bool v1 = parseID3v1(f, t);
  mp3Duration(f, start, v1, t);
  return true;
}

// ---------- FLAC ----------
static void vorbisAssign(TrackTags &t, const String &kv) {
  int eq = kv.indexOf('=');
  if (eq <= 0) return;
  String k = kv.substring(0, eq); k.toUpperCase();
  String v = kv.substring(eq + 1); v.trim();
  if (!v.length()) return;
  if (k == "TITLE") { if (!t.title.length()) t.title = v; }
  else if (k == "ARTIST") { if (!t.artist.length()) t.artist = v; }
  else if (k == "ALBUM") { if (!t.album.length()) t.album = v; }
  else if (k == "ALBUMARTIST" || k == "ALBUM ARTIST") { if (!t.albumArtist.length()) t.albumArtist = v; }
  else if (k == "GENRE") { if (!t.genre.length()) t.genre = genreName(v); }
  else if (k == "TRACKNUMBER") t.track = v.toInt();
  else if (k == "DISCNUMBER") t.disc = v.toInt();
  else if (k == "DATE" || k == "YEAR") { if (!t.year.length()) t.year = v.substring(0, 4); }
}

static bool readFlacTags(fs::File &f, TrackTags &t) {
  TrackTags dummy;
  uint32_t pos = parseID3v2(f, dummy);   // rare ID3 wrapper before "fLaC"
  uint8_t m[4];
  f.seek(pos);
  if (f.read(m, 4) != 4 || memcmp(m, "fLaC", 4) != 0) return false;
  pos += 4;
  uint32_t sz = f.size();
  for (;;) {
    uint8_t h[4];
    f.seek(pos);
    if (f.read(h, 4) != 4) return false;
    bool last = h[0] & 0x80;
    uint8_t type = h[0] & 0x7F;
    uint32_t len = ((uint32_t)h[1] << 16) | (h[2] << 8) | h[3];
    pos += 4;
    if (pos + len > sz) return false;
    if (type == 0 && len >= 34) {                 // STREAMINFO
      uint8_t b[34]; f.read(b, 34);
      t.sampleRate = ((uint32_t)b[10] << 12) | (b[11] << 4) | (b[12] >> 4);
      t.channels = ((b[12] >> 1) & 7) + 1;
      t.bits = (((b[12] & 1) << 4) | (b[13] >> 4)) + 1;
      uint64_t total = ((uint64_t)(b[13] & 0x0F) << 32) | be32(b + 14);
      if (t.sampleRate) t.duration = (uint32_t)(total / t.sampleRate);
    } else if (type == 4) {                        // VORBIS_COMMENT, read entry by entry
      uint8_t l[4];
      f.read(l, 4);
      uint32_t p = pos + 4 + le32(l);
      f.seek(p);
      f.read(l, 4); p += 4;
      uint32_t count = le32(l);
      uint8_t buf[512];
      for (uint32_t i = 0; i < count && p + 4 <= pos + len; i++) {
        f.seek(p); f.read(l, 4); p += 4;
        uint32_t el = le32(l);
        if (el < sizeof(buf) && f.read(buf, el) == (int)el) vorbisAssign(t, rawUtf8(buf, el));
        p += el;
      }
    }
    pos += len;                                    // PICTURE/PADDING skipped, never read
    if (last) break;
  }
  t.dataStart = pos;
  return true;
}

// Fallbacks so every track lands somewhere sensible:
//   /Music/Artist/Album/03 Song.flac -> title "03 Song", album "Album", artist "Artist"
static void applyFallbacks(TrackTags &t, const String &path) {
  if (!t.title.length()) t.title = pathStem(path);
  String dir = pathDir(path);
  if (!t.album.length()) t.album = (dir == MUSIC_DIR) ? String("Unknown Album") : pathBase(dir);
  if (!t.artist.length()) {
    String up = pathDir(dir);
    t.artist = (dir == MUSIC_DIR || up == MUSIC_DIR || up == "/") ? String("Unknown Artist") : pathBase(up);
  }
  if (!t.albumArtist.length()) t.albumArtist = t.artist;
  if (!t.genre.length()) t.genre = "Unknown Genre";
}
