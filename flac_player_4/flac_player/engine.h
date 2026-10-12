// engine.h — gapless playback queue, shuffle/repeat, resume, menu voice clips.
// Runs in its own FreeRTOS task on core 0. Buttons talk to it through small
// mailboxes, so a button press never waits for the audio task.
#pragma once
#include <Arduino.h>
#include <SD.h>
#include "AudioFileSourceSD.h"
#include "AudioGeneratorFLAC.h"
#include "AudioGeneratorMP3.h"
#include "AudioGeneratorWAV.h"
#include "AudioOutputI2S.h"
#include "config.h"
#include "psram.h"
#include "library.h"
#include "track_source.h"

// Output wrapper: I2S stays up between tracks (ESP8266Audio stops it on every
// generator->stop(), which is what causes the gap), and the format can be saved
// and restored around voice clips.
//
// Volume is applied here instead of in AudioOutputI2S: its gain only has 1/64 steps,
// so the quiet end of the volume range would all round to the same level.
//
// The output never sends pure digital silence. The PCM5102 mutes its analog output by
// itself after a run of zero samples (a silent song ending, the pause before music comes
// back after a voice clip) and unmutes on the next sound, and that switch is the small pop.
// A 1 LSB offset (-96 dB, inaudible) keeps it from ever counting zeros.

// Gain that moves to a target a little every sample (fades, ducking)
struct Ramp {
  float v = 1, target = 1, step = 0;
  void set(float x) { v = target = x; }
  void to(float t, int ms, int hz) { target = t; step = fabsf(t - v) / max(1, hz * ms / 1000); }
  void tick() { if (v < target) v = min(target, v + step); else if (v > target) v = max(target, v - step); }
  bool done() const { return v == target; }
};

// Ducking: the voice clip decoder writes here instead of to the DAC, and the music
// output mixes it in. Mono, at the clip's own rate; both sides run in the audio task.
class VoiceSink : public AudioOutput {
public:
  bool begin() override { return true; }
  bool SetRate(int hz) override { rate = hz; return true; }
  bool SetChannels(int c) override { ch = c; return true; }
  bool ConsumeSample(int16_t s[2]) override {
    if (count >= N) return false;
    buf[(head + count) % N] = ch == 1 ? s[0] : (int16_t)((s[0] + s[1]) / 2);
    count++;
    return true;
  }
  bool stop() override { return true; }              // what is buffered still plays out
  void clear() { head = count = 0; phase = 0; }
  bool empty() const { return count == 0; }
  // current voice sample at the music rate (linear interpolation), advance() moves on
  int16_t peek() const {
    if (!count) return 0;
    int16_t a = buf[head], b = count > 1 ? buf[(head + 1) % N] : a;
    return a + (int16_t)((b - a) * phase);
  }
  void advance(int outRate) {
    if (!count || outRate <= 0) return;
    phase += (float)rate / outRate;
    while (phase >= 1 && count) { phase -= 1; head = (head + 1) % N; count--; }
  }
private:
  static const int N = 4096;
  int16_t buf[N];
  int head = 0, count = 0, rate = 22050, ch = 1;
  float phase = 0;
};

class GaplessOut : public AudioOutput {
public:
  VoiceSink voice;            // ducked voice clip, mixed into the music
  Ramp fade, duck;            // music only: pause/resume fade and ducking under the voice
  bool musicPass = false;     // set while the music decoder writes (fade, duck and voice apply)

