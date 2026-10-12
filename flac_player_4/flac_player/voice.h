// voice.h — where voice clips live on the SD card.
// The clips are made on the PC by Voice Studio (Piper); the player only plays them.
// The names are a hash of the text, so the PC and the player always agree on them.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include "config.h"
#include "textutil.h"

static void mkdirs(const String &path) {          // like mkdir -p
  int i = 1;
  while (i > 0) {
    i = path.indexOf('/', i + 1);
    String part = (i > 0) ? path.substring(0, i) : path;
    if (!SD.exists(part)) SD.mkdir(part);
  }
}

struct VoicePaths {
  // song titles, albums, artists, genres
  static String library(const String &text) {
    String h = hex64(fnv64(samText(text)));
    return String(VOICE_LIB_DIR) + "/" + h.substring(0, 1) + "/" + h + ".wav";
  }
  // menu clips: key + text hash, so editing a menu text needs a new clip
  static String menu(const char *key, const char *text) {
    return String(VOICE_MENU_DIR) + "/" + key + "_" + hex64(fnv64(String(text))).substring(0, 6) + ".wav";
  }
};
