// EchoFramePCVR Platform SDK stand-in: LibOVRPlatform64_1.dll for Echo VR PCVR without
// the Oculus app. Ported from EchoFrame's runtime/ovrplatform_standin.c (the Quest build's
// libovrplatformloader.so stand-in), Copyright (c) 2026 heisthecat31, MIT; see
// THIRD_PARTY_NOTICES.md.
//
// Echo's OVR provider (pnsovr.dll) imports 155 Platform SDK functions and, through Meta's
// platform loader linked into it, resolves 7 more with GetProcAddress (ovr_PopMessage and
// the ovr_PlatformInitialize* family). That loader loads LibOVRPlatform64_1.dll from
// LIBOVR_DLL_DIR first and fails with "PreLoaded" (-2) if the copy already loaded through
// pnsovr's imports came from another path; EchoFrame.exe puts this DLL's folder first on
// PATH and in LIBOVR_DLL_DIR, so both are this file. Meta's own DLL needs the Oculus
// service, which isn't there on the Steam Frame.
//
// What Echo's OVR provider gets:
//   ovr_PlatformInitialize*              0 (success)
//   entitlement check                    a reply that isn't an error (an error makes Echo exit)
//   org-scoped ID, user proof            this stand-in's user; Echo logs in to the community
//                                        servers with its own config.json
//   logged-in user, access token,        an error reply ("not available"), as Meta's DLL gives
//   friends, store, presence, invites,   on a PC; Echo logs it and carries on
//   rooms, checkout
//   microphone                           real: WASAPI capture of the default input device
//   VoIP codec, P2P packets              handles that produce no data (Echo encodes and
//                                        sends voice itself; see the microphone below)
// Replies arrive through ovr_PopMessage on the next poll, as they would from Meta's.
// Logs each request type once to platform.log next to this DLL.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXPORT __declspec(dllexport)

static CRITICAL_SECTION g_logLock;
static wchar_t g_logPath[MAX_PATH];

static void Log(const char* fmt, ...)
{
	if (!g_logPath[0]) return;
	EnterCriticalSection(&g_logLock);
	FILE* f = NULL;
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
	LeaveCriticalSection(&g_logLock);
}

// logs the first call of each function
#define ONCE(name) do { static LONG seen_; if (!InterlockedExchange(&seen_, 1)) Log("%s (stand-in)", name); } while (0)

typedef uint64_t ovrRequest;
typedef uint64_t ovrID;

// The user ID and name this stand-in reports. Echo's community build doesn't log in with
// them (config.json does the login).
static const ovrID kUserID = 0x4563686f46504331ull;   // "EchoFPC1"
static const char kUserName[] = "EchoFramePCVR";
static const char kNotAvailable[] = "Meta's Platform SDK isn't available here (EchoFramePCVR stand-in)";

// ovrMessage_Entitlement_GetIsViewerEntitled: the reply type Echo's provider checks
// (pnsovr.dll compares it at RVA 0x96fa9).
#define MESSAGE_ENTITLEMENT 0x186B58B1u

// Every opaque object Echo reads (user, room, arrays, launch details, ...) is this one
// empty block: its accessors below answer with no data, the user's ID and name, or "".
static uint64_t g_empty[32];
static uint64_t g_error[4];
static char g_nonce[33];
static LONG g_initialized;

typedef struct Message {
	struct Message* next;
	uint32_t type;
	ovrRequest id;
	bool error;
	const char* text;
} Message;

static CRITICAL_SECTION g_lock;
static Message* g_head;
static Message* g_tail;
static ovrRequest g_nextId = 1;

static ovrRequest Reply(uint32_t type, bool error, const char* text)
{
	Message* m = calloc(1, sizeof(Message));
	EnterCriticalSection(&g_lock);
	ovrRequest id = g_nextId++;
	if (m) {
		m->type = type;
		m->id = id;
		m->error = error;
		m->text = text;
		if (g_tail) g_tail->next = m; else g_head = m;
		g_tail = m;
	}
	LeaveCriticalSection(&g_lock);
	return id;
}

