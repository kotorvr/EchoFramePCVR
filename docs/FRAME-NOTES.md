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

## 90 Hz, foveation, and what's left to try (2026-10-02, later)

- **90 Hz works.** SteamVR on the Frame switches the display to 72 Hz for any app unless the app's own SteamVR setting says otherwise; the home runs at 120. That setting is `steam.app.<shortcut id>.preferredRefreshRate`, set through vrcmd. EchoFrame found that `xrRequestDisplayRefreshRateFB` is ignored on the Frame.
  - `frame.py refresh [HZ]` shows or sets it; `frame.py install` sets 90.
  - The display offers 72, 80, 90, 96, 100 and 120 Hz.
  - The menu space holds 90.0 fps: GPU 6.8 ms against an 11.1 ms frame.
  - The compositor log names the reason: "HMD driver recommended: 2160x2160 90.0Hz HiddenArea(15.64%)", with a raw render target scale of 0.64, which is where 1728 comes from.
- **Echo's render targets.** Both eyes are rendered side by side into 3456×1728 targets in formats 10 (R16G16B16A16_FLOAT), 13, 26 (R11G11B10_FLOAT), 28 and 29 (R8G8B8A8). There are also 1728×864 half-resolution targets and a 1280×720 mirror window. No views are made before the hooks.
- **Foveated rendering through D3D12 VRS** (`efp_foveation.cpp`, `Foveation = 0..3`, default 2):
  - vkd3d-proton reports tier 2 with 8×8 tiles, but **no 4×4 rate**, so 2×2 is the coarsest;
  - every eye-sized target gets an image with full rate inside a cone around each eye's axis and 2×2 outside it;
  - menu space at 90 Hz: GPU 6.8 → 6.2 ms with 95% of the tiles at 2×2, and the same at the strong level.
  - So pixel shading is a small part of the cost here. On this tile-based GPU in sysmem mode, the cost per pixel looks like memory bandwidth (render-target writes and reads), which VRS doesn't reduce. The lobby may shade more heavily; measure it there.
- **More menu-space A/Bs at 90 Hz** (GPU ms; baseline 6.8 without foveation, 6.2 with it):

  | change | GPU ms |
  |---|---|
  | `multires` true (Echo's stencil mask) | 6.3, no gain; the log still says "Multi-Res: Disabled" |
  | `multires` + `msaa` 1 | 11.3 (MSAA is expensive) |
  | `sharpening` 0 | 7.6, worse or noise |
  | `-legacyvis` | 7.8, worse |

- **Research (subagents, 2026-10-02):**
  - **Echo's settings.** Settings are read at 0xC2E400. `adaptiveresminscale` is clamped to [0.01, 1], so adaptive resolution can go down to 0.5. Echo's adaptive target is 90 fps for the Rift CV1 that Revive reports.
  - **No lower presets.** There is no LOD bias or draw-distance key. `meshes` 0..2 sets the LOD globals at RVA 0x20AFBC8/BC0/BD4.
  - **Quest content isn't usable.** The exe has the Quest "lowspec" path, which loads `<level>_lowspec` levels; the PC data has none of them, and Quest data (ASTC textures, android resource types) can't be loaded.
  - **Post effects are level data.** SSAO, fog, light shafts and bloom are set per level, not in the settings; changing them needs an asset patch or a hook.
  - **Valve's eye-tracked foveation layer.** It is already loaded into Echo: `VK_LAYER_VALVE_fdm_injection` (fragment density map, gaze from `xrGetFoveationEyeTrackedStateMETA`) plus `VK_LAYER_VALVE_rpo`. It stays idle unless `FDM_DEBUG=enable` (and `RPO_DEBUG=enable`) is set; Steam sets those per app, but not for our shortcut. Turnip has `fragmentDensityMap`, the offset extension and the layered extension.
  - **Gaze for our own runtime.** `XR_EXT_eye_gaze_interaction` passes through wineopenxr, which doesn't filter or need thunks for it, if we want our own gaze-driven VRS. The eye-tracking server runs at 15 to 90 Hz.
  - **Half-rate reprojection.** SteamVR can run Echo at half rate with reprojection without app changes (per-app Motion Smoothing). `XR_EXT_frame_synthesis` would need motion vectors plus depth.
  - **Per-pass profiling without code.** Turnip: `MESA_GPU_TRACES=print_csv MESA_GPU_TRACEFILE=/home/steamos/tu.csv` gives per-render-pass GPU times with `tiledRender`. vkd3d: `VKD3D_QUEUE_PROFILE=Z:\home\steamos\q.json`, `VKD3D_SHADER_DUMP_PATH`, and `VKD3D_SHADER_OVERRIDE` (to swap in cheaper SPIR-V by vkd3d hash).
  - **Two shader hashes.** vkd3d's hash is FNV-1 (multiply, then xor); ours in efp_fp64 is FNV-1a, which is why the names in the two logs differ.
- **Wireless.** The Frame's adbd has no TCP mode (`adb tcpip` is refused). sshd runs and takes the `id_rsa_frame_devkit` key that Frame Control installed, so `frame.py` falls back to SSH at the last Wi-Fi address it saw (`artifacts/frame_host`, or `EFP_SSH=steamos@host`).
- **Straight into a match.** Use the echovrce Discord bot's `/create` (Private Arena Match, Private Combat Match or Private Social Lobby), which answers with `spark://c/<uuid>`, then run `frame.py join <link>` (Echo's `-lobbyid`). Offline `-level` starts need patched DLLs.

