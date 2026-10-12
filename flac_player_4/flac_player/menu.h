// menu.h — screenless "virtual menu": every item is spoken by the robotic voice
//
//  Home ─┬─ Now Playing        (continues the last song where it stopped)
//        ├─ Music ─┬─ Albums ────────────── album ─ songs
//        │         ├─ Artists ─ artist ──── albums ─ songs
//        │         ├─ Random Album         (starts playing)
//        │         ├─ Genre ─── genre ───── albums ─ songs
//        │         └─ All Songs ─ songs
//        └─ Settings ─ "Scan library?"    (Play = quick scan, hold Play = full scan)
//
//  Menus:        Prev/Next move (hold = fast), Play select, Home back, hold Home = home
//  Now Playing:  Play pause, Prev/Next song (title is spoken), hold Play = shuffle,
//                hold Prev = repeat off/all/one
//  Always:       Vol+ / Vol-
//
//  Everything that is spoken is also printed on Serial (115200) for debugging.
#pragma once
#include <Arduino.h>
#include "config.h"
#include "textutil.h"
#include "library.h"
#include "engine.h"
#include "buttons.h"

enum ViewType : uint8_t { V_HOME, V_MUSIC, V_ALBUMS, V_ARTISTS, V_GENRES, V_TRACKS, V_ALL_SONGS, V_NOW_PLAYING, V_SCAN_CONFIRM };
enum AlbumScope : uint8_t { SCOPE_ALL, SCOPE_ARTIST, SCOPE_GENRE };

struct View {
  uint8_t type = V_HOME, scope = SCOPE_ALL;
  int parent = -1, sel = 0;
};

static const char *const HOME_ITEMS[] = {"Now Playing", "Music", "Settings"};
static const char *const HOME_VOICES[] = {"home_now_playing", "home_music", "home_settings"};
static const char *const MUSIC_ITEMS[] = {"Albums", "Artists", "Random Album", "Genre", "All Songs"};
static const char *const MUSIC_VOICES[] = {"music_albums", "music_artists", "music_random", "music_genre", "music_all_songs"};

class Menu {
public:
  Menu(Library &l, Engine &e) : lib(l), eng(e) {}

  void begin() {
    btnPrev.begin(PIN_BTN_PREV, "PREV"); btnNext.begin(PIN_BTN_NEXT, "NEXT"); btnPlay.begin(PIN_BTN_PLAY, "PLAY");
    btnHome.begin(PIN_BTN_HOME, "HOME"); btnVolUp.begin(PIN_BTN_VOLUP, "VOL+"); btnVolDn.begin(PIN_BTN_VOLDN, "VOL-");
    depth = 0; stack[0] = View();
  }

  void (*powerOff)() = nullptr;     // set by the sketch: hold Home 5 s

  void enterHome() { depth = 0; stack[0] = View(); onSelectionChanged(); }

  void poll() {
    uint32_t now = millis();
    View &v = stack[depth];
    uint8_t ePrev = btnPrev.poll(now), eNext = btnNext.poll(now), ePlay = btnPlay.poll(now);
    uint8_t eHome = btnHome.poll(now), eUp = btnVolUp.poll(now), eDn = btnVolDn.poll(now);

    if (eUp & (EV_PRESS | EV_REPEAT)) eng.setVolume(eng.getVolume() + 1);
    if (eDn & (EV_PRESS | EV_REPEAT)) eng.setVolume(eng.getVolume() - 1);

    if (btnHome.down && !btnHome.ignore && now - btnHome.t0 >= POWER_OFF_HOLD_MS && powerOff) powerOff();
    if (eHome & EV_LONG) { enterHome(); return; }
    if (eHome & EV_SHORT) { back(); return; }

    if (v.type == V_NOW_PLAYING) {
      if (ePlay & EV_SHORT) eng.togglePause();
      if (ePlay & EV_LONG) {
        bool on = eng.toggleShuffle();
        say(on ? "shuffle_on" : "shuffle_off", true);
      }
      if (ePrev & EV_LONG) {
        static const char *const K[] = {"repeat_off", "repeat_all", "repeat_one"};
        say(K[eng.cycleRepeat()], true);
      }
      if (ePrev & EV_SHORT) eng.prev();
      if (eNext & EV_SHORT) eng.next();
    } else if (v.type == V_SCAN_CONFIRM) {
      if (ePlay & EV_SHORT) runScan(true);
      if (ePlay & EV_LONG) runScan(false);
    } else {
      int n = count(v);
      if (n > 0 && ((ePrev | eNext) & (EV_PRESS | EV_REPEAT))) {
        int step = (eNext & (EV_PRESS | EV_REPEAT)) ? 1 : -1;
        bool held = (ePrev | eNext) & EV_REPEAT;
        if (held && now - max(btnPrev.t0, btnNext.t0) > 2000) step *= 5;   // after 2 s, jump 5 at a time
        int s = v.sel + step;
        if (s < 0) s = (step == -1) ? n - 1 : 0;   // single steps wrap around
        if (s >= n) s = (step == 1) ? 0 : n - 1;
        v.sel = s;
        onSelectionChanged();
      }
      if ((ePlay & EV_SHORT) && n > 0) select(v);
    }
  }