// ---------------------------------------------------------------------------
// startup and messages
// ---------------------------------------------------------------------------
static int Initialize(const char* how, const char* appId)
{
	if (!InterlockedExchange(&g_initialized, 1)) {
		LARGE_INTEGER seed;
		QueryPerformanceCounter(&seed);
		srand((unsigned)(seed.LowPart ^ GetCurrentProcessId()));
		for (int i = 0; i < 32; i++) g_nonce[i] = "0123456789abcdef"[rand() & 15];
	}
	Log("%s(app %s): started (stand-in, no Meta services)", how, appId ? appId : "?");
	return 0;   // ovrPlatformInitialize_Success
}

EXPORT int ovr_PlatformInitializeWindows(const char* appId) { return Initialize("ovr_PlatformInitializeWindows", appId); }
EXPORT int ovr_PlatformInitializeWithAccessToken(ovrID appId, const char* token) { return Initialize("ovr_PlatformInitializeWithAccessToken", NULL); }
EXPORT int ovr_PlatformInitializeWithAccessTokenAndOptions(ovrID appId, const char* token, const void* options, size_t n)
{
	return Initialize("ovr_PlatformInitializeWithAccessTokenAndOptions", NULL);
}
EXPORT int ovr_PlatformInitializeStandaloneAccessToken(const char* token) { return Initialize("ovr_PlatformInitializeStandaloneAccessToken", NULL); }
EXPORT int ovr_Platform_InitializeStandaloneOculus(const void* params) { return Initialize("ovr_Platform_InitializeStandaloneOculus", NULL); }
// The asynchronous form answers with a request ID; nothing in Echo waits on its reply.
EXPORT ovrRequest ovr_PlatformInitializeWindowsAsynchronous(const char* appId)
{
	Initialize("ovr_PlatformInitializeWindowsAsynchronous", appId);
	return Reply(0x6DA7BA8Fu, false, NULL);   // ovrMessage_PlatformInitializeWindowsAsynchronous
}

EXPORT bool ovr_IsPlatformInitialized(void) { return g_initialized != 0; }
EXPORT ovrID ovr_GetLoggedInUserID(void) { return kUserID; }

EXPORT Message* ovr_PopMessage(void)
{
	EnterCriticalSection(&g_lock);
	Message* m = g_head;
	if (m) {
		g_head = m->next;
		if (!g_head) g_tail = NULL;
		m->next = NULL;
	}
	LeaveCriticalSection(&g_lock);
	return m;
}

EXPORT void ovr_FreeMessage(Message* m) { free(m); }
EXPORT uint32_t ovr_Message_GetType(const Message* m) { return m ? m->type : 0; }
EXPORT ovrRequest ovr_Message_GetRequestID(const Message* m) { return m ? m->id : 0; }
EXPORT bool ovr_Message_IsError(const Message* m) { return m && m->error; }
EXPORT void* ovr_Message_GetError(const Message* m) { return m && m->error ? g_error : NULL; }
EXPORT const char* ovr_Message_GetString(const Message* m) { return m && !m->error && m->text ? m->text : ""; }

EXPORT int ovr_Error_GetCode(const void* e) { return 1; }
EXPORT int ovr_Error_GetHttpCode(const void* e) { return 0; }
EXPORT const char* ovr_Error_GetMessage(const void* e) { return kNotAvailable; }

// a message's payload: the empty object, or NULL for an error reply (as Meta's SDK)
#define PAYLOAD(name) EXPORT void* name(const Message* m) { return m && !m->error ? g_empty : NULL; }
PAYLOAD(ovr_Message_GetDestinationArray)
PAYLOAD(ovr_Message_GetOrgScopedID)
PAYLOAD(ovr_Message_GetProductArray)
PAYLOAD(ovr_Message_GetPurchase)
PAYLOAD(ovr_Message_GetPurchaseArray)
PAYLOAD(ovr_Message_GetRoom)
PAYLOAD(ovr_Message_GetRoomInviteNotification)
PAYLOAD(ovr_Message_GetRoomInviteNotificationArray)
PAYLOAD(ovr_Message_GetUser)
PAYLOAD(ovr_Message_GetUserAndRoomArray)
PAYLOAD(ovr_Message_GetUserArray)
PAYLOAD(ovr_Message_GetUserProof)

