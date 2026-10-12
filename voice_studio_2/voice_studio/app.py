"""
FLAC Player Voice Studio - run on your PC, open http://127.0.0.1:5000

Pick the SD card (the folder that contains "Music"), choose a Piper voice and press
Build. It writes exactly what the player's own scan makes:
    System/library.json, Voices/Menu/*.wav, Voices/Library/*/*.wav
but with Piper's natural voice instead of the robotic one.

Setup:  pip install flask piper-tts
Run:    python app.py
"""
import os
import string
import sys
import threading
import time
import webbrowser
from pathlib import Path

from flask import Flask, jsonify, request, Response

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


def run_job(opts):
    try:
        log(f"Loading voice {opts['voice']} (first time downloads ~60 MB)...")
        tts = get_tts(opts["voice"], opts["speed"])
        with tts_lock:
            res = fb.build(opts["root"], tts, quick=opts["quick"], menu_items=menu_items(),
                           regen=opts["regen"], cleanup=opts["cleanup"], menu_only=opts["menu_only"],
                           progress=progress, log=log, cancel=lambda: job["cancel"])
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


@app.post("/api/start")
def api_start():
    d = request.get_json(force=True)
    root = (d.get("root") or "").strip()
    if not root or not Path(root).is_dir():
        return jsonify(error="Choose the SD card folder first."), 400
    if not d.get("menu_only") and not (Path(root) / "Music").is_dir():
        return jsonify(error='That folder has no "Music" folder inside. Choose the SD card root.'), 400
    with job_lock:
        if job["running"]:
            return jsonify(error="Already running."), 409
        job.update(running=True, stage="Starting", done=0, total=0, item="", log=[], result=None,
                   error=None, cancel=False, started=time.time())
    opts = dict(root=root, voice=d.get("voice") or SUGGESTED[0][0], speed=float(d.get("speed", 1.0)),
                quick=bool(d.get("quick", True)), regen=bool(d.get("regen")), cleanup=bool(d.get("cleanup")),
                menu_only=bool(d.get("menu_only")))
    threading.Thread(target=run_job, args=(opts,), daemon=True).start()
    return jsonify(ok=True)


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
    return Response(PAGE, mimetype="text/html")


