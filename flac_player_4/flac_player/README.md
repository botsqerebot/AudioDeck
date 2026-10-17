# FLAC player firmware

ESP32-S3 N16R8 FLAC/MP3 player with **no screen**. The menu is a "virtual menu": every item is a voice clip on the SD card, made on the PC with **Voice Studio** (Piper). It also has a Rockbox-style library index, gapless playback, resume and shuffle/repeat.

## Setup

- **Board:** ESP32S3 Dev Module, Flash 16MB, PSRAM **OPI PSRAM**, Partition "16M Flash (3MB APP/9.9MB FATFS)", USB CDC On Boot **Enabled**.
- **Libraries:** ESP8266Audio 2.x, ArduinoJson 7.x. (ESP8266SAM is no longer needed.)
- **Music:** put it in `/Music` on the SD card. Any folder layout works.
- **Voices and library:** run Voice Studio on the PC with the SD card in. It writes `library.json` and every clip.
- **Scan on the player:** Settings still works (new songs show up), but their clips only exist after the next Voice Studio build. Until then a short tick plays instead.

## Buttons

| Where | Prev / Next | Play | Home | Hold Play | Hold Prev | Hold Home |
|---|---|---|---|---|---|---|
| Menus | move, the item is spoken (hold = fast) | select | back (repeats the item at the top level) | – | – | home |
| Now Playing | previous / next song (quiet) | pause | back | shuffle on/off | repeat off/all/one | home |
| Settings | move, the item and its state are spoken | flip the toggle (or open Scan library, or repeat the battery level) | back | – | – | home |
| Scan question | – | quick scan | cancel | full scan | – | – |

Vol+ and Vol- work everywhere. Prev restarts the song if it has played for more than 3 s.
In Now Playing, **hold Next** to hear the song's title and artist.

## Power

- **Off:** hold Home for 3 s. It says "Powering off", saves where you were, switches off the analog regulator, amp and DAC, and puts the ESP32 into deep sleep.
- **On:** hold Play for 3 s. The green LED blinks while you hold, then the player starts. A shorter press goes back to sleep.
- **Auto off:** with nothing playing and no button pressed for 10 minutes (`AUTO_OFF_MIN`), it powers off the same way.
- **LEDs:** green = on, orange = charging (also while off), red = low battery, blue (LED4) = battery above 60 %.
- **While off and charging,** it wakes for a moment every 30 s to update the orange LED. Unplugged, it only wakes for Play or when a charger is plugged in.
- **LED pins:** green 38, purple 39 (off), orange 40, blue 41, red 42.
- **Battery features** need a battery voltage divider to an ADC pin. The board has none yet. Until you add one, these stay off: the red LED, LED4, **Settings > Battery** ("Battery 73 percent", plus "Charging") and the warnings ("Battery low" at 15 %, "Battery critical" at 3 %, which then saves and powers off).
  To add it, fit 1 MΩ from BAT+ to GPIO 8, 1 MΩ from GPIO 8 to GND and 100 nF from GPIO 8 to GND, then set `PIN_BAT_ADC 8` in `config.h`. The divider draws about 2 µA.

### Current while off

Before deep sleep the firmware stops I2S and the SD card and parks their pins at fixed levels: I2S low, SD CS high, SD CLK low, and MOSI/MISO/Det high-impedance, because the board pull-ups hold those up. The SD card and the PCM5102's digital side stay powered from the always-on 3.3 V rail, and floating pins would keep them from going idle.

What the firmware can't fix, for the next PCB revision:
- **R5** (10 k pull-up on SD Det): if the socket's switch grounds Det while a card is in, this costs 330 µA all the time. Remove it or use 1 M; the firmware already turns on the internal pull-up while awake.
- **MCP1825** 3.3 V regulator: about 120 µA quiescent. A low-Iq LDO that can still supply 500 mA would help.
- **SD card and PCM5102 DVDD** on the always-on rail: put them on a switched rail (a P-FET, or the TPS7A20 analog rail) so they are fully off.

To check: put a multimeter in series with the battery while the player is off.

## What you hear

