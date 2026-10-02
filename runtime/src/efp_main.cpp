// EchoFramePCVR runtime entry point, in place of ReviveXR's main.cpp.
//
// ReviveXR normally injects itself and hooks LoadLibrary so a game asking for the
// Oculus runtime gets Revive. Echo's LibOVR loader checks LIBOVR_DLL_DIR first, so
// this DLL is simply built as LibOVRRT64_1.dll and the launcher points Echo at it.
// Revive still calls AttachDetours/DetachDetours around instance creation; with
// nothing hooked they do nothing.
#include "efp.h"

#include <windows.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void AttachDetours() {}
void DetachDetours() {}

static CRITICAL_SECTION g_lock;
static wchar_t g_dir[MAX_PATH];          // this DLL's folder, with a trailing backslash
static wchar_t g_logPath[MAX_PATH];

static float g_renderScale = 1.0f;
static bool g_hmdCache = true;
static char g_serial[24];
static bool g_fp64Dump;

static FILE* OpenInDir(const wchar_t* name, const wchar_t* mode)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%ls%ls", g_dir, name);
	FILE* f = nullptr;
	_wfopen_s(&f, path, mode);
	return f;
}

void EFP_Log(const char* fmt, ...)
{
	if (!g_logPath[0]) return;
	EnterCriticalSection(&g_lock);
	FILE* f = nullptr;
	if (!_wfopen_s(&f, g_logPath, L"a") && f) {
		SYSTEMTIME t;
		GetLocalTime(&t);
		fprintf(f, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
		va_list ap;
		va_start(ap, fmt);
		vfprintf(f, fmt, ap);
		va_end(ap);
		fputc('\n', f);
		fclose(f);
	}
	LeaveCriticalSection(&g_lock);
}

void EFP_LogXrFail(const char* call, int result, const char* file, int line)
{
	const char* base = strrchr(file, '\\');
	EFP_Log("FAILED %d at %s:%d  %s", result, base ? base + 1 : file, line, call);
}

bool EFP_UnderWine()
{
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	return ntdll && GetProcAddress(ntdll, "wine_get_version");
}

float EFP_RenderScale() { return g_renderScale; }
bool EFP_Fp64Dump() { return g_fp64Dump; }

void EFP_DumpShader(const char* stage, uint64_t hash, const void* code, size_t size)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%lsshaders", g_dir);
	CreateDirectoryW(path, nullptr);
	swprintf_s(path, L"%lsshaders\\%hs_%016llx.dxbc", g_dir, stage, (unsigned long long)hash);
	if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES)
		return;
	FILE* f = nullptr;
	if (!_wfopen_s(&f, path, L"wb") && f) {
		fwrite(code, 1, size, f);
		fclose(f);
	}
}

// Echo reports the headset's serial number when it logs in. OpenXR has none, so unless
// echoframe.ini sets one, it's "EFP" and 12 hex digits made from this machine's (or Wine
// prefix's) MachineGuid: the same on every launch.
const char* EFP_HmdSerial()
{
	if (!g_serial[0]) {
		char guid[64] = "";
		DWORD size = sizeof(guid);
		RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY, nullptr, guid, &size);
		uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a
		for (const char* p = guid; *p; p++) h = (h ^ (unsigned char)*p) * 0x100000001b3ull;
		snprintf(g_serial, sizeof(g_serial), "EFP%012llX", (unsigned long long)(h & 0xFFFFFFFFFFFFull));
	}
	return g_serial;
}
bool EFP_UseHmdCache() { return g_hmdCache; }

// "Key = value" lines; '#' and ';' start comments.
static void ReadSettings()
{
	FILE* f = OpenInDir(L"echoframe.ini", L"r");
	if (!f) return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char key[64], value[64];
		if (line[0] == '#' || line[0] == ';' || sscanf_s(line, " %63[^= ] = %63s", key, (unsigned)sizeof(key), value, (unsigned)sizeof(value)) != 2)
			continue;
		if (!_stricmp(key, "RenderScale")) {
			float s = (float)atof(value);
			if (s >= 0.25f && s <= 2.0f) g_renderScale = s;
		}
		else if (!_stricmp(key, "HmdCache"))
			g_hmdCache = atoi(value) != 0;
		else if (!_stricmp(key, "Fp64Dump"))
			g_fp64Dump = atoi(value) != 0;
		else if (!_stricmp(key, "HmdSerial"))
			strncpy_s(g_serial, value, _TRUNCATE);
	}
	fclose(f);
}

