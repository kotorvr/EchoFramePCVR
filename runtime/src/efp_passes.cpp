// Per-pass GPU timing inside Echo's own frames, labelled with what Echo was drawing
// (echoframe.ini PassTiming = 1, live). Turnip's u_trace says which Vulkan render passes are
// expensive, but not which of Echo's effects they are; this does, by shader.
//
// Every 20 s, for about 150 ms, every command list Echo records gets a GPU timestamp
// (EndQuery) at each render-target binding (OMSetRenderTargets, BeginRenderPass), around each
// Dispatch, and at Close. A segment runs from one timestamp to the next in the same list, and
// is labelled with its render target (size and format of target 0, the number of targets,
// depth), the pixel shader of its first draw (FNV-1a hash of the bytecode, as efp_fp64 logs
// them) and its draw count; a Dispatch with its compute shader. A second later, once the GPU is
// done, the segments are summed per label and the 30 most expensive are logged, in ms per frame.
//
// A segment includes any GPU idle time between submissions of the same list's work, so totals
// can exceed the frame's GPU time; it's the ranking that matters.
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

typedef HRESULT(STDMETHODCALLTYPE* CloseFn)(ID3D12GraphicsCommandList*);
typedef HRESULT(STDMETHODCALLTYPE* ResetFn)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE* DrawInstancedFn)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* DrawIndexedInstancedFn)(ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE* DispatchFn)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE* SetPipelineStateFn)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE* OMSetRenderTargetsFn)(ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL, const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef void(STDMETHODCALLTYPE* BeginRenderPassFn)(ID3D12GraphicsCommandList4*, UINT, const D3D12_RENDER_PASS_RENDER_TARGET_DESC*, const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC*, D3D12_RENDER_PASS_FLAGS);

CloseFn TrueClose;
ResetFn TrueReset;
DrawInstancedFn TrueDrawInstanced;
DrawIndexedInstancedFn TrueDrawIndexedInstanced;
DispatchFn TrueDispatch;
SetPipelineStateFn TrueSetPipelineState;
OMSetRenderTargetsFn TrueOMSetRenderTargets;
BeginRenderPassFn TrueBeginRenderPass;

const UINT kQueries = 32768;

struct Label
{
	uint64_t Shader;        // pixel shader of the first draw, or the compute shader
	uint16_t Width, Height; // render target 0
	uint8_t Format, Targets, Depth, Compute;
	bool operator<(const Label& o) const { return memcmp(this, &o, sizeof(*this)) < 0; }
};

struct Mark { UINT Query; Label What; UINT Draws; };

struct ListState
{
	std::vector<Mark> Marks;   // the open segment is the last one
	ID3D12PipelineState* Pso = nullptr;
};

std::mutex g_lock;
std::unordered_map<ID3D12PipelineState*, uint64_t> g_shaders;   // PSO -> pixel or compute shader hash
std::unordered_map<ID3D12GraphicsCommandList*, ListState> g_lists;
std::vector<std::vector<Mark>> g_closed;                          // finished lists of this window

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12QueryHeap> g_heap;
ComPtr<ID3D12Resource> g_readback;
ComPtr<ID3D12GraphicsCommandList> g_ownList;
const UINT64* g_stamps;
double g_tickMs;
std::atomic<bool> g_sampling;

// Skip = ... : the shader hashes, and the pipelines that use them
SRWLOCK g_skipLock = SRWLOCK_INIT;
std::vector<uint64_t> g_skipHashes;
std::unordered_map<ID3D12PipelineState*, bool> g_skipPsos;   // only pipelines that are skipped
std::atomic<bool> g_skipActive;
thread_local ID3D12GraphicsCommandList* tl_list;   // the list this thread last set a pipeline on
thread_local bool tl_skip;                         // and whether that pipeline is skipped

bool Skipped(ID3D12GraphicsCommandList* list) { return g_skipActive && tl_list == list && tl_skip; }
std::atomic<UINT> g_next;
ULONGLONG g_windowStart, g_windowEnd, g_lastWindow;
unsigned g_frames;

bool Timeable(ID3D12GraphicsCommandList* list)
{
	D3D12_COMMAND_LIST_TYPE t = list->GetType();
	return t == D3D12_COMMAND_LIST_TYPE_DIRECT || t == D3D12_COMMAND_LIST_TYPE_COMPUTE;
}

// Ends the list's open segment and opens one labelled what (caller holds g_lock)
void MarkLocked(ID3D12GraphicsCommandList* list, ListState& s, const Label& what)
{
	UINT q = g_next.fetch_add(1);
	if (q >= kQueries) return;
	list->EndQuery(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q);
	list->ResolveQueryData(g_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, q, 1, g_readback.Get(), UINT64(q) * 8);
	s.Marks.push_back({ q, what, 0 });
}

void AddMark(ID3D12GraphicsCommandList* list, const Label& what)
{
	if (!g_sampling || !Timeable(list)) return;
	std::lock_guard<std::mutex> lk(g_lock);
	MarkLocked(list, g_lists[list], what);
}

Label TargetLabel(UINT count, D3D12_CPU_DESCRIPTOR_HANDLE rt0, bool depth)
{
	Label l = {};
	l.Targets = (uint8_t)count;
	l.Depth = depth;
	int w = 0, h = 0, f = 0;
	if (count && EFP_TargetInfo(rt0.ptr, &w, &h, &f)) {
		l.Width = (uint16_t)w;
		l.Height = (uint16_t)h;
		l.Format = (uint8_t)f;
	}
	return l;
}

void Drew(ID3D12GraphicsCommandList* list)
{
	if (!g_sampling) return;
	std::lock_guard<std::mutex> lk(g_lock);
	auto it = g_lists.find(list);
	if (it == g_lists.end() || it->second.Marks.empty()) return;
	Mark& m = it->second.Marks.back();
	if (!m.Draws++ && !m.What.Compute) {
		auto sh = g_shaders.find(it->second.Pso);
		if (sh != g_shaders.end()) m.What.Shader = sh->second;
	}
}

HRESULT STDMETHODCALLTYPE HookClose(ID3D12GraphicsCommandList* list)
{
	// lists recorded during the window may close after it: they're still finished and counted
	if ((g_sampling || g_windowEnd) && Timeable(list)) {
		std::lock_guard<std::mutex> lk(g_lock);
		auto it = g_lists.find(list);
		if (it != g_lists.end() && !it->second.Marks.empty()) {
			MarkLocked(list, it->second, Label{ ~0ull });   // the end of the last segment
			g_closed.push_back(std::move(it->second.Marks));
		}
		if (it != g_lists.end()) g_lists.erase(it);
	}
	return TrueClose(list);
}

HRESULT STDMETHODCALLTYPE HookReset(ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator, ID3D12PipelineState* pso)
{
	HRESULT hr = TrueReset(list, allocator, pso);
	if (g_sampling && Timeable(list)) {
		std::lock_guard<std::mutex> lk(g_lock);
		ListState& s = g_lists[list];
		s.Marks.clear();
		s.Pso = pso;
		MarkLocked(list, s, Label{});   // work before the first binding
	}
	return hr;
}

void STDMETHODCALLTYPE HookSetPipelineState(ID3D12GraphicsCommandList* list, ID3D12PipelineState* pso)
{
	TrueSetPipelineState(list, pso);
	if (g_skipActive) {
		AcquireSRWLockShared(&g_skipLock);
		tl_list = list;
		tl_skip = g_skipPsos.count(pso) != 0;
		ReleaseSRWLockShared(&g_skipLock);
	}
	if (!g_sampling) return;
	std::lock_guard<std::mutex> lk(g_lock);
	auto it = g_lists.find(list);
	if (it != g_lists.end()) it->second.Pso = pso;
}

void STDMETHODCALLTYPE HookDrawInstanced(ID3D12GraphicsCommandList* list, UINT v, UINT n, UINT sv, UINT si)
{
	if (Skipped(list)) return;
	Drew(list);
	TrueDrawInstanced(list, v, n, sv, si);
}

void STDMETHODCALLTYPE HookDrawIndexedInstanced(ID3D12GraphicsCommandList* list, UINT i, UINT n, UINT si, INT bv, UINT sn)
{
	if (Skipped(list)) return;
	Drew(list);
	TrueDrawIndexedInstanced(list, i, n, si, bv, sn);
}

void STDMETHODCALLTYPE HookDispatch(ID3D12GraphicsCommandList* list, UINT x, UINT y, UINT z)
{
	if (Skipped(list)) return;
	if (g_sampling && Timeable(list)) {
		std::lock_guard<std::mutex> lk(g_lock);
		ListState& s = g_lists[list];
		Label l = {};
		l.Compute = 1;
		auto sh = g_shaders.find(s.Pso);
		if (sh != g_shaders.end()) l.Shader = sh->second;
		Label resume = s.Marks.empty() ? Label{} : s.Marks.back().What;   // what was bound before
		MarkLocked(list, s, l);
		TrueDispatch(list, x, y, z);
		MarkLocked(list, s, resume);
		if (!s.Marks.empty()) s.Marks.back().Draws = 1;   // don't relabel it with a later draw's shader
		return;
	}
	TrueDispatch(list, x, y, z);
}

void STDMETHODCALLTYPE HookOMSetRenderTargets(ID3D12GraphicsCommandList* list, UINT count, const D3D12_CPU_DESCRIPTOR_HANDLE* rtvs,
                                              BOOL range, const D3D12_CPU_DESCRIPTOR_HANDLE* dsv)
{
	if (g_sampling)
		AddMark(list, TargetLabel(count, count && rtvs ? rtvs[0] : D3D12_CPU_DESCRIPTOR_HANDLE{}, dsv != nullptr));
	TrueOMSetRenderTargets(list, count, rtvs, range, dsv);
}

void STDMETHODCALLTYPE HookBeginRenderPass(ID3D12GraphicsCommandList4* list, UINT count, const D3D12_RENDER_PASS_RENDER_TARGET_DESC* rts,
                                           const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC* ds, D3D12_RENDER_PASS_FLAGS flags)
{
	if (g_sampling)
		AddMark(list, TargetLabel(count, count && rts ? rts[0].cpuDescriptor : D3D12_CPU_DESCRIPTOR_HANDLE{}, ds != nullptr));
	TrueBeginRenderPass(list, count, rts, ds, flags);
}

void Report()
{
	std::vector<std::vector<Mark>> lists;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		lists.swap(g_closed);
	}
	struct Sum { double Ms = 0; unsigned Count = 0, Draws = 0; };
	std::map<Label, Sum> sums;
	double total = 0;
	for (auto& marks : lists) {
		for (size_t i = 0; i + 1 < marks.size(); i++) {
			UINT64 a = g_stamps[marks[i].Query], b = g_stamps[marks[i + 1].Query];
			if (!a || !b || b < a) continue;
			double ms = (b - a) * g_tickMs;
			if (ms > 100) continue;
			Sum& s = sums[marks[i].What];
			s.Ms += ms;
			s.Count++;
			s.Draws += marks[i].Draws;
			total += ms;
		}
	}
	double frames = std::max(1u, g_frames);
	std::vector<std::pair<Label, Sum>> top(sums.begin(), sums.end());
	std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second.Ms > b.second.Ms; });
	EFP_Log("passes: %zu command lists over %u frames, %.1f ms of timed GPU work per frame; most expensive (ms per frame, per frame count, draws each):",
	        lists.size(), g_frames, total / frames);
	for (size_t i = 0; i < top.size() && i < 30; i++) {
		const Label& l = top[i].first;
		const Sum& s = top[i].second;
		if (l.Compute)
			EFP_Log("  %6.2f  x%5.1f  compute  cs %016llx", s.Ms / frames, s.Count / frames, (unsigned long long)l.Shader);
		else
			EFP_Log("  %6.2f  x%5.1f  %4.0f draws  %ux%u fmt %u, %u target(s)%s  ps %016llx", s.Ms / frames, s.Count / frames,
			        s.Count ? double(s.Draws) / s.Count : 0.0, l.Width, l.Height, l.Format, l.Targets, l.Depth ? " + depth" : "",
			        (unsigned long long)l.Shader);
	}
	memset((void*)g_stamps, 0, size_t(kQueries) * 8);
}

} // namespace

