// EchoFrame.exe: starts Echo VR PCVR on OpenXR through the EchoFramePCVR runtime.
//
//   EchoFrame.exe [--steamvr] [echo arguments...]
//
// Lives in Echo's bin\win10, next to echovr.exe, with the runtime in bin\win10\EchoFrame\.
// It runs on Windows and, on the Steam Frame, inside Proton. Before starting Echo it:
//   1. makes echovr_openxr.exe, a copy of echovr.exe whose LibOVR runtime signature check
//      always passes (Echo only loads an Oculus-signed LibOVRRT64_1.dll otherwise);
//   2. creates the "OculusHMDConnected" event, which the Oculus service normally owns and
//      Echo's LibOVR shim checks before it starts VR;
//   3. sets LIBOVR_DLL_DIR to bin\win10\EchoFrame\, the first folder Echo's loaders search,
//      and puts it first on PATH (for the Platform SDK stand-in, LibOVRPlatform64_1.dll).
// Only the Echo process started here sees these. --steamvr (Windows only) points this launch's
// OpenXR loader at SteamVR instead of the system runtime. Logs to EchoFrame\launcher.log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <string>

static FILE* g_log;

static void Log(const wchar_t* fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vfwprintf(stdout, fmt, ap);
	va_end(ap);
	fputwc(L'\n', stdout);
	if (g_log) {
		va_start(ap, fmt);
		vfwprintf(g_log, fmt, ap);
		va_end(ap);
		fputwc(L'\n', g_log);
		fflush(g_log);
	}
}

static int Fail(const std::wstring& message, int code)
{
	Log(L"ERROR: %ls", message.c_str());
	MessageBoxW(nullptr, message.c_str(), L"EchoFrame", MB_OK | MB_ICONERROR);
	return code;
}

static bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

static bool ReadFile(const std::wstring& path, std::string& out)
{
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
	char buffer[1 << 16];
	size_t n;
	out.clear();
	while ((n = fread(buffer, 1, sizeof(buffer), f)) > 0) out.append(buffer, n);
	fclose(f);
	return true;
}

static ULONGLONG WriteTime(const std::wstring& path)
{
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return 0;
	return ULARGE_INTEGER{ { a.ftLastWriteTime.dwLowDateTime, a.ftLastWriteTime.dwHighDateTime } }.QuadPart;
}

// The supported build (timestamp 1683152886). Its signature check for the LibOVR runtime
// DLL (WinVerifyTrust, then the "Oculus VR, LLC" signer) starts at this RVA; the copy
// returns 1 ("signed") straight away.
static const DWORD kTimestamp = 1683152886;
static const DWORD kSignatureCheckRva = 0x1365bd0;
static const BYTE kPrologue[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x55, 0x57, 0x41, 0x56 };
static const BYTE kReturnTrue[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };   // mov eax, 1 ; ret

static size_t RvaToFileOffset(const std::string& image, DWORD rva)
{
	if (image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return 0;
	DWORD ntOffset = *(const DWORD*)&image[0x3C];
	if (ntOffset + sizeof(IMAGE_NT_HEADERS64) > image.size()) return 0;
	const IMAGE_NT_HEADERS64* nt = (const IMAGE_NT_HEADERS64*)&image[ntOffset];
	if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.TimeDateStamp != kTimestamp) return 0;
	const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
	for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, section++) {
		if ((const char*)(section + 1) > image.data() + image.size()) return 0;
		if (rva >= section->VirtualAddress && rva < section->VirtualAddress + section->SizeOfRawData)
			return rva - section->VirtualAddress + section->PointerToRawData;
	}
	return 0;
}

static bool MakePatchedCopy(const std::wstring& dir, std::wstring& error)
{
	std::string image;
	if (!ReadFile(dir + L"echovr.exe", image)) { error = L"can't read echovr.exe"; return false; }
	size_t offset = RvaToFileOffset(image, kSignatureCheckRva);
	if (!offset || offset + sizeof(kPrologue) > image.size()) {
		error = L"echovr.exe isn't the supported build (timestamp 1683152886)";
		return false;
	}
	if (memcmp(&image[offset], kPrologue, sizeof(kPrologue))) {
		error = memcmp(&image[offset], kReturnTrue, sizeof(kReturnTrue))
			? L"echovr.exe isn't the supported build (the signature check's bytes differ)"
			: L"echovr.exe is already patched; put the original back";
		return false;
	}
	memcpy(&image[offset], kReturnTrue, sizeof(kReturnTrue));
	std::wstring target = dir + L"echovr_openxr.exe", temp = target + L".new";
	FILE* f = nullptr;
	if (_wfopen_s(&f, temp.c_str(), L"wb") || !f) { error = L"can't write echovr_openxr.exe"; return false; }
	bool ok = fwrite(image.data(), 1, image.size(), f) == image.size();
	ok = !fclose(f) && ok;
	if (!ok || !MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
		DeleteFileW(temp.c_str());
		error = L"can't write echovr_openxr.exe (is Echo running?)";
		return false;
	}
	Log(L"made echovr_openxr.exe (signature check patched at file offset 0x%zx)", offset);
	return true;
}

