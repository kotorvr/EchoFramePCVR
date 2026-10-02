// Shaders that use double precision, on GPUs without it (the Steam Frame's Adreno 750: Turnip
// reports shaderFloat64 = false).
//
// vkd3d-proton refuses such a shader ("Attempting to use FP64 operations in shader ..., but
// this is not supported") and the pipeline creation fails with E_INVALIDARG. Echo treats any
// failed pipeline as fatal ("DirectX error: E_INVALIDARG"). When the device reports no
// double-precision support, the hooks below swap a pixel or compute shader that declares
// doubles (its DXBC/DXIL container's SFI0 feature flags) for a stub that writes zero or does
// nothing, and log each one, so the pipeline exists and Echo carries on. A vertex, hull,
// domain or geometry shader with doubles is passed through unchanged (logged).
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <string.h>

#include "stub_ps.h"   // g_StubPS: build\obj, compiled from runtime\shaders by build.cmd
#include "stub_cs.h"   // g_StubCS

typedef HRESULT(STDMETHODCALLTYPE* CreateGraphicsPSO)(ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateComputePSO)(ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE* CreateStreamPSO)(ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);

static CreateGraphicsPSO TrueCreateGraphicsPSO;
static CreateComputePSO TrueCreateComputePSO;
static CreateStreamPSO TrueCreateStreamPSO;
static LONG g_stubbed;

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

static void Note(const char* stage, const D3D12_SHADER_BYTECODE& code, bool stubbed)
{
	LONG n = InterlockedIncrement(&g_stubbed);
	if (n <= 50)
		EFP_Log("fp64: %s shader %016llx (%zu bytes) uses double precision, which this GPU lacks: %s", stage,
		        (unsigned long long)Hash(code), code.BytecodeLength, stubbed ? "replaced with a stub" : "left as is");
}

static HRESULT STDMETHODCALLTYPE HookCreateGraphicsPSO(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
	if (desc && UsesDoubles(desc->PS)) {
		D3D12_GRAPHICS_PIPELINE_STATE_DESC copy = *desc;
		Note("pixel", desc->PS, true);
		copy.PS = { g_StubPS, sizeof(g_StubPS) };
		return TrueCreateGraphicsPSO(device, &copy, riid, out);
	}
	if (desc) {
		const struct { const char* name; const D3D12_SHADER_BYTECODE* code; } stages[] = {
			{ "vertex", &desc->VS }, { "hull", &desc->HS }, { "domain", &desc->DS }, { "geometry", &desc->GS } };
		for (const auto& s : stages)
			if (UsesDoubles(*s.code)) Note(s.name, *s.code, false);
	}
	return TrueCreateGraphicsPSO(device, desc, riid, out);
}

static HRESULT STDMETHODCALLTYPE HookCreateComputePSO(ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc, REFIID riid, void** out)
{
	if (desc && UsesDoubles(desc->CS)) {
		D3D12_COMPUTE_PIPELINE_STATE_DESC copy = *desc;
		Note("compute", desc->CS, true);
		copy.CS = { g_StubCS, sizeof(g_StubCS) };
		return TrueCreateComputePSO(device, &copy, riid, out);
	}
	return TrueCreateComputePSO(device, desc, riid, out);
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
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))) ||
	    options.DoublePrecisionFloatShaderOps) {
		EFP_Log("fp64: the GPU runs double-precision shaders, no workaround needed");
		return;
	}
	EFP_Log("fp64: the GPU has no double-precision shaders: pixel and compute shaders that use them get stubs");
	EFP_HookVirtual(device, 10, (void*)HookCreateGraphicsPSO, (void**)&TrueCreateGraphicsPSO);
	EFP_HookVirtual(device, 11, (void*)HookCreateComputePSO, (void**)&TrueCreateComputePSO);
	ID3D12Device2* device2 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)))) {
		EFP_HookVirtual(device2, 47, (void*)HookCreateStreamPSO, (void**)&TrueCreateStreamPSO);
		device2->Release();
	}
}
