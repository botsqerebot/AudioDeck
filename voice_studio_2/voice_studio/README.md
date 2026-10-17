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

## Add music
On the **Add music** tab, pick a folder on your PC and press **Check songs**. It shows where each FLAC/MP3 will go:
`Music/<Album artist>/<Album>/<NN Title>.flac` (`<disc>-<NN>` for multi-disc albums), with names made safe for the card.
- Tags come first. Missing tags are filled in from the folder names.
- Songs that are already on the card (same place and size, or the same tags in another folder) are skipped.
- Other formats (M4A, WAV, OGG...) are listed but not copied.

**Copy and build** copies the songs, keeping their dates, then runs a quick Build with the voice from the Build tab, so the new songs get their clips straight away.

## Library & playlists
- **On the card:** every album and song in `library.json`. Search, see which items still have no voice clip, and tick songs to delete them from the card. A quick Build follows.
- **Playlist:** pick a playlist or press **New**, add songs with **+**, then order them with ↑ ↓ and remove them with ×. **Save playlist** writes `Playlists/<name>.m3u8` and updates `library.json` and the playlist's name clip. On the player they're under Music > Playlists.

## What it writes
- `System/library.json`: the same format and sort order as the player's own scan.
- `Voices/Menu/*.wav`: the menu clips, including "Battery N percent" for Settings > Battery.
- `Voices/Library/*/*.wav`: one clip per song title, album, artist, genre and playlist.
- `Playlists/*.m3u8`: your playlists.
- `Voices/piper_manifest.json`: remembers which clips Piper made, so robotic clips and clips from an older voice get replaced.

The file names match the firmware's. If you later scan on the player itself, it reuses these clips and only makes robotic ones for new songs. The next Build replaces those too.

Build can be cancelled and run again. It continues where it stopped.