### Next steps (performance, in order)

1. **In the lobby or a private arena** (`frame.py join`), one session, with the GPU line each time:
   - `Foveation` 0 vs 2;
   - `FDM_DEBUG=enable` with Foveation 0, then with `RPO_DEBUG=enable` too;
   - adaptive resolution on with `adaptiveresminscale` 0.5;
   - TAA off.
2. **The no-code per-pass trace** in the lobby (`MESA_GPU_TRACES`), to see which passes cost the most; then hook or skip them, or override their shaders.
3. **Half rate with reprojection** as the fallback for 90/120 Hz, and `XR_EXT_eye_gaze_interaction` gaze for our VRS if Valve's FDM layer doesn't do it.


## Lobby A/Bs, Valve's FDM layer, per-pass trace (2026-10-02, evening)

All numbers are runtime.log's GPU ms per frame, 1728×1728 per eye, Low preset, 90 Hz display, unless noted.

- **New tools.**
  - `echoframe.ini` is reread while Echo runs. `Foveation` and `VelocityLog` change live: `frame.py ini Foveation=0`.
  - `frame.py timing` shows the last timing, settings and throw lines.
  - `frame.py tudebug FLAGS` writes Turnip's `TU_DEBUG_FILE`, which Turnip rereads live (in the launch options from now on).
  - `tools/ab_lobby.py` runs unattended A/Bs. For each config it restarts Echo into a session, waits for our player to spawn plus 25 s, and averages 40 s of timing lines. Results go to `artifacts/ab-<date>.txt`.
  - `tools/tu_passes.py` ranks a Turnip u_trace by render pass. Run it on the Frame, because the trace grows by about 150 MB a minute.
- **Sessions.** A `/create` private session dies as soon as its only player leaves, so every restart of Echo kills it ("join error lobby invalid").
  - **Rejoining a public social lobby by id works.** After the user joins one once from the menu, Echo's r14log has `lobby_id` in a rich-presence line, and `frame.py join <id>` lands straight in it.
  - `-gametype social_2.0` alone only reaches the menu.
  - The EchoRelay patch's `-offline -level mpl_lobby_b2 -gametype Social_2.0_Private -region uscn` would give a repeatable empty lobby. It needs the patch's dbgcore.dll/EchoLoader on the Frame, which the stock Frame install leaves out. Not tried.
