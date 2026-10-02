// Frame timing for runtime.log: where each frame's time goes, so GPU and settings tuning on
// the Steam Frame can be judged by numbers rather than by feel.
//
// CPU, per frame (QueryPerformanceCounter):
//   wait   time blocked in xrWaitFrame (the runtime's pacing)
//   cpu    from xrWaitFrame returning to xrEndFrame being called (the game's frame)
//   commit time blocked in xrWaitSwapchainImage while the game commits its swapchains
//   end    time spent in xrEndFrame
// GPU, per frame (D3D12 timestamp queries on the game's own queue):
//   gpu    from a timestamp written after xrBeginFrame to one written just before xrEndFrame:
//          the GPU time of the work the game queued for the frame, plus any gaps where the
//          GPU waited for the CPU to submit
//   gpu interval  between consecutive end timestamps: the GPU's actual frame rate
// Summarised every 10 s while frames are submitted.
#include "efp.h"

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <algorithm>
#include <mutex>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

const UINT kSlots = 8;   // frames in flight we can time at once

struct Slot
{
	ComPtr<ID3D12CommandAllocator> Allocator;
	ComPtr<ID3D12GraphicsCommandList> Begin, End;
	long long FrameIndex = -1;
	UINT64 Fence = 0;     // signalled after End; 0 = nothing pending
	bool Began = false;   // Begin executed for FrameIndex, End not yet
};

struct Stat
{
	std::vector<double> Ms;
	void Add(double ms) { Ms.push_back(ms); }
	// "avg 12.3 p95 15.0 max 20.1"
	void Format(char* out, size_t size)
	{
		if (Ms.empty()) { snprintf(out, size, "n/a"); return; }
		double sum = 0;
		for (double v : Ms) sum += v;
		std::sort(Ms.begin(), Ms.end());
		double p95 = Ms[std::min(Ms.size() - 1, (size_t)(Ms.size() * 0.95))];
		snprintf(out, size, "avg %.1f p95 %.1f max %.1f", sum / Ms.size(), p95, Ms.back());
		Ms.clear();
	}
};

std::mutex g_lock;
LARGE_INTEGER g_qpf;

// GPU timer, on the queue the game gave OpenXR
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12QueryHeap> g_queries;
ComPtr<ID3D12Resource> g_readback;
ComPtr<ID3D12Fence> g_fence;
const UINT64* g_stamps;   // the mapped readback buffer: begin, end per slot
UINT64 g_fenceValue;
double g_tickMs;
Slot g_slots[kSlots];
UINT64 g_lastEndStamp;

// CPU timestamps of the current frame
LARGE_INTEGER g_waitStart, g_waitEnd, g_endStart;
double g_commitMs;

// the 10 s window
LARGE_INTEGER g_windowStart, g_lastSubmit;
int g_frames;
double g_slowest;
Stat g_wait, g_cpu, g_commit, g_end, g_gpu, g_gpuInterval;

LARGE_INTEGER Now()
{
	LARGE_INTEGER t;
	QueryPerformanceCounter(&t);
	return t;
}

double Ms(LARGE_INTEGER from, LARGE_INTEGER to)
{
	return double(to.QuadPart - from.QuadPart) * 1000.0 / g_qpf.QuadPart;
}

// Reads the timestamps of every slot whose End has finished on the GPU.
void CollectGpu()
{
	UINT64 done = g_fence->GetCompletedValue();
	// oldest first, so the interval between end timestamps is between consecutive frames
	for (;;) {
		Slot* oldest = nullptr;
		for (Slot& s : g_slots)
			if (s.Fence && s.Fence <= done && (!oldest || s.Fence < oldest->Fence))
				oldest = &s;
		if (!oldest) break;
		UINT i = UINT(oldest - g_slots);
		UINT64 begin = g_stamps[2 * i], end = g_stamps[2 * i + 1];
		if (end >= begin)
			g_gpu.Add((end - begin) * g_tickMs);
		if (g_lastEndStamp && end > g_lastEndStamp)
			g_gpuInterval.Add((end - g_lastEndStamp) * g_tickMs);
		g_lastEndStamp = end;
		oldest->Fence = 0;
	}
}

bool Record(ID3D12Device* device, ID3D12CommandAllocator* allocator, ComPtr<ID3D12GraphicsCommandList>& list,
            D3D12_COMMAND_LIST_TYPE type, UINT query)
{
	if (FAILED(device->CreateCommandList(0, type, allocator, nullptr, IID_PPV_ARGS(&list))))
		return false;
	list->EndQuery(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query);
	list->ResolveQueryData(g_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, query, 1, g_readback.Get(), query * sizeof(UINT64));
	return SUCCEEDED(list->Close());
}

void ReleaseGpu()
{
	// let our command lists finish before their allocators go
	if (g_queue && SUCCEEDED(g_queue->Signal(g_fence.Get(), ++g_fenceValue))) {
		HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		if (done && SUCCEEDED(g_fence->SetEventOnCompletion(g_fenceValue, done)))
			WaitForSingleObject(done, 2000);
		if (done) CloseHandle(done);
	}
	for (Slot& s : g_slots) s = Slot();
	if (g_readback && g_stamps) g_readback->Unmap(0, nullptr);
	g_stamps = nullptr;
	g_readback.Reset();
	g_queries.Reset();
	g_fence.Reset();
	g_queue.Reset();
	g_lastEndStamp = 0;
}

} // namespace