// ---------------------------------------------------------------------------
// requests: each is answered on the next ovr_PopMessage, with the request's own message
// type (OVR_MessageType.h), since pnsovr routes replies by type. The answers match what
// Meta's DLL gives Echo on a PC without an Oculus-launched session ("Must call get_signature
// first" errors for the user, token, store, presence and friends requests): Echo's community
// login is built for that. Answering the access token with a success instead made Echo
// wait forever before sending its login.
// ---------------------------------------------------------------------------
#define REQUEST(name, type, text) EXPORT ovrRequest name() { ONCE(#name); return Reply(type, false, text); }
#define UNAVAILABLE(name, type) EXPORT ovrRequest name() { ONCE(#name " -> not available"); return Reply(type, true, NULL); }

REQUEST(ovr_Entitlement_GetIsViewerEntitled, MESSAGE_ENTITLEMENT, NULL)
UNAVAILABLE(ovr_User_GetLoggedInUser, 0x436F345Du)
REQUEST(ovr_User_GetOrgScopedID, 0x18F0B01Bu, NULL)
UNAVAILABLE(ovr_User_GetAccessToken, 0x06A85ABEu)
REQUEST(ovr_User_GetUserProof, 0x22810483u, NULL)
UNAVAILABLE(ovr_User_GetLoggedInUserFriends, 0x587C2A8Du)
UNAVAILABLE(ovr_User_GetLoggedInUserRecentlyMetUsersAndRooms, 0x295FBA30u)
REQUEST(ovr_User_GetNextUserArrayPage, 0x267CF743u, NULL)
REQUEST(ovr_User_GetNextUserAndRoomArrayPage, 0x7FBDD2DFu, NULL)
UNAVAILABLE(ovr_IAP_GetProductsBySKU, 0x7E9ACAF5u)
UNAVAILABLE(ovr_IAP_GetViewerPurchases, 0x3A0F8419u)
UNAVAILABLE(ovr_IAP_GetViewerPurchasesDurableCache, 0x63599E2Bu)
REQUEST(ovr_IAP_GetNextProductArrayPage, 0x1BD94AAFu, NULL)
REQUEST(ovr_IAP_GetNextPurchaseArrayPage, 0x47570A95u, NULL)
UNAVAILABLE(ovr_Notification_GetRoomInvites, 0x6F916B92u)
REQUEST(ovr_Notification_GetNextRoomInviteNotificationArrayPage, 0x0621FB77u, NULL)
REQUEST(ovr_Notification_MarkAsRead, 0x717259E3u, NULL)
UNAVAILABLE(ovr_RichPresence_Set, 0x3C147509u)
UNAVAILABLE(ovr_RichPresence_Clear, 0x57B752B3u)
UNAVAILABLE(ovr_RichPresence_GetDestinations, 0x586F2D14u)
REQUEST(ovr_RichPresence_GetNextDestinationArrayPage, 0x67367F45u, NULL)
REQUEST(ovr_Room_Leave, 0x72382475u, NULL)

UNAVAILABLE(ovr_Room_CreateAndJoinPrivate2, 0x5A3A6243u)
UNAVAILABLE(ovr_Room_Get, 0x659A8FB8u)
UNAVAILABLE(ovr_Room_GetInvitableUsers2, 0x4F53E8B0u)
UNAVAILABLE(ovr_Room_InviteUser, 0x4129EC13u)
UNAVAILABLE(ovr_Room_Join2, 0x4DAB1C42u)
UNAVAILABLE(ovr_Room_KickUser, 0x49835736u)
UNAVAILABLE(ovr_Room_LaunchInvitableUserFlow, 0x323FE273u)
UNAVAILABLE(ovr_Room_UpdateDataStore, 0x026E4028u)
UNAVAILABLE(ovr_Room_UpdateMembershipLockStatus, 0x370BB7ACu)
UNAVAILABLE(ovr_Room_UpdateOwner, 0x32B63D1Du)
UNAVAILABLE(ovr_Room_UpdatePrivateRoomJoinPolicy, 0x1141029Bu)
UNAVAILABLE(ovr_IAP_LaunchCheckoutFlow, 0x3F9B0D0Du)

