// Fixed foveated rendering for Echo, through D3D12 variable-rate shading (VRS tier 2).
//
// Echo's GPU time on the Steam Frame is almost all per pixel. The lens shows the edges of each
// eye's image at a lower resolution than the middle anyway, so there the pixel shaders can run
// once per 2x2 or 4x4 block instead of per pixel. vkd3d-proton turns D3D12's shading-rate image
// into a Vulkan fragment-shading-rate attachment, which Turnip supports (8x8-pixel tiles).
//
// How: the hooks below see every render-target binding of Echo's command lists
// (OMSetRenderTargets, BeginRenderPass). Render-target views are recorded when they're created
// (CreateRenderTargetView, CopyDescriptorsSimple), so the bound target's size is known. A target
// shaped like one eye's view, or two side by side, at the eye texture's height or half of it gets a
// shading-rate image: full rate within a cone around each eye's optical axis, 2x2 in a ring
// outside it, 4x4 (2x2 on level 1) beyond. Any other target gets full rate everywhere.
// Images are made once per target size and shape.
//
// echoframe.ini: Foveation = 0 (off), 1 (light), 2 (medium), 3 (strong). It can be changed while
// Echo runs, unless it was 0 at the start (then nothing is hooked). runtime.log lists, for
// the first minutes, the render targets Echo binds and which were foveated.
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <math.h>
#include <stdio.h>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

