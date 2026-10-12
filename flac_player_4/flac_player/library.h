// library.h — scans /Music, builds /System/library.json (albums, artists, genres)
// and loads the index on boot. Voice clips are made on the PC by Voice Studio; a scan
// here just links each item to its clip's file name (a short tick plays if it's missing).
#pragma once
#include <Arduino.h>
#include <SD.h>
#include <map>
#include <algorithm>
#include "config.h"
#include "psram.h"
#include "textutil.h"
#include "tags.h"
#include "voice.h"

#define LIBRARY_VERSION 1

// Menu items and spoken feedback that get a voice file. key -> spoken text
struct MenuVoice { const char *key; const char *text; };
static const MenuVoice MENU_VOICES[] = {
  {"home_now_playing", "Now playing"}, {"home_music", "Music"}, {"home_settings", "Settings"},
  {"music_albums", "Albums"}, {"music_artists", "Artists"}, {"music_random", "Random album"},
  {"music_genre", "Genre"}, {"music_all_songs", "All songs"},
  {"scan_confirm", "Scan library? Press play for a quick scan. Hold play for a full scan. Press home to cancel."},
  {"shuffle_on", "Shuffle on"}, {"shuffle_off", "Shuffle off"},
  {"repeat_off", "Repeat off"}, {"repeat_all", "Repeat all"}, {"repeat_one", "Repeat one"},
  {"no_library", "No library. Go to settings and scan."},
  {"nothing_to_resume", "Nothing to resume"}, {"empty", "Empty"},
  {"scan_quick", "Quick scan. Please wait."}, {"scan_full", "Full scan. Please wait."},
  {"scan_done", "Scan complete."}, {"scan_failed", "Scan failed. Check the SD card."},
  {"power_off", "Powering off."},
  {"music_letters", "Letter album"},
  {"letter_a", "A"}, {"letter_b", "B"}, {"letter_c", "C"}, {"letter_d", "D"}, {"letter_e", "E"},
  {"letter_f", "F"}, {"letter_g", "G"}, {"letter_h", "H"}, {"letter_i", "I"}, {"letter_j", "J"},
  {"letter_k", "K"}, {"letter_l", "L"}, {"letter_m", "M"}, {"letter_n", "N"}, {"letter_o", "O"},
  {"letter_p", "P"}, {"letter_q", "Q"}, {"letter_r", "R"}, {"letter_s", "S"}, {"letter_t", "T"},
  {"letter_u", "U"}, {"letter_v", "V"}, {"letter_w", "W"}, {"letter_x", "X"}, {"letter_y", "Y"},
  {"letter_z", "Z"}, {"letter_other", "Numbers and symbols"},
  {"settings_scan", "Scan library"},
  {"duck_on", "Duck music on"}, {"duck_off", "Duck music off"},
  {"fade_on", "Soft fade on"}, {"fade_off", "Soft fade off"},
  {"voltick_on", "Volume ticks on"}, {"voltick_off", "Volume ticks off"},
  {"chime_on", "Startup chime on"}, {"chime_off", "Startup chime off"},
};
static const size_t MENU_VOICE_COUNT = sizeof(MENU_VOICES) / sizeof(MENU_VOICES[0]);

// Accent/case-folding compare without allocating; skips a leading "The "
static const char *skipThe(const char *s) {
  if ((s[0] == 'T' || s[0] == 't') && (s[1] == 'H' || s[1] == 'h') && (s[2] == 'E' || s[2] == 'e') && s[3] == ' ') return s + 4;
  return s;
}
static int foldCmp(const char *a, const char *b) {
  a = skipThe(a); b = skipThe(b);
  const char *fa = "", *fb = "";
  for (;;) {
    if (!*fa) { if (!*a) break; uint32_t cp = utf8Next(a); const char *f = asciiFold(cp); static char ca[2]; ca[0] = cp < 0x80 ? (char)cp : '~'; ca[1] = 0; fa = f ? f : ca; }
    if (!*fb) { if (!*b) { return 1; } uint32_t cp = utf8Next(b); const char *f = asciiFold(cp); static char cb[2]; cb[0] = cp < 0x80 ? (char)cp : '~'; cb[1] = 0; fb = f ? f : cb; }
    int x = tolower((unsigned char)*fa++), y = tolower((unsigned char)*fb++);
    if (x != y) return x - y;
  }
  return (*fb || *b) ? -1 : 0;
}