// ---------------------------------------------------------------------------
// accessors on the empty object
// ---------------------------------------------------------------------------
#define EMPTY_ARRAY(T) \
	EXPORT size_t ovr_##T##_GetSize(const void* a) { return 0; } \
	EXPORT bool ovr_##T##_HasNextPage(const void* a) { return false; } \
	EXPORT void* ovr_##T##_GetElement(const void* a, size_t i) { return g_empty; }
EMPTY_ARRAY(DestinationArray)
EMPTY_ARRAY(ProductArray)
EMPTY_ARRAY(PurchaseArray)
EMPTY_ARRAY(RoomInviteNotificationArray)
EMPTY_ARRAY(UserAndRoomArray)
EMPTY_ARRAY(UserArray)

#define STRING(name) EXPORT const char* name() { return ""; }
STRING(ovr_DataStore_GetValue)
STRING(ovr_Destination_GetApiName)
STRING(ovr_Destination_GetDisplayName)
STRING(ovr_LaunchDetails_GetDeeplinkMessage)
STRING(ovr_LaunchDetails_GetDestinationApiName)
STRING(ovr_LaunchDetails_GetLaunchSource)
STRING(ovr_Product_GetDescription)
STRING(ovr_Product_GetFormattedPrice)
STRING(ovr_Product_GetName)
STRING(ovr_Product_GetSKU)
STRING(ovr_Purchase_GetSKU)
STRING(ovr_User_GetInviteToken)
STRING(ovr_User_GetPresence)
STRING(ovr_User_GetPresenceDeeplinkMessage)

#define VALUE(type, name, v) EXPORT type name() { return v; }
VALUE(ovrID, ovr_User_GetID, kUserID)
VALUE(ovrID, ovr_OrgScopedID_GetID, kUserID)
VALUE(const char*, ovr_User_GetOculusID, kUserName)
VALUE(const char*, ovr_UserProof_GetNonce, g_nonce)
VALUE(ovrID, ovr_Room_GetID, 0)
VALUE(ovrID, ovr_LaunchDetails_GetRoomID, 0)
VALUE(ovrID, ovr_RoomInviteNotification_GetID, 0)
VALUE(ovrID, ovr_RoomInviteNotification_GetRoomID, 0)
VALUE(unsigned long long, ovr_RoomInviteNotification_GetSentTime, 0)
VALUE(int, ovr_LaunchDetails_GetLaunchType, 1)   // ovrLaunchType_Normal
VALUE(int, ovr_Room_GetJoinPolicy, 0)            // ovrRoom_JoinPolicyNone
VALUE(int, ovr_User_GetPresenceStatus, 0)        // ovrUserPresenceStatus_Unknown
VALUE(bool, ovr_Room_GetIsMembershipLocked, false)
VALUE(void*, ovr_ApplicationLifecycle_GetLaunchDetails, g_empty)
VALUE(void*, ovr_Room_GetDataStore, g_empty)
VALUE(void*, ovr_Room_GetOwner, g_empty)
VALUE(void*, ovr_Room_GetUsers, g_empty)
VALUE(void*, ovr_UserAndRoom_GetUser, g_empty)

// ---------------------------------------------------------------------------
// options handles (Echo creates, fills and destroys them)
// ---------------------------------------------------------------------------
#define OPTIONS(T) \
	EXPORT void* ovr_##T##_Create(void) { return calloc(1, 16); } \
	EXPORT void ovr_##T##_Destroy(void* h) { free(h); }
OPTIONS(RichPresenceOptions)
OPTIONS(RoomOptions)

