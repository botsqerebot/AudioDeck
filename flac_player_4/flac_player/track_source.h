// track_source.h — buffered file source that hides tags and cover art from the decoders.
//
// FLAC: serves "fLaC" + STREAMINFO (marked as the last block), then jumps straight
//       to the first audio frame. Embedded cover art is never read.
// MP3:  starts at the first audio frame (after ID3v2) and stops before ID3v1.
//
// The read-ahead buffer lives in here (not in AudioFileSourceBuffer) so getPos() is the
// position the decoder has really consumed. AudioFileSourceBuffer reports the SD read
// position instead, which made the FLAC decoder think a song had ended ~64 KB early.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include "AudioFileSource.h"
#include "tags.h"

class TrackSource : public AudioFileSource {
public:
  TrackSource(uint8_t *buffer, uint32_t bufferSize) : buf(buffer), cap(bufferSize) {}

  bool openTrack(const char *path, bool flac, uint32_t dataStart, uint32_t resumeFilePos = 0) {
    f = SD.open(path, FILE_READ);
    if (!f) return false;
    size = f.size();
    start = 0; end = size; hlen = 0; vpos = 0; resumeV = 0; bufLen = bufPos = 0;

    if (flac && dataStart > 0) {
      TrackTags dummy;
      uint32_t base = parseID3v2(f, dummy);
      uint8_t b[42];
      f.seek(base);
      if (f.read(b, 42) == 42 && !memcmp(b, "fLaC", 4) && (b[4] & 0x7F) == 0) {
        memcpy(hdr, b, 42);
        hdr[4] = 0x80;              // STREAMINFO becomes the last metadata block
        hlen = 42;
        start = dataStart;
      }
    } else if (!flac) {
      start = dataStart;
      if (size >= 128) {
        uint8_t t[3];
        f.seek(size - 128);
        if (f.read(t, 3) == 3 && !memcmp(t, "TAG", 3)) end = size - 128;
      }
    }
    if (start >= end) { start = 0; hlen = 0; }
    if (resumeFilePos > start && resumeFilePos < end) resumeV = hlen + (resumeFilePos - start);
    if (hlen == 0 && resumeV) { vpos = resumeV; resumeV = 0; }
    return true;
  }

  // Fill the read-ahead buffer now (used when pre-opening the next song)
  void prefill() { if (vpos >= hlen && bufPos >= bufLen) refill(); }

  uint32_t read(void *data, uint32_t len) override {
    uint8_t *d = (uint8_t *)data;
    uint32_t got = 0;
    while (len && vpos < hlen) { *d++ = hdr[vpos++]; len--; got++; }
    if (resumeV && vpos >= hlen) {        // header delivered: jump to the resume point
      vpos = resumeV; resumeV = 0;
      bufLen = bufPos = 0;
    }
    while (len) {
      if (bufPos >= bufLen && !refill()) break;
      uint32_t n = min(len, bufLen - bufPos);
      memcpy(d, buf + bufPos, n);
      bufPos += n; vpos += n; d += n; len -= n; got += n;
    }
    return got;
  }

  bool seek(int32_t pos, int dir) override {
    int64_t np = (dir == SEEK_SET) ? pos : (dir == SEEK_CUR) ? (int64_t)vpos + pos : (int64_t)getSize() + pos;
    if (np < 0) np = 0;
    if (np > getSize()) np = getSize();
    vpos = (uint32_t)np;
    resumeV = 0;
    bufLen = bufPos = 0;
    return true;
  }

  bool close() override { if (f) f.close(); return true; }
  bool isOpen() override { return (bool)f; }
  bool eof() { return vpos >= getSize(); }
  uint32_t getSize() override { return hlen + (end - start); }
  uint32_t getPos() override { return vpos; }

  uint32_t toFile(uint32_t v) const { return v <= hlen ? start : start + (v - hlen); }
  uint32_t filePos() const { return toFile(vpos); }
  uint32_t audioStart() const { return start; }
  uint32_t audioEnd() const { return end; }

private:
  bool refill() {
    uint32_t fp = start + (vpos - hlen);
    bufLen = bufPos = 0;
    if (vpos < hlen || fp >= end) return false;
    uint32_t n = min(cap, end - fp);
    if (f.position() != fp) f.seek(fp);
    int r = f.read(buf, n);
    if (r <= 0) return false;
    bufLen = r;
    return true;
  }

  fs::File f;
  uint8_t *buf;
  uint32_t cap;
  uint8_t hdr[42];
  uint32_t size = 0, start = 0, end = 0, hlen = 0, vpos = 0, resumeV = 0;
  uint32_t bufLen = 0, bufPos = 0;
};