- **Launch-option gotcha.** Git Bash rewrites `/home/...` in `KEY=VALUE` arguments to `C:/Program Files/Git/home/...`. The space then breaks Steam's launch command and the game exits at once. `export MSYS_NO_PATHCONV=1` first.
- **Foveation in the lobby** (private lobby, player standing still, switched live): Foveation 0 **36 ms**, Foveation 2 **27–28 ms**. VRS saves about 25% here, against 9% in the menu. In the public lobby, with a fixed view: 36.8 vs 22.3.
  - VRS was also being applied to 2048×2048 and 1024×1024 targets (shadow or reflection maps). Now only targets at the eye size or half of it are foveated.
- **Valve's eye-tracked FDM layer** (`FDM_DEBUG=enable`, Foveation 0):
  - **At first Echo wouldn't start:** "DirectX error: E_INVALIDARG". vkd3d logged "Heap too small for the texture" for the 3456×1728 depth target. The layer makes subsampled, padded images, larger than what `GetResourceAllocationInfo` reported to Echo when it laid out its heaps.
    - Fix: with `FDM_DEBUG` set, the runtime reports eye-sized render and depth targets 1/8 bigger (device vtable slot 25).
    - Echo creates its heaps before it hands LibOVR its device, so the hook goes in at `ovr_Initialize`, through a D3D12 device of our own. vkd3d-proton's devices share one vtable, and the same device singleton.
  - The layer logs "created fdm views for size 1728 (108x54)", so it handles the double-wide 3456×1728 targets.
  - **Lobby: 21.3 ms** (vs 27.8 with our VRS), but **many textures render black**. In the menu it's worse, 12.5 vs 6.8 ms: Turnip forces gmem for every pass with a density map.
  - The likely cause of the black: subsampled images read later through ordinary samplers. vkd3d samples through bindless heaps, not subsampled immutable samplers.
  - Not usable as is. `FDM_DEBUG` options seen in the layer: enable, debug, fdm, synth, zero, med, (low/high), disable_offsets, disable_layered, help. Also `FOVE_LEVEL` and `FDM_SWAPCHAIN_SIZE`.
- **Valve's render-pass optimizer** (`RPO_DEBUG=enable`): no gain, 23–28 ms with Foveation 2 in the same lobby. Dropped.
- **Public lobby, headset lying still** (fixed view, Foveation 2): base **22.3**, Foveation 0 **36.8**, TAA off **18.7**. RenderScale 0.7 + TAA off read **12.3** during an aborted run. The suite stopped when the Frame went to sleep; `frame.py launch` now holds a logind sleep inhibitor.
- **Per-pass trace in the lobby** (`MESA_GPU_TRACES=print_csv`, Foveation 2; shares of GPU time):
  - main pass, 3456×1728, 4 colour targets + depth, about 37 draws, gmem, stores 20 bytes/pixel: **33%**;
  - full-screen 1-draw passes with 2 targets, load/store 8 bytes/pixel (TAA and post): **22%**;
  - `vkCmdCopyImage`, about 10 per frame (Echo's CopyResource/CopyTextureRegion): **11%**. `Census = 1` now logs them;
  - a 3-target + depth pass with about 13 draws, 4× per frame (transparents?): 11%;
  - compute, about 68 dispatches per frame: 10%;
  - 2-target, 4-byte full-screen passes: 8%;
  - 1728×864 half-resolution passes: 4%.
  - LRZ writes are off in most passes ("Stencil may kill fragments").
  - On sysmem passes, load and store ops are bookkeeping only. A `DiscardResource` before a pass only helps where Turnip picks gmem.
- **SteamVR's "Application FPS"** (172–230 while Echo shows 90.0) is not Echo's frame rate. vrcompositor's per-process summary counted 9412 presents in about 114 s, at most 90 a second. The figure matches 1000 / the app GPU time the compositor logs (`ApplicationTime GPU 3–8 ms` in the menu), so it's headroom, not frames shown. Echo is capped by `xrWaitFrame`. Nothing to fix.
