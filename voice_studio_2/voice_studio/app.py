"""
FLAC Player Voice Studio - run on your PC, open http://127.0.0.1:5000

Pick the SD card (the folder that contains "Music"), choose a Piper voice and press
Build. It writes exactly what the player's own scan makes:
    System/library.json, Voices/Menu/*.wav, Voices/Library/*/*.wav
but with Piper's natural voice instead of the robotic one.

It can also copy songs from a PC folder onto the card (sorted by their tags), show
what is on the card, and make playlists (Playlists/*.m3u8). The page is static/index.html.

Setup:  pip install flask piper-tts
Run:    python app.py
"""
import json
import os
import string
import sys
import threading
import time
import webbrowser
from pathlib import Path

from flask import Flask, jsonify, request, send_from_directory

import fpbuilder as fb

HERE = Path(__file__).resolve().parent
VOICES_DIR = HERE / "piper_voices"
app = Flask(__name__)

SUGGESTED = [
    ("en_US-lessac-medium", "English (US) - Lessac, clear (default)"),
    ("en_US-amy-medium", "English (US) - Amy"),
    ("en_US-ryan-high", "English (US) - Ryan, high quality"),
    ("en_GB-alan-medium", "English (UK) - Alan"),
    ("en_GB-jenny_dioco-medium", "English (UK) - Jenny"),
    ("no_NO-talesyntese-medium", "Norsk - Talesyntese"),
]

job = {"running": False, "stage": "", "done": 0, "total": 0, "item": "", "log": [],
       "result": None, "error": None, "cancel": False, "started": 0}
job_lock = threading.Lock()
tts_cache = {}
tts_lock = threading.Lock()


def get_tts(voice, speed):
    key = (voice, round(speed, 2))
    with tts_lock:
        if key not in tts_cache:
            tts_cache.clear()           # keep one model in memory
            tts_cache[key] = fb.PiperTTS(voice, VOICES_DIR, speed)
        return tts_cache[key]


def menu_items():
    # next to the app, or the firmware in the repo (newest flac_player_N first)
    fw = sorted(HERE.parent.parent.glob("flac_player*/flac_player/library.h"), reverse=True)
    for p in (HERE / "library.h", HERE.parent / "flac_player" / "library.h", *fw):
        items = fb.read_library_h(p)
        if items:
            return items
    return fb.MENU_VOICES


def log(msg):
    with job_lock:
        job["log"].append(time.strftime("%H:%M:%S ") + msg)
        job["log"] = job["log"][-300:]


def progress(stage, done, total, item):
    with job_lock:
        job.update(stage=stage, done=done, total=total, item=item)


def do_build(tts, opts):
    return fb.build(opts["root"], tts, quick=opts.get("quick", True), menu_items=menu_items(),
                    regen=opts.get("regen", False), cleanup=opts.get("cleanup", False),
                    menu_only=opts.get("menu_only", False), progress=progress, log=log, cancel=lambda: job["cancel"])


def do_import(tts, opts):
    items = fb.plan_import(opts["root"], opts["src"])
    skipped = sum(it["status"] != "copy" for it in items)
    if skipped:
        log(f"Skipping {skipped} files (already on the card, duplicates or unsupported)")
    fb.copy_import(opts["root"], items, progress=progress, log=log, cancel=lambda: job["cancel"])
    return do_build(tts, opts)


def do_playlists(tts, opts):
    return fb.update_playlists(opts["root"], tts, progress=progress, log=log, cancel=lambda: job["cancel"])


def run_job(work, opts):
    try:
        log(f"Loading voice {opts['voice']} (first time downloads ~60 MB)...")
        tts = get_tts(opts["voice"], opts["speed"])
        with tts_lock:
            res = work(tts, opts)
        took = int(time.time() - job["started"])
        log(f"Done in {took // 60} min {took % 60} s: {res.get('made', 0)} clips made, "
            f"{res.get('kept', 0)} already up to date.")
        with job_lock:
            job["result"] = res
    except fb.Cancelled:
        log("Cancelled. Nothing is broken: run Build again to continue where it stopped.")
        with job_lock:
            job["error"] = "cancelled"
    except Exception as e:                      # show any problem on the page
        log(f"ERROR: {e}")
        with job_lock:
            job["error"] = str(e)
    finally:
        with job_lock:
            job["running"] = False
            job["stage"] = "Finished" if not job["error"] else "Stopped"