  // Scan progress: a soft beep every 15 s so you know it is still working
  static Menu *self;
  static void scanProgress(const char *stage, uint32_t done, uint32_t total, const char *item) {
    Menu *m = self;
    uint32_t now = millis();
    if (now - m->lastScanTalk < 15000) return;
    m->lastScanTalk = now;
    Serial.printf("[scan] %s %lu/%lu  (%s)\n", stage, (unsigned long)done, (unsigned long)total, item);
    m->eng.beep(880, 60);
  }

private:
  Library &lib;
  Engine &eng;
  Button btnPrev, btnNext, btnPlay, btnHome, btnVolUp, btnVolDn;
  View stack[8];
  int depth = 0;
  uint32_t lastScanTalk = 0;

  void say(const char *menuKey, bool immediate = false) {
    Serial.printf("[say] %s\n", menuKey);
    eng.speak(lib.menuVoice(menuKey), immediate);
  }

  // ---------- navigation ----------
  void push(const View &nv) {
    if (depth < 7) depth++;
    stack[depth] = nv;
    onSelectionChanged();
  }
  void back() {
    if (depth > 0) depth--;
    onSelectionChanged();         // at the top level this repeats the current item
  }

  void onSelectionChanged() {
    View &v = stack[depth];
    int n = count(v);
    if (v.sel >= n) v.sel = n ? n - 1 : 0;
    switch (v.type) {
      case V_NOW_PLAYING: eng.speak(""); return;      // no talking over the music
      case V_SCAN_CONFIRM: say("scan_confirm"); return;
      default:
        if (n == 0) { say("empty"); return; }
        Serial.printf("[menu] %s > %d/%d %s\n", title(v).c_str(), v.sel + 1, n, label(v, v.sel).c_str());
        eng.speak(voice(v, v.sel));
    }
  }

  int albumAt(const View &v, int i) {
    if (v.scope == SCOPE_ARTIST) return lib.artists[v.parent]["albums"][i] | -1;
    if (v.scope == SCOPE_GENRE) return lib.genres[v.parent]["albums"][i] | -1;
    return i;
  }

  int count(const View &v) {
    switch (v.type) {
      case V_HOME: return 3;
      case V_MUSIC: return 5;
      case V_ALBUMS:
        if (v.scope == SCOPE_ARTIST) return lib.artists[v.parent]["albums"].size();
        if (v.scope == SCOPE_GENRE) return lib.genres[v.parent]["albums"].size();
        return lib.albums.size();
      case V_ARTISTS: return lib.artists.size();
      case V_GENRES: return lib.genres.size();
      case V_TRACKS: return lib.albums[v.parent]["tracks"].size();
      case V_ALL_SONGS: return lib.allSongs.size();
      default: return 0;
    }
  }

