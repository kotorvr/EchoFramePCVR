#!/usr/bin/env python3
"""Unattended A/B of Echo settings in an echovrce session, judged by runtime.log's frame timing.

  ab_lobby.py SESSION CONFIG [CONFIG...]

SESSION is an echovrce session id (frame.py join), or offline-lobby / offline-arena for an
offline session of Echo's own (frame.py offline): no other players, the same scene every time.

Each CONFIG is NAME:item|item|... where an item is
  env:KEY=VALUE      environment variable for Echo (e.g. env:FDM_DEBUG=enable)
  ini:KEY=VALUE      the runtime's echoframe.ini (e.g. ini:Foveation=0, ini:RenderScale=0.8)
  gfx:KEY=VALUE      Echo's graphics settings (e.g. gfx:temporalaa=false)
  live:KEY=VALUE     echoframe.ini change made after the first measurement, then measured again
                     (e.g. live:Foveation=0, live:Poke=20AFBC8:f:4.0); several live items are
                     applied one after another, each measured with the ones before it
  tu:FLAGS           Turnip debug flags written live, as a step like live (e.g. tu:nolrz)
For every config Echo is restarted into the session (frame.py join), the run waits until the
player has spawned plus SETTLE seconds, then averages the GPU/fps lines of the next MEASURE
seconds. ini and gfx changes are undone after each config. Results are appended to
artifacts/ab-<date>.txt. Meant for a headset lying still (fixed view) or a player standing still.
"""
import json
import os
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import frame  # noqa: E402

SETTLE = int(os.environ.get("AB_SETTLE", "25"))
MEASURE = int(os.environ.get("AB_MEASURE", "40"))
GPU = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.\d+\]\s+ms: gpu avg ([\d.]+) p95 ([\d.]+)")
FPS = re.compile(r"\[(\d\d):(\d\d):(\d\d)\.\d+\] frames: ([\d.]+) fps over \d+ s \(display ([\d.]+) Hz\), slowest frame ([\d.]+)")
DEFAULT_INI = {"Foveation": "2", "RenderScale": "1.0", "Patch": "", "Poke": "", "Skip": "", "PassTiming": "0"}


def secs(m):
    return int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3))


def frame_now():
    h, m, s = frame.sh("date +%T").strip().split(":")
    return int(h) * 3600 + int(m) * 60 + int(s)


def spawned():
    # our own player (the account Echo logged in with) spawning in the session
    out = frame.sh("L=$(ls -t " + frame.ROOT + "/_local/r14logs/* | head -1); "
                   "ID=$(grep -o -m1 'Logging in OVR-ORG-[0-9]*' \"$L\" | cut -d' ' -f3); "
                   "if [ -n \"$ID\" ]; then grep -c \"User $ID (.*) spawned\" \"$L\"; else echo 0; fi; "
                   "grep -c 'join error' \"$L\"", check=False).split()
    return (int(out[0]) if out else 0), (int(out[1]) if len(out) > 1 else 0)


def window(start, end):
    log = frame.remote_path(frame.BIN) + "/EchoFrame/runtime.log"   # expanded: it goes in single quotes
    text = frame.sh(f"grep -E 'frames:|  ms:' '{log}'", check=False)
    gpu = [(float(m.group(4)), float(m.group(5))) for m in GPU.finditer(text) if start <= secs(m) <= end]
    fps = [(float(m.group(4)), float(m.group(5)), float(m.group(6))) for m in FPS.finditer(text) if start <= secs(m) <= end]
    if not gpu:
        return None
    n = len(gpu)
    return {"gpu": sum(g[0] for g in gpu) / n, "p95": sum(g[1] for g in gpu) / n,
            "fps": sum(f[0] for f in fps) / max(1, len(fps)), "hz": min((f[1] for f in fps), default=0),
            "slowest": max((f[2] for f in fps), default=0), "n": n}


def run(session, name, items, out):
    env, ini, gfx, live = [], {}, {}, []
    for it in items:
        kind, kv = it.split(":", 1)
        k, v = kv.split("=", 1) if "=" in kv else (kv, "")
        if kind == "env":
            env.append(kv)
        elif kind == "ini":
            ini[k] = v
        elif kind == "gfx":
            gfx[k] = v
        elif kind == "live":
            live.append(("ini", kv))
        elif kind == "tu":
            live.append(("tu", kv))
    frame.stop()
    frame.cmd_ini([f"{k}={v}" for k, v in {**DEFAULT_INI, **ini}.items()])
    settings = frame.settings_path()
    if gfx:
        frame.sh(f"cp '{settings}' '{settings}.ab-orig'")
        frame.cmd_graphics([f"{k}={v}" for k, v in gfx.items()])
    if session.startswith("offline-"):
        frame.cmd_offline([session[len("offline-"):]] + env)
    else:
        frame.cmd_join([session] + env)
    t0 = time.time()
    ok = False
    while time.time() - t0 < 240:
        time.sleep(5)
        s, err = spawned()
        if err:
            break
        if s:
            ok = True
            break
    result = []
    if not ok:
        result.append(f"{name}: no spawn ({'join error' if err else 'timeout'})")
    else:
        time.sleep(SETTLE)
        a = frame_now()
        time.sleep(MEASURE + 2)
        r = window(a, frame_now())
        result.append(f"{name}: " + (f"gpu {r['gpu']:.1f} ms (p95 {r['p95']:.1f}), {r['fps']:.1f} fps, display {r['hz']:.0f} Hz, "
                                     f"slowest {r['slowest']:.0f} ms ({r['n']} lines)" if r else "no timing lines"))
        for kind, kv in live:
            if kind == "tu":
                frame.cmd_tudebug([kv])
            else:
                frame.cmd_ini([kv])
            time.sleep(12)
            a = frame_now()
            time.sleep(MEASURE + 2)
            r = window(a, frame_now())
            result.append(f"{name} + {kind} {kv}: " + (f"gpu {r['gpu']:.1f} ms (p95 {r['p95']:.1f}), {r['fps']:.1f} fps, "
                                                     f"display {r['hz']:.0f} Hz ({r['n']} lines)" if r else "no timing lines"))
        if any(kind == "tu" for kind, _ in live):
            frame.cmd_tudebug([""])
    frame.stop()
    if gfx:   # Echo's settings as they were
        frame.sh(f"cp '{settings}.ab-orig' '{settings}'")
    for line in result:
        print(line, flush=True)
        out.write(time.strftime("%H:%M ") + line + "\n")
        out.flush()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    frame.SERIAL = frame.find_frame()
    session = sys.argv[1]
    path = os.path.join(frame.REPO, "artifacts", time.strftime("ab-%Y%m%d.txt"))
    with open(path, "a", encoding="utf-8") as out:
        for spec in sys.argv[2:]:
            name, _, items = spec.partition(":")
            run(session, name, [i for i in items.split("|") if i], out)
    frame.cmd_ini([f"{k}={v}" for k, v in DEFAULT_INI.items()])


if __name__ == "__main__":
    main()
