// Shaders that use double precision, on GPUs without it (the Steam Frame's Adreno 750: Turnip
// reports shaderFloat64 = false).
//
// vkd3d-proton refuses such a shader ("Attempting to use FP64 operations in shader ..., but
// this is not supported") and the pipeline creation fails with E_INVALIDARG. Echo treats any
// failed pipeline as fatal ("DirectX error: E_INVALIDARG"). When the device reports no
// double-precision support, the hooks below give each shader that declares doubles (its
// container's SFI0 feature flags):
//   1. its DXIL with the DOUBLE type demoted to FLOAT (efp_dxil.cpp), when that's safe; Echo's
//      uses are a compiler artifact, so the shader behaves the same;
//   2. otherwise, or if the pipeline still fails, a stub: a vertex shader whose draw is clipped
//      away (that effect doesn't show; the pixel shader is stubbed with it), a pixel shader
//      that writes zero, a compute shader that does nothing.
// Each shader is logged once. With Fp64Dump = 1 in echoframe.ini, every shader that declares
// doubles is also saved to EchoFrame/shaders (on any GPU), to study them.
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "stub_ps.h"   // g_StubPS: build\obj, compiled from runtime\shaders by build.cmd
#include "stub_cs.h"   // g_StubCS
#include "stub_vs.h"   // g_StubVS

typedef HRESULT(STDMETHODCALLTYPE* CreateGraphicsPSO)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateComputePSO)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateStreamPSO)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

static CreateGraphicsPSO TrueCreateGraphicsPSO;
static CreateComputePSO TrueCreateComputePSO;
static CreateStreamPSO TrueCreateStreamPSO;
static bool g_lacksFp64;   // the device has no double-precision shaders: stub where possible

// D3D_SHADER_REQUIRES_DOUBLES | D3D_SHADER_REQUIRES_11_1_DOUBLE_EXTENSIONS in SFI0
static const uint64_t kDoubleFlags = 0x1 | 0x20;

// The SFI0 (shader feature info) flags of a DXBC or DXIL container; 0 if there are none.
static uint64_t FeatureFlags(const D3D12_SHADER_BYTECODE& code)
{
	const uint8_t* p = (const uint8_t*)code.pShaderBytecode;
	if (!p || code.BytecodeLength < 32 || memcmp(p, "DXBC", 4))
		return 0;
	uint32_t count;
	memcpy(&count, p + 28, 4);
	if (32 + (size_t)count * 4 > code.BytecodeLength)
		return 0;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t offset;
		memcpy(&offset, p + 32 + i * 4, 4);
		if ((size_t)offset + 16 <= code.BytecodeLength && !memcmp(p + offset, "SFI0", 4)) {
			uint64_t flags;
			memcpy(&flags, p + offset + 8, 8);
			return flags;
		}
	}
	return 0;
}

static uint64_t Hash(const D3D12_SHADER_BYTECODE& code)   // FNV-1a, to name the shader in the log
{
	uint64_t h = 0xcbf29ce484222325ull;
	for (size_t i = 0; i < code.BytecodeLength; i++)
		h = (h ^ ((const uint8_t*)code.pShaderBytecode)[i]) * 0x100000001b3ull;
	return h;
}

static bool UsesDoubles(const D3D12_SHADER_BYTECODE& code) { return (FeatureFlags(code) & kDoubleFlags) != 0; }

// Per shader (by hash): its demoted copy, or why it couldn't be demoted. Logged once each.
struct Fixed
{
	std::vector<uint8_t> code;
	std::string why;   // empty: demoted
};
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::unordered_map<uint64_t, Fixed> g_fixed;

// The double-free version of a shader: demoted (efp_dxil.cpp), or null if it can't be.
static const Fixed& Demote(const char* stage, const D3D12_SHADER_BYTECODE& code)
{
	uint64_t hash = Hash(code);
	AcquireSRWLockExclusive(&g_lock);
	auto it = g_fixed.find(hash);
	if (it == g_fixed.end()) {
		Fixed f;
		if (!EFP_DemoteDxilDoubles(code.pShaderBytecode, code.BytecodeLength, f.code, f.why) && f.why.empty())
			f.why = "unknown";
		EFP_Log("fp64: %s shader %016llx (%zu bytes) uses double precision%s: %s%s", stage, (unsigned long long)hash,
		        code.BytecodeLength, g_lacksFp64 ? ", which this GPU lacks" : "",
		        !g_lacksFp64 ? "left as is" : f.why.empty() ? "demoted to single precision" : "can't demote (",
		        !g_lacksFp64 || f.why.empty() ? "" : (f.why + "), stubbed").c_str());
		if (EFP_Fp64Dump())
			EFP_DumpShader(stage, hash, code.pShaderBytecode, code.BytecodeLength);
		it = g_fixed.emplace(hash, std::move(f)).first;
	}
	const Fixed& fixed = it->second;   // entries are never removed: the reference stays valid
	ReleaseSRWLockExclusive(&g_lock);
	return fixed;
}

