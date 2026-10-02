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

// Community patches to Echo's DLLs add code in the padding after an executable section's
// declared size (pnsovr.dll's org-scoped ID at RVA 0x1f9500, past .text's VirtualSize of
// 0x1f84e4). Windows maps the whole page executable and runs it; under Wine with FEX only
// the declared size is code, and jumping there is an access violation. This widens each
// executable section's VirtualSize to its raw data, when that ends before the next section.
// Returns how many sections changed; the file is rewritten only then (backup: <name>.efp-orig).
static int WidenExecutableSections(const std::wstring& path, bool backup)
{
	std::string image;
	if (!ReadFile(path, image) || image.size() < 0x40 || image[0] != 'M' || image[1] != 'Z') return 0;
	DWORD ntOffset = *(const DWORD*)&image[0x3C];
	if (ntOffset + sizeof(IMAGE_NT_HEADERS64) > image.size()) return 0;
	IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)&image[ntOffset];
	if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
	IMAGE_SECTION_HEADER* sections = IMAGE_FIRST_SECTION(nt);
	WORD count = nt->FileHeader.NumberOfSections;
	if ((char*)(sections + count) > image.data() + image.size()) return 0;
	int changed = 0;
	for (WORD i = 0; i < count; i++) {
		IMAGE_SECTION_HEADER& s = sections[i];
		if (!(s.Characteristics & IMAGE_SCN_MEM_EXECUTE) || s.SizeOfRawData <= s.Misc.VirtualSize) continue;
		DWORD next = i + 1 < count ? sections[i + 1].VirtualAddress : nt->OptionalHeader.SizeOfImage;
		if (s.VirtualAddress + s.SizeOfRawData > next) continue;
		Log(L"%ls: section %.8hs size 0x%lx -> 0x%lx (code past its declared end)", path.c_str() + path.find_last_of(L'\\') + 1,
		    (const char*)s.Name, s.Misc.VirtualSize, s.SizeOfRawData);
		s.Misc.VirtualSize = s.SizeOfRawData;
		changed++;
	}
	if (!changed) return 0;
	if (backup && !Exists(path + L".efp-orig") && !CopyFileW(path.c_str(), (path + L".efp-orig").c_str(), TRUE)) return 0;
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") || !f) {
		Log(L"couldn't rewrite %ls", path.c_str());
		return 0;
	}
	fwrite(image.data(), 1, image.size(), f);
	fclose(f);
	return changed;
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

	if (wineVersion) {
		WidenExecutableSections(dir + L"echovr_openxr.exe", false);
		for (const wchar_t* dll : { L"pnsovr.dll", L"pnsrad.dll", L"pnsradmatchmaking.dll", L"pnsradgameserver.dll", L"pnsdemo.dll" })
			if (Exists(dir + dll)) WidenExecutableSections(dir + dll, true);
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
	std::wstring platformDir = runtimeDir;
	if (wineVersion) {
		// The Platform SDK loader ignores LIBOVR_DLL_DIR in an elevated process, and Wine's
		// processes are. Its other folder is <Oculus Base>\Support\oculus-runtime\ from the
		// registry: in this Wine prefix that becomes EchoFrame\, with the stand-in copied there.
		platformDir = runtimeDir + L"Support\\oculus-runtime\\";
		CreateDirectoryW((runtimeDir + L"Support").c_str(), nullptr);
		CreateDirectoryW(platformDir.c_str(), nullptr);
		if (!CopyFileW((runtimeDir + L"LibOVRPlatform64_1.dll").c_str(), (platformDir + L"LibOVRPlatform64_1.dll").c_str(), FALSE))
			Log(L"warning: couldn't copy the Platform SDK stand-in to %ls (error %lu)", platformDir.c_str(), GetLastError());
		// pnsovr opens the key in the 32-bit view (KEY_WOW64_32KEY); both views get it
		LSTATUS rc = ERROR_SUCCESS;
		for (REGSAM view : { KEY_WOW64_32KEY, KEY_WOW64_64KEY }) {
			HKEY key;
			LSTATUS r = RegCreateKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oculus VR, LLC\\Oculus", 0, nullptr, 0,
			                            KEY_SET_VALUE | view, nullptr, &key, nullptr);
			if (r == ERROR_SUCCESS) {
				r = RegSetValueExW(key, L"Base", 0, REG_SZ, (const BYTE*)runtimeDir.c_str(), (DWORD)((runtimeDir.size() + 1) * sizeof(wchar_t)));
				RegCloseKey(key);
			}
			if (r != ERROR_SUCCESS) rc = r;
		}
		Log(rc == ERROR_SUCCESS ? L"Oculus Base (this Wine prefix): %ls" : L"warning: couldn't set Oculus Base to %ls", runtimeDir.c_str());
	}
	std::wstring path = platformDir;
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
