// Changes to Echo's own code and globals, from echoframe.ini, to make its renderer cheaper on
// the Steam Frame (addresses for echovr.exe build 631547; see docs/FRAME-NOTES.md).
//
//   Patch = rva:old:new,...   code bytes (hex), written once when the runtime loads, before
//                             Echo sets up its renderer, and only where the old bytes match
//                             (another build is left alone). E.g. 5863F7:7434:9090 makes the
//                             renderer setup always drop the features it drops on mobile.
//   Poke = rva:type:value,... globals written after every frame (live), so a setting Echo
//                             reapplies, or a flag a level script sets, stays as given.
//                             type: f float, i int32, b byte. E.g. 20AFBC8:f:3.0 (mesh LOD distance).
//   Peek = rva:bytes          logs that many bytes (as int32 and float) once, when set (live).
// RVAs are relative to echovr's image base (the process's main module).
#include "efp.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct Poke { uint32_t Rva; char Type; union { float F; int32_t I; uint8_t B; }; };

std::mutex g_lock;
std::vector<Poke> g_pokes;
std::string g_peek;
bool g_peekPending;

uint8_t* Base() { return (uint8_t*)GetModuleHandleW(nullptr); }

// The image's size, so an address can be checked before it's touched
uint32_t ImageSize()
{
	uint8_t* b = Base();
	IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
	return nt->OptionalHeader.SizeOfImage;
}

std::vector<uint8_t> Hex(const char* s, const char* end)
{
	std::vector<uint8_t> out;
	for (const char* p = s; p + 1 < end; p += 2) {
		char byte[3] = { p[0], p[1], 0 };
		out.push_back((uint8_t)strtoul(byte, nullptr, 16));
	}
	return out;
}

} // namespace

void EFP_ApplyPatches(const char* spec)
{
	const uint32_t size = ImageSize();
	for (const char* p = spec; *p;) {
		const char* end = strchr(p, ',');
		if (!end) end = p + strlen(p);
		const char* c1 = (const char*)memchr(p, ':', end - p);
		const char* c2 = c1 ? (const char*)memchr(c1 + 1, ':', end - c1 - 1) : nullptr;
		if (c2) {
			uint32_t rva = strtoul(p, nullptr, 16);
			std::vector<uint8_t> from = Hex(c1 + 1, c2), to = Hex(c2 + 1, end);
			uint8_t* at = Base() + rva;
			if (from.size() != to.size() || from.empty() || rva + from.size() > size)
				EFP_Log("patch %x: malformed, skipped", rva);
			else if (memcmp(at, from.data(), from.size()))
				EFP_Log("patch %x: the bytes there aren't the expected ones (another Echo build?), skipped", rva);
			else {
				DWORD old;
				VirtualProtect(at, to.size(), PAGE_EXECUTE_READWRITE, &old);
				memcpy(at, to.data(), to.size());
				VirtualProtect(at, to.size(), old, &old);
				FlushInstructionCache(GetCurrentProcess(), at, to.size());
				EFP_Log("patch %x: %zu byte(s) changed", rva, to.size());
			}
		}
		p = *end ? end + 1 : end;
	}
}

void EFP_SetPokes(const char* spec)
{
	std::vector<Poke> pokes;
	const uint32_t size = ImageSize();
	for (const char* p = spec; *p;) {
		const char* end = strchr(p, ',');
		if (!end) end = p + strlen(p);
		std::string item(p, end);
		char type = 0;
		char value[64] = "";
		unsigned rva = 0;
		if (sscanf_s(item.c_str(), "%x:%c:%63s", &rva, &type, 1, value, (unsigned)sizeof(value)) == 3 && rva + 4 <= size) {
			Poke k = { rva, type };
			if (type == 'f') k.F = (float)atof(value);
			else if (type == 'i') k.I = atoi(value);
			else if (type == 'b') k.B = (uint8_t)atoi(value);
			else { p = *end ? end + 1 : end; continue; }
			pokes.push_back(k);
		}
		p = *end ? end + 1 : end;
	}
	std::lock_guard<std::mutex> lk(g_lock);
	g_pokes.swap(pokes);
}

void EFP_SetPeek(const char* spec)
{
	std::lock_guard<std::mutex> lk(g_lock);
	if (g_peek != spec) {
		g_peek = spec;
		g_peekPending = !g_peek.empty();
	}
}

void EFP_PatchFrame()
{
	std::lock_guard<std::mutex> lk(g_lock);
	for (const Poke& k : g_pokes) {
		uint8_t* at = Base() + k.Rva;
		if (k.Type == 'f') *(volatile float*)at = k.F;
		else if (k.Type == 'i') *(volatile int32_t*)at = k.I;
		else *(volatile uint8_t*)at = k.B;
	}
	if (g_peekPending) {
		g_peekPending = false;
		unsigned rva = 0, bytes = 0;
		if (sscanf_s(g_peek.c_str(), "%x:%u", &rva, &bytes) == 2 && bytes && rva + bytes <= ImageSize()) {
			for (unsigned o = 0; o + 4 <= bytes; o += 16) {
				char line[256];
				int n = snprintf(line, sizeof(line), "peek %x:", rva + o);
				for (unsigned i = o; i < o + 16 && i + 4 <= bytes; i += 4) {
					int32_t v;
					float f;
					memcpy(&v, Base() + rva + i, 4);
					memcpy(&f, Base() + rva + i, 4);
					n += snprintf(line + n, sizeof(line) - n, "  %d (%g)", v, f);
				}
				EFP_Log("%s", line);
			}
		}
	}
}
