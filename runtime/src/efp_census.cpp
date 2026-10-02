// What Echo copies and resolves on the GPU, logged to runtime.log (echoframe.ini Census = 1).
//
// A Turnip trace of the social lobby shows about ten vkCmdCopyImage per frame, about 11% of
// the GPU time. vkd3d-proton turns D3D12 CopyResource / CopyTextureRegion between textures into
// those, so this logs, every 30 s for the first ten minutes, each kind of copy Echo records:
// source and destination size and format, and how many per second. That tells which copies are
// eye-sized (a history or depth copy that a hook could avoid) and which are small uploads.
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <map>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

typedef void(STDMETHODCALLTYPE* CopyTextureRegionFn)(ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                                     const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void(STDMETHODCALLTYPE* CopyResourceFn)(ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
typedef void(STDMETHODCALLTYPE* ResolveSubresourceFn)(ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);

CopyTextureRegionFn TrueCopyTextureRegion;
CopyResourceFn TrueCopyResource;
ResolveSubresourceFn TrueResolveSubresource;

struct Key
{
	char Op;                       // 'R' CopyResource, 'T' CopyTextureRegion, 'S' ResolveSubresource
	uint8_t SrcDim, DstDim;        // D3D12_RESOURCE_DIMENSION
	uint16_t SrcFormat, DstFormat;
	uint32_t SrcW, SrcH, DstW, DstH, BoxW, BoxH;
	bool operator<(const Key& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

std::mutex g_lock;
std::map<Key, unsigned> g_counts;
ComPtr<ID3D12GraphicsCommandList> g_list;   // keeps the hooked vtable's owner alive
ULONGLONG g_start, g_last;
bool g_on;

void Describe(ID3D12Resource* r, uint8_t& dim, uint16_t& format, uint32_t& w, uint32_t& h)
{
	if (!r) return;
	D3D12_RESOURCE_DESC d = r->GetDesc();
	dim = (uint8_t)d.Dimension;
	format = (uint16_t)d.Format;
	w = (uint32_t)std::min<UINT64>(d.Width, 0xFFFFFFFF);
	h = d.Height;
}

void Count(Key k)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (g_on) g_counts[k]++;
}

void STDMETHODCALLTYPE HookCopyTextureRegion(ID3D12GraphicsCommandList* list, const D3D12_TEXTURE_COPY_LOCATION* dst, UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION* src, const D3D12_BOX* box)
{
	TrueCopyTextureRegion(list, dst, x, y, z, src, box);
	if (!g_on || !dst || !src) return;
	Key k = { 'T' };
	Describe(src->pResource, k.SrcDim, k.SrcFormat, k.SrcW, k.SrcH);
	Describe(dst->pResource, k.DstDim, k.DstFormat, k.DstW, k.DstH);
	if (box) { k.BoxW = box->right - box->left; k.BoxH = box->bottom - box->top; }
	Count(k);
}

void STDMETHODCALLTYPE HookCopyResource(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, ID3D12Resource* src)
{
	TrueCopyResource(list, dst, src);
	if (!g_on) return;
	Key k = { 'R' };
	Describe(src, k.SrcDim, k.SrcFormat, k.SrcW, k.SrcH);
	Describe(dst, k.DstDim, k.DstFormat, k.DstW, k.DstH);
	Count(k);
}

void STDMETHODCALLTYPE HookResolveSubresource(ID3D12GraphicsCommandList* list, ID3D12Resource* dst, UINT dstSub, ID3D12Resource* src, UINT srcSub,
                                              DXGI_FORMAT format)
{
	TrueResolveSubresource(list, dst, dstSub, src, srcSub, format);
	if (!g_on) return;
	Key k = { 'S' };
	Describe(src, k.SrcDim, k.SrcFormat, k.SrcW, k.SrcH);
	Describe(dst, k.DstDim, k.DstFormat, k.DstW, k.DstH);
	Count(k);
}

} // namespace

void EFP_InstallCensus(ID3D12Device* device)
{
	if (!EFP_Census() || g_list) return;
	ComPtr<ID3D12CommandAllocator> allocator;
	if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
	    || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&g_list)))) {
		EFP_Log("census: couldn't create a command list, off");
		return;
	}
	g_list->Close();
	// command list slots 16 CopyTextureRegion, 17 CopyResource, 19 ResolveSubresource
	EFP_HookVirtual(g_list.Get(), 16, (void*)HookCopyTextureRegion, (void**)&TrueCopyTextureRegion);
	EFP_HookVirtual(g_list.Get(), 17, (void*)HookCopyResource, (void**)&TrueCopyResource);
	EFP_HookVirtual(g_list.Get(), 19, (void*)HookResolveSubresource, (void**)&TrueResolveSubresource);
	g_on = true;
	EFP_Log("census: logging Echo's GPU copies every 30 s for ten minutes");
}

void EFP_CensusFrame()
{
	if (!g_on) return;
	ULONGLONG now = GetTickCount64();
	if (!g_start) g_start = g_last = now;
	if (now - g_last < 30000) return;
	double seconds = (now - g_last) / 1000.0;
	g_last = now;
	std::vector<std::pair<unsigned, Key>> top;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		for (auto& c : g_counts) top.push_back({ c.second, c.first });
		g_counts.clear();
		if (now - g_start > 10 * 60 * 1000) g_on = false;
	}
	std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.first > b.first; });
	EFP_Log("census: GPU copies in the last %.0f s (per second; R CopyResource, T CopyTextureRegion, S resolve; dim 1 buffer 3 2D):", seconds);
	for (size_t i = 0; i < top.size() && i < 14; i++) {
		const Key& k = top[i].second;
		EFP_Log("  %c %6.1f/s  dim%u %ux%u fmt %u -> dim%u %ux%u fmt %u%s", k.Op, top[i].first / seconds, k.SrcDim, k.SrcW, k.SrcH,
		        k.SrcFormat, k.DstDim, k.DstW, k.DstH, k.DstFormat, k.BoxW ? " (box)" : "");
	}
}
