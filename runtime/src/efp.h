#pragma once
// EchoFramePCVR additions to ReviveXR (the patch in patches/revive-efp.patch calls these):
// runtime.log, echoframe.ini, the headset calibration cache and vtable hooks.
#include <stdint.h>
#include <openxr/openxr.h>

void EFP_Log(const char* fmt, ...);
void EFP_LogXrFail(const char* call, int result, const char* file, int line);

// Running under Wine/Proton (ntdll exports wine_get_version).
bool EFP_UnderWine();

// echoframe.ini next to this DLL. It's read again whenever it changes while Echo runs
// (EFP_SettingsPoll, once a frame); only the keys marked "live" take effect then.
void EFP_SettingsPoll();
float EFP_RenderScale();     // RenderScale = 1.0: multiplies the eye texture size Echo is given
bool EFP_UseHmdCache();      // HmdCache = 1: start from hmd_cache.txt instead of a temporary session
const char* EFP_HmdSerial(); // HmdSerial = ...: the headset serial number Echo reports (default: made per machine)
bool EFP_Fp64Dump();         // Fp64Dump = 1: save the shaders that use double precision (EchoFrame/shaders)
void EFP_DumpShader(const char* stage, uint64_t hash, const void* code, size_t size);

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

// GPUs without double-precision shaders (the Steam Frame's): stub the pixel and compute
// shaders that need them instead of letting pipeline creation fail (efp_fp64.cpp).
struct ID3D12Device;
#ifdef __cplusplus
#include <string>
#include <vector>
// efp_dxil.cpp: the shader with its DOUBLE type demoted to FLOAT, or false and why not
bool EFP_DemoteDxilDoubles(const void* code, size_t size, std::vector<uint8_t>& out, std::string& why);
#endif
void EFP_InstallFp64Workaround(ID3D12Device* device);

// Frame timing summary in runtime.log, every 10 s while frames are submitted (efp_timing.cpp).
// GPU time comes from timestamp queries on the game's D3D12 queue.
bool EFP_FrameTiming();      // FrameTiming = 1: measure GPU time (CPU timing is always on)
struct ID3D12CommandQueue;
void EFP_TimingStart(ID3D12CommandQueue* queue);   // the D3D12 session is created on this queue
void EFP_TimingStop();                             // before the session (and maybe the queue) goes
void EFP_TimingWaitFrame(bool returned);           // around xrWaitFrame
void EFP_TimingBeginFrame(long long frameIndex);   // after xrBeginFrame
void EFP_TimingCommit(double waitedMs);            // time in xrWaitSwapchainImage
void EFP_TimingEndFrame(long long frameIndex);     // just before xrEndFrame
void EFP_TimingFrameSubmitted(int64_t predictedDisplayPeriodNs);   // after xrEndFrame

// Fixed foveated rendering through D3D12 variable-rate shading (efp_foveation.cpp).
int EFP_Foveation();         // Foveation = 0 off, 1 light, 2 medium, 3 strong (live, if not 0 at start)
void EFP_InstallFoveation(ID3D12Device* device, ID3D12CommandQueue* queue);
// at ovr_Initialize, before Echo creates its D3D12 device: hooks that must see its renderer start
// (vkd3d-proton's devices share one vtable, so a device of our own is enough to install them)
void EFP_EarlyD3D12();
// each eye's texture size and field of view (tangents), as Echo asks for them
void EFP_FoveationEye(int eye, int width, int height, float left, float right, float up, float down);
void EFP_FoveationFrame();   // after each frame: logs the render targets Echo binds, for a while

// Throw measurement: reported controller velocities against the tracked motion (efp_input.cpp).
bool EFP_VelocityLog();      // VelocityLog = 1 (live)
// after each hand's location for Echo: calledAt and forTime in QPC seconds, given = what Echo got
void EFP_InputTrack(XrInstance instance, XrSpace handSpace, XrSpace base, int hand, double calledAt, double forTime,
                    const XrSpaceVelocity& given);
void EFP_InputGrip(int hand, float value);

// Echo's GPU copies and resolves, logged for the first minutes (efp_census.cpp).
bool EFP_Census();          // Census = 1
void EFP_InstallCensus(ID3D12Device* device);
void EFP_CensusFrame();     // after each frame

// Per-pass GPU timing labelled by shader, sampled every 20 s (efp_passes.cpp).
bool EFP_PassTiming();       // PassTiming = 1 (live)
void EFP_InstallPasses(ID3D12Device* device, ID3D12CommandQueue* queue);
void EFP_PassesFrame();      // after each frame
void EFP_PassesNotePso(void* pso, uint64_t shaderHash);   // from the pipeline-creation hooks
// Skip = hash,hash,... (live): draws and dispatches whose pixel/compute shader has one of these
// hashes (as the passes lines name them) aren't executed, to measure what an effect costs.
void EFP_SetSkip(const char* list);
// a render-target view's size and format, if efp_foveation saw it created
bool EFP_TargetInfo(uintptr_t rtv, int* width, int* height, int* format);

// Echo's own code and globals (efp_patch.cpp): Patch = rva:old:new (at load, checked),
// Poke = rva:type:value (written after every frame, live), Peek = rva:bytes (logged once, live).
void EFP_ApplyPatches(const char* spec);
void EFP_SetPokes(const char* spec);
void EFP_SetPeek(const char* spec);
void EFP_PatchFrame();       // after each frame

// Eye-tracked foveation (efp_gaze.cpp): the gaze through XR_EXT_eye_gaze_interaction moves the
// full-rate region of efp_foveation's shading-rate images. GazeFoveation = 1 (default).
bool EFP_GazeFoveation();
bool EFP_GazeInit(XrInstance instance);          // our action set and binding; false: no gaze
XrActionSet EFP_GazeActionSet();                 // to attach and sync with Revive's, or null
void EFP_GazeAttach(XrSession session);          // after the action sets are attached (null: detached)
void EFP_GazeUpdate(XrSession session, XrSpace viewSpace, XrTime time);   // each frame, after xrSyncActions
void EFP_FoveationGaze(bool valid, float tx, float ty);                     // gaze direction as tangents