static bool Replace(const char* stage, D3D12_SHADER_BYTECODE& code)
{
	if (!UsesDoubles(code)) return true;
	const Fixed& f = Demote(stage, code);
	if (!g_lacksFp64) return true;
	if (!f.why.empty()) return false;
	code = { f.code.data(), f.code.size() };
	return true;
}

static void Stub(D3D12_GRAPHICS_PIPELINE_STATE_DESC& copy)
{
	// The draw is clipped away whatever the pixel shader is. The pixel shader becomes the stub
	// too: vkd3d-proton refuses a pixel shader input the stub vertex shader doesn't output
	// ("No corresponding output signature element found").
	copy.VS = { g_StubVS, sizeof(g_StubVS) };
	copy.PS = { g_StubPS, sizeof(g_StubPS) };
	copy.HS = copy.DS = copy.GS = {};
}

static HRESULT STDMETHODCALLTYPE HookCreateGraphicsPSO(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
	if (!desc || !(UsesDoubles(desc->VS) || UsesDoubles(desc->PS) || UsesDoubles(desc->HS) || UsesDoubles(desc->DS) || UsesDoubles(desc->GS)))
		return TrueCreateGraphicsPSO(device, desc, riid, out);
	D3D12_GRAPHICS_PIPELINE_STATE_DESC copy = *desc;
	bool ok = Replace("vertex", copy.VS);
	ok = Replace("pixel", copy.PS) && ok;
	ok = Replace("hull", copy.HS) && ok;
	ok = Replace("domain", copy.DS) && ok;
	ok = Replace("geometry", copy.GS) && ok;
	if (!g_lacksFp64)
		return TrueCreateGraphicsPSO(device, desc, riid, out);
	HRESULT hr = E_FAIL;
	if (ok) {
		hr = TrueCreateGraphicsPSO(device, &copy, riid, out);
		static LONG failures;
		if (FAILED(hr) && InterlockedIncrement(&failures) <= 10)
			EFP_Log("fp64: a pipeline with demoted shaders failed (0x%08lx), stubbed", hr);
	}
	if (FAILED(hr)) {
		copy = *desc;
		Stub(copy);
		hr = TrueCreateGraphicsPSO(device, &copy, riid, out);
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookCreateComputePSO(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
	if (!desc || !UsesDoubles(desc->CS))
		return TrueCreateComputePSO(device, desc, riid, out);
	D3D12_COMPUTE_PIPELINE_STATE_DESC copy = *desc;
	bool ok = Replace("compute", copy.CS);
	if (!g_lacksFp64)
		return TrueCreateComputePSO(device, desc, riid, out);
	HRESULT hr = ok ? TrueCreateComputePSO(device, &copy, riid, out) : E_FAIL;
	if (FAILED(hr)) {
		copy.CS = { g_StubCS, sizeof(g_StubCS) };
		hr = TrueCreateComputePSO(device, &copy, riid, out);
	}
	return hr;
}

// Pipeline state streams (ID3D12Device2::CreatePipelineState): logged, not changed yet.
static HRESULT STDMETHODCALLTYPE HookCreateStreamPSO(ID3D12Device2* device, const D3D12_PIPELINE_STATE_STREAM_DESC* desc, REFIID riid, void** out)
{
	HRESULT hr = TrueCreateStreamPSO(device, desc, riid, out);
	static LONG logged;
	if (FAILED(hr) && InterlockedIncrement(&logged) <= 10)
		EFP_Log("fp64: a pipeline state stream failed (0x%08lx); streams aren't checked for doubles yet", hr);
	return hr;
}

void EFP_InstallFp64Workaround(ID3D12Device* device)
{
	D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
	g_lacksFp64 = SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) &&
	              !options.DoublePrecisionFloatShaderOps;
	if (!g_lacksFp64 && !EFP_Fp64Dump()) {
		EFP_Log("fp64: the GPU runs double-precision shaders, no workaround needed");
		return;
	}
	EFP_Log(g_lacksFp64 ? "fp64: the GPU has no double-precision shaders: shaders that use them get stubs"
	                    : "fp64: logging and dumping the shaders that use double precision (Fp64Dump = 1)");
	EFP_HookVirtual(device, 10, (void*)HookCreateGraphicsPSO, (void**)&TrueCreateGraphicsPSO);
	EFP_HookVirtual(device, 11, (void*)HookCreateComputePSO, (void**)&TrueCreateComputePSO);
	ID3D12Device2* device2 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)))) {
		EFP_HookVirtual(device2, 47, (void*)HookCreateStreamPSO, (void**)&TrueCreateStreamPSO);
		device2->Release();
	}
}