typedef void (*ScanProgressFn)(const char *stage, uint32_t done, uint32_t total, const char *item);

class Library {
public:
  Library() : doc(&spiRamAllocator) {}

  bool loaded = false;
  PVec<JsonObject> tracks, albums, artists, genres;
  PVec<uint16_t> allSongs;
  // Letter Album: first letters that have albums ('a'-'z', then '#' for anything else).
  // The albums of letters[i] are letterAlbums[letterStart[i] .. letterStart[i + 1]).
  PVec<char> letters;
  PVec<uint16_t> letterStart, letterAlbums;

  // Same folding as the album sort ("The " skipped, accents folded), so a letter's
  // albums come in the same order as in Albums
  static char letterOf(JsonObject album) {
    String k = sortKey(String(str(album, "name")));
    char c = k.length() ? k[0] : 0;
    return (c >= 'a' && c <= 'z') ? c : '#';
  }

  static const char *str(JsonObject o, const char *key) { const char *s = o[key]; return s ? s : ""; }

  // Menu clips don't depend on the library, so they work before the first scan.
  String menuVoice(const char *key) {
    for (size_t i = 0; i < MENU_VOICE_COUNT; i++)
      if (!strcmp(MENU_VOICES[i].key, key)) return VoicePaths::menu(key, MENU_VOICES[i].text);
    return String();
  }

  // Called at boot: reports menu clips that Voice Studio hasn't made yet
  int checkMenuVoices() {
    int missing = 0;
    for (size_t i = 0; i < MENU_VOICE_COUNT; i++)
      if (!SD.exists(VoicePaths::menu(MENU_VOICES[i].key, MENU_VOICES[i].text))) {
        Serial.printf("[voice] missing menu clip: %s \"%s\"\n", MENU_VOICES[i].key, MENU_VOICES[i].text);
        missing++;
      }
    return missing;
  }

  bool load() {
    loaded = false;
    clearIndex();
    fs::File f = SD.open(LIBRARY_JSON, FILE_READ);
    if (!f) return false;
    size_t len = f.size();
    char *buf = (char *)psramMalloc(len + 1);
    if (!buf) { f.close(); return false; }
    size_t got = 0;
    while (got < len) {
      int r = f.read((uint8_t *)buf + got, min((size_t)32768, len - got));
      if (r <= 0) break;
      got += r;
    }
    f.close();
    DeserializationError err = deserializeJson(doc, (const char *)buf, got);
    heap_caps_free(buf);
    if (err || (doc["version"] | 0) != LIBRARY_VERSION) { doc.clear(); return false; }
    buildIndex();
    loaded = true;
    return true;
  }

