# EchoFramePCVR

The full **Echo VR PCVR** game (`echovr.exe`, not the Quest APK) running **on the Steam
Frame itself**, with no PC and no streaming.

There's no source code for Echo, so it can't be rebuilt for the Frame's ARM chip. The Frame
already runs Windows games on the headset, through Proton (Windows compatibility, with
vkd3d-proton for Direct3D 12) and FEX (x86-64 to ARM64). What Echo is missing there is the
Oculus runtime. EchoFramePCVR replaces it:

```
echovr_openxr.exe (Echo, x86-64, D3D12)        <- EchoFrame.exe makes and starts it
 ├─ LibOVRRT64_1.dll        Oculus PC runtime API (LibOVR 1.55) on OpenXR: ReviveXR + our patch
 │    └─ OpenXR loader (static) -> Proton's wineopenxr -> the Frame's SteamVR (ARM64)
 └─ pnsovr.dll -> LibOVRPlatform64_1.dll   Oculus Platform SDK stand-in: login, microphone
```

It's the PC counterpart of [EchoFrame](https://github.com/heisthecat31/EchoFrame), which
does this for the Quest APK through Lepton. This repository ships **no game files**: you
use your own Echo VR PCVR install (build `goldmaster 631547`, `echovr.exe` timestamp
`1683152886`).

## Status

| Phase | State |
| --- | --- |
| 0. Runtime, Platform stand-in and launcher, tested on Windows + SteamVR | ✅ Echo starts, renders through OpenXR, logs in to echovrce, reaches the lobby menu (headless SteamVR test) |
| 1. Frame recon and a D3D12 OpenXR test app under Proton + FEX on the Frame | next |
| 2. Echo boots on the Frame | |
| 3. Playable: controllers, audio and mic, matches | |
| 4. Performance (render scale, settings, FEX/vkd3d tuning) | |
| 5. One-click installer; Echo Arcade on the Frame | |

Notes from each phase go in [docs/FRAME-NOTES.md](docs/FRAME-NOTES.md).

## What's in here

| Path | What |
| --- | --- |
| `runtime/src/` | Our additions to ReviveXR: `runtime.log`, `echoframe.ini`, the headset calibration cache, vtable hooks, frame timing |
| `patches/revive-efp.patch` | Our changes to Revive (`ab73167`) |
| `patches/openxr-loader-static-crt.patch` | Static CRT for the static OpenXR loader |
| `platform/efp_platform.c` | `LibOVRPlatform64_1.dll`, the Oculus Platform SDK stand-in |
| `launcher/efp_launcher.cpp` | `EchoFrame.exe` |
| `build.cmd` | Fetches the pinned sources into `deps\`, builds everything into `build\out\` |

### Why each piece

- **LibOVRRT64_1.dll.** Echo talks to the headset through LibOVR. This is ReviveXR (MIT) built
  under the Oculus runtime's name, so Echo loads it as the runtime. Our patch:
  - asks for OpenXR 1.0 (SteamVR, the Frame's runtime too, has no 1.1);
  - chains the EPIC FOV struct only when the extension is on;
  - waits a bounded time for READY;
  - suggests Touch bindings next to Index (SteamVR binds the Frame's controllers from Touch);
  - **swaps vtable slots instead of Detours code patches** for Revive's D3D descriptor hooks. Under FEX, DXVK and vkd3d-proton can be ARM64EC code, which an x86-64 code patch would corrupt;
  - **caches the headset's views** in `hmd_cache.txt`, so later launches skip Revive's temporary D3D11 OpenXR session;
  - reports a stable HMD serial number (Echo sends one at login);
  - adds a `RenderScale` setting and logs every failed OpenXR call.

  It's built with the static CRT and a static OpenXR loader, so it needs no `msvcp140.dll`. On the Frame, Proton's prefix has ARM64EC builds of the MSVC runtime that x86-64 code crashes in (ValveSoftware/Proton#10211).
- **echovr_openxr.exe.** Echo only loads an Oculus-signed runtime. `EchoFrame.exe` makes this
  copy of `echovr.exe` with the signature check (RVA `0x1365bd0`) returning "signed". It
  checks the bytes first and refuses any other build. `echovr.exe` is never changed.
- **LibOVRPlatform64_1.dll.** Echo's OVR provider (`pnsovr.dll`) needs Meta's Platform SDK,
  which needs the Oculus service. The stand-in answers its 162 functions locally, the way
  Meta's DLL answers them on a PC: initialized and entitled; user, token, store and
  presence requests fail. Login to the community servers comes from `config.json`, as
  usual. The microphone is real (WASAPI). `pnsovr`'s loader takes this DLL from
  `LIBOVR_DLL_DIR` and refuses a copy loaded from anywhere else, so the launcher puts that
  folder first on `PATH`.
- **EchoFrame.exe.** Makes `echovr_openxr.exe` when needed, creates the `OculusHMDConnected`
  event, sets `LIBOVR_DLL_DIR`/`PATH`, starts Echo and waits.

## Building (Windows)

Needs Visual Studio 2022 with the C++ tools (its CMake and Ninja are used) and git.

```bash
build.cmd
```

Output in `build\out\`: `EchoFrame.exe`, `LibOVRRT64_1.dll`, `LibOVRPlatform64_1.dll`,
`echoframe.ini`.

## Installing into an Echo VR folder

```
ready-at-dawn-echo-arena\bin\win10\EchoFrame.exe
ready-at-dawn-echo-arena\bin\win10\EchoFrame\LibOVRRT64_1.dll
ready-at-dawn-echo-arena\bin\win10\EchoFrame\LibOVRPlatform64_1.dll
ready-at-dawn-echo-arena\bin\win10\EchoFrame\echoframe.ini
```

Start `EchoFrame.exe`. On Windows, `--steamvr` uses SteamVR for that launch whatever the
system OpenXR runtime is. Logs go to `bin\win10\EchoFrame\`: `launcher.log`, `runtime.log`
(OpenXR runtime, extensions, bound controller profiles, failed calls, frame rate) and
`platform.log`. Echo's own log is in `_local\r14logs\`.

`echoframe.ini`:

| Setting | Default | |
| --- | --- | --- |
| `RenderScale` | `1.0` | eye texture size relative to the headset's recommended size (0.25 to 2.0) |
| `HmdCache` | `1` | start from `hmd_cache.txt` instead of a temporary OpenXR session |
| `HmdSerial` | per machine | the headset serial number Echo reports |

Delete `hmd_cache.txt` after changing headset or OpenXR runtime.

The Steam Frame side (Proton, FEX, the Steam shortcut, installing over adb) comes in
phases 1 and 2.

## Credits

ReviveXR by LibreVR, EchoFrame's Platform SDK stand-in by heisthecat31, and the prior work
listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md): EchoXR, libovr-openxr-rs,
RiftLift, Frame Control.
