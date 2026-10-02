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

## Steam Frame (to fill in during phase 1)

Known before testing, from Proton#10211 and CircuitLord TF2VR #45 (SteamOS "vr" 20260928, Proton Experimental ARM64 11.0-20260924, SteamVR/OpenXR 2.18.1):

- x86-64 Windows OpenXR apps work through wineopenxr, once the x86-64 MSVC runtime isn't loaded from the prefix's `system32`.
- The default per-eye size is 1728×1728.
- Run `/opt/steamvr/bin/linuxarm64/vrcmd --set-settings-int steamvr.powersaveFramesToThrottle=0`; without it SteamVR runs apps at 36 of 72 Hz.
