#!/usr/bin/env python3
"""EchoFramePCVR on the Steam Frame, from the PC, over adb (Developer Mode, USB-C or Wi-Fi).

  frame.py recon                      what the Frame has (SteamOS, Proton, SteamVR, OpenXR, GPU)
  frame.py push-game ECHO_DIR         copies an Echo VR PCVR install to the Frame (resumable)
  frame.py install                    copies build/out/ to it and sets up the Steam shortcut
  frame.py launch                     starts Echo on the Frame (stopping a running one first)
  frame.py stop                       ends Echo's session (Echo, its crash reporter, Wine)
  frame.py logs                       pulls every log into artifacts/logs/<time>/
  frame.py shell CMD...               runs a command in the Frame's shell

The Frame's adb is a SteamOS shell as the logged-in user. Echo goes to
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
VRCMD = "XDG_RUNTIME_DIR=/run/user/$(id -u) /opt/steamvr/bin/linuxarm64/vrcmd"
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


def find_frame():
    serial = os.environ.get("EFP_SERIAL")
    if serial:
        return serial
    for line in run("devices").splitlines()[1:]:
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "device":
            if "steamos" in run("-s", parts[0], "shell", "grep -s ^ID= /etc/os-release", check=False):
                return parts[0]
    sys.exit("no Steam Frame on adb: turn on Developer Mode and plug it in (or adb connect its address)")


SERIAL = None


def sh(command, timeout=None, check=True):
    return run("-s", SERIAL, "shell", command, timeout=timeout, check=check)


def push(local, remote, sync=False, timeout=None):
    args = ["-s", SERIAL, "push"] + (["--sync"] if sync else []) + [local, remote]
    p = subprocess.run([ADB, *args], timeout=timeout)
    if p.returncode:
        sys.exit(f"copying {local} failed")


def remote_path(path):
    return sh(f'echo "{path}"').strip()


def throttle_off():
    sh(f"{VRCMD} --set-settings-int steamvr.powersaveFramesToThrottle=0 >/dev/null 2>&1", check=False)


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
    print(f"installed; shortcut {shortcut} runs EchoFrame.exe with {tool}")


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
    sh(f"pkill -f 'reaper SteamLaunch AppId={shortcut} ' ; pkill -9 -f BsSndRpt64.exe; pkill -9 -f echovr_openxr.exe; "
       f"for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -f 'AppId={shortcut} ' >/dev/null || break; sleep 1; done; "
       f"pkill -9 -f 'AppId={shortcut} '", check=False)


def cmd_stop(args):
    stop()
    print("stopped")


def cmd_launch(args):
    stop()
    throttle_off()
    gid = game_id()
    sh(f"nohup steam steam://rungameid/{gid} >/dev/null 2>&1 &")
    print(f"started game {gid}; logs with: frame.py logs")


def cmd_logs(args):
    root = remote_path(ROOT)
    home = remote_path("$HOME")
    dest = os.path.join(REPO, "artifacts", "logs", time.strftime("%Y%m%d-%H%M%S"))
    os.makedirs(dest, exist_ok=True)
    gid = game_id()
    files = [f"{root}/bin/win10/EchoFrame/launcher.log", f"{root}/bin/win10/EchoFrame/runtime.log",
             f"{root}/bin/win10/EchoFrame/platform.log", f"{root}/bin/win10/EchoFrame/hmd_cache.txt",
             f"{home}/steam-{gid}.log"]
    newest = sh(f"ls -t '{root}/_local/r14logs/' 2>/dev/null | head -1", check=False).strip()
    if newest:
        files.append(f"{root}/_local/r14logs/{newest}")
    for name in ("vrserver.txt", "vrcompositor.txt", "vrclient_echovr_openxr.txt", "vrclient_EchoFrame.txt"):
        files.append(f"{home}/.local/share/Steam/logs/{name}")
    files += sh(f"ls -t {home}/.local/share/Steam/logs/xrclient_* 2>/dev/null | head -3", check=False).split()
    for f in files:
        p = subprocess.run([ADB, "-s", SERIAL, "pull", f, dest], capture_output=True, text=True)
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
                "launch": cmd_launch, "stop": cmd_stop, "logs": cmd_logs, "shell": cmd_shell}
    if sys.argv[1] not in commands:
        sys.exit(f"unknown command {sys.argv[1]}")
    SERIAL = find_frame()
    commands[sys.argv[1]](sys.argv[2:])


if __name__ == "__main__":
    main()