  void attach(AudioOutputI2S *o) { out = o; out->SetGain(1.0f); }
  bool begin() override { if (!started) started = out->begin(); return started; }
  bool SetRate(int hz) override { if (hz != rate) { rate = hz; out->SetRate(hz); } return true; }
  bool SetChannels(int c) override { if (c != ch) { ch = c; out->SetChannels(c); } return true; }
  bool SetGain(float g) override { gain = g; return true; }
  float getGain() const { return gain; }
  bool ConsumeSample(int16_t s[2]) override {
    int32_t l = s[0], r = ch == 1 ? s[0] : s[1];
    if (musicPass) {
      float g = fade.v * duck.v;
      int32_t vo = voice.peek();
      l = lrintf(l * g) + vo;
      r = lrintf(r * g) + vo;
    }
    int16_t v[2] = {scale(l), scale(r)};
    if (v[0] == 0 && v[1] == 0) v[0] = v[1] = 1;     // no zero run: the DAC never auto-mutes
    if (!out->ConsumeSample(v)) return false;
    if (musicPass) { fade.tick(); duck.tick(); voice.advance(rate); }
    return true;
  }
  bool stop() override { return true; }               // swallowed: never tear down I2S per track
  void saveFormat() { savedRate = rate; savedCh = ch; }
  void restoreFormat() { if (savedRate) SetRate(savedRate); if (savedCh) SetChannels(savedCh); }
  void fadeTo(float t, int ms) { fade.to(t, ms, rate); }
  void duckTo(float t, int ms) { duck.to(t, ms, rate); }
  // DMA repeats its last buffer when starved, so feed silence whenever nothing plays
  void fillSilence() { int16_t z[2] = {0, 0}; for (int i = 0; started && i < 2048 && ConsumeSample(z); i++) {} }
private:
  AudioOutputI2S *out = nullptr;
  bool started = false;
  int rate = 0, ch = 0, savedRate = 0, savedCh = 0;
  float gain = 1.0f;
  int16_t scale(int32_t x) const {
    int32_t y = lrintf(x * gain);
    return (int16_t)constrain(y, -32768, 32767);
  }
};

enum QueueType : uint8_t { Q_NONE = 0, Q_ALBUM = 1, Q_ALL_SONGS = 2 };
enum RepeatMode : uint8_t { REPEAT_OFF = 0, REPEAT_ALL = 1, REPEAT_ONE = 2 };

struct PlayerStatus {
  bool active = false, paused = false, shuffle = false;
  uint8_t repeat = REPEAT_OFF;
  int track = -1, pos = 0, len = 0, volume = VOLUME_DEFAULT;
  uint32_t elapsed = 0, duration = 0;
};

// Settings menu toggles, saved in state.json
struct Options {
  bool duck = true;       // voice over quieter music instead of pausing it
  bool fade = true;       // soft fade on pause/resume
  bool volTick = true;    // tick at the lowest and highest volume
  bool chime = true;      // two-tone chime at power-on
};
#define MENU_POS_COUNT 5  // remembered list positions (see Menu::posSlot)

class Engine {
public:
  explicit Engine(Library &l) : lib(l) {}

  void begin() {
    mtx = xSemaphoreCreateRecursiveMutex();
    pinMode(PIN_DAC_XSMT, OUTPUT); digitalWrite(PIN_DAC_XSMT, LOW);
    i2s = new AudioOutputI2S();
    i2s->SetPinout(PIN_I2S_BCK, PIN_I2S_LRCK, PIN_I2S_DIN);
    i2s->SetBuffers(I2S_DMA_BUFFERS, I2S_DMA_BYTES);
    out.attach(i2s);
    out.SetRate(44100);
    out.SetChannels(2);
    out.begin();
    for (int i = 0; i < 2; i++) readBuf[i] = (uint8_t *)psramMalloc(READ_AHEAD_BYTES);
    setVolume(volume);
    xTaskCreatePinnedToCore(taskFn, "audio", 12288, this, 5, nullptr, 0);   // UI runs on core 1
  }

  // ---------- commands from the UI ----------
  // Starting from the menu always plays in album order (shuffle and repeat reset)
  void playAlbum(int albumId, int startIndex) { Lock l(mtx); buildQueue(Q_ALBUM, albumId); startQueue(startIndex); }
  void playAllSongs(int startIndex)          { Lock l(mtx); buildQueue(Q_ALL_SONGS, -1); startQueue(startIndex); }

  void togglePause() {
    Lock l(mtx);
    if (!active) { resumeSaved(); return; }
    if (pausing) { pausing = false; out.fadeTo(1, FADE_MS); return; }    // pressed again mid-fade
    if (!paused) {
      pausedAt = millis();
      bool sounding = (int32_t)(millis() - musicResumeAt) >= 0;          // not held for a voice clip
      if (opts.fade && sounding) { pausing = true; out.fadeTo(0, FADE_MS); return; }   // loop() pauses when the fade ends
      paused = true;
    } else {
      paused = false;
      musicResumeAt = 0;                       // Play means now, not after the voice pause
      if (millis() - pausedAt >= RESUME_REWIND_AFTER_MS) rewind(RESUME_REWIND_SEC);
      if (opts.fade) { out.fade.set(0); out.fadeTo(1, FADE_MS); }
    }
    stateDirty = true;
    updateMute();
  }