#define NOOP(name) EXPORT void name() {}
NOOP(ovr_RoomOptions_SetOrdering)
NOOP(ovr_RichPresenceOptions_SetApiName)
NOOP(ovr_RichPresenceOptions_SetCurrentCapacity)
NOOP(ovr_RichPresenceOptions_SetDeeplinkMessageOverride)
NOOP(ovr_RichPresenceOptions_SetEndTime)
NOOP(ovr_RichPresenceOptions_SetExtraContext)
NOOP(ovr_RichPresenceOptions_SetInstanceId)
NOOP(ovr_RichPresenceOptions_SetIsJoinable)
NOOP(ovr_RichPresenceOptions_SetMaxCapacity)
NOOP(ovr_RichPresenceOptions_SetStartTime)

// ---------------------------------------------------------------------------
// microphone: WASAPI capture of the default input device
// ---------------------------------------------------------------------------
// Echo's OVR provider reads 16-bit mono PCM at 48 kHz from ovr_Microphone_GetPCM and encodes
// and sends it itself (as on Quest, where EchoFrame found incoming voice never touches the
// Platform SDK). A capture thread converts whatever the device gives (shared mode, the
// engine's mix format, usually 32-bit float stereo at 48 kHz) to that, into a 250 ms ring
// buffer: when Echo falls behind, the oldest samples go. The level is logged every 5 s.
// (the SDK declares these IIDs without defining them for C)
static const GUID kCLSID_MMDeviceEnumerator = { 0xbcde0395, 0xe52f, 0x467c, { 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e } };
static const GUID kIID_IMMDeviceEnumerator = { 0xa95664d2, 0x9614, 0x4f35, { 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 } };
static const GUID kIID_IAudioClient = { 0x1cb9ad4c, 0xdbfa, 0x4c32, { 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 } };
static const GUID kIID_IAudioCaptureClient = { 0xc8adbd64, 0xe71e, 0x48a0, { 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 } };
static const GUID kSUBTYPE_IEEE_FLOAT = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

#define MIC_RATE 48000
#define MIC_RING (MIC_RATE / 4)

typedef struct {
	CRITICAL_SECTION lock;
	HANDLE thread, stop;
	int16_t ring[MIC_RING];
	size_t head, count;
	uint64_t captured, lastLog, dropped;
	int peak;
	double sumSq;
	double phase;          // resampling position, when the device isn't 48 kHz
} Mic;

static void MicPush(Mic* m, float v)
{
	if (v > 1.0f) v = 1.0f;
	if (v < -1.0f) v = -1.0f;
	int16_t s = (int16_t)(v * 32767.0f);
	int a = s < 0 ? -s : s;
	if (a > m->peak) m->peak = a;
	m->sumSq += (double)s * s;
	m->ring[(m->head + m->count) % MIC_RING] = s;
	if (m->count < MIC_RING) m->count++;
	else { m->head = (m->head + 1) % MIC_RING; m->dropped++; }
	m->captured++;
}

