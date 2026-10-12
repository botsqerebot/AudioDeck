// textutil.h — UTF-8 helpers, voice clip name text, sorting, hashing
#pragma once
#include <Arduino.h>
#include "config.h"

static void appendUtf8(String &s, uint32_t cp) {
  if (cp < 0x80) s += (char)cp;
  else if (cp < 0x800) { s += (char)(0xC0 | (cp >> 6)); s += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { s += (char)(0xE0 | (cp >> 12)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
  else { s += (char)(0xF0 | (cp >> 18)); s += (char)(0x80 | ((cp >> 12) & 0x3F)); s += (char)(0x80 | ((cp >> 6) & 0x3F)); s += (char)(0x80 | (cp & 0x3F)); }
}

static String latin1ToUtf8(const uint8_t *d, size_t n) {
  String s; s.reserve(n + 4);
  for (size_t i = 0; i < n && d[i]; i++) appendUtf8(s, d[i]);
  return s;
}

static String rawUtf8(const uint8_t *d, size_t n) {
  String s; s.reserve(n + 1);
  for (size_t i = 0; i < n && d[i]; i++) s += (char)d[i];
  return s;
}

// Decode one code point and advance p. Invalid bytes come back as '?'.
static uint32_t utf8Next(const char *&p) {
  uint8_t c = (uint8_t)*p++;
  if (c < 0x80) return c;
  int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : -1;
  if (extra < 0) return '?';
  uint32_t cp = c & (0x3F >> extra);
  for (int i = 0; i < extra; i++) {
    uint8_t n = (uint8_t)*p;
    if ((n & 0xC0) != 0x80) return '?';
    cp = (cp << 6) | (n & 0x3F); p++;
  }
  return cp;
}

// ASCII spelling of common accented letters (Norwegian included)
static const char *asciiFold(uint32_t cp) {
  switch (cp) {
    case 0xE6: return "ae"; case 0xC6: return "AE";
    case 0xF8: return "o";  case 0xD8: return "O";
    case 0xE5: return "aa"; case 0xC5: return "AA";
    case 0xE4: return "a";  case 0xC4: return "A";
    case 0xF6: return "o";  case 0xD6: return "O";
    case 0xFC: return "u";  case 0xDC: return "U";
    case 0xDF: return "ss";
    case 0xE9: case 0xE8: case 0xEA: case 0xEB: return "e";
    case 0xC9: case 0xC8: case 0xCA: case 0xCB: return "E";
    case 0xE1: case 0xE0: case 0xE2: case 0xE3: return "a";
    case 0xC1: case 0xC0: case 0xC2: case 0xC3: return "A";
    case 0xED: case 0xEC: case 0xEE: case 0xEF: return "i";
    case 0xCD: case 0xCC: case 0xCE: case 0xCF: return "I";
    case 0xF3: case 0xF2: case 0xF4: case 0xF5: return "o";
    case 0xD3: case 0xD2: case 0xD4: case 0xD5: return "O";
    case 0xFA: case 0xF9: case 0xFB: return "u";
    case 0xDA: case 0xD9: case 0xDB: return "U";
    case 0xF1: return "n"; case 0xD1: return "N";
    case 0xE7: return "c"; case 0xC7: return "C";
    case 0xFD: case 0xFF: return "y";
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2013: case 0x2014: return "-";
    case 0x2026: return "...";
    default: return nullptr;
  }
}

// Normalised text used to name voice clips (Voice Studio does exactly the same)
static String samText(const String &in) {
  String out; out.reserve(in.length() + 8);
  const char *p = in.c_str();
  while (*p) {
    uint32_t cp = utf8Next(p);
    const char *f = asciiFold(cp);
    if (f) { out += f; continue; }
    if (cp == '&' || cp == '+') { out += " AND "; continue; }
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') || (cp >= '0' && cp <= '9') ||
        cp == ' ' || cp == '.' || cp == ',' || cp == '?' || cp == '!' || cp == '\'' || cp == '-') {
      out += (char)cp;
    } else {
      out += ' ';
    }
  }
  // collapse spaces, uppercase
  String clean; clean.reserve(out.length());
  bool space = true;
  for (size_t i = 0; i < out.length(); i++) {
    char c = out[i];
    if (c == ' ') { if (!space) clean += ' '; space = true; }
    else { clean += (char)toupper((unsigned char)c); space = false; }
  }
  clean.trim();
  if (clean.length() > VOICE_MAX_CHARS) {
    int cut = clean.lastIndexOf(' ', VOICE_MAX_CHARS);
    clean = clean.substring(0, cut > 20 ? cut : VOICE_MAX_CHARS);
  }
  if (clean.length() == 0) clean = "UNKNOWN";
  return clean;
}

// Sort key: case/accents folded, leading "The " ignored (like Rockbox)
static String sortKey(const String &in) {
  String out; out.reserve(in.length());
  const char *p = in.c_str();
  while (*p) {
    uint32_t cp = utf8Next(p);
    const char *f = asciiFold(cp);
    if (f) { for (; *f; f++) out += (char)tolower((unsigned char)*f); }
    else if (cp < 0x80) out += (char)tolower((int)cp);
    else out += '~';   // unknown non-ASCII sorts last
  }
  if (out.startsWith("the ")) out = out.substring(4);
  return out;
}

static uint64_t fnv64(const String &s) {
  uint64_t h = 1469598103934665603ULL;
  for (size_t i = 0; i < s.length(); i++) { h ^= (uint8_t)s[i]; h *= 1099511628211ULL; }
  return h;
}

static String hex64(uint64_t v) {
  char b[17];
  snprintf(b, sizeof(b), "%08lx%08lx", (unsigned long)(v >> 32), (unsigned long)(v & 0xFFFFFFFF));
  return String(b);
}

static String pathBase(const String &p) { int i = p.lastIndexOf('/'); return i >= 0 ? p.substring(i + 1) : p; }
static String pathDir(const String &p)  { int i = p.lastIndexOf('/'); return i > 0 ? p.substring(0, i) : String("/"); }
static String pathStem(const String &p) { String b = pathBase(p); int i = b.lastIndexOf('.'); return i > 0 ? b.substring(0, i) : b; }
