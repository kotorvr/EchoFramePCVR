#pragma once
// EchoFramePCVR additions to ReviveXR (the patch in patches/revive-efp.patch calls these):
// runtime.log, echoframe.ini, the headset calibration cache and vtable hooks.
#include <stdint.h>
#include <openxr/openxr.h>

void EFP_Log(const char* fmt, ...);
void EFP_LogXrFail(const char* call, int result, const char* file, int line);

// Running under Wine/Proton (ntdll exports wine_get_version).
bool EFP_UnderWine();

// echoframe.ini next to this DLL.
float EFP_RenderScale();     // RenderScale = 1.0: multiplies the eye texture size Echo is given
bool EFP_UseHmdCache();      // HmdCache = 1: start from hmd_cache.txt instead of a temporary session
const char* EFP_HmdSerial(); // HmdSerial = ...: the headset serial number Echo reports (default: made per machine)

// What a temporary OpenXR session would tell us before Echo has made its own: the
// per-eye field of view and eye poses, and the play-area size. Saved once a session
// has located the views; checked against the runtime and the recommended eye size.
struct EFP_HmdCache
{
	char Runtime[XR_MAX_RUNTIME_NAME_SIZE];
	uint32_t Width[2], Height[2];
	XrFovf Fov[2];
	XrPosef Pose[2];
	XrExtent2Df Bounds;
};
bool EFP_LoadHmdCache(EFP_HmdCache* out);
void EFP_SaveHmdCache(const EFP_HmdCache& cache);

// Replaces one virtual function of a COM object by swapping its vtable slot (no code
// patching, so it works whatever the callee is compiled to, e.g. ARM64EC under FEX).
// Returns the slot's address, or nullptr if the slot already points at hook.
void** EFP_HookVirtual(void* instance, unsigned slot, void* hook, void** original);
void EFP_RestoreVirtual(void** slotAddress, void* original);

// Frame pacing summary in runtime.log, every 10 s while frames are submitted.
void EFP_FrameSubmitted(int64_t predictedDisplayPeriodNs);