# ---------------------------------------------------------------- API
@app.get("/api/ls")
def api_ls():
    p = request.args.get("path", "").strip()
    if not p:
        if os.name == "nt":
            drives = [f"{d}:\\" for d in string.ascii_uppercase if os.path.exists(f"{d}:\\")]
            return jsonify(path="", parent=None, dirs=drives, hasMusic=False, isRoot=True)
        p = "/"
    path = Path(p)
    if not path.is_dir():
        return jsonify(error=f"Not a folder: {p}"), 400
    try:
        dirs = sorted((d.name for d in path.iterdir() if d.is_dir() and not d.name.startswith(".")), key=str.lower)
    except PermissionError:
        dirs = []
    parent = None if path.parent == path else str(path.parent)
    if os.name == "nt" and path.parent == path:
        parent = ""
    lib = path / "System" / "library.json"
    return jsonify(path=str(path), parent=parent, dirs=dirs, hasMusic=(path / "Music").is_dir(),
                   hasLibrary=lib.exists(), isRoot=False)


@app.get("/api/voices")
def api_voices():
    have = {p.stem for p in VOICES_DIR.glob("*.onnx")} if VOICES_DIR.exists() else set()
    out = [{"id": v, "label": l, "downloaded": v in have} for v, l in SUGGESTED]
    out += [{"id": v, "label": v, "downloaded": True} for v in sorted(have - {v for v, _ in SUGGESTED})]
    return jsonify(out)


@app.post("/api/preview")
def api_preview():
    d = request.get_json(force=True)
    try:
        tts = get_tts(d.get("voice") or SUGGESTED[0][0], float(d.get("speed", 1.0)))
        with tts_lock:
            wav = tts.wav_bytes((d.get("text") or "Now playing").strip()[:300])
    except Exception as e:
        return jsonify(error=str(e)), 500
    return Response(wav, mimetype="audio/wav")


class BadRequest(Exception):
    pass


@app.errorhandler(BadRequest)
def bad_request(e):
    return jsonify(error=str(e)), 400


def card_root(root, need_music=True):
    """The chosen SD card folder, checked."""
    root = (root or "").strip()
    if not root or not Path(root).is_dir():
        raise BadRequest("Choose the SD card folder first.")
    if need_music and not (Path(root) / "Music").is_dir():
        raise BadRequest('That folder has no "Music" folder inside. Choose the SD card root.')
    return Path(root)


def inside(base, rel):
    """<base>/<rel>, refusing anything that escapes base."""
    p = (base / rel.lstrip("/")).resolve()
    if not p.is_relative_to(base.resolve()):
        raise BadRequest("Path is outside the card.")
    return p


def start_job(work, d, note=None, reply=None, **extra):
    """Run work(tts, opts) in the background; the page follows it with /api/status."""
    with job_lock:
        if job["running"]:
            return jsonify(error="Already running. Wait for it to finish or cancel it."), 409
        job.update(running=True, stage="Starting", done=0, total=0, item="", log=[], result=None,
                   error=None, cancel=False, started=time.time())
    if note:
        log(note)
    opts = dict(voice=d.get("voice") or SUGGESTED[0][0], speed=float(d.get("speed", 1.0)), **extra)
    threading.Thread(target=run_job, args=(work, opts), daemon=True).start()
    return jsonify(ok=True, **(reply or {}))


@app.post("/api/start")
def api_start():
    d = request.get_json(force=True)
    root = card_root(d.get("root"), need_music=not d.get("menu_only"))
    return start_job(do_build, d, root=str(root), quick=bool(d.get("quick", True)), regen=bool(d.get("regen")),
                     cleanup=bool(d.get("cleanup")), menu_only=bool(d.get("menu_only")))


# ---- add music from a PC folder
def import_src(root, src):
    src = Path((src or "").strip())
    if not src.is_dir():
        raise BadRequest("Choose the folder with your songs.")
    music = (root / "Music").resolve()
    s = src.resolve()
    if s == music or s.is_relative_to(music) or music.is_relative_to(s):
        raise BadRequest("Choose a folder outside the SD card's Music folder.")
    return src


@app.get("/api/import/preview")
def api_import_preview():
    root = card_root(request.args.get("root"))
    src = import_src(root, request.args.get("src"))
    items = fb.plan_import(root, src)
    counts = {}
    for it in items:
        counts[it["status"]] = counts.get(it["status"], 0) + 1
    return jsonify(items=items, counts=counts)


