# Frame notes

Findings, phase by phase. Newest at the bottom of each section.

## Echo VR PCVR (build goldmaster 631547, echovr.exe timestamp 1683152886)

- x86-64, Direct3D 12 (`d3d12.dll`/`dxgi.dll` delay-imported). Oculus's LibOVR CAPI shim is linked in.
- The LibOVR shim:
  - looks for `LibOVRRT%hs_%d.dll` (so `LibOVRRT64_1.dll`) in `LIBOVR_DLL_DIR`, then by a relative/`SearchPathW` lookup;
  - has no registry lookup;
  - verifies the DLL's Authenticode signature (WinVerifyTrust, signer "Oculus VR, LLC") before `LoadLibraryW`, every time, at RVA `0x1365bd0`;
  - resolves 94 `ovr_*` symbols, all required except `ovr_ReportClientInfo`;
  - requests minor version 55.
- `pnsovr.dll` (the OVR provider):
  - imports 155 functions from `LibOVRPlatform64_1.dll`;
  - resolves 7 more with GetProcAddress: `ovr_PopMessage`, `ovr_PlatformInitializeWindows` and the rest of the initialize family.
- Meta's platform loader, linked into pnsovr at RVA `0x98a90`:
  - loads `LibOVRPlatform64_1.dll` from `LIBOVR_DLL_DIR`, then `<Oculus Base>\Support\oculus-runtime\`, with plain `LoadLibraryW` (no signature check);
  - returns `-2` (PreLoaded) when the copy already loaded through the imports has a different path;
  - pnsovr checks the reply type `0x186B58B1` (entitlement) at RVA `0x96fa9`;
  - replies are routed by message type.
- Login (`[LOGIN] Logging in OVR-ORG-<id>`) is sent only after `ovr_User_GetAccessToken` has answered. With Meta's DLL on a PC (no Oculus-launched session) it answers with an error ("Must call get_signature first"). A success with an empty token makes Echo wait forever. The login request carries `hmdserialnumber` and `system_info.headset_type`.

## Phase 0: Windows + SteamVR (2026-10-02)

- Tested on Windows 11, RTX 4090, SteamVR 2.17.10, with SteamVR's null driver (headless).
- OpenXR: SteamVR offers 41 extensions, including `XR_KHR_D3D12_enable`. Echo creates its D3D12 session on its direct queue (feature level 12_0).
- Without a cache, Revive's temporary D3D11 session failed on the null driver: its LUID matched no adapter, which gave `E_INVALIDARG`. Fixed by falling back to the hardware adapter. The real session then wrote `hmd_cache.txt`, and later launches skip the temporary session.
- Frames reach SteamVR. The null driver doesn't throttle `xrWaitFrame` and never focuses the session (state stays SYNCHRONIZED), so Echo waits at the lobby menu without input focus.
- Login works through the stand-in: `[NSUSER] LoginId`, the profile, `active group = Echo VR Lounge`.

## Steam Frame: phase 1 recon (2026-10-02)

- The device: SteamOS `VARIANT_ID="vr"`, build 20260930.6234839, kernel 6.18, aarch64, 8 cores, 15 GiB RAM, 208 GB free in `/home`.
- adb (Developer Mode, USB) is a shell as user `steamos`, `$HOME=/home/steamos`. The adb serial is `frame`.
- Steam (`steamrtarm64/steam -cef-enable-debugging -deckard -gamepadui -steamos3 -vrgamepadui`) runs inside gamescope with `--backend openvr`.
- SteamVR: `/opt/steamvr`, `vrserver` and `vrcompositor` from `bin/linuxarm64`. `bin/version.txt` says 1790822802.
- OpenXR: `~/.config/openxr/1/active_runtime.json` names SteamVR (`bin/linuxarm64/vrclient.so`). `openvrpaths.vrpath` has runtime `/opt/steamvr`.
- GPU: Turnip Adreno 750, Mesa 26.3.0-devel, Vulkan 1.4.362.
- SteamVR settings: `preferredRefreshRate` 120 and 90 are present; there's no `powersaveFramesToThrottle` entry.
- Proton: only `Proton 11.0 (ARM64)` is installed (`proton-11.0-2c-arm64`). It has:
  - FEX: `xtajit64.dll`, `libarm64ecfex.dll`, `libwow64fex.dll`;
  - `wineopenxr.dll` (x86_64-windows, aarch64-windows) with an aarch64-unix `wineopenxr.so` and `wineopenxr64.json`;
  - `vrclient_x64.dll`/`.so`;
  - vkd3d-proton for x86_64-windows and aarch64-windows;
  - Proton's own OpenXR loader 1.1.36 (aarch64).
- wineopenxr offers Windows apps `XR_KHR_D3D11_enable`, `XR_KHR_D3D12_enable` (through the vkd3d-proton interop interface), `XR_KHR_win32_convert_performance_counter_time`, `XR_EPIC_view_configuration_fov`, `XR_FB_display_refresh_rate`, cylinder/cube/depth layers and more. That's every extension ReviveXR requires.
- The default prefix has x86-64 `msvcp140.dll`/`vcruntime140.dll` in `x86_64-windows` and ARM64 ones in `aarch64-windows` (Proton#10211). Our DLLs don't import the MSVC runtime.

## Steam Frame: phase 2, first launches (2026-10-02)

- **Proton choice.** The compat tool `proton_11` makes Steam run the x86-64 "Proton 11.0" inside the FEX-Emu compat tool (`steamapps/common/FEX-Emu/fex-compat-tool`). Its Linux-side OpenXR loader can't load SteamVR's ARM64 runtime, so Proton's `steam.exe` VR setup failed ("failed to determine active runtime file path"). It then never wrote the Vulkan-extension registry keys wineopenxr needs, and wineopenxr's negotiation failed with -6, giving Echo `XR_ERROR_RUNTIME_UNAVAILABLE`.
  - **`proton_11-arm64`** (Proton 11.0 (ARM64), app 4628740) works: wineopenxr reaches SteamVR/OpenXR 2.18.2 (system "cv", 1728×1728 per eye, 51 extensions).
  - Tool names come from `appcache/appinfo.vdf`; this Steam client has no `GetAvailableCompatTools`.
  - A prefix made by the x86 Proton has to be deleted before switching.
- **Shortcut setup.** Launch options for a non-Steam shortcut go through `SetShortcutLaunchOptions`, not `SetAppLaunchOptions`. The compat tool through `SpecifyCompatTool` lands in `config.vdf` `CompatToolMapping`.
- **Headset views.** Revive's temporary D3D11 session works through DXVK and wineopenxr. The Frame's views: left FOV tangents-as-angles -1.025/0.886/0.860/-1.050 rad (asymmetric), IPD 66 mm. Saved to `hmd_cache.txt`.
- **First swapchain image.** `xrAcquireSwapchainImage` right after `xrCreateSwapchain`, before the session runs, fails with `XR_ERROR_CALL_ORDER_INVALID` (-37) on the Frame's SteamVR; Windows SteamVR allows it. Revive now defers that first acquire to the first commit.
- **FP64.** Turnip on the Adreno 750 has `shaderFloat64 = false` and no switch to emulate it: soft-fp64 is in the driver but not exposed, and the only related driconf option is `tu_enable_softfloat32`.
  - vkd3d-proton refuses Echo's shader `cd1db21accf2be54` ("Attempting to use FP64 operations ... not supported"). Pipeline creation fails, and Echo stops with "DirectX error: E_INVALIDARG".
  - The runtime now hooks `CreateGraphicsPipelineState`, `CreateComputePipelineState` and `CreatePipelineState` (vtable slots 10, 11, 47). When the device reports no double precision, it swaps a pixel or compute shader whose SFI0 flags declare doubles for a stub (`runtime/shaders`).
- **Stuck sessions.** After a crash, Echo's BugSplat reporter (`BsSndRpt64.exe`) keeps the Steam session open, and Steam won't start the shortcut again. `frame.py stop` ends the session.

- **FP64, round two.** The double-precision shaders are vertex shaders: six DXIL (SM6) particle-style vertex shaders, e.g. `f0fcc10513a97272` (99 KB).
  - The doubles come from a compiler quirk: `uitofp i1 -> double`, `fabs`, `fcmp une 0.0`, which is just the bool.
  - For now each one is replaced by a stub vertex shader that outputs w = 0, so the draw is clipped away. The pixel shader is stubbed too, because vkd3d-proton checks that VS outputs match PS inputs.
  - Those effects don't show. The proper fix is demoting doubles to floats in dxil-spirv / vkd3d-proton.
- **Platform SDK on Wine.** pnsovr's Platform SDK loader ignores `LIBOVR_DLL_DIR` in an elevated process (it checks the token's integrity level), and Wine's processes are elevated. It then only tries `<Oculus Base>\Support\oculus-runtime\`, read from `HKLM\SOFTWARE\Oculus VR, LLC\Oculus` `Base` in the 32-bit registry view (`KEY_WOW64_32KEY`).
  - Under Wine the launcher writes `Base` = `EchoFrame\` to both views in the prefix.
  - It copies the stand-in to `EchoFrame\Support\oculus-runtime\` and puts that folder first on PATH.
- **Code past a section's end.** The community patches to `pnsovr.dll` and the others put code in the padding after `.text`'s VirtualSize (pnsovr's org-scoped ID at RVA 0x1f9500). Windows runs it; under Wine/FEX, jumping there is an access violation (execute).
  - Under Wine the launcher widens each executable section's VirtualSize to its raw size: in `echovr_openxr.exe` and the `pns*.dll` files, keeping `<dll>.efp-orig` backups.
- **Result.** Echo VR PCVR runs on the Steam Frame: OpenXR session through wineopenxr, frames at 72.0 fps (72 Hz, 1728×1728 per eye, slowest frame about 16 ms once loaded), login to echovrce (`[SOCIALGROUPS] active group = Echo VR Lounge`).
- **`frame.py stop`.** `pkill -f` matched its own adb shell (its command line contains the pattern) and killed it before the real targets. Patterns are now written `[A]ppId=...`.

Known before testing, from Proton#10211 and CircuitLord TF2VR #45 (SteamOS "vr" 20260928, Proton Experimental ARM64 11.0-20260924, SteamVR/OpenXR 2.18.1):

- x86-64 Windows OpenXR apps work through wineopenxr, once the x86-64 MSVC runtime isn't loaded from the prefix's `system32`.
- The default per-eye size is 1728×1728.
- Run `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-int steamvr.powersaveFramesToThrottle=0`; without it SteamVR runs apps at 36 of 72 Hz.

## First play session on the Frame (2026-10-02, after FP64 demotion)

User report: lobby visible, controllers work, playable in the menu space, but:

- **Effects.** About half showed. All six double-precision vertex shaders were demoted to single precision without a fallback, so the rest is probably Echo's own Low preset (`quality.fx 0`, `bloom false`, `volumetrics false`, chosen by Echo for this GPU), not the runtime. Not yet confirmed.
- **Performance.** Unplayable in the social lobby. The private menu space (level 0xAC36…) held 72 fps for minutes, with the slowest frames around 15 ms.
  - Entering the social lobby (level 0x3F99…, 19:02:31) brought frames of 100–300 ms. SteamVR then pinned the app at **24 Hz** (one third of 72).
  - Measured there: the GPU sits at its maximum clock (903 MHz) while Echo's main thread is at about 25% and its task threads around 13%.
  - So it's GPU-bound, about 40 ms of GPU per frame against a 13.9 ms budget.
  - Settings at the time: Low preset, TAA on, no MSAA, Multi-Res and adaptive res off, 1728×1728 per eye.
- **Throttle setting.** `steamvr.powersaveFramesToThrottle` was **1** the whole time. vrcmd takes `steamvr.powersaveFramesToThrottle` and `0` as separate arguments, with `LD_LIBRARY_PATH=/opt/steamvr/bin/linuxarm64`; "key=value" is rejected as "Unknown command". frame.py's `throttle_off` was silently failing; it's fixed and now verified (reads back `=0`).
- **No settings button.** Bindings come through SteamVR's Touch emulation (`left/right hand bound to /interaction_profiles/oculus/touch_controller`). The session flapped between FOCUSED and VISIBLE, apparently because the menu button opens SteamVR's dashboard.
  - The runtime offers `XR_VALVE_frame_controller_interaction`. SteamVR's own description of the controller is saved in `docs/frame/frame_controller_profile.json`:
    - left: `view`, d-pad;
    - right: `menu`, `a`, `b`, `x`, `y`;
    - both: `system`, plus trigger, grip and stick (see the file).
  - Also offered: `XR_EXT_frame_synthesis`, `XR_EXT_frame_composition_report`, `XR_VALVE_timing_utils`.

### Next steps (in order)

1. **GPU and CPU frame timing in runtime.log.** D3D12 timestamp queries on Echo's queue around each frame (WaitToBeginFrame → EndFrame), plus CPU time from xrWaitFrame returning to xrEndFrame. Every tuning step below should be judged by these numbers.
2. **GPU cost.** A/B in the social lobby:
   - `RenderScale` 0.7 / 0.6;
   - TAA off;
   - Turnip/vkd3d options: `TU_DEBUG=gmem`/`sysmem`, and vkd3d-proton's `VKD3D_CONFIG`;
   - fewer render passes;
   - target 36 Hz with SteamVR reprojection (`XR_EXT_frame_synthesis`) if 72 is out of reach.
3. **Native Frame controller bindings** (`XR_VALVE_frame_controller_interaction`) in the Revive patch. The Echo menu goes on the left `view` button, so the right `menu`/`system` stays SteamVR's. Log the bound profile.
4. **Effects.** Compare in the headset against the PC at the same Low preset to separate missing-by-preset from missing-by-bug. Check `Fp64Dump` for any shader stubbed instead of demoted.
5. **Later:** mic and voice chat check, a full match, a one-click installer, Echo Arcade.

## Frame timing and first GPU A/Bs (2026-10-02)

- **Timing in runtime.log** (`efp_timing.cpp`, `FrameTiming = 1` in echoframe.ini). Every 10 s there's a line with avg/p95/max in ms:
  - `gpu`: D3D12 timestamps on Echo's queue, written after xrBeginFrame and just before xrEndFrame;
  - `gpu interval`: end timestamp to end timestamp;
  - `cpu`: xrWaitFrame returning → xrEndFrame;
  - `xrWaitFrame`, `swapchain wait` (xrWaitSwapchainImage in commit), `xrEndFrame`.
- **Menu space, 72 Hz, 1728×1728 per eye, Low preset:** GPU 6.8 ms, CPU 1.2 ms, the rest is spent in xrWaitFrame. So the CPU is not the problem.
- **A/B in the menu space** (GPU ms; the menu holds 72 fps in every case):

  | change | GPU ms |
  |---|---|
  | baseline | 6.8 |
  | `TU_DEBUG=sysmem` | 6.8 (Turnip already chooses sysmem) |
  | `TU_DEBUG=gmem` | 9.7 (worse) |
  | RenderScale 0.7 (49% of the pixels) | 3.7 |
  | TAA off | 5.0 |

  The GPU cost is almost all per pixel: 49% of the pixels cost 54% of the time.
- **Starting in the lobby doesn't work.** `-level mpl_lobby_b2 -gametype social_2.0` loads level 0x3F99… without a server, logs "Only one CR15NetMetricsCS is allowed to exist" and stays on the loading screen; it never logs in. Lobby numbers still need someone in the headset.
- **The launcher quoted every argument.** Echo's parser takes `"-level" "mpl_lobby_b2"` as `-level` with no value and exits with code 0 before writing a log; the message only shows in the Proton log (OutputDebugString). Now only arguments with spaces are quoted. `frame.py launch` takes Echo arguments and `KEY=VALUE` environment variables.
- **Native Frame bindings.** `XR_VALVE_frame_controller_interaction` is enabled and `/interaction_profiles/valve/frame_controller` is suggested first, before Index and Touch. The component paths come from SteamVR's `vrclient.so`.
  - Echo's menu is on the left `view` button.
  - X/Y come from the left d-pad (down/left and up/right), A/B from the right diamond (a/x and b/y).
  - Right `menu`/`system` stay with SteamVR.
  - SteamVR rejects `thumbrest/touch` for this profile. Rejected paths are now found one at a time and left out instead of losing the whole profile.
  - Not yet checked in the headset: the log line saying which profile each hand bound to.