bool EFP_LoadHmdCache(EFP_HmdCache* out)
{
	FILE* f = OpenInDir(L"hmd_cache.txt", L"r");
	if (!f) return false;
	EFP_HmdCache c = {};
	int got = 0;
	char line[512];
	while (fgets(line, sizeof(line), f)) {
		int eye;
		if (!strncmp(line, "runtime=", 8)) {
			strncpy_s(c.Runtime, line + 8, _TRUNCATE);
			c.Runtime[strcspn(c.Runtime, "\r\n")] = 0;
			got |= 1;
		}
		else if (sscanf_s(line, "eye%d=", &eye) == 1 && (eye == 0 || eye == 1)) {
			XrFovf& v = c.Fov[eye];
			XrPosef& p = c.Pose[eye];
			if (sscanf_s(strchr(line, '=') + 1, "%u %u %f %f %f %f %f %f %f %f %f %f %f",
			             &c.Width[eye], &c.Height[eye], &v.angleLeft, &v.angleRight, &v.angleUp, &v.angleDown,
			             &p.position.x, &p.position.y, &p.position.z,
			             &p.orientation.x, &p.orientation.y, &p.orientation.z, &p.orientation.w) == 13)
				got |= 2 << eye;
		}
		else if (sscanf_s(line, "bounds=%f %f", &c.Bounds.width, &c.Bounds.height) == 2)
			got |= 8;
	}
	fclose(f);
	if (got != 15) return false;
	*out = c;
	return true;
}

void EFP_SaveHmdCache(const EFP_HmdCache& c)
{
	FILE* f = OpenInDir(L"hmd_cache.txt", L"w");
	if (!f) { EFP_Log("hmd cache: can't write hmd_cache.txt"); return; }
	fprintf(f, "# EchoFramePCVR: headset view data, read before Echo creates its session.\n"
	           "# Delete this file after changing headset or OpenXR runtime.\n");
	fprintf(f, "runtime=%s\n", c.Runtime);
	for (int eye = 0; eye < 2; eye++) {
		const XrFovf& v = c.Fov[eye];
		const XrPosef& p = c.Pose[eye];
		fprintf(f, "eye%d=%u %u %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n", eye,
		        c.Width[eye], c.Height[eye], v.angleLeft, v.angleRight, v.angleUp, v.angleDown,
		        p.position.x, p.position.y, p.position.z, p.orientation.x, p.orientation.y, p.orientation.z, p.orientation.w);
	}
	fprintf(f, "bounds=%.3f %.3f\n", c.Bounds.width, c.Bounds.height);
	fclose(f);
	EFP_Log("hmd cache: saved (left fov %.3f %.3f %.3f %.3f, ipd %.4f m)", c.Fov[0].angleLeft, c.Fov[0].angleRight,
	        c.Fov[0].angleUp, c.Fov[0].angleDown, fabsf(c.Pose[1].position.x - c.Pose[0].position.x));
}

void** EFP_HookVirtual(void* instance, unsigned slot, void* hook, void** original)
{
	void** slotAddress = *(void***)instance + slot;
	if (*slotAddress == hook)
		return nullptr;                              // a second session on the same device type
	*original = *slotAddress;
	DWORD old;
	if (!VirtualProtect(slotAddress, sizeof(void*), PAGE_READWRITE, &old)) {
		EFP_Log("vtable hook: VirtualProtect failed (%lu), slot %u not hooked", GetLastError(), slot);
		return nullptr;
	}
	*slotAddress = hook;
	VirtualProtect(slotAddress, sizeof(void*), old, &old);
	return slotAddress;
}

void EFP_RestoreVirtual(void** slotAddress, void* original)
{
	DWORD old;
	if (VirtualProtect(slotAddress, sizeof(void*), PAGE_READWRITE, &old)) {
		*slotAddress = original;
		VirtualProtect(slotAddress, sizeof(void*), old, &old);
	}
}

void EFP_FrameSubmitted(int64_t periodNs)
{
	static LARGE_INTEGER freq, start, last;
	static int frames;
	static double worst;
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	if (!freq.QuadPart) {
		QueryPerformanceFrequency(&freq);
		start = last = now;
		EFP_Log("frames: first frame submitted");
		return;
	}
	double gap = double(now.QuadPart - last.QuadPart) / freq.QuadPart;
	if (gap > worst) worst = gap;
	last = now;
	frames++;
	double span = double(now.QuadPart - start.QuadPart) / freq.QuadPart;
	if (span >= 10.0) {
		EFP_Log("frames: %.1f fps over %.0f s (display %.1f Hz), slowest frame %.1f ms",
		        frames / span, span, periodNs > 0 ? 1e9 / periodNs : 0.0, worst * 1000.0);
		start = now;
		frames = 0;
		worst = 0;
	}
}

BOOL APIENTRY DllMain(HANDLE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_lock);
		GetModuleFileNameW((HMODULE)module, g_dir, MAX_PATH);
		if (wchar_t* slash = wcsrchr(g_dir, L'\\')) slash[1] = 0;
		swprintf_s(g_logPath, L"%lsruntime.log", g_dir);
		if (FILE* f = OpenInDir(L"runtime.log", L"w")) fclose(f);      // a fresh log per launch
		ReadSettings();
		EFP_Log("EchoFramePCVR runtime loaded (LibOVR on OpenXR, ReviveXR)%s", EFP_UnderWine() ? ", under Wine/Proton" : "");
		EFP_Log("settings: RenderScale %.2f, HmdCache %d, HmdSerial %s", g_renderScale, g_hmdCache ? 1 : 0, EFP_HmdSerial());
	}
	return TRUE;
}
