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
| Scan question | – | quick scan | cancel | full scan | – | – |

Vol+ and Vol- work everywhere. Prev restarts the song if it has played for more than 3 s.

## Power

- **Off:** hold Home for 5 s. It says "Powering off", saves where you were, switches off the analog regulator, amp and DAC, and puts the ESP32 into deep sleep.
- **On:** hold Play for 5 s. The green LED blinks while you hold, then the player starts. A shorter press goes back to sleep.
- **LEDs:** green = on, orange = charging (also while off), red = low battery.
- **While off and charging,** it wakes for a moment every 30 s to update the orange LED. Unplugged, it only wakes for Play or when a charger is plugged in.
- **LED pins:** green 38, purple 39 (off), orange 40, blue 41 (off), red 42.
- **Low battery (red)** needs a battery voltage divider to an ADC pin: set `PIN_BAT_ADC` and `BAT_DIVIDER` in `config.h`. GPIO 8 is free for it.

## What you hear

- **Moving in a menu:** a press cuts the clip that is playing at once, and the new item is spoken 0.15 s later. Music pauses for the clip and comes back 1.2 s later.
- **Albums:** song titles are spoken while you browse an album. Once a song plays, Prev/Next change songs without talking, and the album plays to the end.
- **Random Album:** says the name of the album it picked.
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
  "allSongs": [ 4, 0, 7 ]
}
```

**Sort order**

- Albums, artists and genres are sorted A–Z. Case and accents are ignored, and so is a leading "The".
- An artist's albums are sorted by year.
- Album tracks are sorted by disc, then track number.

**How tracks are grouped**

- Artists are grouped by album artist, so compilations stay together under "Various Artists".
- An album appears under every genre its tracks carry.

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
