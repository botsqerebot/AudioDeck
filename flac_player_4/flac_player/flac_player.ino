// flac_player.ino — ESP32-S3 N16R8 FLAC/MP3 player
//
// No screen: every menu item is a voice clip on the SD card, made on the PC by Voice Studio.
// Libraries (Arduino Library Manager): ESP8266Audio (2.x), ArduinoJson (7.x).
// Board: ESP32S3 Dev Module, Flash 16MB, PSRAM "OPI PSRAM", Partition "16M Flash (3MB APP/9.9MB FATFS)",
//        USB CDC On Boot "Enabled" (Serial log over USB).
//
// SD card layout:
//   /Music/...            your FLAC and MP3 files, any folder structure
//   /System/library.json  created by Settings > scan
//   /System/state.json    resume point, volume, shuffle/repeat
//   /Voices/Menu/*.wav    menu voice clips          (made by Voice Studio)
//   /Voices/Library/...   song/album/artist/genre clips (made by Voice Studio)

#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include "config.h"
#include "library.h"
#include "engine.h"
#include "menu.h"
#include "power.h"

Library library;
Engine engine(library);
Menu menu(library, engine);

// Hold Home 3 s: say goodbye, save the resume point, switch everything off, deep sleep
static void powerOff() {
  Serial.println("[power] off");
  engine.speak(library.menuVoice("power_off"), true);
  delay(1800);
  engine.stopAll();                    // also saves the resume point
  delay(150);
  SD.end();
  Power::sleepNow();
}

static bool startSD() {
  SPI.begin(PIN_SD_CLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
  for (int i = 0; i < 3; i++) {
    if (SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ)) return true;
    delay(200);
  }
  return SD.begin(PIN_SD_CS, SPI, SD_SPI_HZ / 2);      // retry slower
}

void setup() {
  // Log over USB only. GPIO43/44 are UART0 TX/RX on the S3 but here they are LRCK and
  // XSMT, so the UART is never started. Tools > USB CDC On Boot: Enabled to see the log.
#if ARDUINO_USB_CDC_ON_BOOT
  Serial.begin(115200);
#if ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);            // never stall the player when nothing reads the log
#endif
#endif

  Power::handleWake();                 // woke from power-off: only continue after Play was held 3 s

  pinMode(PIN_DAC_XSMT, OUTPUT); digitalWrite(PIN_DAC_XSMT, LOW);   // muted until something plays
  pinMode(PIN_LDO_EN, OUTPUT);   digitalWrite(PIN_LDO_EN, HIGH);    // analog regulator on
  delay(20);
  pinMode(PIN_AMP_SHDN, OUTPUT); digitalWrite(PIN_AMP_SHDN, LOW);
  pinMode(PIN_SD_DET, INPUT_PULLUP);
  pinMode(PIN_CHG_PGOOD, INPUT_PULLUP);
  pinMode(PIN_CHG_STAT, INPUT_PULLUP);
  if (!psramFound()) Serial.println("PSRAM not enabled - set PSRAM to OPI in Tools menu");

  // Audio first, so the beeps work even when the SD card is missing
  engine.begin();
  delay(50);
  digitalWrite(PIN_AMP_SHDN, HIGH);    // amp on after I2S is running (avoids the power-on pop)
  menu.begin();
  menu.powerOff = powerOff;

  while (!startSD()) {
    Serial.println("No SD card");
    engine.beep(330, 200, 2);          // two low beeps: insert the SD card
    delay(3000);
  }

  int missing = library.checkMenuVoices();
  if (missing) Serial.printf("%d menu clips missing - run Voice Studio on the PC\n", missing);
  uint32_t t0 = millis();
  bool haveLib = library.load();
  Serial.printf("Library: %s, %u tracks, %u albums in %lu ms\n", haveLib ? "loaded" : "missing",
                (unsigned)library.tracks.size(), (unsigned)library.albums.size(), (unsigned long)(millis() - t0));
  engine.loadState();
  if (engine.options().chime) engine.chime();     // ready (Settings > Startup chime)

  menu.enterHome();                    // says "Now playing"
  if (!haveLib) engine.speak(library.menuVoice("no_library"), true);
}

void loop() {
  menu.poll();
  Power::update();
  engine.periodicSave();
  delay(5);
}