typedef void(STDMETHODCALLTYPE* OMSetRenderTargetsFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef void(STDMETHODCALLTYPE* BeginRenderPassFn)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);
typedef void(STDMETHODCALLTYPE* CreateRtvFn)(ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef D3D12_RESOURCE_ALLOCATION_INFO*(STDMETHODCALLTYPE* GetResourceAllocationInfoFn)(ID3D12Device*, D3D12_RESOURCE_ALLOCATION_INFO*, UINT, UINT, const D3D12_RESOURCE_DESC*);
typedef void(STDMETHODCALLTYPE* CopyDescriptorsSimpleFn)(ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);

OMSetRenderTargetsFn TrueOMSetRenderTargets;
BeginRenderPassFn TrueBeginRenderPass;
CreateRtvFn TrueCreateRtv;
CopyDescriptorsSimpleFn TrueCopyDescriptorsSimple;
GetResourceAllocationInfoFn TrueGetResourceAllocationInfo;

// What a render-target view points at
struct Target
{
	uint16_t Width, Height;
	uint8_t Format;
	uint8_t Flags;   // kLayered, kMultisampled
};
const uint8_t kLayered = 1, kMultisampled = 2;

SRWLOCK g_lock = SRWLOCK_INIT;
std::unordered_map<SIZE_T, Target> g_targets;   // RTV descriptor address -> target
UINT g_rtvIncrement;

// Per eye: the eye texture's size and its field of view (tangents, all positive)
struct Eye { int Width, Height; float Left, Right, Up, Down; };
Eye g_eyes[2];

int g_level;
UINT g_tile;                  // shading-rate image tile size in pixels (8 on the Frame)
bool g_rate4x4;               // 4x4 (an "additional" shading rate) is supported
ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12GraphicsCommandList> g_ownList;   // keeps a list alive; its vtable is the one hooked

enum Shape : uint8_t { NotEye, OneEye, BothEyes };
std::map<uint64_t, ComPtr<ID3D12Resource>> g_images;   // (width, height, shape) -> image; null: failed
std::vector<ComPtr<ID3D12Resource>> g_retired;          // images of an earlier level, maybe still in flight

// Census of what Echo binds, logged for the first minutes
struct CensusKey
{
	uint16_t Width, Height; uint8_t Format, Flags, Shape;
	bool operator<(const CensusKey& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};
std::map<CensusKey, unsigned> g_census;
unsigned g_unknown;           // bindings of a view we didn't see created
bool g_censusOn = true;

Shape Classify(const Target& t)
{
	const Eye& e = g_eyes[0];
	if (!e.Width || !e.Height || (t.Flags & (kLayered | kMultisampled)))
		return NotEye;
	float eyeAspect = float(e.Width) / e.Height, aspect = float(t.Width) / t.Height;
	// the eye texture's own size or half of it: other square targets (2048x2048, 1024x1024 in the
	// lobby) are shadow or reflection maps, not views
	float scale = float(t.Height) / e.Height;
	if (fabsf(scale - 1.0f) > 0.03f && fabsf(scale - 0.5f) > 0.02f)
		return NotEye;
	if (fabsf(aspect / eyeAspect - 1.0f) < 0.03f) return OneEye;
	if (fabsf(aspect / (2 * eyeAspect) - 1.0f) < 0.03f) return BothEyes;
	return NotEye;
}

// The rate for a point of one eye's view, given as tangents from its optical axis.
uint8_t Rate(float tx, float ty)
{
	static const float inner[] = { 0, 0.70f, 0.50f, 0.35f }, outer[] = { 0, 1.10f, 0.90f, 0.65f };
	float t = sqrtf(tx * tx + ty * ty);
	if (t < inner[g_level]) return D3D12_SHADING_RATE_1X1;
	if (t < outer[g_level] || g_level == 1 || !g_rate4x4) return D3D12_SHADING_RATE_2X2;
	return D3D12_SHADING_RATE_4X4;
}

// Point (u, v) in [0,1] of an eye's image, as tangents. A one-eye target could be either eye,
// so it takes the mean of the two (they mirror each other horizontally).
void Tangents(int eye, float u, float v, float& tx, float& ty)
{
	const Eye& e = g_eyes[eye >= 0 ? eye : 0];
	if (eye >= 0) {
		tx = -e.Left + u * (e.Left + e.Right);
	} else {
		const Eye& r = g_eyes[1];
		float left = (e.Left + r.Left) * 0.5f, right = (e.Right + r.Right) * 0.5f;
		tx = -left + u * (left + right);
	}
	ty = e.Up - v * (e.Up + e.Down);
}

ComPtr<ID3D12Resource> MakeImage(UINT width, UINT height, Shape shape)
{
	UINT tw = (width + g_tile - 1) / g_tile, th = (height + g_tile - 1) / g_tile;
	UINT pitch = (tw + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
	std::vector<uint8_t> data(size_t(pitch) * th);
	unsigned counts[3] = {};
	for (UINT y = 0; y < th; y++) {
		for (UINT x = 0; x < tw; x++) {
			float px = (x + 0.5f) * g_tile, py = (y + 0.5f) * g_tile;
			float tx, ty;
			if (shape == BothEyes) {
				float half = width * 0.5f;
				int eye = px < half ? 0 : 1;
				Tangents(eye, (px - eye * half) / half, py / height, tx, ty);
			} else {
				Tangents(-1, px / width, py / height, tx, ty);
			}
			uint8_t r = Rate(tx, ty);
			data[size_t(y) * pitch + x] = r;
			counts[r == D3D12_SHADING_RATE_1X1 ? 0 : r == D3D12_SHADING_RATE_2X2 ? 1 : 2]++;
		}
	}

	D3D12_HEAP_PROPERTIES def = { D3D12_HEAP_TYPE_DEFAULT }, up = { D3D12_HEAP_TYPE_UPLOAD };
	D3D12_RESOURCE_DESC td = {};
	td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	td.Width = tw;
	td.Height = th;
	td.DepthOrArraySize = 1;
	td.MipLevels = 1;
	td.Format = DXGI_FORMAT_R8_UINT;
	td.SampleDesc.Count = 1;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = data.size();
	bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

	ComPtr<ID3D12Resource> image, upload;
	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12Fence> fence;
	void* mapped = nullptr;
	if (FAILED(g_device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&image)))
	    || FAILED(g_device->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload)))
	    || FAILED(upload->Map(0, nullptr, &mapped))
	    || FAILED(g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
	    || FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
	    || FAILED(g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
		EFP_Log("foveation: couldn't create a %ux%u shading-rate image", tw, th);
		return nullptr;
	}
	memcpy(mapped, data.data(), data.size());
	upload->Unmap(0, nullptr);

	D3D12_TEXTURE_COPY_LOCATION dst = { image.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
	D3D12_TEXTURE_COPY_LOCATION src = { upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
	src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UINT, tw, th, 1, pitch };
	list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
	D3D12_RESOURCE_BARRIER barrier = {};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = image.Get();
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
	barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE;
	list->ResourceBarrier(1, &barrier);
	list->Close();
	ID3D12CommandList* lists[] = { list.Get() };
	g_queue->ExecuteCommandLists(1, lists);
	g_queue->Signal(fence.Get(), 1);
	HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (done && SUCCEEDED(fence->SetEventOnCompletion(1, done)))
		WaitForSingleObject(done, 2000);
	if (done) CloseHandle(done);

	unsigned total = tw * th;
	EFP_Log("foveation: %ux%u %s target: full rate %.0f%%, 2x2 %.0f%%, 4x4 %.0f%% of the tiles", width, height,
	        shape == BothEyes ? "two-eye" : "one-eye", 100.0 * counts[0] / total, 100.0 * counts[1] / total, 100.0 * counts[2] / total);
	return image;
}

// The shading-rate image for a bound target, or null for full rate. Takes g_lock shared.
ID3D12Resource* ImageFor(D3D12_CPU_DESCRIPTOR_HANDLE rtv)
{
	int level = EFP_Foveation();   // echoframe.ini can change it while Echo runs
	if (level != g_level) {
		AcquireSRWLockExclusive(&g_lock);
		if (level != g_level) {
			for (auto& img : g_images) g_retired.push_back(img.second);
			g_images.clear();
			g_level = level;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
	AcquireSRWLockShared(&g_lock);
	auto it = g_targets.find(rtv.ptr);
	Target t = {};
	bool known = it != g_targets.end();
	if (known) t = it->second;
	Shape shape = known && g_level ? Classify(t) : NotEye;
	uint64_t key = (uint64_t(t.Width) << 32) | (uint64_t(t.Height) << 8) | shape;
	ID3D12Resource* image = nullptr;
	bool make = false;
	if (shape != NotEye) {
		auto img = g_images.find(key);
		if (img != g_images.end()) image = img->second.Get();
		else make = true;
	}
	bool census = g_censusOn;
	ReleaseSRWLockShared(&g_lock);

	if (make || census) {
		AcquireSRWLockExclusive(&g_lock);
		if (make) {
			auto img = g_images.find(key);   // another thread may have made it meanwhile
			if (img == g_images.end())
				img = g_images.emplace(key, MakeImage(t.Width, t.Height, shape)).first;
			image = img->second.Get();
		}
		if (g_censusOn) {
			if (known) g_census[CensusKey{ t.Width, t.Height, t.Format, t.Flags, uint8_t(image ? shape : NotEye) }]++;
			else g_unknown++;
		}
		ReleaseSRWLockExclusive(&g_lock);
	}
	return image;
}

void Apply(ID3D12GraphicsCommandList* list, ID3D12Resource* image)
{
	// vkd3d-proton's command lists implement every ID3D12GraphicsCommandList version with one
	// vtable (checked in EFP_InstallFoveation), so the cast is safe.
	ID3D12GraphicsCommandList5* list5 = static_cast<ID3D12GraphicsCommandList5*>(list);
	static const D3D12_SHADING_RATE_COMBINER fromImage[2] = { D3D12_SHADING_RATE_COMBINER_PASSTHROUGH, D3D12_SHADING_RATE_COMBINER_OVERRIDE };
	list5->RSSetShadingRate(D3D12_SHADING_RATE_1X1, image ? fromImage : nullptr);
	list5->RSSetShadingRateImage(image);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                              BOOL range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
	TrueOMSetRenderTargets(list, count, rtvs, range, dsv);
	Apply(list, count && rtvs ? ImageFor(rtvs[0]) : nullptr);
}

void STDMETHODCALLTYPE HookBeginRenderPass(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
	Apply(list, count && rts ? ImageFor(rts[0].cpuDescriptor) : nullptr);
	TrueBeginRenderPass(list, count, rts, ds, flags);
}

void STDMETHODCALLTYPE HookCreateRtv(ID3D12Device* device, ID3D12Resource* resource, const D3D12_RENDER_TARGET_VIEW_DESC* desc, D3D12_CPU_DESCRIPTOR_HANDLE handle)
{
	TrueCreateRtv(device, resource, desc, handle);
	Target t = {};
	if (resource) {
		D3D12_RESOURCE_DESC rd = resource->GetDesc();
		UINT mip = 0;
		bool layered = rd.DepthOrArraySize > 1;
		if (desc) {
			switch (desc->ViewDimension) {
			case D3D12_RTV_DIMENSION_TEXTURE2D: mip = desc->Texture2D.MipSlice; layered = false; break;
			case D3D12_RTV_DIMENSION_TEXTURE2DARRAY: mip = desc->Texture2DArray.MipSlice; layered = desc->Texture2DArray.ArraySize > 1; break;
			case D3D12_RTV_DIMENSION_TEXTURE2DMS: layered = false; break;
			case D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY: layered = desc->Texture2DMSArray.ArraySize > 1; break;
			default: layered = true; break;   // buffers, 1D and 3D targets: never foveated
			}
		}
		t.Width = (uint16_t)std::min<UINT64>(0xFFFF, std::max<UINT64>(1, rd.Width >> mip));
		t.Height = (uint16_t)std::max<UINT>(1, rd.Height >> mip);
		t.Format = (uint8_t)(desc && desc->Format ? desc->Format : rd.Format);
		t.Flags = (layered ? kLayered : 0) | (rd.SampleDesc.Count > 1 ? kMultisampled : 0);
	}
	AcquireSRWLockExclusive(&g_lock);
	if (resource) g_targets[handle.ptr] = t;
	else g_targets.erase(handle.ptr);
	ReleaseSRWLockExclusive(&g_lock);
}

void STDMETHODCALLTYPE HookCopyDescriptorsSimple(ID3D12Device* device, UINT count, D3D12_CPU_DESCRIPTOR_HANDLE dst,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_DESCRIPTOR_HEAP_TYPE type)
{
	TrueCopyDescriptorsSimple(device, count, dst, src, type);
	if (type != D3D12_DESCRIPTOR_HEAP_TYPE_RTV) return;
	AcquireSRWLockExclusive(&g_lock);
	for (UINT i = 0; i < count; i++) {
		auto it = g_targets.find(src.ptr + SIZE_T(i) * g_rtvIncrement);
		if (it != g_targets.end()) g_targets[dst.ptr + SIZE_T(i) * g_rtvIncrement] = it->second;
		else g_targets.erase(dst.ptr + SIZE_T(i) * g_rtvIncrement);
	}
	ReleaseSRWLockExclusive(&g_lock);
}

// Valve's FDM layer (FDM_DEBUG=enable) makes the eye-sized render targets it gives a density map
// bigger than the size D3D12 reported for them. Echo places its targets in heaps laid out with the
// reported sizes, so vkd3d-proton then refuses one ("Heap too small for the texture") and Echo stops
// with E_INVALIDARG. With the layer on, eye-sized render and depth targets are reported 1/8 bigger.
bool g_padEyeTargets;
unsigned g_padded;

D3D12_RESOURCE_ALLOCATION_INFO* STDMETHODCALLTYPE HookGetResourceAllocationInfo(ID3D12Device* device, D3D12_RESOURCE_ALLOCATION_INFO* info,
                                                                                UINT mask, UINT count, const D3D12_RESOURCE_DESC* descs)
{
	TrueGetResourceAllocationInfo(device, info, mask, count, descs);
	if (!g_padEyeTargets || info->SizeInBytes == UINT64_MAX) return info;
	for (UINT i = 0; i < count; i++) {
		const D3D12_RESOURCE_DESC& d = descs[i];
		if (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Width >= 1024 && d.Height >= 512
		    && (d.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) {
			UINT64 align = std::max<UINT64>(info->Alignment, 65536);
			info->SizeInBytes = (info->SizeInBytes + info->SizeInBytes / 8 + align - 1) & ~(align - 1);
			if (g_padded++ < 8)
				EFP_Log("fdm: %llux%u target (format %d) reported %llu bytes, for the FDM layer's bigger images", d.Width, d.Height,
				        (int)d.Format, info->SizeInBytes);
			break;
		}
	}
	return info;
}

} // namespace

void EFP_FoveationEye(int eye, int width, int height, float left, float right, float up, float down)
{
	if (eye < 0 || eye > 1) return;
	AcquireSRWLockExclusive(&g_lock);
	Eye e = { width, height, left, right, up, down };
	if (memcmp(&g_eyes[eye], &e, sizeof(e))) {
		g_eyes[eye] = e;
		g_images.clear();   // made for the old size or field of view
	}
	ReleaseSRWLockExclusive(&g_lock);
}

void EFP_EarlyD3D12()
{
	char fdm[128] = "";
	if (g_padEyeTargets || !GetEnvironmentVariableA("FDM_DEBUG", fdm, sizeof(fdm)) || !strstr(fdm, "enable"))
		return;
	typedef HRESULT(WINAPI* CreateDeviceFn)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
	HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
	CreateDeviceFn create = d3d12 ? (CreateDeviceFn)GetProcAddress(d3d12, "D3D12CreateDevice") : nullptr;
	static ComPtr<ID3D12Device> device;   // kept: vkd3d-proton hands Echo the same device for the adapter
	if (!create || FAILED(create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
		EFP_Log("fdm: couldn't create a D3D12 device to hook; Echo will likely fail to start with the FDM layer");
		return;
	}
	g_padEyeTargets = true;
	EFP_HookVirtual(device.Get(), 25, (void*)HookGetResourceAllocationInfo, (void**)&TrueGetResourceAllocationInfo);
	EFP_Log("fdm: Valve's FDM layer is on (FDM_DEBUG=%s): eye-sized targets are reported bigger", fdm);
}

void EFP_InstallFoveation(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	g_level = EFP_Foveation();
	if (!g_level) {
		EFP_Log("foveation: off (Foveation = 0; it can only be switched on while Echo runs if it wasn't 0 at the start)");
		return;
	}
	if (g_device) return;   // already installed (a second session on the same device)
	D3D12_FEATURE_DATA_D3D12_OPTIONS6 o6 = {};
	if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &o6, sizeof(o6))) || o6.VariableShadingRateTier < D3D12_VARIABLE_SHADING_RATE_TIER_2) {
		EFP_Log("foveation: the GPU has no variable-rate shading tier 2 (tier %d), off", (int)o6.VariableShadingRateTier);
		return;
	}
	g_tile = o6.ShadingRateImageTileSize;
	g_rate4x4 = o6.AdditionalShadingRatesSupported != FALSE;

	ComPtr<ID3D12CommandAllocator> allocator;
	ComPtr<ID3D12GraphicsCommandList> list;
	ComPtr<ID3D12GraphicsCommandList5> list5;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
	    || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))
	    || FAILED(list.As(&list5)) || (void*)list5.Get() != (void*)list.Get()) {
		EFP_Log("foveation: command lists aren't ID3D12GraphicsCommandList5 with one vtable, off");
		return;
	}
	list->Close();
	g_device = device;
	g_queue = queue;
	g_ownList = list;
	g_rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	// Device slots 20 (CreateRenderTargetView, after Revive's own hook) and 24
	// (CopyDescriptorsSimple); command list slots 46 (OMSetRenderTargets), 68 (BeginRenderPass).
	EFP_HookVirtual(device, 20, (void*)HookCreateRtv, (void**)&TrueCreateRtv);
	EFP_HookVirtual(device, 24, (void*)HookCopyDescriptorsSimple, (void**)&TrueCopyDescriptorsSimple);
	EFP_HookVirtual(list.Get(), 46, (void*)HookOMSetRenderTargets, (void**)&TrueOMSetRenderTargets);
	EFP_HookVirtual(list.Get(), 68, (void*)HookBeginRenderPass, (void**)&TrueBeginRenderPass);
	static const char* names[] = { "off", "light", "medium", "strong" };
	EFP_Log("foveation: %s (Foveation = %d), %ux%u tiles, 4x4 rate %s", names[g_level], g_level, g_tile, g_tile,
	        g_rate4x4 ? "available" : "not available (2x2 at most)");
}

void EFP_FoveationFrame()
{
	if (!g_device || !g_censusOn) return;
	static ULONGLONG start, last;
	ULONGLONG now = GetTickCount64();
	if (!start) start = last = now;
	if (now - last < 30000) return;
	last = now;
	AcquireSRWLockExclusive(&g_lock);
	std::vector<std::pair<unsigned, CensusKey>> top;
	for (auto& c : g_census) top.push_back({ c.second, c.first });
	std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
	EFP_Log("foveation: render targets bound in the last 30 s (%u bindings of views made before the hooks):", g_unknown);
	static const char* shapes[] = { "full rate", "foveated, one eye", "foveated, two eyes" };
	for (size_t i = 0; i < top.size() && i < 12; i++) {
		const CensusKey& k = top[i].second;
		EFP_Log("  %ux%u format %u%s%s: %u, %s", k.Width, k.Height, k.Format, k.Flags & kLayered ? " layered" : "",
		        k.Flags & kMultisampled ? " msaa" : "", top[i].first, shapes[k.Shape]);
	}
	g_census.clear();
	g_unknown = 0;
	if (now - start > 10 * 60 * 1000) g_censusOn = false;   // ten minutes is enough to see Echo's passes
	ReleaseSRWLockExclusive(&g_lock);
}