static DWORD WINAPI MicThread(LPVOID param)
{
	Mic* m = param;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	IMMDeviceEnumerator* enumerator = NULL;
	IMMDevice* device = NULL;
	IAudioClient* client = NULL;
	IAudioCaptureClient* capture = NULL;
	WAVEFORMATEX* format = NULL;
	HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &kIID_IMMDeviceEnumerator, (void**)&enumerator);
	if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, eCapture, eCommunications, &device);
	if (SUCCEEDED(hr)) hr = IMMDevice_Activate(device, &kIID_IAudioClient, CLSCTX_ALL, NULL, (void**)&client);
	if (SUCCEEDED(hr)) hr = IAudioClient_GetMixFormat(client, &format);
	if (SUCCEEDED(hr)) hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, 0, 200 * 10000, 0, format, NULL);
	if (SUCCEEDED(hr)) hr = IAudioClient_GetService(client, &kIID_IAudioCaptureClient, (void**)&capture);
	if (SUCCEEDED(hr)) hr = IAudioClient_Start(client);
	if (FAILED(hr)) {
		Log("microphone didn't start (0x%08lx)", hr);
		goto done;
	}

	bool isFloat = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
		(format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->wBitsPerSample == 32 &&
		 !memcmp(&((WAVEFORMATEXTENSIBLE*)format)->SubFormat, &kSUBTYPE_IEEE_FLOAT, sizeof(GUID)));
	const int channels = format->nChannels, bits = format->wBitsPerSample;
	const double step = (double)format->nSamplesPerSec / MIC_RATE;
	Log("microphone started: %lu Hz, %d channel(s), %d-bit %s", format->nSamplesPerSec, channels, bits, isFloat ? "float" : "PCM");

	while (WaitForSingleObject(m->stop, 10) == WAIT_TIMEOUT) {
		UINT32 frames;
		while (SUCCEEDED(IAudioCaptureClient_GetNextPacketSize(capture, &frames)) && frames) {
			BYTE* data;
			DWORD flags;
			if (FAILED(IAudioCaptureClient_GetBuffer(capture, &data, &frames, &flags, NULL, NULL))) break;
			EnterCriticalSection(&m->lock);
			for (UINT32 i = 0; i < frames; i++) {
				float v = 0;   // the first channel
				if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
					if (isFloat) v = ((const float*)data)[i * channels];
					else if (bits == 16) v = ((const int16_t*)data)[i * channels] / 32768.0f;
					else if (bits == 32) v = ((const int32_t*)data)[i * channels] / 2147483648.0f;
				}
				// nearest-sample rate conversion to 48 kHz (devices are 48 kHz almost always)
				for (m->phase += 1.0; m->phase >= step; m->phase -= step) MicPush(m, v);
			}
			LeaveCriticalSection(&m->lock);
			IAudioCaptureClient_ReleaseBuffer(capture, frames);
		}
		if (m->captured - m->lastLog >= 5 * MIC_RATE) {
			double n = (double)(m->captured - m->lastLog);
			Log("microphone level: peak %.1f dBFS, RMS %.1f dBFS; samples Echo didn't read in time: %llu of %.0f",
				20.0 * log10((m->peak > 0 ? m->peak : 1) / 32768.0), 10.0 * log10((m->sumSq > 0 ? m->sumSq : 1) / n / (32768.0 * 32768.0)),
				(unsigned long long)m->dropped, n);
			m->lastLog = m->captured;
			m->peak = 0;
			m->sumSq = 0;
			m->dropped = 0;
		}
	}
	IAudioClient_Stop(client);
	Log("microphone stopped after %llu samples", (unsigned long long)m->captured);
done:
	if (format) CoTaskMemFree(format);
	if (capture) IAudioCaptureClient_Release(capture);
	if (client) IAudioClient_Release(client);
	if (device) IMMDevice_Release(device);
	if (enumerator) IMMDeviceEnumerator_Release(enumerator);
	CoUninitialize();
	return 0;
}

EXPORT void* ovr_Microphone_Create(void)
{
	Mic* m = calloc(1, sizeof(Mic));
	if (m) InitializeCriticalSection(&m->lock);
	Log("ovr_Microphone_Create (stand-in, WASAPI capture)");
	return m;
}

EXPORT void ovr_Microphone_Stop(void* h)
{
	Mic* m = h;
	if (!m || !m->thread) return;
	SetEvent(m->stop);
	WaitForSingleObject(m->thread, 2000);
	CloseHandle(m->thread);
	CloseHandle(m->stop);
	m->thread = m->stop = NULL;
}

EXPORT void ovr_Microphone_Start(void* h)
{
	Mic* m = h;
	if (!m || m->thread) return;
	m->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	m->thread = CreateThread(NULL, 0, MicThread, m, 0, NULL);
}

EXPORT void ovr_Microphone_Destroy(void* h)
{
	Mic* m = h;
	if (!m) return;
	ovr_Microphone_Stop(m);
	DeleteCriticalSection(&m->lock);
	free(m);
}

EXPORT size_t ovr_Microphone_GetPCM(void* h, int16_t* out, size_t n)
{
	Mic* m = h;
	if (!m || !out) return 0;
	EnterCriticalSection(&m->lock);
	size_t got = n < m->count ? n : m->count;
	for (size_t i = 0; i < got; i++) out[i] = m->ring[(m->head + i) % MIC_RING];
	m->head = (m->head + got) % MIC_RING;
	m->count -= got;
	LeaveCriticalSection(&m->lock);
	return got;
}