  // label/title are only used for the Serial log
  String label(const View &v, int i) {
    switch (v.type) {
      case V_HOME: return HOME_ITEMS[i];
      case V_MUSIC: return MUSIC_ITEMS[i];
      case V_ALBUMS: return Library::str(lib.albums[albumAt(v, i)], "name");
      case V_ARTISTS: return Library::str(lib.artists[i], "name");
      case V_GENRES: return Library::str(lib.genres[i], "name");
      case V_TRACKS: return Library::str(lib.tracks[lib.albums[v.parent]["tracks"][i] | 0], "title");
      case V_ALL_SONGS: return Library::str(lib.tracks[lib.allSongs[i]], "title");
      default: return "";
    }
  }
  String title(const View &v) {
    switch (v.type) {
      case V_HOME: return "Home";
      case V_MUSIC: return "Music";
      case V_ALBUMS:
        if (v.scope == SCOPE_ARTIST) return Library::str(lib.artists[v.parent], "name");
        if (v.scope == SCOPE_GENRE) return Library::str(lib.genres[v.parent], "name");
        return "Albums";
      case V_ARTISTS: return "Artists";
      case V_GENRES: return "Genre";
      case V_TRACKS: return Library::str(lib.albums[v.parent], "name");
      case V_ALL_SONGS: return "All Songs";
      default: return "";
    }
  }

  String voice(const View &v, int i) {
    switch (v.type) {
      case V_HOME: return lib.menuVoice(HOME_VOICES[i]);
      case V_MUSIC: return lib.menuVoice(MUSIC_VOICES[i]);
      case V_ALBUMS: return Library::str(lib.albums[albumAt(v, i)], "voice");
      case V_ARTISTS: return Library::str(lib.artists[i], "voice");
      case V_GENRES: return Library::str(lib.genres[i], "voice");
      case V_TRACKS: return Library::str(lib.tracks[lib.albums[v.parent]["tracks"][i] | 0], "voice");
      case V_ALL_SONGS: return Library::str(lib.tracks[lib.allSongs[i]], "voice");
      default: return "";
    }
  }

  void select(View &v) {
    View nv;
    switch (v.type) {
      case V_HOME:
        if (v.sel == 0) {
          if (!eng.status().active && !eng.resumeLast()) { say("nothing_to_resume", true); return; }
          nv.type = V_NOW_PLAYING; push(nv);
        } else if (v.sel == 1) { nv.type = V_MUSIC; push(nv); }
        else { nv.type = V_SCAN_CONFIRM; push(nv); }
        return;
      case V_MUSIC:
        if (!lib.loaded || lib.tracks.empty()) { say("no_library", true); return; }
        if (v.sel == 0) { nv.type = V_ALBUMS; nv.scope = SCOPE_ALL; push(nv); }
        else if (v.sel == 1) { nv.type = V_ARTISTS; push(nv); }
        else if (v.sel == 2) {
          int a = lib.randomAlbum();
          if (a < 0) return;
          eng.playAlbum(a, 0);
          nv.type = V_NOW_PLAYING; push(nv);
          Serial.printf("[menu] random album: %s\n", Library::str(lib.albums[a], "name"));
          eng.speak(Library::str(lib.albums[a], "voice"), true);   // say which album it picked
        }
        else if (v.sel == 3) { nv.type = V_GENRES; push(nv); }
        else { nv.type = V_ALL_SONGS; push(nv); }
        return;
      case V_ALBUMS: nv.type = V_TRACKS; nv.parent = albumAt(v, v.sel); push(nv); return;
      case V_ARTISTS: nv.type = V_ALBUMS; nv.scope = SCOPE_ARTIST; nv.parent = v.sel; push(nv); return;
      case V_GENRES: nv.type = V_ALBUMS; nv.scope = SCOPE_GENRE; nv.parent = v.sel; push(nv); return;
      case V_TRACKS:
        eng.cancelVoice();
        eng.playAlbum(v.parent, v.sel);
        nv.type = V_NOW_PLAYING; push(nv); return;
      case V_ALL_SONGS:
        eng.cancelVoice();
        eng.playAllSongs(v.sel);
        nv.type = V_NOW_PLAYING; push(nv); return;
      default: return;
    }
  }

  void runScan(bool fast) {
    eng.stopAll();
    self = this;
    say(fast && lib.loaded ? "scan_quick" : "scan_full", true);
    delay(2500);                                   // let the clip play before the SD gets busy
    lastScanTalk = millis();
    uint32_t t0 = millis();
    bool ok = lib.scan(fast, scanProgress);
    Serial.printf("[scan] %s, %u songs, %u albums in %lu s\n", ok ? "done" : "FAILED", (unsigned)lib.tracks.size(),
                  (unsigned)lib.albums.size(), (unsigned long)((millis() - t0) / 1000));
    eng.resetQueue();
    enterHome();
    say(ok ? "scan_done" : "scan_failed", true);
  }
};
Menu *Menu::self = nullptr;
