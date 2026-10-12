// config.h — pins, paths and tuning for the FLAC player
#pragma once

// ---------- Pins (from your PCB) ----------
#define PIN_I2S_BCK     1
#define PIN_I2S_DIN     2
#define PIN_I2S_LRCK    43
#define PIN_DAC_XSMT    44   // PCM5102 soft mute: HIGH = play, LOW = mute
#define PIN_AMP_SHDN    45   // MAX9722 SHDN: HIGH = on
#define PIN_LDO_EN      48   // TPS7A2033 EN: HIGH = on

#define PIN_SD_DET      4
#define PIN_SD_MISO     5
#define PIN_SD_CLK      6
#define PIN_SD_MOSI     7
#define PIN_SD_CS       15

#define PIN_CHG_PGOOD   9
#define PIN_CHG_STAT    10

#define PIN_BTN_VOLUP   3
#define PIN_BTN_VOLDN   13
#define PIN_BTN_PREV    14
#define PIN_BTN_HOME    16
#define PIN_BTN_PLAY    17
#define PIN_BTN_NEXT    46

// Status LEDs (GPIO 38-42: green, purple, orange, blue, red)
#define PIN_LED_GREEN   38           // LED1: on while the player is on
#define PIN_LED_PURPLE  39           // LED2: not used yet, kept off
#define PIN_LED_ORANGE  40           // LED3: charging
#define PIN_LED_BLUE    41           // LED4: not used yet, kept off
#define PIN_LED_RED     42           // LED5: low battery (needs PIN_BAT_ADC)
#define LED_ACTIVE_HIGH 1            // 1 = pin HIGH lights the LED

// Battery sense: ADC pin on a divider from the battery (-1 = none, red LED stays off).
// The board has none yet; GPIO 8 is free and ADC-capable (2 x 100k divider + 100 nF to GND).
#define PIN_BAT_ADC     -1
#define BAT_DIVIDER     2.0f         // battery volts / pin volts (2.0 for two equal resistors)
#define BAT_LOW_V       3.45f        // red LED below this

// Power button holds
#define POWER_OFF_HOLD_MS  3000      // hold Home this long to power off
#define POWER_ON_HOLD_MS   3000      // hold Play this long to power on
#define CHARGE_CHECK_SEC   30        // while off and charging: wake this often to update the orange LED
#define AUTO_OFF_MIN       10        // power off after this long with nothing playing and no button pressed

#define BUTTON_ACTIVE_LOW 1          // buttons pull the pin to GND
#define SD_SPI_HZ       40000000     // drop to 20000000 if the card gives errors

// ---------- SD layout ----------
#define MUSIC_DIR       "/Music"
#define VOICE_DIR       "/Voices"
#define VOICE_MENU_DIR  "/Voices/Menu"
#define VOICE_LIB_DIR   "/Voices/Library"
#define SYSTEM_DIR      "/System"
#define LIBRARY_JSON    "/System/library.json"
#define STATE_JSON      "/System/state.json"

// ---------- Audio ----------
#define READ_AHEAD_BYTES   (32 * 1024)   // per track, lives in PSRAM
#define I2S_DMA_BUFFERS    8
#define I2S_DMA_BYTES      2304
#define VOLUME_STEPS       80            // one Vol+/Vol- press = one step
#define VOLUME_DEFAULT     40
#define MAX_GAIN           1.0f          // gain at full volume (ESP8266Audio allows up to 4.0)
#define VOLUME_SAFE_START  40            // at power-on the volume is lowered to this if it was higher
#define FADE_MS            150           // Settings > Soft fade: pause/resume fade length
#define DUCK_LEVEL         0.25f         // Settings > Duck music: music level under a voice clip (-12 dB)
#define DUCK_MS            60            // how fast the music dips and comes back
#define RESUME_REWIND_SEC  3             // resuming after a long pause (or power-off) starts this much earlier
#define RESUME_REWIND_AFTER_MS 60000     // "long pause"

// ---------- Voice clips (made on the PC by Voice Studio) ----------
#define VOICE_HOVER_DELAY_MS   150   // wait after the last press before speaking (old clip is cut at once)
#define VOICE_RESUME_MS        4000  // music resumes this long after the last voice clip (time to keep scrolling)
#define VOICE_MAX_CHARS        120   // clip names are a hash of the first 120 chars (matches Voice Studio)

// ---------- Buttons ----------
#define BTN_PRESS_MS       25        // press must be stable this long
#define BTN_RELEASE_MS     60        // release must be stable this long (stops double steps)
#define LONG_PRESS_MS      900       // hold for shuffle, repeat and home (long enough to avoid accidents)
#define REPEAT_DELAY_MS    700       // hold Prev/Next this long before it starts auto-scrolling
#define REPEAT_RATE_MS     150
#define BUTTON_DEBUG       1         // print every button event on Serial
#define STATE_SAVE_MS      5000      // resume position saved this often while playing
#define PREV_RESTART_SEC   3         // Prev after this many seconds restarts the song