void EFP_PassesNotePso(void* pso, uint64_t shaderHash)
{
	if (!pso) return;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		g_shaders[(ID3D12PipelineState*)pso] = shaderHash;
	}
	AcquireSRWLockExclusive(&g_skipLock);
	if (std::find(g_skipHashes.begin(), g_skipHashes.end(), shaderHash) != g_skipHashes.end())
		g_skipPsos[(ID3D12PipelineState*)pso] = true;
	ReleaseSRWLockExclusive(&g_skipLock);
}

void EFP_SetSkip(const char* list)
{
	std::vector<uint64_t> hashes;
	for (const char* p = list; *p;) {
		char* end = nullptr;
		unsigned long long h = _strtoui64(p, &end, 16);
		if (end == p) { p++; continue; }
		if (h) hashes.push_back(h);
		p = end;
	}
	std::unordered_map<ID3D12PipelineState*, bool> psos;
	{
		std::lock_guard<std::mutex> lk(g_lock);
		for (auto& s : g_shaders)
			if (std::find(hashes.begin(), hashes.end(), s.second) != hashes.end()) psos[s.first] = true;
	}
	AcquireSRWLockExclusive(&g_skipLock);
	bool changed = hashes != g_skipHashes;
	g_skipHashes = hashes;
	g_skipPsos.swap(psos);
	g_skipActive = !g_skipHashes.empty();
	ReleaseSRWLockExclusive(&g_skipLock);
	if (changed && !hashes.empty())
		EFP_Log("skip: %zu shader(s), %zu pipeline(s) so far", hashes.size(), g_skipPsos.size());
}