  // Full scan. fast = reuse tags/voices of files whose size and date are unchanged.
  bool scan(bool fast, ScanProgressFn cb) {
    JsonDocument nd(&spiRamAllocator);
    nd["version"] = LIBRARY_VERSION;
    JsonArray ntracks = nd["tracks"].to<JsonArray>();

    // ---- lookup table for the old index (path hash -> track) ----
    PVec<std::pair<uint64_t, uint16_t>> old;
    if (fast && loaded) {
      old.reserve(tracks.size());
      for (size_t i = 0; i < tracks.size(); i++) old.push_back({fnv64(String(str(tracks[i], "path"))), (uint16_t)i});
      std::sort(old.begin(), old.end());
    }

    // ---- 1. walk /Music ----
    std::vector<String> dirs;
    dirs.push_back(MUSIC_DIR);
    PVec<uint8_t> reusedFlag;
    uint32_t found = 0;
    while (!dirs.empty()) {
      String d = dirs.back(); dirs.pop_back();
      fs::File dir = SD.open(d);
      if (!dir || !dir.isDirectory()) continue;
      for (fs::File e = dir.openNextFile(); e; e = dir.openNextFile()) {
        String name = pathBase(String(e.name()));
        String path = String(e.path());
        if (name.startsWith(".")) { e.close(); continue; }           // macOS ._ files etc.
        if (e.isDirectory()) { dirs.push_back(path); e.close(); continue; }
        String lower = name; lower.toLowerCase();
        bool flac = lower.endsWith(".flac"), mp3 = lower.endsWith(".mp3");
        if (!flac && !mp3) { e.close(); continue; }
        uint32_t size = e.size(), mtime = (uint32_t)e.getLastWrite();

        bool reused = false;
        if (!old.empty()) {
          uint64_t h = fnv64(path);
          auto it = std::lower_bound(old.begin(), old.end(), std::make_pair(h, (uint16_t)0));
          for (; it != old.end() && it->first == h; ++it) {
            JsonObject o = tracks[it->second];
            if ((o["size"] | 0u) == size && (o["mtime"] | 0u) == mtime && path == str(o, "path")) {
              JsonObject t = ntracks.add<JsonObject>();
              t.set(o);
              reused = true;
              break;
            }
          }
        }
        if (!reused) {
          TrackTags tg;
          bool ok = flac ? readFlacTags(e, tg) : readMp3Tags(e, tg);
          if (ok) {
            applyFallbacks(tg, path);
            JsonObject t = ntracks.add<JsonObject>();
            t["path"] = path;
            t["title"] = tg.title;
            t["artist"] = tg.artist;
            t["album"] = tg.album;
            t["albumArtist"] = tg.albumArtist;
            t["genre"] = tg.genre;
            t["year"] = tg.year;
            t["track"] = tg.track;
            t["disc"] = tg.disc;
            t["duration"] = tg.duration;
            t["format"] = flac ? "flac" : "mp3";
            t["sampleRate"] = tg.sampleRate;
            t["bits"] = tg.bits;
            t["channels"] = tg.channels;
            t["size"] = size;
            t["mtime"] = mtime;
            t["dataStart"] = tg.dataStart;
          }
        }
        e.close();
        if (ntracks.size() > reusedFlag.size()) reusedFlag.push_back(reused ? 1 : 0);
        found++;
        if (cb && (found % 4 == 0)) cb("Scanning", found, 0, name.c_str());
        if (ntracks.size() >= 65000) break;
      }
      dir.close();
    }

    PVec<JsonObject> T;
    for (JsonObject o : ntracks) T.push_back(o);
    uint32_t nT = T.size();
    if (cb) cb("Sorting", 0, 0, "");

    // ---- 2. group into albums (album artist + album name) ----
    struct Group { PVec<uint16_t> items; int year = 9999; };
    std::map<String, int> albumMap, artistMap, genreMap;
    std::vector<Group> albumG, artistG, genreG;
    PVec<int> trackAlbum(nT), trackGenre(nT), albumArtistOf, albumFirst, genreFirst;

    auto groupOf = [](std::map<String, int> &m, std::vector<Group> &g, const String &key) {
      auto it = m.find(key);
      if (it != m.end()) return it->second;
      int id = g.size(); m[key] = id; g.emplace_back(); return id;
    };

    for (uint32_t i = 0; i < nT; i++) {
      String aa = str(T[i], "albumArtist"), al = str(T[i], "album");
      int a = groupOf(albumMap, albumG, sortKey(aa) + "\x1f" + sortKey(al));
      if ((int)albumFirst.size() <= a) { albumFirst.push_back(i); albumArtistOf.push_back(groupOf(artistMap, artistG, sortKey(aa))); artistG[albumArtistOf[a]].items.push_back(a); }
      albumG[a].items.push_back(i);
      int y = String(str(T[i], "year")).toInt();
      if (y > 0 && y < albumG[a].year) albumG[a].year = y;
      trackAlbum[i] = a;
      trackGenre[i] = groupOf(genreMap, genreG, sortKey(String(str(T[i], "genre"))));
      if ((int)genreFirst.size() <= trackGenre[i]) genreFirst.push_back(i);
    }
    // an album shows under every genre its tracks carry
    for (size_t a = 0; a < albumG.size(); a++) {
      std::vector<int> seen;
      for (uint16_t t : albumG[a].items) {
        int g = trackGenre[t];
        if (std::find(seen.begin(), seen.end(), g) == seen.end()) { seen.push_back(g); genreG[g].items.push_back(a); }
      }
    }

    // ---- 3. sort ----
    auto trackLess = [&](uint16_t x, uint16_t y) {
      int dx = T[x]["disc"] | 0, dy = T[y]["disc"] | 0;
      if (dx != dy) return dx < dy;
      int tx = T[x]["track"] | 0, ty = T[y]["track"] | 0;
      if (tx != ty) return tx < ty;
      return foldCmp(str(T[x], "title"), str(T[y], "title")) < 0;
    };
    for (auto &g : albumG) std::sort(g.items.begin(), g.items.end(), trackLess);

    auto albumName = [&](int a) { return str(T[albumFirst[a]], "album"); };
    auto artistName = [&](int r) { return str(T[albumFirst[artistG[r].items[0]]], "albumArtist"); };
    auto genreLabel = [&](int g) { return str(T[genreFirst[g]], "genre"); };

    PVec<int> albumOrder(albumG.size()), artistOrder(artistG.size()), genreOrder(genreG.size());
    for (size_t i = 0; i < albumOrder.size(); i++) albumOrder[i] = i;
    for (size_t i = 0; i < artistOrder.size(); i++) artistOrder[i] = i;
    for (size_t i = 0; i < genreOrder.size(); i++) genreOrder[i] = i;
    std::sort(albumOrder.begin(), albumOrder.end(), [&](int x, int y) {
      int c = foldCmp(albumName(x), albumName(y));
      return c ? c < 0 : foldCmp(artistName(albumArtistOf[x]), artistName(albumArtistOf[y])) < 0;
    });
    std::sort(artistOrder.begin(), artistOrder.end(), [&](int x, int y) { return foldCmp(artistName(x), artistName(y)) < 0; });
    std::sort(genreOrder.begin(), genreOrder.end(), [&](int x, int y) { return foldCmp(genreLabel(x), genreLabel(y)) < 0; });
    PVec<int> albumRank(albumG.size()), artistRank(artistG.size());
    for (size_t i = 0; i < albumOrder.size(); i++) albumRank[albumOrder[i]] = i;
    for (size_t i = 0; i < artistOrder.size(); i++) artistRank[artistOrder[i]] = i;

    for (auto &g : artistG) std::sort(g.items.begin(), g.items.end(), [&](uint16_t x, uint16_t y) {
      if (albumG[x].year != albumG[y].year) return albumG[x].year < albumG[y].year;    // by year, then name
      return foldCmp(albumName(x), albumName(y)) < 0;
    });
    for (auto &g : genreG) std::sort(g.items.begin(), g.items.end(), [&](uint16_t x, uint16_t y) { return albumRank[x] < albumRank[y]; });

    // ---- 4. write groups ----
    JsonArray jAlbums = nd["albums"].to<JsonArray>();
    for (int a : albumOrder) {
      JsonObject o = jAlbums.add<JsonObject>();
      o["name"] = albumName(a);
      o["artist"] = artistName(albumArtistOf[a]);
      o["artistId"] = artistRank[albumArtistOf[a]];
      o["genre"] = str(T[albumG[a].items[0]], "genre");
      o["year"] = albumG[a].year == 9999 ? 0 : albumG[a].year;
      uint32_t dur = 0;
      JsonArray ids = o["tracks"].to<JsonArray>();
      for (uint16_t t : albumG[a].items) { ids.add(t); dur += T[t]["duration"] | 0u; T[t]["albumId"] = albumRank[a]; }
      o["duration"] = dur;
    }
    JsonArray jArtists = nd["artists"].to<JsonArray>();
    for (int r : artistOrder) {
      JsonObject o = jArtists.add<JsonObject>();
      o["name"] = artistName(r);
      JsonArray ids = o["albums"].to<JsonArray>();
      for (uint16_t a : artistG[r].items) ids.add(albumRank[a]);
    }
    JsonArray jGenres = nd["genres"].to<JsonArray>();
    for (int g : genreOrder) {
      JsonObject o = jGenres.add<JsonObject>();
      o["name"] = genreLabel(g);
      JsonArray ids = o["albums"].to<JsonArray>();
      for (uint16_t a : genreG[g].items) ids.add(albumRank[a]);
    }
    PVec<uint16_t> songs(nT);
    for (uint32_t i = 0; i < nT; i++) songs[i] = i;
    std::sort(songs.begin(), songs.end(), [&](uint16_t x, uint16_t y) { return foldCmp(str(T[x], "title"), str(T[y], "title")) < 0; });
    JsonArray jAll = nd["allSongs"].to<JsonArray>();
    for (uint16_t s : songs) jAll.add(s);

    JsonObject counts = nd["counts"].to<JsonObject>();
    counts["tracks"] = nT; counts["albums"] = jAlbums.size(); counts["artists"] = jArtists.size(); counts["genres"] = jGenres.size();

    // ---- 5. voice clip names (same names Voice Studio uses) ----
    const uint32_t nMenu = MENU_VOICE_COUNT;
    uint32_t total = nMenu + nT + jAlbums.size() + jArtists.size() + jGenres.size(), done = 0;
    JsonObject mv = nd["menuVoices"].to<JsonObject>();
    for (size_t i = 0; i < MENU_VOICE_COUNT; i++) {
      mv[MENU_VOICES[i].key] = VoicePaths::menu(MENU_VOICES[i].key, MENU_VOICES[i].text);
      ++done;
    }
    auto voiceFor = [&](JsonObject o, const char *field, bool trustOld) {
      if (trustOld && o["voice"].is<const char *>()) return;     // unchanged file: keep its clip
      o["voice"] = VoicePaths::library(str(o, field));
    };
    for (uint32_t i = 0; i < nT; i++) {
      voiceFor(T[i], "title", fast && reusedFlag[i]);
      if (cb && (++done % 2 == 0)) cb("Voices", done, total, str(T[i], "title"));
    }
    for (JsonObject o : jAlbums)  { voiceFor(o, "name", false); if (cb) cb("Voices", ++done, total, str(o, "name")); }
    for (JsonObject o : jArtists) { voiceFor(o, "name", false); if (cb) cb("Voices", ++done, total, str(o, "name")); }
    for (JsonObject o : jGenres)  { voiceFor(o, "name", false); if (cb) cb("Voices", ++done, total, str(o, "name")); }

    // ---- 6. save (write tmp, then rename) ----
    if (cb) cb("Saving", 0, 0, "");
    mkdirs(SYSTEM_DIR);
    size_t len = measureJson(nd);
    char *buf = (char *)psramMalloc(len + 1);
    if (!buf) return false;
    serializeJson(nd, buf, len + 1);
    String tmp = String(LIBRARY_JSON) + ".tmp";
    fs::File f = SD.open(tmp, FILE_WRITE);
    bool ok = (bool)f;
    for (size_t off = 0; ok && off < len; off += 32768) {
      size_t chunk = min((size_t)32768, len - off);
      ok = f.write((const uint8_t *)buf + off, chunk) == chunk;
    }
    if (f) f.close();
    heap_caps_free(buf);
    if (!ok) { SD.remove(tmp); return false; }
    if (SD.exists(LIBRARY_JSON)) SD.remove(LIBRARY_JSON);
    if (!SD.rename(tmp, LIBRARY_JSON)) return false;

    // ---- 7. swap in the new index ----
    clearIndex();
    T.clear();
    doc = std::move(nd);
    buildIndex();
    loaded = true;
    return true;
  }