// SteamVR's OpenXR manifest: %LOCALAPPDATA%\openvr\openvrpaths.vrpath names SteamVR's folder.
static std::wstring SteamVRManifest()
{
	wchar_t local[MAX_PATH];
	std::string text;
	if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH) ||
	    !ReadFile(std::wstring(local) + L"\\openvr\\openvrpaths.vrpath", text))
		return L"";
	size_t key = text.find("\"runtime\"");
	size_t open = key == std::string::npos ? key : text.find('"', text.find('[', key));
	size_t close = open == std::string::npos ? open : text.find('"', open + 1);
	if (close == std::string::npos) return L"";
	std::string folder;
	for (size_t i = open + 1; i < close; i++) {
		if (text[i] == '\\' && i + 1 < close) i++;      // JSON escapes
		folder += text[i];
	}
	wchar_t wide[MAX_PATH];
	if (!MultiByteToWideChar(CP_UTF8, 0, folder.c_str(), -1, wide, MAX_PATH)) return L"";
	std::wstring manifest = std::wstring(wide) + L"\\steamxr_win64.json";
	return Exists(manifest) ? manifest : L"";
}

int wmain(int argc, wchar_t** argv)
{
	wchar_t self[MAX_PATH];
	GetModuleFileNameW(nullptr, self, MAX_PATH);
	std::wstring dir = self;
	dir.resize(dir.find_last_of(L"\\/") + 1);                 // the bin\win10 folder, with its slash
	std::wstring runtimeDir = dir + L"EchoFrame\\";
	CreateDirectoryW(runtimeDir.c_str(), nullptr);
	_wfopen_s(&g_log, (runtimeDir + L"launcher.log").c_str(), L"w");

	bool steamVR = false;
	std::wstring args;
	for (int i = 1; i < argc; i++) {
		if (!wcscmp(argv[i], L"--steamvr")) { steamVR = true; continue; }
		args += L" \"";
		args += argv[i];
		args += L"\"";
	}

	Log(L"EchoFrame launcher");
	typedef const char* (CDECL* WineVersion)();
	HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
	WineVersion wineVersion = ntdll ? (WineVersion)GetProcAddress(ntdll, "wine_get_version") : nullptr;
	if (wineVersion)
		Log(L"running under Wine %hs (OpenXR goes through Proton's wineopenxr)", wineVersion());

	if (!Exists(dir + L"echovr.exe"))
		return Fail(L"EchoFrame.exe has to be in Echo VR's bin\\win10 folder, next to echovr.exe.", 2);
	if (!Exists(runtimeDir + L"LibOVRRT64_1.dll"))
		return Fail(L"bin\\win10\\EchoFrame\\LibOVRRT64_1.dll is missing.", 2);

	// remade whenever echovr.exe is newer than the copy
	if (WriteTime(dir + L"echovr.exe") > WriteTime(dir + L"echovr_openxr.exe")) {
		std::wstring error;
		if (!MakePatchedCopy(dir, error))
			return Fail(L"Couldn't make echovr_openxr.exe: " + error + L".", 4);
	}

	if (steamVR && !wineVersion) {
		std::wstring manifest = SteamVRManifest();
		if (manifest.empty())
			Log(L"--steamvr: SteamVR not found, using the system OpenXR runtime");
		else {
			SetEnvironmentVariableW(L"XR_RUNTIME_JSON", manifest.c_str());
			Log(L"OpenXR runtime for this launch: %ls", manifest.c_str());
		}
	}

	HANDLE hmdEvent = CreateEventW(nullptr, TRUE, TRUE, L"OculusHMDConnected");
	DWORD eventError = GetLastError();
	Log(L"OculusHMDConnected event: %ls", !hmdEvent ? L"couldn't create it"
	                                     : eventError == ERROR_ALREADY_EXISTS ? L"already there (Oculus service running)" : L"created");

	// LIBOVR_DLL_DIR is where Echo's LibOVR loader and pnsovr's Platform SDK loader look first.
	// The Platform SDK loader refuses a LibOVRPlatform64_1.dll loaded from anywhere else, and
	// pnsovr's imports load it before that: with EchoFrame\ first on PATH they find the same
	// file (bin\win10 itself has none). It also keeps the Oculus app's copy out.
	SetEnvironmentVariableW(L"LIBOVR_DLL_DIR", runtimeDir.c_str());
	std::wstring path = runtimeDir;
	if (DWORD n = GetEnvironmentVariableW(L"PATH", nullptr, 0)) {
		std::wstring old(n, L'\0');
		old.resize(GetEnvironmentVariableW(L"PATH", &old[0], n));
		path += L";" + old;
	}
	SetEnvironmentVariableW(L"PATH", path.c_str());
	if (Exists(dir + L"LibOVRPlatform64_1.dll"))
		Log(L"warning: bin\\win10\\LibOVRPlatform64_1.dll would load before EchoFrame\\'s; remove it");

	std::wstring command = L"\"" + dir + L"echovr_openxr.exe\"" + args;
	Log(L"starting %ls", command.c_str());
	STARTUPINFOW startup = { sizeof(startup) };
	PROCESS_INFORMATION process = {};
	if (!CreateProcessW(nullptr, &command[0], nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &startup, &process))
		return Fail(L"Couldn't start echovr_openxr.exe (error " + std::to_wstring(GetLastError()) + L").", 3);
	CloseHandle(process.hThread);
	WaitForSingleObject(process.hProcess, INFINITE);
	DWORD code = 0;
	GetExitCodeProcess(process.hProcess, &code);
	CloseHandle(process.hProcess);
	Log(L"Echo exited with code %lu (0x%08lx)", code, code);
	if (hmdEvent) CloseHandle(hmdEvent);
	if (g_log) fclose(g_log);
	return (int)code;
}