  // Next/Prev never block: presses are counted and the audio task applies them all,
  // so three quick presses always mean exactly three songs.
  void next() { portENTER_CRITICAL(&mbMux); skipReq = skipReq + 1; portEXIT_CRITICAL(&mbMux); }
  void prev() { portENTER_CRITICAL(&mbMux); skipReq = skipReq - 1; portEXIT_CRITICAL(&mbMux); }

  bool toggleShuffle() {
    Lock l(mtx);
    shuffle = !shuffle;
    if (!order.empty() && qpos >= 0) {
      int item = order[qpos];
      if (shuffle) shuffleFrom(item, esp_random()); else makeIdentity();
      qpos = findPos(item);
      if (active) slot[cur].qpos = qpos;
      dropPrefetch();
    }
    stateDirty = true;
    return shuffle;
  }
  uint8_t cycleRepeat() {
    Lock l(mtx);
    repeat = (repeat + 1) % 3;
    dropPrefetch();
    stateDirty = true;
    return repeat;
  }
  void setVolume(int v) {
    Lock l(mtx);
    volume = constrain(v, 0, VOLUME_STEPS);
    float x = (float)volume / VOLUME_STEPS;
    out.SetGain(MAX_GAIN * x * x);          // squared curve sounds more even
    stateDirty = true;
  }
  int getVolume() { return volume; }

  // Volume tick: the audio task plays it, so the buttons never wait
  void volumeTick() { tickReq = true; }

  Options options() { Lock l(mtx); return opts; }
  void setOptions(const Options &o) { Lock l(mtx); opts = o; stateDirty = true; }

  // Remembered menu list positions. Saved with the next state write (not on every
  // scroll step, that would write the SD card all the time).
  int menuPos(int i) { Lock l(mtx); return (i >= 0 && i < MENU_POS_COUNT) ? menuPosSaved[i] : 0; }
  void setMenuPos(int i, int sel) { Lock l(mtx); if (i >= 0 && i < MENU_POS_COUNT) menuPosSaved[i] = sel; }

  // Music or a voice clip is playing (auto power-off waits while this is true)
  bool busy() { Lock l(mtx); return (active && !paused) || voiceOn || pendingVoice.length(); }

  // Two soft rising tones at power-on (blocking)
  void chime() {
    Lock l(mtx);
    setDac(true);
    out.saveFormat();
    out.SetRate(22050);
    out.SetChannels(2);
    tone(660, 90);
    tone(880, 120);
    drainSilence(120);
    out.restoreFormat();
    updateMute();
  }

  // Voice clip for the highlighted item. Never blocks the buttons: the request goes into a
  // mailbox, and the audio task cuts the clip that is playing within ~1 ms.
  // An empty path just stops the current clip. A path to a missing file plays a short tick.
  void speak(const String &path, bool immediate = false) {
    portENTER_CRITICAL(&mbMux);
    strncpy(mbPath, path.c_str(), sizeof(mbPath) - 1);
    mbPath[sizeof(mbPath) - 1] = 0;
    mbImmediate = immediate;
    mbSeq = mbSeq + 1;
    portEXIT_CRITICAL(&mbMux);
  }
  void cancelVoice() { speak(""); }

  // Short beeps straight to the DAC (blocking). Used where no voice file can exist:
  // no SD card, scan progress.
  void beep(int hz, int ms, int count = 1) {
    Lock l(mtx);
    pendingVoice = ""; stopVoice();
    setDac(true);
    out.saveFormat();
    out.SetRate(22050);
    out.SetChannels(2);
    for (int k = 0; k < count; k++) {
      tone(hz, ms);
      tone(0, 90);
    }
    drainSilence(120);
    out.restoreFormat();
    musicResumeAt = millis() + 400;
    updateMute();
  }

  void stopAll() {           // used before a library scan
    Lock l(mtx);
    saveStateLocked();
    stopSlots();
    active = false; paused = false; pausing = false;
    pendingVoice = ""; stopVoice();
    updateMute();
  }

  // Home > Now Playing: if nothing is loaded, continue the last song where it stopped
  bool resumeLast() { Lock l(mtx); if (!active) resumeSaved(); return active; }

