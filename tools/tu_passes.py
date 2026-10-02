#!/usr/bin/env python3
"""Ranks the GPU work in a Turnip u_trace CSV (MESA_GPU_TRACES=print_csv MESA_GPU_TRACEFILE=...).

  tu_passes.py TRACE.csv [LAST_BYTES] [FPS]

Reads the last LAST_BYTES of the file (default 300 MB: the trace grows by ~150 MB a minute),
pairs each start_X/end_X event, and sums GPU time per kind of render pass (size, attachments,
depth, load/store bytes per pixel, tiled or sysmem) and per compute dispatch, as ms per frame at
FPS (Echo's frame rate during the trace, from runtime.log; u_trace's own frame counter counts
vkd3d submissions, not Echo's frames).
Run it on the Frame (python3 is there) to avoid copying gigabytes.
"""
import collections
import os
import sys


def attrs(fields):
    out = {}
    for f in fields:
        f = f.strip()
        if "=" in f:
            k, v = f.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def main():
    path = sys.argv[1]
    last = int(float(sys.argv[2])) if len(sys.argv) > 2 else 300_000_000
    fps = float(sys.argv[3]) if len(sys.argv) > 3 else 90.0
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        f.seek(max(0, size - last))
        data = f.read().decode("utf-8", "replace").splitlines()[1:]   # the first line is partial

    open_events = {}          # event name -> (time, start attrs)
    passes = collections.defaultdict(lambda: [0.0, 0, 0])   # key -> [ms, count, draws]
    compute = collections.defaultdict(lambda: [0.0, 0])
    other = collections.defaultdict(lambda: [0.0, 0])
    first = last_t = None
    gpu_total = 0.0
    for line in data:
        parts = line.split(",")
        if len(parts) < 4 or not parts[2].isdigit():
            continue
        t, event = int(parts[2]), parts[3]
        if first is None or t < first:
            first = t
        if last_t is None or t > last_t:
            last_t = t
        a = attrs(parts[4:])
        if event.startswith("start_"):
            open_events[event[6:]] = (t, a)
            continue
        if not event.startswith("end_"):
            continue
        name = event[4:]
        if name not in open_events:
            continue
        t0, s = open_events.pop(name)
        ms = (t - t0) / 1e6
        if ms < 0 or ms > 200:
            continue
        if name == "render_pass":
            key = (f"{s.get('width')}x{s.get('height')} att{s.get('attachment_count')} depth={s.get('hasDepth')} "
                   f"load{s.get('loadCPP')} store{s.get('storeCPP')} clear{s.get('clearCPP')} "
                   f"{'gmem' if a.get('tiledRender') == 'true' else 'sysmem'} bins={s.get('numberOfBins', '-')}")
            p = passes[key]
            p[0] += ms
            p[1] += 1
            p[2] += int(a.get("drawCount", "0") or 0)
            gpu_total += ms
        elif name in ("compute", "compute_indirect"):
            key = f"{name} {s.get('group_x', s.get('groupx', '?'))}x{s.get('group_y', '?')}x{s.get('group_z', '?')} local {s.get('local_x', '?')}x{s.get('local_y', '?')}"
            c = compute[key]
            c[0] += ms
            c[1] += 1
            gpu_total += ms
        elif name in ("copy_image", "copy_buffer_to_image", "copy_buffer", "blit_image", "resolve", "generic_clear",
                      "sysmem_clear", "update_buffer", "fill_buffer"):
            o = other[name]
            o[0] += ms
            o[1] += 1

    span = (last_t - first) / 1e9 if first is not None else 1
    n = max(1.0, span * fps)
    print(f"{len(data)} lines, {span:.1f} s of GPU time line, {n:.0f} frames at {fps} fps: "
          f"{gpu_total / n:.2f} ms of render passes + compute per frame")
    print("\nrender passes, ms per frame (count per frame, draws per pass):")
    for k, (ms, cnt, draws) in sorted(passes.items(), key=lambda x: -x[1][0])[:30]:
        print(f"  {ms / n:7.3f}  x{cnt / n:5.1f}  {draws / max(1, cnt):6.0f} draws  {k}")
    print("\ncompute, ms per frame:")
    for k, (ms, cnt) in sorted(compute.items(), key=lambda x: -x[1][0])[:15]:
        print(f"  {ms / n:7.3f}  x{cnt / n:5.1f}  {k}")
    print("\nother (may overlap passes), ms per frame:")
    for k, (ms, cnt) in sorted(other.items(), key=lambda x: -x[1][0]):
        print(f"  {ms / n:7.3f}  x{cnt / n:5.1f}  {k}")


if __name__ == "__main__":
    main()
