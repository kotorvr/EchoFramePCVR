#!/usr/bin/env python3
"""EchoFramePCVR on the Steam Frame, from the PC, over adb (Developer Mode, USB-C or Wi-Fi).

  frame.py recon                      what the Frame has (SteamOS, Proton, SteamVR, OpenXR, GPU)
  frame.py push-game ECHO_DIR         copies an Echo VR PCVR install to the Frame (resumable)
  frame.py install                    copies build/out/ to it and sets up the Steam shortcut
  frame.py launch [KEY=VALUE...] [ECHO_ARGS...]
                                      starts Echo on the Frame (stopping a running one first), with
                                      extra environment variables and arguments (e.g. TU_DEBUG=sysmem)
  frame.py join SPARK_LINK            starts Echo straight into an echovrce session from the Discord
                                      bot's /create (private arena, combat or social lobby)
  frame.py refresh [HZ]               shows or sets Echo's display rate (72, 80, 90, 96, 100, 120)
  frame.py stop                       ends Echo's session (Echo, its crash reporter, Wine)
  frame.py graphics [KEY=VALUE...]    shows or sets Echo's graphics settings on the Frame (Echo stopped);
                                      "frame" applies the Frame profile (no adaptive res, 72 fps, no MSAA)
  frame.py wait [SECONDS]             follows a launch until frames flow, Echo crashes or exits
  frame.py logs                       pulls every log into artifacts/logs/<time>/
  frame.py shell CMD...               runs a command in the Frame's shell

The Frame's adb is a SteamOS shell as the logged-in user. Off USB, every command goes over
Wi-Fi by SSH instead (the address saved the last time the Frame was on USB, or EFP_SSH). Echo goes to
~/EchoVR/ready-at-dawn-echo-arena and runs from a non-Steam shortcut ("Echo VR (PCVR)")
under Proton for ARM64, which runs x86-64 Windows code with FEX.
"""
import json
import os
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "build", "out")
ROOT = "$HOME/EchoVR/ready-at-dawn-echo-arena"
BIN = ROOT + "/bin/win10"
STATE = "$HOME/EchoVR/efp"                  # the shortcut id and helper scripts on the Frame
NAME = "Echo VR (PCVR)"
VRCMD = "export XDG_RUNTIME_DIR=/run/user/$(id -u) LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64; /opt/steamvr/bin/linuxarm64/vrcmd"
# Proton writes $HOME/steam-<game id>.log with PROTON_LOG=1
LAUNCH_OPTIONS = "PROTON_LOG=1 %command%"

# What an Echo install needs on the Frame. bin/win10 leaves out mod loaders, plugins and logs:
# the Frame build starts from a stock Echo.
GAME_DIRS = ["_data", "content", "sourcedb", "OC_ASSET_FILES"]
BIN_SKIP = {"dbgcore.dll", "plugins", "plugin_logs", "echoloader.json", "haptics_config.txt",
            "EchoFrame", "EchoFrame.exe", "echovr_openxr.exe", "LibOVRPlatform64_1.dll"}


def find_adb():
    adb = shutil.which("adb")
    if adb:
        return adb
    local = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "platform-tools", "adb.exe")
    if os.path.exists(local):
        return local
    sys.exit("adb not found: install Google's platform-tools")


ADB = find_adb()


