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

Known before testing, from Proton#10211 and CircuitLord TF2VR #45 (SteamOS "vr" 20260928, Proton Experimental ARM64 11.0-20260924, SteamVR/OpenXR 2.18.1):

- x86-64 Windows OpenXR apps work through wineopenxr, once the x86-64 MSVC runtime isn't loaded from the prefix's `system32`.
- The default per-eye size is 1728×1728.
- Run `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-int steamvr.powersaveFramesToThrottle=0`; without it SteamVR runs apps at 36 of 72 Hz.
