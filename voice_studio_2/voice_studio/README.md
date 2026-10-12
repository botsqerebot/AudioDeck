# Voice Studio

Builds the FLAC player's `library.json` and all voice clips on your PC with Piper. The scan matches the player's own, but with natural voices.

## Install (once)
```
pip install -r requirements.txt
```

## Run
```
python app.py
```
The page opens at http://127.0.0.1:5000.

1. **SD card:** pick the folder that contains `Music` (e.g. `E:\`) and press **Use this folder**.
2. **Voice:** choose a voice and press **Listen** to try it. It downloads (~60 MB) the first time.
3. **Build:** press **Build**, then put the card back in the player.

## What it writes
- `System/library.json`: the same format and sort order as the player's own scan.
- `Voices/Menu/*.wav`: the 17 menu clips.
- `Voices/Library/*/*.wav`: one clip per song title, album, artist and genre.
- `Voices/piper_manifest.json`: remembers which clips Piper made, so robotic clips and clips from an older voice get replaced.

The file names match the firmware's. If you later scan on the player itself, it reuses these clips and only makes robotic ones for new songs. The next Build replaces those too.

Build can be cancelled and run again. It continues where it stopped.