  int randomAlbum() { return albums.empty() ? -1 : (int)(esp_random() % albums.size()); }

private:
  JsonDocument doc;

  void clearIndex() {
    tracks.clear(); albums.clear(); artists.clear(); genres.clear(); allSongs.clear();
    letters.clear(); letterStart.clear(); letterAlbums.clear();
  }

  void buildIndex() {
    clearIndex();
    for (JsonObject o : doc["tracks"].as<JsonArray>()) tracks.push_back(o);
    for (JsonObject o : doc["albums"].as<JsonArray>()) albums.push_back(o);
    for (JsonObject o : doc["artists"].as<JsonArray>()) artists.push_back(o);
    for (JsonObject o : doc["genres"].as<JsonArray>()) genres.push_back(o);
    for (JsonVariant v : doc["allSongs"].as<JsonArray>()) allSongs.push_back(v.as<uint16_t>());
    buildLetters();
  }

  void buildLetters() {
    PVec<char> of(albums.size());
    for (size_t a = 0; a < albums.size(); a++) of[a] = letterOf(albums[a]);
    for (int i = 0; i < 27; i++) {
      char c = i < 26 ? 'a' + i : '#';
      size_t from = letterAlbums.size();
      for (size_t a = 0; a < albums.size(); a++) if (of[a] == c) letterAlbums.push_back(a);
      if (letterAlbums.size() > from) { letters.push_back(c); letterStart.push_back(from); }
    }
    letterStart.push_back(letterAlbums.size());
  }
};