  // After a rescan track ids change, so forget the in-memory queue (the saved
  // state keeps the file path and is matched again on resume)
  void resetQueue() { Lock l(mtx); stopSlots(); active = false; pausing = false; items.clear(); order.clear(); qtype = Q_NONE; qpos = -1; updateMute(); }

  PlayerStatus status() {
    Lock l(mtx);
    PlayerStatus s;
    s.active = active; s.paused = paused; s.shuffle = shuffle; s.repeat = repeat; s.volume = volume;
    s.len = order.size();
    if (qpos >= 0 && qpos < (int)order.size()) {
      s.track = items[order[qpos]]; s.pos = qpos;
      s.duration = lib.tracks[s.track]["duration"] | 0u;
      s.elapsed = active ? elapsedLocked() : 0;
    }
    return s;
  }

  // Called from the UI loop: writes /System/state.json when something changed
  void periodicSave() {
    uint32_t now = millis();
    bool due;
    { Lock l(mtx); due = stateDirty || (active && !paused && now - lastSave > STATE_SAVE_MS); }
    if (due) { Lock l(mtx); saveStateLocked(); }
  }

  // Restore last session's volume and modes at boot (does not start playback)
  void loadState() {
    Lock l(mtx);
    JsonDocument st;
    if (!readState(st)) return;
    int v = st["volume"] | VOLUME_DEFAULT, steps = st["volSteps"] | 20;   // older files used 20 steps
    if (steps > 0 && steps != VOLUME_STEPS) v = (v * VOLUME_STEPS + steps / 2) / steps;
    setVolume(min(v, VOLUME_SAFE_START));                  // never start loud
    shuffle = st["shuffle"] | false;
    repeat = st["repeat"] | 0;
    JsonObject o = st["options"];
    opts.duck = o["duck"] | true;
    opts.fade = o["fade"] | true;
    opts.volTick = o["volTick"] | true;
    opts.chime = o["chime"] | true;
    for (int i = 0; i < MENU_POS_COUNT; i++) menuPosSaved[i] = st["menu"][i] | 0;
    stateDirty = false;
  }

private:
  struct Lock {
    SemaphoreHandle_t m;
    explicit Lock(SemaphoreHandle_t x) : m(x) { xSemaphoreTakeRecursive(m, portMAX_DELAY); }
    ~Lock() { xSemaphoreGiveRecursive(m); }
  };
  struct Slot {
    TrackSource *src = nullptr;
    AudioGenerator *gen = nullptr;    // created only when the song starts (keeps RAM free)
    bool flac = false, begun = false;
    int qpos = -1, track = -1;
  };

  Library &lib;
  SemaphoreHandle_t mtx = nullptr;
  AudioOutputI2S *i2s = nullptr;
  GaplessOut out;
  uint8_t *readBuf[2] = {nullptr, nullptr};
  Slot slot[2];
  int cur = 0;
  bool prepared = false, nextReady = false;
  bool active = false, paused = false;
  bool pausing = false;            // fading out, paused when the fade ends
  uint32_t pausedAt = 0;
  Options opts;
  int menuPosSaved[MENU_POS_COUNT] = {0};

  // queue
  uint8_t qtype = Q_NONE;
  int qid = -1, qpos = -1;
  PVec<uint16_t> items, order;     // items = track ids; order = play order (indices into items)
  bool shuffle = false;
  uint8_t repeat = REPEAT_OFF;
  uint32_t seed = 0;
  int shuffleFirst = 0;
  int volume = VOLUME_DEFAULT;

  // voice + button mailboxes
  AudioFileSourceSD voiceSrc;
  AudioGeneratorWAV voiceGen;
  bool voiceOn = false, voiceMixed = false;    // voiceMixed: the clip plays over ducked music
  volatile bool tickReq = false;
  uint32_t voiceTail = 0;
  String pendingVoice;
  portMUX_TYPE mbMux = portMUX_INITIALIZER_UNLOCKED;
  char mbPath[128] = {0};
  bool mbImmediate = false;
  volatile uint32_t mbSeq = 0;
  volatile int32_t skipReq = 0;
  uint32_t mbSeen = 0, cutMuteUntil = 0;
  uint32_t voiceAt = 0, musicResumeAt = 0;
  bool dacOn = false;

  // state
  bool stateDirty = false;
  uint32_t lastSave = 0;