PAGE = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Voice Studio</title>
<style>
:root{--bg:#f6f5f2;--card:#fff;--ink:#1d1d1f;--mute:#6b6b70;--line:#e3e1dc;--acc:#2f6f5e;--acc2:#e8f1ee;--bad:#b3261e}
@media (prefers-color-scheme:dark){:root{--bg:#141414;--card:#1e1e1e;--ink:#eee;--mute:#9a9a9a;--line:#333;--acc:#5fb39b;--acc2:#1f2d29;--bad:#ff8a80}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--ink);font:15px/1.45 system-ui,-apple-system,Segoe UI,sans-serif}
main{max-width:860px;margin:0 auto;padding:28px 16px 60px}
h1{font-size:22px;margin:0 0 4px}p.sub{color:var(--mute);margin:0 0 22px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:18px;margin-bottom:16px}
.card h2{font-size:13px;letter-spacing:.06em;text-transform:uppercase;color:var(--mute);margin:0 0 12px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
input[type=text],select{font:inherit;color:inherit;background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:8px 10px}
input[type=text]{flex:1;min-width:200px}
button{font:inherit;border:1px solid var(--line);background:var(--card);color:var(--ink);border-radius:8px;padding:8px 14px;cursor:pointer}
button.primary{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
button:disabled{opacity:.5;cursor:default}
.list{border:1px solid var(--line);border-radius:8px;max-height:230px;overflow:auto;margin-top:10px}
.list div{padding:7px 12px;border-bottom:1px solid var(--line);cursor:pointer}.list div:last-child{border:0}
.list div:hover{background:var(--acc2)}
.badge{display:inline-block;font-size:12px;padding:2px 8px;border-radius:99px;background:var(--acc2);color:var(--acc);margin-left:6px}
.badge.bad{color:var(--bad);background:transparent;border:1px solid var(--bad)}
label.opt{display:flex;gap:8px;align-items:flex-start;margin:8px 0;cursor:pointer}label.opt small{color:var(--mute);display:block}
.bar{height:10px;background:var(--bg);border:1px solid var(--line);border-radius:99px;overflow:hidden;margin:10px 0 6px}
.bar>i{display:block;height:100%;width:0;background:var(--acc);transition:width .3s}
.stat{color:var(--mute);font-size:14px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
pre{background:var(--bg);border:1px solid var(--line);border-radius:8px;padding:10px;max-height:260px;overflow:auto;font-size:12.5px;margin:10px 0 0;white-space:pre-wrap}
.err{color:var(--bad);margin-top:8px}
</style></head><body><main>
<h1>Voice Studio</h1>
<p class="sub">Builds the player's music library and natural voice clips on your PC, then you put the SD card back in the player.</p>

<div class="card"><h2>1. SD card</h2>
  <div class="row"><input type="text" id="path" placeholder="Folder that contains Music, e.g. E:\">
    <button id="go">Open</button><button id="up">Up</button><button id="use" class="primary">Use this folder</button></div>
  <div id="pathinfo" class="stat" style="margin-top:8px"></div>
  <div class="list" id="dirs"></div>
</div>

<div class="card"><h2>2. Voice</h2>
  <div class="row"><select id="voice"></select>
    <label>Speed <select id="speed"><option value="0.8">Fast</option><option value="0.9">A bit fast</option><option value="1.0" selected>Normal</option><option value="1.15">Slow</option></select></label></div>
  <div class="row" style="margin-top:10px"><input type="text" id="sample" value="Now playing. Albums. Artists. Bjørn Eidsvåg.">
    <button id="preview">Listen</button></div>
  <audio id="audio" controls style="width:100%;margin-top:10px;display:none"></audio>
  <div id="perr" class="err"></div>
</div>

<div class="card"><h2>3. Build</h2>
  <label class="opt"><input type="checkbox" id="quick" checked><span>Quick scan<small>Only read songs that are new or changed since the last library.json.</small></span></label>
  <label class="opt"><input type="checkbox" id="regen"><span>Remake all voice clips<small>Normally only missing clips, robotic ones and clips from another voice or speed are made.</small></span></label>
  <label class="opt"><input type="checkbox" id="cleanup"><span>Delete unused song clips<small>Removes clips for songs that are no longer on the card.</small></span></label>
  <label class="opt"><input type="checkbox" id="menuonly"><span>Menu voices only<small>Just the menu clips, no library scan.</small></span></label>
  <div class="row" style="margin-top:12px"><button id="start" class="primary">Build</button><button id="cancel" disabled>Cancel</button></div>
  <div class="bar"><i id="fill"></i></div>
  <div class="stat" id="status">Idle</div>
  <div id="serr" class="err"></div>
  <pre id="log"></pre>
</div>
</main>
<script>
const $=id=>document.getElementById(id);
let cur="", parent=null, chosen=localStorage.getItem("vs_root")||"";
async function ls(p){
  const r=await fetch("/api/ls?path="+encodeURIComponent(p)); const d=await r.json();
  if(d.error){$("pathinfo").textContent=d.error;return}
  cur=d.path; parent=d.parent; $("path").value=d.path;
  $("dirs").innerHTML=""; d.dirs.forEach(n=>{const e=document.createElement("div");e.textContent=n;
    e.onclick=()=>ls(d.isRoot?n:(d.path.replace(/[\\/]+$/,"")+(d.path.includes("\\")?"\\":"/")+n));$("dirs").appendChild(e)});
  $("pathinfo").innerHTML=d.isRoot?"Choose a drive":(d.hasMusic?'Music folder found<span class="badge">ready</span>'+(d.hasLibrary?'<span class="badge">has library.json</span>':''):'No Music folder here<span class="badge bad">not the SD root</span>');
  $("up").disabled=parent===null;
}
$("go").onclick=()=>ls($("path").value);
$("path").onkeydown=e=>{if(e.key==="Enter")ls($("path").value)};
$("up").onclick=()=>ls(parent??"");
$("use").onclick=()=>{chosen=cur;localStorage.setItem("vs_root",chosen);$("pathinfo").innerHTML="Using "+chosen+'<span class="badge">selected</span>'};
async function voices(){const v=await (await fetch("/api/voices")).json();
  $("voice").innerHTML=v.map(x=>`<option value="${x.id}">${x.label}${x.downloaded?"":"  (downloads)"}</option>`).join("");
  const s=localStorage.getItem("vs_voice"); if(s)$("voice").value=s;
  const sp=localStorage.getItem("vs_speed"); if(sp)$("speed").value=sp;}
$("voice").onchange=()=>localStorage.setItem("vs_voice",$("voice").value);
$("speed").onchange=()=>localStorage.setItem("vs_speed",$("speed").value);
$("preview").onclick=async()=>{$("perr").textContent="";$("preview").disabled=true;$("preview").textContent="Making...";
  try{const r=await fetch("/api/preview",{method:"POST",headers:{"Content-Type":"application/json"},
    body:JSON.stringify({voice:$("voice").value,speed:$("speed").value,text:$("sample").value})});
    if(!r.ok){$("perr").textContent=(await r.json()).error;return}
    const a=$("audio");a.src=URL.createObjectURL(await r.blob());a.style.display="block";a.play();voices();}
  finally{$("preview").disabled=false;$("preview").textContent="Listen"}};
$("start").onclick=async()=>{$("serr").textContent="";
  const root=chosen||cur;
  const r=await fetch("/api/start",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({
    root,voice:$("voice").value,speed:$("speed").value,quick:$("quick").checked,regen:$("regen").checked,
    cleanup:$("cleanup").checked,menu_only:$("menuonly").checked})});
  if(!r.ok){$("serr").textContent=(await r.json()).error;return} poll();};
$("cancel").onclick=()=>fetch("/api/cancel",{method:"POST"});
function fmt(s){return Math.floor(s/60)+":"+String(s%60).padStart(2,"0")}
async function poll(){const s=await (await fetch("/api/status")).json();
  $("start").disabled=s.running;$("cancel").disabled=!s.running;
  const pct=s.total?Math.round(100*s.done/s.total):(s.running?5:(s.result?100:0));
  $("fill").style.width=pct+"%";
  let eta=""; if(s.running&&s.total&&s.done>5&&s.stage==="Voices"){eta="  ~"+fmt(Math.round(s.elapsed/s.done*(s.total-s.done)))+" left"}
  $("status").textContent=s.stage+(s.total?` ${s.done}/${s.total}`:(s.done?` ${s.done} files`:""))+(s.item?"  "+s.item:"")+eta;
  $("log").textContent=s.log.join("\n");$("log").scrollTop=1e9;
  if(s.error&&s.error!=="cancelled")$("serr").textContent=s.error;
  if(s.running)setTimeout(poll,500);}
ls(chosen||"");voices();poll();
</script></body></html>"""


if __name__ == "__main__":
    port = int(os.environ.get("PORT", 5000))
    url = f"http://127.0.0.1:{port}"
    print(f"Voice Studio running at {url}  (Ctrl+C to stop)")
    if "--no-browser" not in sys.argv:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    app.run(host="127.0.0.1", port=port, debug=False, threaded=True)