void EFP_InstallPasses(ID3D12Device* device, ID3D12CommandQueue* queue)
{
	if (g_device) return;
	UINT64 freq = 0;
	if (FAILED(queue->GetTimestampFrequency(&freq)) || !freq) return;
	g_tickMs = 1000.0 / double(freq);
	D3D12_QUERY_HEAP_DESC hd = { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kQueries };
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_READBACK };
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = UINT64(kQueries) * 8;
	rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	void* mapped = nullptr;
	ComPtr<ID3D12CommandAllocator> allocator;
	if (FAILED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&g_heap)))
	    || FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback)))
	    || FAILED(g_readback->Map(0, nullptr, &mapped))
	    || FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)))
	    || FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&g_ownList)))) {
		EFP_Log("passes: couldn't create the timestamp queries, off");
		g_heap.Reset();
		g_readback.Reset();
		return;
	}
	g_ownList->Close();
	g_stamps = (const UINT64*)mapped;
	memset(mapped, 0, size_t(kQueries) * 8);
	g_device = device;
	// command list slots: 9 Close, 10 Reset, 12 DrawInstanced, 13 DrawIndexedInstanced,
	// 14 Dispatch, 25 SetPipelineState, 46 OMSetRenderTargets, 68 BeginRenderPass
	ID3D12GraphicsCommandList* l = g_ownList.Get();
	EFP_HookVirtual(l, 9, (void*)HookClose, (void**)&TrueClose);
	EFP_HookVirtual(l, 10, (void*)HookReset, (void**)&TrueReset);
	EFP_HookVirtual(l, 12, (void*)HookDrawInstanced, (void**)&TrueDrawInstanced);
	EFP_HookVirtual(l, 13, (void*)HookDrawIndexedInstanced, (void**)&TrueDrawIndexedInstanced);
	EFP_HookVirtual(l, 14, (void*)HookDispatch, (void**)&TrueDispatch);
	EFP_HookVirtual(l, 25, (void*)HookSetPipelineState, (void**)&TrueSetPipelineState);
	EFP_HookVirtual(l, 46, (void*)HookOMSetRenderTargets, (void**)&TrueOMSetRenderTargets);
	EFP_HookVirtual(l, 68, (void*)HookBeginRenderPass, (void**)&TrueBeginRenderPass);
	EFP_Log("passes: per-pass GPU timing ready (PassTiming = 1 to sample every 20 s)");
}

void EFP_PassesFrame()
{
	if (!g_device) return;
	ULONGLONG now = GetTickCount64();
	if (g_sampling) {
		g_frames++;
		if (now - g_windowStart >= 150 || g_next >= kQueries) {
			g_sampling = false;
			g_windowEnd = now;
		}
		return;
	}
	if (g_windowEnd && now - g_windowEnd >= 1000) {   // the GPU has finished the window's work
		Report();
		g_windowEnd = 0;
	}
	if (EFP_PassTiming() && !g_windowEnd && now - g_lastWindow >= 20000) {
		std::lock_guard<std::mutex> lk(g_lock);
		g_lists.clear();
		g_closed.clear();
		g_next = 0;
		g_frames = 0;
		g_windowStart = g_lastWindow = now;
		g_sampling = true;
	}
}