  static void taskFn(void *arg) {
    Engine *e = (Engine *)arg;
    for (;;) { e->loop(); vTaskDelay(1); }
  }

  void loop() {
    Lock l(mtx);
    uint32_t now = millis();

    // ---- Next/Prev presses ----
    int32_t skip;
    portENTER_CRITICAL(&mbMux); skip = skipReq; skipReq = 0; portEXIT_CRITICAL(&mbMux);
    if (skip) applySkip(skip);

    // ---- voice requests ----
    if (mbSeq != mbSeen) {
      char p[sizeof(mbPath)]; bool imm;
      portENTER_CRITICAL(&mbMux);
      memcpy(p, mbPath, sizeof(p)); imm = mbImmediate; mbSeen = mbSeq;
      portEXIT_CRITICAL(&mbMux);
      cutVoice();
      pendingVoice = p;
      voiceAt = now + (imm ? 0 : VOICE_HOVER_DELAY_MS);
      if (!pendingVoice.length()) musicResumeAt = 0;
    }
    if (cutMuteUntil && (int32_t)(now - cutMuteUntil) >= 0) { cutMuteUntil = 0; updateMute(); }
    if (voiceOn && voiceMixed) {
      if (!active || paused) stopVoice();          // music stopped under the clip: drop it
      else {
        if (voiceGen.isRunning() && !voiceGen.loop()) { voiceGen.stop(); voiceSrc.close(); }   // top up the mix buffer
        if (!voiceGen.isRunning() && out.voice.empty()) stopVoice();                          // clip fully heard
      }
    } else if (voiceOn) {
      if (!voiceTail) {
        if (voiceGen.isRunning() && voiceGen.loop()) return;
        voiceGen.stop(); voiceSrc.close();
        voiceTail = now + 150;                     // clip done: let the DMA play out its end
      }
      if ((int32_t)(now - voiceTail) < 0) { out.fillSilence(); return; }
      stopVoice();
      musicResumeAt = now + VOICE_RESUME_MS;
      if (opts.fade) { out.fade.set(0); out.fadeTo(1, FADE_MS); }   // music fades back in
      updateMute();
    }
    if (pendingVoice.length() && !cutMuteUntil && (int32_t)(now - voiceAt) >= 0) {
      startVoice();
      if (!voiceMixed) return;
    }
    if (tickReq && !(voiceOn && !voiceMixed)) {    // not in the middle of a clip on its own
      tickReq = false;
      float g = out.getGain(), x = 0.2f;           // audible even at volume 0
      out.SetGain(max(g, MAX_GAIN * x * x));
      playTick();
      out.SetGain(g);
    }

    // ---- music ----
    if (pausing && (!active || out.fade.done())) {  // fade-out finished: now really paused
      pausing = false;
      if (active) { paused = true; stateDirty = true; }
      updateMute();
    }
    if (!voiceMixed && !pendingVoice.length() && out.duck.target != 1) out.duckTo(1, DUCK_MS);   // clip over: music back up
    if (active && !paused && (voiceMixed || (int32_t)(now - musicResumeAt) >= 0)) {
      out.musicPass = true;
      musicStep();
      out.musicPass = false;
    } else out.fillSilence();
  }

  // ---------- gapless core ----------
  void musicStep() {
    Slot &c = slot[cur];
    if (!c.gen) { active = false; updateMute(); return; }
    if (c.gen->isRunning() && c.gen->loop()) {
      if (!prepared) {           // pre-open (and pre-read) the next song while this one plays
        prepared = true;
        int p = nextPos(c.qpos, false);
        nextReady = (p >= 0) && openSlot(cur ^ 1, p, 0);
        if (nextReady) slot[cur ^ 1].src->prefill();
      }
      return;
    }
    // end of song: start the next one in the same pass so there is no gap
    int endedPos = c.qpos;
    freeSlot(cur);
    if (nextReady) {
      cur ^= 1;
      prepared = nextReady = false;
      if (beginSlot(cur)) { songStarted(); return; }
      Serial.printf("[play] could not start %s\n", trackPath(slot[cur].track));
      endedPos = slot[cur].qpos;
      freeSlot(cur);
    }
    // prefetch failed or the song didn't start: try the following songs (max 3), then stop
    for (int tries = 0, p = nextPos(endedPos, true); tries < 3 && p >= 0; tries++, p = nextPos(p, true)) {
      if (startAt(p, 0)) return;
    }
    Serial.println("[play] end of queue");
    active = false; paused = false;
    stateDirty = true;
    updateMute();
  }