@app.post("/api/import/start")
def api_import_start():
    d = request.get_json(force=True)
    root = card_root(d.get("root"))
    src = import_src(root, d.get("src"))
    return start_job(do_import, d, root=str(root), src=str(src), quick=True)


# ---- library on the card
@app.get("/api/library")
def api_library():
    root = card_root(request.args.get("root"))
    try:
        lib = json.loads((root / fb.LIBRARY_JSON.lstrip("/")).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return jsonify(albums=[], playlists=[], missing=0, hasLibrary=False)
    tracks = lib.get("tracks", [])
    missing = 0

    def gone(o):
        nonlocal missing
        v = o.get("voice")
        m = not v or not (root / v.lstrip("/")).exists()
        missing += m
        return m

    albums = []
    for a in lib.get("albums", []):
        albums.append({"name": a["name"], "artist": a.get("artist", ""), "year": a.get("year", 0),
                       "missingClip": gone(a),
                       "tracks": [{"title": tracks[i]["title"], "artist": tracks[i].get("artist", ""),
                                   "duration": tracks[i].get("duration", 0), "path": tracks[i]["path"],
                                   "missingClip": gone(tracks[i])} for i in a.get("tracks", [])]})
    for o in lib.get("artists", []) + lib.get("genres", []) + lib.get("playlists", []):
        gone(o)
    return jsonify(albums=albums, missing=missing, hasLibrary=True)


@app.post("/api/library/delete")
def api_library_delete():
    d = request.get_json(force=True)
    root = card_root(d.get("root"))
    music = (root / "Music").resolve()
    paths = d.get("paths") or []
    for rel in paths:
        if not rel.lower().endswith(fb.AUDIO_EXTS):
            raise BadRequest(f"Not a song: {rel}")
        p = inside(music.parent, rel)
        if not p.is_relative_to(music):
            raise BadRequest("Only songs in Music can be deleted.")
    with job_lock:
        if job["running"]:
            return jsonify(error="Already running. Wait for it to finish or cancel it."), 409
    removed = 0
    for rel in paths:
        p = inside(music.parent, rel)
        if p.is_file():
            p.unlink()
            removed += 1
            for parent in p.parents:                     # drop folders left empty
                if parent == music or parent == music.parent:
                    break
                try:
                    parent.rmdir()
                except OSError:
                    break
    return start_job(do_build, d, note=f"Deleted {removed} songs", root=str(root), quick=True)


# ---- playlists
@app.get("/api/playlists")
def api_playlists():
    root = card_root(request.args.get("root"))
    out = [dict(zip(("name", "paths"), fb.read_playlist(f))) for f in fb.playlist_files(root)]
    out.sort(key=lambda p: fb.fold_key(p["name"]))
    return jsonify(out)


@app.post("/api/playlists/save")
def api_playlists_save():
    d = request.get_json(force=True)
    root = card_root(d.get("root"))
    name = (d.get("name") or "").strip()
    if not name:
        raise BadRequest("Give the playlist a name.")
    paths = [p for p in (d.get("paths") or []) if isinstance(p, str) and p.startswith("/Music/")]
    with job_lock:
        if job["running"]:
            return jsonify(error="Already running. Wait for it to finish or cancel it."), 409
    used = fb.save_playlist(root, name, paths, old_name=d.get("oldName"))
    return start_job(do_playlists, d, note=f"Saved playlist {used} ({len(paths)} songs)",
                     reply={"name": used}, root=str(root))


@app.post("/api/playlists/delete")
def api_playlists_delete():
    d = request.get_json(force=True)
    root = card_root(d.get("root"))
    with job_lock:
        if job["running"]:
            return jsonify(error="Already running. Wait for it to finish or cancel it."), 409
    fb.delete_playlist(root, d.get("name") or "")
    return start_job(do_playlists, d, note=f"Deleted playlist {d.get('name')}", root=str(root))


@app.post("/api/cancel")
def api_cancel():
    with job_lock:
        job["cancel"] = True
    return jsonify(ok=True)


@app.get("/api/status")
def api_status():
    with job_lock:
        s = {k: job[k] for k in ("running", "stage", "done", "total", "item", "log", "result", "error")}
        s["elapsed"] = int(time.time() - job["started"]) if job["started"] else 0
    return jsonify(s)


@app.get("/")
def index():
    return send_from_directory(HERE / "static", "index.html")


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    url = f"http://127.0.0.1:{port}"
    print(f"Voice Studio running at {url}  (Ctrl+C to stop)")
    if "--no-browser" not in sys.argv:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=port, debug=False, threaded=True)