// ---------------------------------------------------------------------------
// VoIP and peer-to-peer packets: handles that produce no data
// ---------------------------------------------------------------------------
EXPORT void* ovr_Voip_CreateEncoder(void) { ONCE("ovr_Voip_CreateEncoder"); return calloc(1, 16); }
EXPORT void* ovr_Voip_CreateDecoder(void) { return calloc(1, 16); }
EXPORT void ovr_Voip_DestroyEncoder(void* h) { free(h); }
EXPORT void ovr_Voip_DestroyDecoder(void* h) { free(h); }
EXPORT size_t ovr_Voip_GetOutputBufferMaxSize(void) { return 4800; }   // 100 ms at 48 kHz
EXPORT size_t ovr_Voip_GetPCM(ovrID sender, int16_t* out, size_t n) { return 0; }
EXPORT size_t ovr_Voip_GetPCMSize(ovrID sender) { return 0; }
EXPORT size_t ovr_VoipEncoder_GetCompressedData(void* h, uint8_t* out, size_t n) { return 0; }
EXPORT size_t ovr_VoipDecoder_GetDecodedPCM(void* h, float* out, size_t n) { return 0; }
NOOP(ovr_VoipEncoder_AddPCM)
NOOP(ovr_VoipDecoder_Decode)
NOOP(ovr_Voip_Accept)
NOOP(ovr_Voip_Start)
NOOP(ovr_Voip_Stop)
NOOP(ovr_Voip_SetMicrophoneMuted)

EXPORT void* ovr_Net_ReadPacket(void) { return NULL; }
EXPORT bool ovr_Net_SendPacket() { return false; }
EXPORT bool ovr_Net_SendPacketToCurrentRoom() { return false; }
EXPORT bool ovr_Net_AcceptForCurrentRoom(void) { return false; }
NOOP(ovr_Net_CloseForCurrentRoom)
NOOP(ovr_Packet_Free)
VALUE(const void*, ovr_Packet_GetBytes, NULL)
VALUE(size_t, ovr_Packet_GetSize, 0)
VALUE(ovrID, ovr_Packet_GetSenderID, 0)

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
EXPORT bool ovrID_FromString(ovrID* out, const char* s)
{
	if (!out || !s || !*s) return false;
	char* end;
	*out = _strtoui64(s, &end, 10);
	return *end == 0;
}

typedef struct {   // OVR_KeyValuePair.h
	const char* key_;
	int valueType_;
	const char* stringValue_;
	int intValue_;
	double doubleValue_;
} ovrKeyValuePair;

EXPORT ovrKeyValuePair ovrKeyValuePair_makeString(const char* key, const char* value)
{
	ovrKeyValuePair kv = { key, 0 /* ovrKeyValuePairType_String */, value, 0, 0 };
	return kv;
}

EXPORT const char* ovrPlatformInitializeResult_ToString(int v) { return v == 0 ? "SUCCESS" : "UNKNOWN"; }
EXPORT const char* ovrLaunchType_ToString(int v)
{
	static const char* names[] = { "UNKNOWN", "NORMAL", "INVITE", "COORDINATED", "DEEPLINK" };
	return v >= 0 && v < 5 ? names[v] : "UNKNOWN";
}
EXPORT const char* ovrRoomJoinPolicy_ToString(int v)
{
	static const char* names[] = { "NONE", "EVERYONE", "FRIENDS_OF_MEMBERS", "FRIENDS_OF_OWNER", "INVITED_USERS" };
	return v >= 0 && v < 5 ? names[v] : "UNKNOWN";
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved)
{
	if (reason == DLL_PROCESS_ATTACH) {
		InitializeCriticalSection(&g_logLock);
		InitializeCriticalSection(&g_lock);
		GetModuleFileNameW(module, g_logPath, MAX_PATH);
		wchar_t* slash = wcsrchr(g_logPath, L'\\');
		if (slash) wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - g_logPath), L"platform.log");
		FILE* f = NULL;
		if (!_wfopen_s(&f, g_logPath, L"w") && f) fclose(f);
		Log("EchoFramePCVR Platform SDK stand-in loaded");
	}
	return TRUE;
}