void EFP_TimingStart(ID3D12CommandQueue* queue)
{
	std::lock_guard<std::mutex> lk(g_lock);
	QueryPerformanceFrequency(&g_qpf);
	ReleaseGpu();
	if (!EFP_FrameTiming() || !queue) return;

	D3D12_COMMAND_QUEUE_DESC qd = queue->GetDesc();
	if (qd.Type != D3D12_COMMAND_LIST_TYPE_DIRECT && qd.Type != D3D12_COMMAND_LIST_TYPE_COMPUTE) {
		EFP_Log("frame timing: queue type %d has no timestamps, GPU time not measured", (int)qd.Type);
		return;
	}
	UINT64 freq = 0;
	ComPtr<ID3D12Device> device;
	if (FAILED(queue->GetTimestampFrequency(&freq)) || !freq || FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
		EFP_Log("frame timing: no timestamp frequency, GPU time not measured");
		return;
	}
	g_tickMs = 1000.0 / double(freq);

	D3D12_QUERY_HEAP_DESC hd = {};
	hd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
	hd.Count = 2 * kSlots;
	D3D12_HEAP_PROPERTIES heap = {};
	heap.Type = D3D12_HEAP_TYPE_READBACK;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	rd.Width = 2 * kSlots * sizeof(UINT64);
	rd.Height = 1;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.SampleDesc.Count = 1;
	rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	void* mapped = nullptr;
	bool ok = SUCCEEDED(device->CreateQueryHeap(&hd, IID_PPV_ARGS(&g_queries)))
	       && SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&g_readback)))
	       && SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)))
	       && SUCCEEDED(g_readback->Map(0, nullptr, &mapped));
	g_stamps = (const UINT64*)mapped;
	for (UINT i = 0; ok && i < kSlots; i++) {
		Slot& s = g_slots[i];
		ok = SUCCEEDED(device->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&s.Allocator)))
		  && Record(device.Get(), s.Allocator.Get(), s.Begin, qd.Type, 2 * i)
		  && Record(device.Get(), s.Allocator.Get(), s.End, qd.Type, 2 * i + 1);
	}
	if (!ok) {
		EFP_Log("frame timing: couldn't create the timestamp queries, GPU time not measured");
		ReleaseGpu();
		return;
	}
	g_queue = queue;
	EFP_Log("frame timing: GPU timestamps on queue %p, %.3f MHz", queue, freq / 1e6);
}

void EFP_TimingStop()
{
	std::lock_guard<std::mutex> lk(g_lock);
	ReleaseGpu();
}

void EFP_TimingWaitFrame(bool returned)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
	if (!returned) {
		g_waitStart = Now();
		return;
	}
	g_waitEnd = Now();
	g_wait.Add(Ms(g_waitStart, g_waitEnd));
	g_commitMs = 0;
}

void EFP_TimingBeginFrame(long long frameIndex)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (!g_queue) return;
	Slot& s = g_slots[frameIndex % kSlots];
	if (s.Fence) CollectGpu();
	if (s.Fence) { s.Began = false; return; }   // still on the GPU from kSlots frames ago: skip this one
	ID3D12CommandList* list = s.Begin.Get();
	g_queue->ExecuteCommandLists(1, &list);
	s.FrameIndex = frameIndex;
	s.Began = true;
}

void EFP_TimingCommit(double waitedMs)
{
	std::lock_guard<std::mutex> lk(g_lock);
	g_commitMs += waitedMs;
}

void EFP_TimingEndFrame(long long frameIndex)
{
	std::lock_guard<std::mutex> lk(g_lock);
	g_endStart = Now();
	if (g_waitEnd.QuadPart) g_cpu.Add(Ms(g_waitEnd, g_endStart));
	g_commit.Add(g_commitMs);
	if (!g_queue) return;
	Slot& s = g_slots[frameIndex % kSlots];
	if (!s.Began || s.FrameIndex != frameIndex) return;
	ID3D12CommandList* list = s.End.Get();
	g_queue->ExecuteCommandLists(1, &list);
	s.Fence = ++g_fenceValue;
	s.Began = false;
	g_queue->Signal(g_fence.Get(), s.Fence);
}

void EFP_TimingFrameSubmitted(int64_t periodNs)
{
	std::lock_guard<std::mutex> lk(g_lock);
	LARGE_INTEGER now = Now();
	if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
	if (g_endStart.QuadPart) g_end.Add(Ms(g_endStart, now));
	if (g_queue) CollectGpu();
	if (!g_windowStart.QuadPart) {
		g_windowStart = g_lastSubmit = now;
		EFP_Log("frames: first frame submitted");
		return;
	}
	g_slowest = std::max(g_slowest, Ms(g_lastSubmit, now));
	g_lastSubmit = now;
	g_frames++;
	double span = Ms(g_windowStart, now) / 1000.0;
	if (span < 10.0) return;

	char wait[64], cpu[64], commit[64], end[64], gpu[64], interval[64];
	g_wait.Format(wait, sizeof(wait));
	g_cpu.Format(cpu, sizeof(cpu));
	g_commit.Format(commit, sizeof(commit));
	g_end.Format(end, sizeof(end));
	g_gpu.Format(gpu, sizeof(gpu));
	g_gpuInterval.Format(interval, sizeof(interval));
	EFP_Log("frames: %.1f fps over %.0f s (display %.1f Hz), slowest frame %.1f ms",
	        g_frames / span, span, periodNs > 0 ? 1e9 / periodNs : 0.0, g_slowest);
	EFP_Log("  ms: gpu %s | gpu interval %s | cpu %s | xrWaitFrame %s | swapchain wait %s | xrEndFrame %s",
	        gpu, interval, cpu, wait, commit, end);
	g_windowStart = now;
	g_frames = 0;
	g_slowest = 0;
}