  void applySkip(int32_t skip) {
    if (!active || order.empty()) return;
    int p = slot[cur].qpos;
    bool restart = (skip == -1 && elapsedLocked() >= PREV_RESTART_SEC);   // Prev after a few s: same song
    if (restart) {
      // p stays
    } else if (skip > 0) {
      for (int i = 0; i < skip; i++) {
        int n = nextPos(p, true);
        if (n < 0) { n = (int)order.size() - 1; i = skip; }   // stop at the last song
        p = n;
      }
    } else {
      for (int i = 0; i < -skip; i++) p = p > 0 ? p - 1 : (repeat == REPEAT_ALL ? (int)order.size() - 1 : 0);
    }
    playAt(p, 0);
  }

  const char *trackPath(int track) { return track >= 0 ? Library::str(lib.tracks[track], "path") : "?"; }

  bool openSlot(int i, int p, uint32_t resumeFilePos) {
    freeSlot(i);
    Slot &s = slot[i];
    int track = items[order[p]];
    JsonObject t = lib.tracks[track];
    s.flac = !strcmp(Library::str(t, "format"), "flac");
    s.src = new TrackSource(readBuf[i], READ_AHEAD_BYTES);
    if (!readBuf[i] || !s.src->openTrack(Library::str(t, "path"), s.flac, t["dataStart"] | 0u, resumeFilePos)) {
      Serial.printf("[play] could not open %s\n", Library::str(t, "path"));
      freeSlot(i);
      return false;
    }
    s.qpos = p; s.track = track;
    return true;
  }
  bool beginSlot(int i) {
    Slot &s = slot[i];
    if (!s.src) return false;
    if (s.flac) s.gen = new AudioGeneratorFLAC(); else s.gen = new AudioGeneratorMP3();
    s.begun = s.gen && s.gen->begin(s.src, &out);
    return s.begun;
  }
  void freeSlot(int i) {
    Slot &s = slot[i];
    if (s.gen) { if (s.begun) s.gen->stop(); delete s.gen; s.gen = nullptr; }
    if (s.src) { s.src->close(); delete s.src; s.src = nullptr; }
    s.begun = false; s.qpos = -1; s.track = -1;
  }
  void stopSlots() { freeSlot(0); freeSlot(1); prepared = nextReady = false; }
  void dropPrefetch() { if (active) { freeSlot(cur ^ 1); prepared = nextReady = false; } }

  bool startAt(int p, uint32_t resumeFilePos) {
    stopSlots();
    cur = 0;
    if (p < 0 || p >= (int)order.size()) return false;
    if (!openSlot(cur, p, resumeFilePos)) return false;
    if (!beginSlot(cur)) {
      Serial.printf("[play] could not start %s\n", trackPath(slot[cur].track));
      freeSlot(cur);
      return false;
    }
    songStarted();
    return true;
  }
  void playAt(int p, uint32_t resumeFilePos) {
    pausing = false;
    out.fade.set(1);
    if (!startAt(p, resumeFilePos)) { active = false; updateMute(); return; }
    paused = false;
    musicResumeAt = 0;
    updateMute();
  }
  // Restart the current song a few seconds back (after a long pause)
  void rewind(uint32_t sec) {
    Slot &s = slot[cur];
    if (!s.src || s.track < 0) return;
    uint32_t dur = lib.tracks[s.track]["duration"] | 0u;
    uint32_t fp = s.src->filePos(), a = s.src->audioStart(), b = s.src->audioEnd();
    if (!dur || b <= a || fp <= a) return;
    uint32_t back = (uint32_t)((uint64_t)(b - a) * sec / dur);
    playAt(s.qpos, fp > a + back ? fp - back : 0);       // 0 = from the start
  }
  void songStarted() {
    active = true;
    qpos = slot[cur].qpos;
    stateDirty = true;
    Serial.printf("[play] %d/%d %s\n", qpos + 1, (int)order.size(), trackPath(slot[cur].track));
  }