- **Moving in a menu:** a press cuts the clip that is playing at once, and the new item is spoken 0.15 s later. With **Duck music** on (Settings), music keeps playing quieter under the voice. With it off, music pauses for the clip and comes back 4 s later, so you have time to keep scrolling.
- **Albums:** song titles are spoken while you browse an album. Once a song plays, Prev/Next change songs without talking, and the album plays to the end.
- **Letter Album:** Prev/Next steps through the first letters of your albums (only letters that have albums, "Numbers and symbols" last). Play opens that letter's albums. "The" is skipped, like in the A-Z sort.
- **Volume:** 80 steps. A saved volume from the old 20-step firmware is converted on the first boot. At power-on the volume is lowered to 50% if it was left higher. With **Volume ticks** on, a tick plays when you reach the lowest or highest volume.
- **Remembered lists:** Albums, Letter Album, Artists, Genre and All Songs reopen on the item you were last on, also after a power-off.
- **Pause/resume:** with **Soft fade** on, the music fades out and in. Resuming after more than a minute, or after power-off, starts 3 s earlier.
- **Startup chime:** two short rising tones at power-on (Settings > Startup chime).
- **Random Album:** says the name of the album it picked.
- **Playlists:** made in Voice Studio. Music > Playlists says each playlist name, Play opens its songs, and picking a song plays the playlist from there.
- **Picking a song from a menu** always plays in order with shuffle and repeat off.
- **Missing clip:** a short tick.
- **Scanning:** says "Quick scan" or "Full scan", beeps every 15 s while working, then says "Scan complete".
- **No SD card:** two low beeps every few seconds.
- **Serial monitor:** prints every button event (`[btn] NEXT press`) and everything that is spoken. Set `BUTTON_DEBUG 0` in `config.h` to hide the button lines.

## Scanning

- **Quick scan:** reuses tags and voice clips for files whose size and date haven't changed. Only new or edited files are read and voiced.
- **Full scan:** re-reads every file. Voice clips that already exist are still reused, because they are named by a hash of the spoken text.
- **Speed:** a first scan of a large library takes a while, at roughly 0.1–0.2 s per voice clip. Boot only loads `library.json`.

## SD layout

```
/Music/...                        your files
/System/library.json              the index
/System/state.json                resume point, volume, shuffle, repeat
/Playlists/<name>.m3u8            playlists (one /Music/... path per line, made by Voice Studio)
/Voices/Menu/<key>_<hash>.wav     menu and feedback clips
/Voices/Library/<h>/<hash>.wav    one clip per song title, album, artist, genre
```

## library.json

```json
{
  "version": 1,
  "counts":     { "tracks": 812, "albums": 64, "artists": 41, "genres": 12 },
  "menuVoices": { "home_music": "/Voices/Menu/home_music.wav", ... },
  "tracks": [ {
      "path": "/Music/Artist/Album/01.flac", "title": "...", "artist": "...",
      "album": "...", "albumArtist": "...", "genre": "Rock", "year": "1999",
      "track": 1, "disc": 1, "duration": 245, "format": "flac",
      "sampleRate": 44100, "bits": 16, "channels": 2,
      "size": 31457280, "mtime": 1728000000, "dataStart": 8432,
      "albumId": 0, "voice": "/Voices/Library/c/c7ea013bfb4ef604.wav" } ],
  "albums":  [ { "name": "...", "artist": "...", "artistId": 0, "genre": "...", "year": 1999,
                 "duration": 2710, "tracks": [0, 1, 2], "voice": "..." } ],
  "artists": [ { "name": "...", "albums": [0, 5], "voice": "..." } ],
  "genres":  [ { "name": "...", "albums": [0, 3], "voice": "..." } ],
  "allSongs": [ 4, 0, 7 ],
  "playlists": [ { "name": "Road trip", "tracks": [7, 2], "voice": "..." } ]
}
```

**Sort order**

- Albums, artists and genres are sorted A–Z. Case and accents are ignored, and so is a leading "The".
- An artist's albums are sorted by year.
- Album tracks are sorted by disc, then track number.

**How tracks are grouped**

- Artists are grouped by album artist, so compilations stay together under "Various Artists".
- An album appears under every genre its tracks carry.

**Playlists** are read from `/Playlists/*.m3u8` (and `.m3u`) by both the player's scan and Voice Studio. Lines starting with `#` are skipped, `\` becomes `/`, and leading `../` is dropped. Songs that are no longer on the card are left out. Playlists are sorted A–Z.

**Missing tags** are filled in from the folder names: `/Music/Artist/Album/03 Song.flac`.

## Files

| File | What it does |
|---|---|
| `flac_player.ino` | boot, power pins, main loop |
| `config.h` | pins, paths, timings |
| `library.h` | scanner, grouping and sorting, JSON save and load |
| `tags.h` | ID3v2.2/2.3/2.4 and ID3v1 tags, FLAC Vorbis comments, durations |
| `voice.h` | voice clip file names (shared with Voice Studio) |
| `engine.h` | gapless queue, shuffle/repeat, resume, voice playback (core 0) |
| `track_source.h` | feeds decoders audio only, skipping tags and cover art |
| `menu.h`, `buttons.h` | spoken menu and buttons (core 1) |
| `power.h` | power off/on, deep sleep, LEDs, battery |
| `textutil.h`, `psram.h` | UTF-8/Norwegian letters, PSRAM allocators |