def run(*args, timeout=None, check=True):
    p = subprocess.run([ADB, *args], capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
    if check and p.returncode:
        sys.exit(f"adb {' '.join(args[:3])} failed: {(p.stderr or p.stdout).strip()[-400:]}")
    return p.stdout


# Without USB (the Frame charging on the wall), commands go over Wi-Fi through SSH as steamos.
# The Frame's adbd has no TCP mode, but its sshd runs and takes the key Frame Control installs.
# The address is saved whenever the Frame is on USB; EFP_SSH=user@host and EFP_SSH_KEY override.
HOST_FILE = os.path.join(REPO, "artifacts", "frame_host")
SSH_KEY = os.environ.get("EFP_SSH_KEY", os.path.join(os.path.expanduser("~"), ".ssh", "id_rsa_frame_devkit"))
SSH_OPTS = ["-i", SSH_KEY, "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", "-o", "StrictHostKeyChecking=accept-new"]


def ssh_ok(host):
    p = subprocess.run(["ssh", *SSH_OPTS, host, "grep -s ^ID= /etc/os-release"], capture_output=True, text=True, timeout=20)
    return "steamos" in p.stdout


def find_frame():
    """The adb serial of a Frame on USB, or "ssh:user@host" for one on Wi-Fi."""
    if os.environ.get("EFP_SSH"):
        return "ssh:" + os.environ["EFP_SSH"]
    serial = os.environ.get("EFP_SERIAL")
    if serial:
        return serial
    for line in run("devices").splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            if "steamos" in run("-s", parts[0], "shell", "grep -s ^ID= /etc/os-release", check=False):
                ip = run("-s", parts[0], "shell", "ip -4 -o addr show wlan0 | awk '{print $4}' | cut -d/ -f1", check=False).strip()
                if ip:
                    os.makedirs(os.path.dirname(HOST_FILE), exist_ok=True)
                    with open(HOST_FILE, "w") as f:
                        f.write(f"steamos@{ip}\n")
                return parts[0]
    if os.path.exists(HOST_FILE) and os.path.exists(SSH_KEY):
        host = open(HOST_FILE).read().strip()
        if ssh_ok(host):
            return "ssh:" + host
        sys.exit(f"no Steam Frame on USB, and SSH to {host} (last Wi-Fi address) doesn't answer")
    sys.exit("no Steam Frame on adb: turn on Developer Mode and plug it in (or adb connect its address)")


SERIAL = None


def over_ssh():
    return SERIAL.startswith("ssh:")


def sh(command, timeout=None, check=True):
    if not over_ssh():
        return run("-s", SERIAL, "shell", command, timeout=timeout, check=check)
    p = subprocess.run(["ssh", *SSH_OPTS, SERIAL[4:], command], capture_output=True, text=True,
                       encoding="utf-8", errors="replace", timeout=timeout)
    if check and p.returncode:
        sys.exit(f"ssh {command[:60]} failed: {(p.stderr or p.stdout).strip()[-400:]}")
    return p.stdout


def push(local, remote, sync=False, timeout=None):
    if over_ssh():
        if sync:
            sys.exit("push-game needs the Frame on USB")
        p = subprocess.run(["scp", "-q", *SSH_OPTS, local, f"{SERIAL[4:]}:{remote}"], timeout=timeout)
    else:
        args = ["-s", SERIAL, "push"] + (["--sync"] if sync else []) + [local, remote]
        p = subprocess.run([ADB, *args], timeout=timeout)
    if p.returncode:
        sys.exit(f"copying {local} failed")


def pull(remote, dest):
    if over_ssh():
        return subprocess.run(["scp", "-q", *SSH_OPTS, f"{SERIAL[4:]}:{remote}", dest], capture_output=True, text=True)
    return subprocess.run([ADB, "-s", SERIAL, "pull", remote, dest], capture_output=True, text=True)


def remote_path(path):
    return sh(f'echo "{path}"').strip()


def throttle_off():
    """SteamOS's power-save profile makes SteamVR throttle apps (steamvr.powersaveFramesToThrottle
    = 1, found set on the Frame). vrcmd takes the key and the value as separate arguments
    ("section.key=value" is "Unknown command") and needs its own folder on LD_LIBRARY_PATH."""
    out = sh(f"{VRCMD} --set-settings-int steamvr.powersaveFramesToThrottle 0 2>&1 | tail -1; "
             f"{VRCMD} --settings-int steamvr.powersaveFramesToThrottle 2>&1 | tail -1", check=False)
    if "=0" not in out:
        print(f"warning: couldn't turn SteamVR's power-save throttling off: {out.strip()[-160:]}")


# The display rate while Echo runs. SteamVR on the Frame picks 72 Hz for an app unless the app's
# own SteamVR setting asks otherwise (the home runs at 120); xrRequestDisplayRefreshRateFB is
# ignored there. The app key is steam.app.<shortcut id>. 72, 80, 90, 96, 100 and 120 Hz exist.
DEFAULT_REFRESH = 90


def refresh_rate(hz=None):
    shortcut = sh(f"cat '{remote_path(STATE)}/shortcut.id' 2>/dev/null", check=False).strip()
    if not shortcut.isdigit():
        sys.exit("no shortcut yet: run frame.py install first")
    key = f"steam.app.{shortcut}.preferredRefreshRate"
    if hz is not None:
        sh(f"{VRCMD} --set-settings-float {key} {float(hz)} 2>&1 | tail -1", check=False)
    out = sh(f"{VRCMD} --settings-float {key} 2>&1 | tail -1", check=False).strip()
    return out.split("=")[-1] if "=" in out else ""


def cmd_refresh(args):
    value = refresh_rate(float(args[0]) if args else None)
    print(f"Echo's display rate: {value or 'SteamVR default (72 Hz)'}")


def steam(*args, timeout=60):
    script = remote_path(STATE) + "/steam_cdp.py"
    quoted = " ".join("'" + a.replace("'", "'\\''") + "'" for a in args)
    out = sh(f"python3 '{script}' {quoted} 2>&1", timeout=timeout, check=False).strip().splitlines()
    return out[-1].strip() if out else ""


# ---------------------------------------------------------------------------------------
RECON = r'''
P="$HOME/.local/share/Steam"
echo "## system"; grep -E '^(BUILD_ID|VERSION_ID|VARIANT_ID)=' /etc/os-release; uname -r; nproc --all; free -h | head -2
df -h "$HOME" | tail -1
echo "## compatibility tools"; ls "$P/steamapps/common" | grep -i proton; ls "$P/compatibilitytools.d" 2>/dev/null
for d in "$P"/steamapps/common/Proton*; do echo "$d: $(cat "$d/version" 2>/dev/null)"; done
echo "## SteamVR"; cat /opt/steamvr/bin/version.txt 2>/dev/null; pgrep -a vrserver | cut -c1-120
echo "## OpenXR runtime"; cat "$HOME/.config/openxr/1/active_runtime.json" 2>/dev/null
echo "## OpenVR paths"; cat "$HOME/.config/openvr/openvrpaths.vrpath" 2>/dev/null
echo "## Vulkan"; (vulkaninfo --summary 2>/dev/null || echo "no vulkaninfo") | grep -E "deviceName|driverName|driverInfo|apiVersion" | head -6
echo "## SteamVR settings"; grep -E 'powersave|preferredRefreshRate|supersample' "$HOME/.config/openvr/config/steamvr.vrsettings" 2>/dev/null
echo "## Echo on the Frame"; ls "$HOME/EchoVR/ready-at-dawn-echo-arena" 2>/dev/null; cat "$HOME/EchoVR/efp/shortcut.id" 2>/dev/null
'''


def cmd_recon(args):
    out = sh(RECON, timeout=60, check=False)
    print(out)
    os.makedirs(os.path.join(REPO, "artifacts"), exist_ok=True)
    path = os.path.join(REPO, "artifacts", time.strftime("recon-%Y%m%d-%H%M%S.txt"))
    with open(path, "w", encoding="utf-8") as f:
        f.write(out)
    print(f"saved {path}")


def cmd_push_game(args):
    if not args:
        sys.exit("usage: frame.py push-game <ready-at-dawn-echo-arena folder>")
    src = os.path.abspath(args[0])
    if not os.path.exists(os.path.join(src, "bin", "win10", "echovr.exe")):
        sys.exit(f"{src} isn't an Echo VR install (no bin\\win10\\echovr.exe)")
    root = remote_path(ROOT)
    sh(f"mkdir -p '{root}/bin/win10' '{root}/_local'")
    for d in GAME_DIRS:
        if os.path.isdir(os.path.join(src, d)):
            print(f"== {d}")
            push(os.path.join(src, d), root, sync=True)
    print("== bin/win10")
    for name in sorted(os.listdir(os.path.join(src, "bin", "win10"))):
        if name not in BIN_SKIP:
            push(os.path.join(src, "bin", "win10", name), f"{root}/bin/win10/", sync=True)
    config = os.path.join(src, "_local", "config.json")
    if os.path.exists(config) and "yes" not in sh(f"[ -f '{root}/_local/config.json' ] && echo yes", check=False):
        push(config, f"{root}/_local/config.json")
        print("copied _local/config.json (the community servers)")
    print(f"Echo is on the Frame in {root}")


def cmd_install(args):
    for f in ("EchoFrame.exe", "LibOVRRT64_1.dll", "LibOVRPlatform64_1.dll", "echoframe.ini"):
        if not os.path.exists(os.path.join(OUT, f)):
            sys.exit(f"build/out/{f} is missing: run build.cmd first")
    root, state = remote_path(ROOT), remote_path(STATE)
    if "yes" not in sh(f"[ -f '{root}/bin/win10/echovr.exe' ] && echo yes", check=False):
        sys.exit("Echo isn't on the Frame yet: run frame.py push-game first")
    sh(f"mkdir -p '{root}/bin/win10/EchoFrame' '{state}'")
    push(os.path.join(OUT, "EchoFrame.exe"), f"{root}/bin/win10/EchoFrame.exe")
    for f in ("LibOVRRT64_1.dll", "LibOVRPlatform64_1.dll"):
        push(os.path.join(OUT, f), f"{root}/bin/win10/EchoFrame/{f}")
    if "yes" not in sh(f"[ -f '{root}/bin/win10/EchoFrame/echoframe.ini' ] && echo yes", check=False):
        push(os.path.join(OUT, "echoframe.ini"), f"{root}/bin/win10/EchoFrame/echoframe.ini")
    push(os.path.join(REPO, "frame", "steam_cdp.py"), f"{state}/steam_cdp.py")

    shortcut = sh(f"cat '{state}/shortcut.id' 2>/dev/null", check=False).strip()
    if not shortcut.isdigit() or steam("has", shortcut) != "yes":
        shortcut = steam("add", NAME, f"{root}/bin/win10/EchoFrame.exe", f"{root}/bin/win10")
        if not shortcut.isdigit():
            sys.exit(f"Steam didn't add the shortcut ({shortcut[:200] or 'no answer'}); is the Frame on its home screen?")
        sh(f"echo {shortcut} > '{state}/shortcut.id'")
        print(f"added Steam shortcut {shortcut}")
    # Proton 11.0 (ARM64), app 4628740: Wine, wineopenxr and vkd3d-proton run natively and only
    # Echo's x86-64 code goes through FEX. ("proton_11" makes Steam run the x86-64 Proton inside
    # FEX-Emu, whose Linux OpenXR loader can't load SteamVR's ARM64 runtime: wineopenxr fails.)
    # The Frame's Steam client has no GetAvailableCompatTools; names are in its appinfo.vdf.
    tool = os.environ.get("EFP_PROTON", "proton_11-arm64")
    print("configure:", steam("configure", shortcut, tool, LAUNCH_OPTIONS))
    throttle_off()
    rate = refresh_rate()
    if not rate or rate.startswith("0"):
        rate = refresh_rate(DEFAULT_REFRESH)
    print(f"installed; shortcut {shortcut} runs EchoFrame.exe with {tool} at {rate} Hz")


def game_id():
    shortcut = sh(f"cat '{remote_path(STATE)}/shortcut.id' 2>/dev/null", check=False).strip()
    if not shortcut.isdigit():
        sys.exit("no shortcut yet: run frame.py install first")
    return (int(shortcut) << 32) | 0x02000000


def stop():
    """Ends the shortcut's whole session: Steam's reaper for it and everything under it. Echo's
    crash reporter (BsSndRpt64.exe) otherwise keeps a crashed session open, and Steam won't
    start the shortcut again while it runs."""
    shortcut = sh(f"cat '{remote_path(STATE)}/shortcut.id' 2>/dev/null", check=False).strip()
    if not shortcut.isdigit():
        return
    # SIGKILL (Wine's processes outlived SIGTERM). The [x] in each pattern keeps pkill -f from
    # matching this shell's own command line, which contains the patterns.
    sh(f"pkill -9 -f '[B]sSndRpt64.exe'; pkill -9 -f '[e]chovr_openxr.exe'; pkill -9 -f '[E]choFrame.exe'; "
       f"pkill -9 -f '[A]ppId={shortcut} '; sleep 2", check=False)


def cmd_stop(args):
    stop()
    print("stopped")


def cmd_launch(args):
    """KEY=VALUE arguments go into Echo's environment (e.g. TU_DEBUG=sysmem), the rest to Echo
    itself; without any, the shortcut's launch options go back to the default."""
    stop()
    throttle_off()
    gid = game_id()
    env = [a for a in args if "=" in a and not a.startswith("-")]
    options = " ".join([*env, LAUNCH_OPTIONS, *(a for a in args if a not in env)])
    if steam("configure", str(gid >> 32), os.environ.get("EFP_PROTON", "proton_11-arm64"), options) != "ok":
        sys.exit("Steam didn't take the launch options; is the Frame on its home screen?")
    sh(f"date +%s > '{remote_path(STATE)}/launched'; nohup steam steam://rungameid/{gid} >/dev/null 2>&1 &")
    print(f"started game {gid}; logs with: frame.py logs")


def cmd_join(args):
    """Starts Echo straight into an echovrce session: a private arena, combat match or social
    lobby made with the echovrce Discord bot's /create (mode: Private Arena Match, Private
    Combat Match or Private Social Lobby), which answers with a spark://c/<id> link. Echo's
    -lobbyid joins that session when it starts. KEY=VALUE arguments as for launch."""
    import re
    ids = [m for a in args for m in re.findall(r"[0-9A-Fa-f]{8}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{4}-[0-9A-Fa-f]{12}", a)]
    if not ids:
        sys.exit("usage: frame.py join <spark link or session id> [KEY=VALUE...]")
    env = [a for a in args if "=" in a and not a.startswith("-") and "://" not in a]
    cmd_launch(env + ["-lobbyid", ids[0].upper()])
    print(f"joining echovrce session {ids[0].upper()}")


WAIT_PROBE = r'''
R="{root}/bin/win10/EchoFrame"; L=$(ls -t "{root}/_local/r14logs/"* 2>/dev/null | head -1)
echo "start=$(stat -c %Y "$R/runtime.log" 2>/dev/null || echo 0)"
echo "frames=$(grep -c 'frames: [0-9]' "$R/runtime.log" 2>/dev/null)"
echo "crash=$(grep -c 'Crash detected' "$L" 2>/dev/null)"
echo "log=$(stat -c %Y "$L" 2>/dev/null || echo 0)"
echo "running=$(pgrep -f echovr_openxr.exe >/dev/null && echo 1 || echo 0)"
'''


def probe(root):
    out = dict(l.split("=", 1) for l in sh(WAIT_PROBE.format(root=root), check=False).split() if "=" in l)
    return {k: int(v) if v.strip().isdigit() else 0 for k, v in out.items()}


def cmd_wait(args):
    """Follows the last launch (frame.py launch records when it started)."""
    root = remote_path(ROOT)
    limit = int(args[0]) if args else 300
    launched = sh(f"cat '{remote_path(STATE)}/launched' 2>/dev/null", check=False).strip()
    since = int(launched) if launched.isdigit() else int(sh("date +%s").strip())
    start = time.time()
    state = "timeout"
    while time.time() - start < limit:
        p = probe(root)
        if p.get("start", 0) > since:   # a stopped run may have written in the launch second
            if p.get("frames", 0) >= 2:
                state = "frames"
                break
            if p.get("crash", 0) and p.get("log", 0) >= since:
                state = "crash"
                break
            if not p.get("running") and time.time() - start > 20:
                state = "exited"
                break
        time.sleep(5)
    print(f"== {state} after {int(time.time() - start)} s")
    print(sh(f"grep -v -e '  extension:' -e 'runtime offers' '{root}/bin/win10/EchoFrame/runtime.log' | tail -n 25 | cut -c1-200; "
             f"echo '-- platform'; head -6 '{root}/bin/win10/EchoFrame/Support/oculus-runtime/platform.log' 2>/dev/null; "
             f"echo '-- echo'; L=$(ls -t '{root}/_local/r14logs/'* | head -1); "
             f"grep -v -i -E 'password|token|Resetting player|Memory|GPU Memory' \"$L\" | tail -n 8 | cut -c1-170", check=False))


# Echo's settings in the shortcut's Wine prefix (the same file as %LOCALAPPDATA%/rad/loneecho/ on Windows) 
def settings_path():
    shortcut = sh(f"cat '{remote_path(STATE)}/shortcut.id' 2>/dev/null", check=False).strip()
    return (f"{remote_path('$HOME')}/.local/share/Steam/steamapps/compatdata/{shortcut}/pfx/drive_c/users/steamuser/"
            "AppData/Local/rad/loneecho/settings_mp_v2.json")


# Adaptive resolution drops to its floor (0.7) on the Frame's GPU, which looked very blurry; its
# target was 90 fps on a 72 Hz display. Multi-Res is an NVIDIA feature.
FRAME_GRAPHICS = {"adaptiveresolution": False, "adaptiverestargetframerate": 72, "resolutionscale": 1.0,
                  "msaa": 0, "multires": False}


def cmd_graphics(args):
    path = settings_path()
    text = sh(f"cat '{path}' 2>/dev/null", check=False)
    if not text.strip():
        sys.exit("Echo hasn't written its settings on the Frame yet: launch it once")
    settings = json.loads(text)
    graphics = settings.setdefault("graphics", {})
    if not args:
        print(json.dumps(graphics, indent=1))
        return
    if "yes" in sh("pgrep -f '[e]chovr_openxr.exe' >/dev/null && echo yes", check=False):
        sys.exit("Echo is running and would overwrite its settings: frame.py stop first")
    changes = dict(FRAME_GRAPHICS) if args == ["frame"] else {}
    for a in args:
        if "=" in a:
            k, v = a.split("=", 1)
            changes[k] = json.loads(v) if v not in ("true", "false") else v == "true"
    graphics.update(changes)
    local = os.path.join(REPO, "artifacts", "settings_mp_v2.json")
    os.makedirs(os.path.dirname(local), exist_ok=True)
    with open(local, "w", encoding="utf-8", newline="\n") as f:
        json.dump(settings, f, indent=2)
    sh(f"cp '{path}' '{path}.efp-bak' 2>/dev/null", check=False)
    push(local, path)
    print("set: " + ", ".join(f"{k}={v}" for k, v in changes.items()))


def cmd_logs(args):
    root = remote_path(ROOT)
    home = remote_path("$HOME")
    dest = os.path.join(REPO, "artifacts", "logs", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(dest, exist_ok=True)
    gid = game_id()
    files = [f"{root}/bin/win10/EchoFrame/launcher.log", f"{root}/bin/win10/EchoFrame/runtime.log",
             f"{root}/bin/win10/EchoFrame/Support/oculus-runtime/platform.log", f"{root}/bin/win10/EchoFrame/hmd_cache.txt",
             f"{home}/steam-{gid}.log"]
    newest = sh(f"ls -t '{root}/_local/r14logs/' 2>/dev/null | head -1", check=False).strip()
    if newest:
        files.append(f"{root}/_local/r14logs/{newest}")
    for name in ("vrserver.txt", "vrcompositor.txt", "vrclient_echovr_openxr.txt", "vrclient_EchoFrame.txt"):
        files.append(f"{home}/.local/share/Steam/logs/{name}")
    files += sh(f"ls -t {home}/.local/share/Steam/logs/xrclient_* 2>/dev/null | head -3", check=False).split()
    for f in files:
        p = pull(f, dest)
        print(("  " if p.returncode == 0 else "  (missing) ") + f)
    print(f"logs in {dest}")


def cmd_shell(args):
    print(sh(" ".join(args), check=False), end="")


def main():
    global SERIAL
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return
    commands = {"recon": cmd_recon, "push-game": cmd_push_game, "install": cmd_install,
                "launch": cmd_launch, "join": cmd_join, "refresh": cmd_refresh, "stop": cmd_stop, "wait": cmd_wait, "graphics": cmd_graphics, "logs": cmd_logs, "shell": cmd_shell}
    if sys.argv[1] not in commands:
        sys.exit(f"unknown command {sys.argv[1]}")
    SERIAL = find_frame()
    commands[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