  uint32_t elapsedLocked() {
    Slot &s = slot[cur];
    if (!s.src || s.track < 0) return 0;
    uint32_t dur = lib.tracks[s.track]["duration"] | 0u;
    uint32_t fp = s.src->filePos(), a = s.src->audioStart(), b = s.src->audioEnd();
    if (b <= a || fp <= a) return 0;
    return (uint32_t)((uint64_t)dur * (fp - a) / (b - a));
  }

  // ---------- queue ----------
  void buildQueue(uint8_t type, int id) {
    qtype = type; qid = id;
    items.clear();
    if (type == Q_ALBUM && id >= 0 && id < (int)lib.albums.size()) {
      for (JsonVariant v : lib.albums[id]["tracks"].as<JsonArray>()) items.push_back(v.as<uint16_t>());
    } else if (type == Q_ALL_SONGS) {
      items.assign(lib.allSongs.begin(), lib.allSongs.end());
    }
  }
  void startQueue(int startIndex) {
    if (items.empty()) return;
    shuffle = false;                 // a fresh pick from the menu plays in order, to the end
    repeat = REPEAT_OFF;
    startIndex = constrain(startIndex, 0, (int)items.size() - 1);
    makeIdentity();
    playAt(startIndex, 0);
  }
  void makeIdentity() { order.resize(items.size()); for (size_t i = 0; i < order.size(); i++) order[i] = i; }
  void shuffleFrom(int firstItem, uint32_t s) {       // reproducible from (seed, firstItem)
    seed = s ? s : 1; shuffleFirst = firstItem;
    makeIdentity();
    if (order.empty()) return;
    std::swap(order[0], order[firstItem]);
    uint32_t x = seed;
    for (size_t i = order.size() - 1; i > 1; i--) {
      x ^= x << 13; x ^= x >> 17; x ^= x << 5;
      size_t j = 1 + x % i;                            // keeps the current song first
      std::swap(order[i], order[j]);
    }
  }
  int findPos(int item) { for (size_t i = 0; i < order.size(); i++) if (order[i] == item) return i; return 0; }
  int nextPos(int p, bool userSkip) {
    if (repeat == REPEAT_ONE && !userSkip) return p;
    if (p + 1 < (int)order.size()) return p + 1;
    if (repeat != REPEAT_OFF) return 0;
    return -1;
  }

  // ---------- voice ----------
  void startVoice() {
    String p = pendingVoice; pendingVoice = "";
    if (!voiceSrc.open(p.c_str())) { playTick(); return; }      // clip not made yet
    voiceTail = 0;
    // Ducking: music is really sounding, so mix the clip over it instead of pausing it
    if (opts.duck && active && !paused && !pausing && (int32_t)(millis() - musicResumeAt) >= 0) {
      out.voice.clear();
      voiceOn = voiceMixed = voiceGen.begin(&voiceSrc, &out.voice);
      if (voiceOn) out.duckTo(DUCK_LEVEL, DUCK_MS);
      else voiceSrc.close();
      return;
    }
    if (pausing) { pausing = false; paused = true; stateDirty = true; }   // mid fade-out: just be paused
    out.saveFormat();
    voiceOn = voiceGen.begin(&voiceSrc, &out);
    if (!voiceOn) { voiceSrc.close(); out.restoreFormat(); }
    updateMute();
  }
  void stopVoice() {
    if (!voiceOn) return;
    voiceGen.stop();
    voiceSrc.close();
    if (voiceMixed) out.voice.clear(); else out.restoreFormat();
    voiceOn = voiceMixed = false;
    voiceTail = 0;
  }
  // Stop the clip mid-word: mute the DAC at once while the DMA drains, so the old
  // clip is never heard after the button press. A ducked clip just leaves the mix
  // (the music keeps playing, and stays ducked if another clip is coming).
  void cutVoice() {
    if (!voiceOn) return;
    bool mixed = voiceMixed;
    stopVoice();
    if (mixed) return;
    cutMuteUntil = millis() + 130;
    setDac(false);
  }
  void drainSilence(uint32_t ms) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) { out.fillSilence(); delay(2); }
  }
  // sine tone at 22050 Hz, written straight to the DMA (hz = 0 for silence)
  void tone(int hz, int ms) {
    int n = 22050L * ms / 1000;
    float ph = 0, step = 2.0f * PI * hz / 22050.0f;
    for (int i = 0; i < n; i++) {
      float env = min(1.0f, min(i, n - i) / 200.0f);          // soft edges, no clicks
      int16_t v = hz ? (int16_t)(sinf(ph) * 9000 * env) : 0;
      int16_t s[2] = {v, v};
      ph += step; if (ph > 2 * PI) ph -= 2 * PI;
      while (!out.ConsumeSample(s)) delay(1);
    }
  }
  void playTick() {            // "nothing to say here": a 40 ms tick, music barely interrupted
    bool wasOn = dacOn;
    setDac(true);
    out.saveFormat();
    out.SetRate(22050);
    out.SetChannels(2);
    tone(1500, 40);
    drainSilence(110);
    out.restoreFormat();
    if (!wasOn) updateMute();
  }
  void updateMute() {
    bool on = ((active && !paused) || voiceOn || pendingVoice.length()) && !cutMuteUntil;
    setDac(on);
  }
  void setDac(bool on) { dacOn = on; digitalWrite(PIN_DAC_XSMT, on ? HIGH : LOW); }

  // ---------- resume state ----------
  void saveStateLocked() {
    lastSave = millis();
    stateDirty = false;
    JsonDocument st;
    st["volume"] = volume;
    st["volSteps"] = VOLUME_STEPS;
    st["shuffle"] = shuffle;
    st["repeat"] = repeat;
    JsonObject o = st["options"].to<JsonObject>();
    o["duck"] = opts.duck;
    o["fade"] = opts.fade;
    o["volTick"] = opts.volTick;
    o["chime"] = opts.chime;
    JsonArray mp = st["menu"].to<JsonArray>();
    for (int i = 0; i < MENU_POS_COUNT; i++) mp.add(menuPosSaved[i]);
    if (qtype != Q_NONE && !items.empty() && qpos >= 0 && qpos < (int)order.size()) {
      st["queue"] = qtype;
      st["id"] = qid;
      int item = order[qpos];
      st["item"] = item;
      st["seed"] = seed;
      st["first"] = shuffleFirst;
      st["path"] = Library::str(lib.tracks[items[item]], "path");
      st["file"] = (active && slot[cur].src) ? slot[cur].src->filePos() : 0;
    }
    String json; serializeJson(st, json);
    String tmp = String(STATE_JSON) + ".tmp";
    fs::File f = SD.open(tmp, FILE_WRITE);
    if (!f) return;
    f.print(json); f.close();
    if (SD.exists(STATE_JSON)) SD.remove(STATE_JSON);
    SD.rename(tmp, STATE_JSON);
  }
  bool readState(JsonDocument &st) {
    fs::File f = SD.open(STATE_JSON, FILE_READ);
    if (!f) f = SD.open(String(STATE_JSON) + ".tmp", FILE_READ);
    if (!f) return false;
    bool ok = !deserializeJson(st, f);
    f.close();
    return ok;
  }
  // "Now Playing" with nothing loaded: continue the last song where it stopped
  void resumeSaved() {
    JsonDocument st;
    if (!lib.loaded || !readState(st) || !st["queue"].is<int>()) return;
    buildQueue(st["queue"] | 0, st["id"] | -1);
    if (items.empty()) return;
    int item = st["item"] | 0;
    const char *path = st["path"] | "";
    if (item >= (int)items.size() || strcmp(Library::str(lib.tracks[items[item]], "path"), path) != 0) {
      item = -1;                                   // library changed since: find the song by path
      for (size_t i = 0; i < items.size(); i++)
        if (!strcmp(Library::str(lib.tracks[items[i]], "path"), path)) { item = i; break; }
      if (item < 0) return;
    }
    shuffle = st["shuffle"] | false;
    repeat = st["repeat"] | 0;
    if (shuffle) {
      int first = st["first"] | item;
      if (first >= (int)items.size()) first = item;
      shuffleFrom(first, st["seed"] | 1u);
    } else makeIdentity();
    // start a few seconds before where it stopped, so the last words aren't missed
    uint32_t file = st["file"] | 0u;
    JsonObject t = lib.tracks[items[item]];
    uint32_t dur = t["duration"] | 0u, size = t["size"] | 0u, start = t["dataStart"] | 0u;
    if (file && dur && size > start) {
      uint32_t back = (uint32_t)((uint64_t)(size - start) * RESUME_REWIND_SEC / dur);
      file = file > start + back ? file - back : 0;
    }
    playAt(findPos(item), file);
  }
};
