// DS4Native.cpp - DS4-Native, a tray app that turns DualShock 4 controllers into Xbox (XInput) pads.
//
// Each DualShock 4 (USB, Bluetooth or the Sony wireless adapter) gets a worker thread that
// reads its HID reports, feeds a virtual Xbox 360 controller on the ViGEmBus driver, and
// sends rumble and the light bar colour back. The real controller is hidden from games with
// the HidHide driver, so they only see the Xbox pad (no double input).
//
// The window (double-click the tray icon) has a tab per connected controller, a 3D view of it
// in the middle that follows the controller's motion sensors, and that controller's settings
// either side. Settings are kept per controller (by its Bluetooth address) in
// HKCU\Software\DS4Native; button-combo shortcuts are shared by all controllers.
//
// Razer Chroma: the light bar can follow Razer Synapse through Chroma Connect (the Chroma
// Broadcast API). Razer's DLL runs in a helper copy of this exe ("/chroma"), because a call it
// doesn't like can hang the process it's in.
//
// Build: build.bat. Needs the ViGEmBus driver; HidHide is optional (for hiding).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winioctl.h>
#include <shellapi.h>
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <commctrl.h>
#include <bluetoothapis.h>
#include <setupapi.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <dbt.h>
#include <tlhelp32.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <atomic>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>
#include <ViGEm/Client.h>
#include "SpoutDX.h"                        // Spout2 (the stream overlay)

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "bthprops.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
                        "version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static const wchar_t kAppName[] = L"DS4-Native";
static const wchar_t kVersion[] = L"1.2.0";
static const wchar_t kRegKey[]  = L"Software\\DS4Native";
static const wchar_t kRunKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const UINT WM_TRAY     = WM_APP + 1;
static const UINT WM_PADS     = WM_APP + 2;     // a controller came, went or changed (from its thread)
static const UINT WM_SHORTCUT = WM_APP + 3;     // a combo fired: wParam action, lParam buttons
static const UINT WM_CHROMA   = WM_APP + 4;     // from the Chroma helper: wParam what, lParam value
static const UINT WM_VOLUME   = WM_APP + 5;     // touchpad volume changed: wParam %, lParam muted
enum TrayMenuId { ID_MENU_OPEN = 400, ID_MENU_ENABLED, ID_MENU_EXIT };
static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------------------
// Settings: one int per setting, per controller

enum Setting {
    S_EMULATE, S_HIDE, S_RUMBLE, S_TOUCHBACK, S_PSGUIDE, S_IDLE, S_MOTION,    // switches, in card order
    S_LIGHTMODE, S_HUE, S_BRIGHT, S_DEADL, S_DEADR, S_DEADT, S_RUMBLEGAIN, S_RAINBOW,
    S_TPMOUSE, S_TPSPLIT, S_TPTWO, S_TPSCROLL, S_TPSPEED,       // the touchpad as a mouse
    S_AUDIO, S_AUDIOOUT, S_AUDIOVOL,                            // sound to the controller (Bluetooth)
    S_TPTAP, S_TPVOLUME, S_MIC,
    S_COUNT
};
enum LightMode { LIGHT_BATTERY, LIGHT_COLOUR, LIGHT_RAINBOW, LIGHT_CHROMA, LIGHT_OFF, LIGHT_COUNT };
enum TwoFinger { TWO_RIGHT, TWO_MIDDLE, TWO_COUNT };
enum AudioOut { AUDIO_JACK, AUDIO_SPEAKER };
static const wchar_t* const kSettingNames[S_COUNT] = {
    L"Emulate", L"Hide", L"Rumble", L"TouchpadBack", L"PsGuide", L"IdleSleep", L"Motion",
    L"Light", L"Hue", L"Brightness", L"DeadLeft", L"DeadRight", L"DeadTrigger", L"RumbleGain", L"RainbowSpeed",
    L"TouchMouse", L"TouchSplit", L"TouchTwoFinger", L"TouchScroll", L"TouchSpeed",
    L"Audio", L"AudioOut", L"AudioVolume", L"TouchTap", L"TouchVolume", L"Mic",
};
static const int kSettingDefaults[S_COUNT] = { 1, 1, 1, 1, 1, 0, 1, LIGHT_BATTERY, 210, 40, 8, 8, 0, 100, 25,
                                               0, 1, TWO_RIGHT, 1, 50, 0, AUDIO_JACK, 70, 0, 0, 0 };
static const wchar_t* const kTwoFingerNames[TWO_COUNT] = { L"Right click", L"Middle click" };

// Switch settings: title and detail.
static const struct { int setting; const wchar_t* title; const wchar_t* detail; } kToggleText[] = {
    { S_EMULATE,   L"Xbox controller",   L"Games see an Xbox pad" },
    { S_HIDE,      L"Hide DualShock",    L"No double input (HidHide)" },
    { S_RUMBLE,    L"Rumble",            L"Pass vibration through" },
    { S_TOUCHBACK, L"Touchpad click",    L"Presses Back / View" },
    { S_PSGUIDE,   L"PS button",         L"Presses the Guide button" },
    { S_IDLE,      L"Sleep when idle",   L"Bluetooth off after 10 min" },
    { S_MOTION,    L"3D motion",         L"The preview follows the pad" },
    { S_TPMOUSE,   L"Touchpad as mouse", L"Slide to move the cursor" },
    { S_TPSPLIT,   L"Left and right click", L"Click the left or right half" },
    { S_TPSCROLL,  L"Two-finger scroll", L"Drag two fingers to scroll" },
    { S_AUDIO,     L"Sound to the controller", L"Plays a sound device on it" },
    { S_TPTAP,     L"Tap to click",      L"A quick tap clicks too" },
    { S_TPVOLUME,  L"Touchpad volume",   L"Slide to change, click to mute" },
    { S_MIC,       L"Headset microphone", L"To a recording device" },
};
static const int kEmulationToggles[] = { S_EMULATE, S_HIDE, S_RUMBLE, S_IDLE, S_MOTION };   // (Rumble strength under Rumble)
static const int kEmulationCount = (int)ARRAYSIZE(kEmulationToggles);
static const ULONGLONG kIdleMs = 10 * 60 * 1000;

static std::atomic<bool> g_enabled{true};   // the master switch
static bool g_testMic;                      // self-test (DS4NATIVE_MICDRY): mic on, decoded and counted, played nowhere
static bool g_noInput;                      // self-test: never send input (mouse, keys, actions, virtual pad)
static bool g_testSpout;                    // self-test (DS4NATIVE_SPOUT): the stream overlay's sender on
static SRWLOCK g_dumpLock = SRWLOCK_INIT;   // ... and the first reports of each kind, in hex
static std::string g_dump;
static int g_dumpCount[256];
enum ThemeMode { THEME_AUTO, THEME_LIGHT, THEME_DARK };
static int  g_themeMode = THEME_AUTO;
static bool g_onTop;
static bool g_spoutOn;                      // the stream overlay (Spout2) is on

static DWORD RegGet(const wchar_t* sub, const wchar_t* name, DWORD def) {
    std::wstring key = kRegKey;
    if (sub && *sub) key = key + L"\\" + sub;
    DWORD v = def, size = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_REG_DWORD, nullptr, &v, &size);
    return v;
}

static void RegPut(const wchar_t* sub, const wchar_t* name, DWORD v) {
    std::wstring key = kRegKey;
    if (sub && *sub) key = key + L"\\" + sub;
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_DWORD, &v, sizeof(v));
}

static void LoadGlobals() {
    g_enabled = true;                       // (no switch for it in the window any more: "Xbox controller" does it)
    g_themeMode = (int)min(RegGet(nullptr, L"Theme", THEME_AUTO), (DWORD)THEME_DARK);
    g_onTop = RegGet(nullptr, L"OnTop", 0) != 0;
    g_spoutOn = RegGet(nullptr, L"StreamOverlay", 0) != 0;
}

static void ApplySpout();
static void SaveGlobals() {
    RegPut(nullptr, L"Enabled", g_enabled);
    RegPut(nullptr, L"Theme", g_themeMode);
    RegPut(nullptr, L"OnTop", g_onTop);
    RegPut(nullptr, L"StreamOverlay", g_spoutOn);
}

static std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

static bool StartsWithWindows() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

static void SetStartsWithWindows(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" /tray";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

// The app was called PadNative before: its settings (per controller, shortcuts, Chroma App ID)
// are copied over once, and "Start with Windows" moves to the new name.
static void MigrateOldSettings() {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKey, 0, KEY_READ, &key) == ERROR_SUCCESS) { RegCloseKey(key); return; }
    HKEY old;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\PadNative", 0, KEY_READ, &old) != ERROR_SUCCESS) return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) == ERROR_SUCCESS) {
        RegCopyTreeW(old, nullptr, key);
        RegCloseKey(key);
    }
    RegCloseKey(old);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"PadNative", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS) {
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, L"PadNative");
        SetStartsWithWindows(true);
    }
}

static bool ProcessRunning(const wchar_t* exe) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{ sizeof(pe) };
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        found = !_wcsicmp(pe.szExeFile, exe);
    }
    CloseHandle(snap);
    return found;
}

// ---------------------------------------------------------------------------------------
// HidHide: hides the real controller from every app that isn't on its whitelist (us).
// Lists are multi-strings exchanged through \\.\HidHide; only one handle may be open at once.

namespace hidhide {

static const DWORD kGetWhitelist = 0x80016000, kSetWhitelist = 0x80016004, kGetBlacklist = 0x80016008,
                   kSetBlacklist = 0x8001600C, kGetActive = 0x80016010, kSetActive = 0x80016014;
typedef std::vector<std::wstring> List;

static HANDLE Open() {
    return CreateFileW(L"\\\\.\\HidHide", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
}

static bool Installed() {
    HANDLE h = Open();
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
    return GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND;   // busy counts
}

static bool GetList(HANDLE h, DWORD code, List& out) {
    DWORD needed = 0, bytes = 0;            // the driver wants a buffer of exactly the list's size
    if (!DeviceIoControl(h, code, nullptr, 0, nullptr, 0, &needed, nullptr)) return false;
    std::vector<wchar_t> buf(needed / sizeof(wchar_t) + 2);
    if (needed && !DeviceIoControl(h, code, nullptr, 0, buf.data(), needed, &bytes, nullptr)) return false;
    out.clear();
    size_t count = bytes / sizeof(wchar_t);
    for (size_t i = 0; i < count && buf[i];) {
        std::wstring s(&buf[i]);
        i += s.size() + 1;
        out.push_back(s);
    }
    return true;
}

static bool SetList(HANDLE h, DWORD code, const List& list) {
    std::vector<wchar_t> buf;
    for (const std::wstring& s : list) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0);
    }
    buf.push_back(0);
    DWORD bytes = 0;
    return DeviceIoControl(h, code, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)), nullptr, 0, &bytes, nullptr) != 0;
}

static bool Contains(const List& list, const std::wstring& s) {
    for (const std::wstring& x : list) if (!_wcsicmp(x.c_str(), s.c_str())) return true;
    return false;
}

static void Remove(List& list, const std::wstring& s) {
    list.erase(std::remove_if(list.begin(), list.end(), [&](const std::wstring& x) { return !_wcsicmp(x.c_str(), s.c_str()); }),
               list.end());
}

// The whitelist holds image paths in device form (\Device\HarddiskVolume4\...).
static std::wstring DevicePath(const std::wstring& dosPath) {
    if (dosPath.size() < 3 || dosPath[1] != L':') return dosPath;
    wchar_t drive[3] = { dosPath[0], L':', 0 }, target[512];
    if (!QueryDosDeviceW(drive, target, 512)) return dosPath;
    return std::wstring(target) + dosPath.substr(2);
}

static bool g_ready;        // we're on the whitelist

static bool WhitelistSelf() {
    HANDLE h = Open();
    if (h == INVALID_HANDLE_VALUE) return false;
    std::wstring me = DevicePath(ExePath());
    List list;
    bool ok = GetList(h, kGetWhitelist, list);
    if (ok && !Contains(list, me)) {
        list.push_back(me);
        ok = SetList(h, kSetWhitelist, list);
    }
    CloseHandle(h);
    return g_ready = ok;
}

// The device instances we put on the blacklist (kept in the registry so a crash can't leave
// a controller hidden for good; entries the user added themselves are never touched).
static std::set<std::wstring> g_ours;

static void LoadOurs() {
    wchar_t buf[8192];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Hidden", RRF_RT_REG_MULTI_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return;
    for (const wchar_t* s = buf; *s; s += wcslen(s) + 1) g_ours.insert(s);
}

static void SaveOurs() {
    std::vector<wchar_t> buf;
    for (const std::wstring& s : g_ours) {
        buf.insert(buf.end(), s.begin(), s.end());
        buf.push_back(0);
    }
    buf.push_back(0);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"Hidden", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

// Makes the blacklist hold exactly `want` out of the entries we manage.
static bool Sync(const std::set<std::wstring>& want) {
    static std::set<std::wstring> last;
    static bool synced;
    if (synced && want == last) return true;
    if (want.empty() && g_ours.empty()) { last = want; synced = true; return true; }
    HANDLE h = Open();
    if (h == INVALID_HANDLE_VALUE) return false;
    List list;
    bool ok = GetList(h, kGetBlacklist, list), changed = false;
    if (ok) {
        for (auto it = g_ours.begin(); it != g_ours.end();) {
            if (want.count(*it)) { ++it; continue; }
            if (Contains(list, *it)) { Remove(list, *it); changed = true; }
            it = g_ours.erase(it);
        }
        for (const std::wstring& id : want) {
            if (Contains(list, id)) continue;
            list.push_back(id);
            g_ours.insert(id);
            changed = true;
        }
        if (changed) ok = SetList(h, kSetBlacklist, list);
        // Hiding only works while HidHide is active: switch it on, and back off once nothing
        // of ours is hidden any more if it was us who switched it on.
        BOOLEAN active = FALSE;
        DWORD bytes = 0;
        bool known = DeviceIoControl(h, kGetActive, nullptr, 0, &active, 1, &bytes, nullptr) != 0;
        bool weActivated = RegGet(nullptr, L"Activated", 0) != 0;
        if (known && !want.empty() && !active) {
            active = TRUE;
            if (DeviceIoControl(h, kSetActive, &active, 1, nullptr, 0, &bytes, nullptr)) RegPut(nullptr, L"Activated", 1);
        } else if (known && want.empty() && weActivated) {
            active = FALSE;
            DeviceIoControl(h, kSetActive, &active, 1, nullptr, 0, &bytes, nullptr);
            RegPut(nullptr, L"Activated", 0);
        }
    }
    CloseHandle(h);
    SaveOurs();
    if (ok) { last = want; synced = true; }
    return ok;
}

}   // namespace hidhide

// ---------------------------------------------------------------------------------------
// Razer Chroma (Chroma Connect / Broadcast): the colour Synapse is showing, from the helper.

enum ChromaState { CHROMA_OFF, CHROMA_MISSING, CHROMA_STARTING, CHROMA_FAILED, CHROMA_WAITING, CHROMA_LIVE, CHROMA_HUNG };
enum ChromaMsg { CHROMA_MSG_INIT = 1, CHROMA_MSG_COLOUR, CHROMA_MSG_STATUS };
static std::atomic<int>   g_chromaState{CHROMA_OFF};
static std::atomic<long>  g_chromaError{0};
static std::atomic<DWORD> g_chromaColour{0};       // 0x00BBGGRR
static HANDLE    g_chromaProcess;
static ULONGLONG g_chromaStart;
static const long kRzResourceDisabled = 4309, kRzAccessDenied = 5, kRzUnknownAppId = 0x80070000 | 1168;

// Razer's sample/test App ID. Real use needs an App ID from Razer (registry value ChromaAppId),
// or Chroma Connect's developer mode for this test ID.
static const GUID kDefaultChromaAppId = { 0x4c24ad2b, 0xd0a6, 0x409f, { 0xb8, 0x95, 0xec, 0x76, 0x32, 0xf3, 0xbe, 0xe6 } };

static std::wstring ChromaDllPath() {
    wchar_t dir[MAX_PATH];
    GetSystemDirectoryW(dir, MAX_PATH);
    return std::wstring(dir) + L"\\RzChromaBroadcastAPI64.dll";
}

// ---- the helper process: DS4Native.exe /chroma <parent pid> <tray hwnd>

static HWND g_chromaTarget;

static int StatusValue(void* data) {
    INT_PTR v = (INT_PTR)data;
    if (v == 1 || v == 2 || !data) return (int)v;
    __try { return *(int*)data; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static LONG OnChromaBroadcast(BYTE type, void* data) {
    if (type == 1 && data) {            // BROADCAST_EFFECT: five colours, CL1 first
        PostMessageW(g_chromaTarget, WM_CHROMA, CHROMA_MSG_COLOUR, *(const DWORD*)data & 0xFFFFFF);
    } else if (type == 2) {             // BROADCAST_STATUS: 1 live, 2 not live
        PostMessageW(g_chromaTarget, WM_CHROMA, CHROMA_MSG_STATUS, StatusValue(data));
    }
    return 0;
}

static int ChromaHelperMain(const wchar_t* args) {
    DWORD parentPid = 0;
    unsigned long long hwnd = 0;
    if (swscanf_s(args, L"%lu %llu", &parentPid, &hwnd) != 2) return 1;
    g_chromaTarget = (HWND)(ULONG_PTR)hwnd;
    HANDLE parent = OpenProcess(SYNCHRONIZE, FALSE, parentPid);
    if (!parent) return 1;
    HMODULE dll = LoadLibraryW(ChromaDllPath().c_str());
    typedef LONG (*InitFn)(GUID);
    typedef LONG (*CallbackFn)(BYTE, void*);
    typedef LONG (*RegisterFn)(CallbackFn);
    InitFn init = dll ? (InitFn)GetProcAddress(dll, "Init") : nullptr;
    RegisterFn reg = dll ? (RegisterFn)GetProcAddress(dll, "RegisterEventNotification") : nullptr;
    if (!init || !reg) {
        PostMessageW(g_chromaTarget, WM_CHROMA, CHROMA_MSG_INIT, -1);
        TerminateProcess(GetCurrentProcess(), 2);
    }
    GUID id = kDefaultChromaAppId;
    wchar_t text[64];
    DWORD size = sizeof(text);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"ChromaAppId", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS) {
        GUID custom;
        if (SUCCEEDED(CLSIDFromString(text, &custom))) id = custom;
    }
    LONG result = init(id);
    if (result == 0) result = reg(OnChromaBroadcast);   // never after a failed Init: Razer's DLL hangs
    PostMessageW(g_chromaTarget, WM_CHROMA, CHROMA_MSG_INIT, result);
    if (result == 0) WaitForSingleObject(parent, INFINITE);
    TerminateProcess(GetCurrentProcess(), 0);           // ExitProcess can hang in Razer's DLL
    return 0;
}

// ---- the main process side

static void StopChroma() {
    if (g_chromaProcess) {
        TerminateProcess(g_chromaProcess, 0);
        CloseHandle(g_chromaProcess);
        g_chromaProcess = nullptr;
    }
}

static void StartChroma(HWND tray) {
    if (g_chromaState != CHROMA_OFF) return;
    if (GetFileAttributesW(ChromaDllPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        g_chromaState = CHROMA_MISSING;
        return;
    }
    wchar_t cmd[MAX_PATH + 96];
    swprintf_s(cmd, L"\"%s\" /chroma %lu %llu", ExePath().c_str(), GetCurrentProcessId(), (unsigned long long)(ULONG_PTR)tray);
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_chromaState = CHROMA_FAILED;
        g_chromaError = (long)GetLastError();
        return;
    }
    CloseHandle(pi.hThread);
    g_chromaProcess = pi.hProcess;
    g_chromaStart = GetTickCount64();
    g_chromaState = CHROMA_STARTING;
}

static const wchar_t* ChromaText() {
    switch (g_chromaState) {
    case CHROMA_MISSING: return L"Razer Synapse not installed";
    case CHROMA_FAILED:  return g_chromaError == kRzResourceDisabled || g_chromaError == kRzAccessDenied
                                ? L"Needs a Chroma Connect App ID" : L"Synapse refused the connection";
    case CHROMA_WAITING: return L"Waiting for Synapse effects";
    case CHROMA_LIVE:    return L"Live from Synapse";
    case CHROMA_HUNG:    return L"Synapse isn't answering";
    default:             return L"Connecting to Synapse\x2026";
    }
}

// ---------------------------------------------------------------------------------------
// Controllers

enum PadButton : DWORD {
    B_SQUARE = 1 << 0, B_CROSS = 1 << 1, B_CIRCLE = 1 << 2, B_TRIANGLE = 1 << 3,
    B_L1 = 1 << 4, B_R1 = 1 << 5, B_L2 = 1 << 6, B_R2 = 1 << 7, B_SHARE = 1 << 8, B_OPTIONS = 1 << 9,
    B_L3 = 1 << 10, B_R3 = 1 << 11, B_PS = 1 << 12, B_TOUCH = 1 << 13,
    B_UP = 1 << 14, B_RIGHT = 1 << 15, B_DOWN = 1 << 16, B_LEFT = 1 << 17,
};

struct PadState {
    BYTE  lx = 128, ly = 128, rx = 128, ry = 128, l2 = 0, r2 = 0;
    DWORD buttons = 0;
    bool  touch[2] = {};
    short tx[2] = {}, ty[2] = {};       // 0..1919, 0..942
    int   battery = -1;                 // %, -1 unknown
    bool  charging = false;
    bool  phones = false, mic = false;  // a headset / its microphone plugged into the jack
    bool  imu = false;                  // the motion sensors are in the reports
    short gyro[3] = {}, accel[3] = {};  // raw: ~16.4 per deg/s, ~8192 per g
    // Motion (x right, y up, z towards you): the orientation is the gyro integrated, raw (it's
    // only levelled from gravity when it starts or is reset), and `lin` is the acceleration other
    // than gravity, in g, in the world's frame.
    float q[4] = { 1, 0, 0, 0 };        // pad -> world rotation (w, x, y, z)
    float lin[3] = {};
    bool  oriented = false;
    BYTE  tid[2] = {};                  // touch ids: a new id is a new finger
    BYTE  counter = 0;                  // the report's own count (6 bits): gaps are lost reports
};

// Generic HID gamepads (anything that isn't a DualShock 4: 8BitDo pads in DirectInput mode,
// USB pads, ...): which HID button is which Xbox button (0: none), and where the axes are.
struct GenericMap { USHORT a, b, x, y, lb, rb, lt, rt, back, start, guide, ls, rs; const wchar_t* name; };
// 8BitDo's DirectInput layout (as in SDL's controller database), and a common default.
static const GenericMap k8BitDoMap = { 2, 1, 5, 4, 7, 8, 9, 10, 11, 12, 13, 14, 15, L"8BitDo layout" };
static const GenericMap kStandardMap = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 13, 11, 12, L"standard layout" };

struct GenericAxis { USHORT page = 0, usage = 0; LONG lo = 0, hi = 0; bool present = false; };
struct GenericLayout {
    PHIDP_PREPARSED_DATA parsed = nullptr;
    GenericMap map = kStandardMap;
    GenericAxis lx, ly, rx, ry, lt, rt, hat;
    GenericAxis battery;                    // a battery level in its input reports, if it has one
    GenericAxis batteryFeature;             // ... or in a feature report (asked for now and then)
    BYTE batteryReport = 0;                 // that feature report's id
    std::wstring fields;                    // what it reports, for Driver status
};

struct Pad {
    std::wstring path, instanceId, serial, name;
    USHORT pid = 0, vid = 0;
    bool   bluetooth = false, dongle = false, fake = false;
    bool   generic = false;                 // a generic HID gamepad, not a DualShock 4
    GenericLayout layout;
    HANDLE h = INVALID_HANDLE_VALUE, thread = nullptr;
    USHORT inLen = 0, outLen = 0, featLen = 0;
    std::atomic<bool> stop{false}, gone{false}, ready{false};
    std::atomic<int>  set[S_COUNT];
    std::atomic<int>  lastLight{LIGHT_BATTERY};     // the light mode before rainbow was switched on
    SRWLOCK  lock = SRWLOCK_INIT;
    PadState state;                     // under lock
    std::atomic<unsigned> seq{0};       // bumped when state changes (the window repaints)
    std::atomic<ULONGLONG> lastReport{0};
    PVIGEM_TARGET target = nullptr;     // the pad's thread only
    std::atomic<bool> emulating{false};
    std::atomic<int>  player{-1};
    std::atomic<int>  rumbleStrong{0}, rumbleWeak{0};
    std::atomic<ULONGLONG> testUntil{0};
    std::atomic<bool> audioOn{false};   // streaming sound to it (its output thread)
    HANDLE outputWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);   // something to send changed
    std::atomic<bool> writerStop{false};
    std::atomic<unsigned> audioSent{0}, audioFailed{0}, audioDropouts{0};
    std::atomic<bool> micOn{false};     // its microphone is switched on (output thread)
    std::atomic<bool> motionReset{false};   // level the motion preview again (double-click)
    std::atomic<int>  minutesLeft{-1};  // battery estimate (-1: none, e.g. charging)
    std::atomic<bool> estimateSure{false};
    std::atomic<unsigned> micFrames{0}, micBad{0};
    std::atomic<unsigned> micEcho{0};   // frames in the mic reports that are the music sent to it (skipped)
    std::atomic<int> micTarget{-1};     // the target byte of the last mic report
    std::atomic<bool> audioMuted{false};    // the controller's sound muted (touchpad volume)
    std::atomic<int> link{-1};          // % of the controller's reports that arrived (last ~2 s), -1 unknown
    std::atomic<unsigned> crcBad{0};    // reports dropped as corrupted
    std::atomic<int> reportRate{0};     // reports a second
    // The input report interval asked for (output report byte 1). 4 normally; 0 while sound
    // plays: the controller then fits its reports into what the link has left (measured: at 4,
    // ~5-9% of reports were lost with sound on; at 0, none).
    std::atomic<int> poll{4};
    bool pollFixed = false;             // set for testing (DS4NATIVE_POLL)
    Pad() { for (int i = 0; i < S_COUNT; i++) set[i] = kSettingDefaults[i]; }
};

static HWND g_tray, g_window;
static std::vector<Pad*> g_pads;        // UI thread
static Pad* g_selected;
static PVIGEM_CLIENT g_vigem;           // null: no ViGEmBus
static SRWLOCK g_vigemLock = SRWLOCK_INIT;
static std::atomic<DWORD> g_openError{0};
static SRWLOCK g_serialLock = SRWLOCK_INIT;
static std::set<std::wstring> g_serials;   // controllers in use (one reachable by USB and Bluetooth is used once)

static void LoadPadSettings(Pad* p) {
    for (int i = 0; i < S_COUNT; i++) p->set[i] = (int)RegGet(p->serial.c_str(), kSettingNames[i], kSettingDefaults[i]);
    p->set[S_LIGHTMODE] = min(p->set[S_LIGHTMODE].load(), (int)LIGHT_COUNT - 1);
    if (g_testMic) p->set[S_MIC] = 1;
}

static void SavePadSettings(Pad* p) {
    if (p->fake) return;
    for (int i = 0; i < S_COUNT; i++) RegPut(p->serial.c_str(), kSettingNames[i], p->set[i]);
}

static void HsvToRgb(float h, float s, float v, BYTE rgb[3]) {
    h = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    int i = (int)h;
    float f = h - i, p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    float r, g, b;
    switch (i) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    rgb[0] = (BYTE)(r * 255 + 0.5f); rgb[1] = (BYTE)(g * 255 + 0.5f); rgb[2] = (BYTE)(b * 255 + 0.5f);
}

static const int kBatteryLowLevel = 35, kBatteryCriticalLevel = 15;    // %: yellow, red

// The light bar's colour at full brightness (also the glow in the window).
static void LightColour(Pad* p, int battery, BYTE rgb[3]) {
    switch (p->set[S_LIGHTMODE]) {
    case LIGHT_COLOUR: HsvToRgb((float)p->set[S_HUE], 1, 1, rgb); break;
    case LIGHT_RAINBOW: {               // speed 1..100: one turn of the colour wheel in 100 / speed s
        double degrees = GetTickCount64() * 0.0036 * p->set[S_RAINBOW];
        HsvToRgb((float)fmod(degrees, 360.0), 1, 1, rgb);
        break;
    }
    case LIGHT_CHROMA:
        if (g_chromaState == CHROMA_LIVE) {
            DWORD c = g_chromaColour;
            rgb[0] = (BYTE)c; rgb[1] = (BYTE)(c >> 8); rgb[2] = (BYTE)(c >> 16);
        } else {
            HsvToRgb((float)p->set[S_HUE], 1, 1, rgb);  // until Synapse sends colours
        }
        break;
    case LIGHT_OFF: rgb[0] = rgb[1] = rgb[2] = 0; break;
    default:
        if (battery < 0) { rgb[0] = 10; rgb[1] = 132; rgb[2] = 255; }
        else if (battery < kBatteryCriticalLevel) { rgb[0] = 255; rgb[1] = 40; rgb[2] = 30; }
        else if (battery < kBatteryLowLevel) { rgb[0] = 255; rgb[1] = 190; rgb[2] = 0; }
        else { rgb[0] = 30; rgb[1] = 220; rgb[2] = 70; }
    }
}

// ---- shortcuts: button combos held together on any controller

enum Action {
    A_DISCONNECT, A_EMULATION, A_PAD_EMULATION, A_LIGHT_NEXT, A_RAINBOW, A_SHOW, A_SCREENSHOT, A_PLAYPAUSE, A_MUTE,
    A_TPMOUSE, A_AUDIO,         // new ones go last: shortcuts are saved by number
    A_COUNT
};
static const wchar_t* const kActionNames[A_COUNT] = {
    L"Disconnect Bluetooth", L"Emulation on / off (all pads)", L"Xbox pad on / off (this pad)", L"Next light bar mode",
    L"Rainbow on / off", L"Open DS4-Native", L"Screenshot (Win + PrtScn)", L"Play / pause media", L"Mute sound",
    L"Touchpad mouse on / off", L"Controller sound on / off",
};
struct Shortcut { DWORD buttons; DWORD action; };
static const int kMaxShortcuts = 10;
static const Shortcut kDefaultShortcut = { B_PS | B_OPTIONS, A_DISCONNECT };
static SRWLOCK g_shortcutLock = SRWLOCK_INIT;
static std::vector<Shortcut> g_shortcuts;   // written by the UI thread under the lock
static std::atomic<bool> g_recording{false};    // the UI is recording a combo: none fire

static void LoadShortcuts() {
    Shortcut data[kMaxShortcuts];
    DWORD size = sizeof(data);
    LSTATUS r = RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"Shortcuts", RRF_RT_REG_BINARY, nullptr, data, &size);
    AcquireSRWLockExclusive(&g_shortcutLock);
    g_shortcuts.clear();
    if (r != ERROR_SUCCESS) {
        g_shortcuts.push_back(kDefaultShortcut);
    } else {
        for (DWORD i = 0; i < size / sizeof(Shortcut); i++) {
            if (data[i].buttons && data[i].action < A_COUNT) g_shortcuts.push_back(data[i]);
        }
    }
    ReleaseSRWLockExclusive(&g_shortcutLock);
}

static void SaveShortcuts() {
    AcquireSRWLockShared(&g_shortcutLock);
    std::vector<Shortcut> copy = g_shortcuts;
    ReleaseSRWLockShared(&g_shortcutLock);
    static Shortcut none;
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"Shortcuts", REG_BINARY, copy.empty() ? &none : copy.data(),
                    (DWORD)(copy.size() * sizeof(Shortcut)));
}

static const struct { DWORD bit; const wchar_t* name; } kComboNames[] = {
    { B_PS, L"PS" }, { B_SHARE, L"Share" }, { B_OPTIONS, L"Options" }, { B_TOUCH, L"Touchpad" },
    { B_L1, L"L1" }, { B_R1, L"R1" }, { B_L2, L"L2" }, { B_R2, L"R2" }, { B_L3, L"L3" }, { B_R3, L"R3" },
    { B_TRIANGLE, L"Triangle" }, { B_CIRCLE, L"Circle" }, { B_CROSS, L"Cross" }, { B_SQUARE, L"Square" },
    { B_UP, L"Up" }, { B_DOWN, L"Down" }, { B_LEFT, L"Left" }, { B_RIGHT, L"Right" },
};

static std::wstring ComboText(DWORD buttons) {
    std::wstring s;
    for (const auto& c : kComboNames) {
        if (!(buttons & c.bit)) continue;
        if (!s.empty()) s += L" + ";
        s += c.name;
    }
    return s;
}

static int BitCount(DWORD v) { int n = 0; for (; v; v &= v - 1) n++; return n; }

// ---- reports

static const struct ButtonMap { DWORD pad; USHORT xbox; const wchar_t* from; const wchar_t* to; } kButtonMap[] = {
    { B_CROSS, XUSB_GAMEPAD_A, L"Cross", L"A" },            { B_CIRCLE, XUSB_GAMEPAD_B, L"Circle", L"B" },
    { B_SQUARE, XUSB_GAMEPAD_X, L"Square", L"X" },          { B_TRIANGLE, XUSB_GAMEPAD_Y, L"Triangle", L"Y" },
    { B_L1, XUSB_GAMEPAD_LEFT_SHOULDER, L"L1", L"LB" },     { B_R1, XUSB_GAMEPAD_RIGHT_SHOULDER, L"R1", L"RB" },
    { B_L2, 0, L"L2", L"LT" },                              { B_R2, 0, L"R2", L"RT" },
    { B_SHARE, XUSB_GAMEPAD_BACK, L"Share", L"Back" },      { B_OPTIONS, XUSB_GAMEPAD_START, L"Options", L"Start" },
    { B_L3, XUSB_GAMEPAD_LEFT_THUMB, L"L3", L"LS" },        { B_R3, XUSB_GAMEPAD_RIGHT_THUMB, L"R3", L"RS" },
    { B_PS, 0, L"PS", L"Guide" },                           { B_TOUCH, 0, L"Touchpad", L"Back" },
    { B_UP, XUSB_GAMEPAD_DPAD_UP, L"D-pad up", L"D-pad up" },       { B_RIGHT, XUSB_GAMEPAD_DPAD_RIGHT, L"D-pad right", L"D-pad right" },
    { B_DOWN, XUSB_GAMEPAD_DPAD_DOWN, L"D-pad down", L"D-pad down" }, { B_LEFT, XUSB_GAMEPAD_DPAD_LEFT, L"D-pad left", L"D-pad left" },
};

// Returns false for reports that carry no state. `synced`: the wireless adapter has a controller.
static bool ParseReport(const Pad* p, const BYTE* b, DWORD n, PadState& s, bool* synced) {
    const BYTE* d;
    bool full;
    *synced = true;
    if (b[0] >= 0x11 && b[0] <= 0x19 && n >= 46) {                           // Bluetooth, extended
        if (b[0] != 0x11 && !(b[1] & 0x80)) return false;                   // sound only, no state
        d = b + 2;
        full = true;
    }
    else if (b[0] == 0x01 && n >= 10) { d = b; full = !p->bluetooth && n >= 44; }  // USB, or Bluetooth's basic report
    else return false;
    s.lx = d[1]; s.ly = d[2]; s.rx = d[3]; s.ry = d[4];
    static const DWORD kHat[9] = { B_UP, B_UP | B_RIGHT, B_RIGHT, B_RIGHT | B_DOWN, B_DOWN, B_DOWN | B_LEFT, B_LEFT,
                                   B_LEFT | B_UP, 0 };
    DWORD buttons = kHat[min(d[5] & 0x0F, 8)];
    if (d[5] & 0x10) buttons |= B_SQUARE;
    if (d[5] & 0x20) buttons |= B_CROSS;
    if (d[5] & 0x40) buttons |= B_CIRCLE;
    if (d[5] & 0x80) buttons |= B_TRIANGLE;
    if (d[6] & 0x01) buttons |= B_L1;
    if (d[6] & 0x02) buttons |= B_R1;
    if (d[6] & 0x04) buttons |= B_L2;
    if (d[6] & 0x08) buttons |= B_R2;
    if (d[6] & 0x10) buttons |= B_SHARE;
    if (d[6] & 0x20) buttons |= B_OPTIONS;
    if (d[6] & 0x40) buttons |= B_L3;
    if (d[6] & 0x80) buttons |= B_R3;
    if (d[7] & 0x01) buttons |= B_PS;
    if (d[7] & 0x02) buttons |= B_TOUCH;
    s.buttons = buttons;
    s.counter = d[7] >> 2;
    s.l2 = d[8]; s.r2 = d[9];
    s.imu = false;
    if (!full) return true;
    if (p->dongle && (d[31] & 0x04)) { *synced = false; return true; }
    auto word = [d](int at) { return (short)(d[at] | (d[at + 1] << 8)); };
    for (int i = 0; i < 3; i++) {
        s.gyro[i] = word(13 + i * 2);
        s.accel[i] = word(19 + i * 2);
    }
    s.imu = true;
    s.charging = (d[30] & 0x10) != 0;
    s.phones = (d[30] & 0x20) != 0;
    s.mic = (d[30] & 0x40) != 0;
    int level = d[30] & 0x0F;
    s.battery = min(100, level * 100 / (s.charging ? 11 : 8));
    for (int i = 0; i < 2; i++) {
        const BYTE* t = d + 35 + i * 4;
        s.touch[i] = !(t[0] & 0x80);
        s.tid[i] = t[0] & 0x7F;
        s.tx[i] = (short)(t[1] | ((t[2] & 0x0F) << 8));
        s.ty[i] = (short)((t[2] >> 4) | (t[3] << 4));
    }
    return true;
}

// Smooths gravity out of the accelerometer (tilt) and integrates the gyro's turn (yaw),
// letting the yaw drift back to straight ahead so the model never stays turned.
static void QuatRotate(const float q[4], const float v[3], float out[3]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float tx = 2 * (y * v[2] - z * v[1]), ty = 2 * (z * v[0] - x * v[2]), tz = 2 * (x * v[1] - y * v[0]);
    out[0] = v[0] + w * tx + (y * tz - z * ty);
    out[1] = v[1] + w * ty + (z * tx - x * tz);
    out[2] = v[2] + w * tz + (x * ty - y * tx);
}

static void UpdateMotion(PadState& s, float dt, bool reset) {
    if (!s.imu) return;
    float a[3] = { s.accel[0] / 8192.0f, s.accel[1] / 8192.0f, s.accel[2] / 8192.0f };
    float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!s.oriented || reset) {             // level, facing forward, from where gravity points
        if (len < 0.5f) return;
        float ux = a[0] / len, uy = a[1] / len, uz = a[2] / len;
        if (uy < -0.999f) { s.q[0] = 0; s.q[1] = 1; s.q[2] = 0; s.q[3] = 0; }
        else {
            float w = 1 + uy, x = -uz, z = ux, n = sqrtf(w * w + x * x + z * z);
            s.q[0] = w / n; s.q[1] = x / n; s.q[2] = 0; s.q[3] = z / n;
        }
        s.lin[0] = s.lin[1] = s.lin[2] = 0;
        s.oriented = true;
        return;
    }
    if (dt <= 0) return;
    // Raw rotation: the gyro (about 16.4 per degree a second) turns the pad in its own frame.
    float wx = s.gyro[0] / 16.4f * (kPi / 180), wy = s.gyro[1] / 16.4f * (kPi / 180), wz = s.gyro[2] / 16.4f * (kPi / 180);
    float* q = s.q;
    float h = 0.5f * dt;
    float dw = h * (-q[1] * wx - q[2] * wy - q[3] * wz), dx = h * (q[0] * wx + q[2] * wz - q[3] * wy);
    float dy = h * (q[0] * wy + q[3] * wx - q[1] * wz), dz = h * (q[0] * wz + q[1] * wy - q[2] * wx);
    q[0] += dw; q[1] += dx; q[2] += dy; q[3] += dz;
    float n = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    for (int i = 0; i < 4; i++) q[i] /= n;
    // What's left of the accelerometer once gravity is taken out: the pad being moved.
    float world[3];
    QuatRotate(q, a, world);
    world[1] -= 1;
    float k = 1 - expf(-dt / 0.05f);
    for (int i = 0; i < 3; i++) s.lin[i] += (world[i] - s.lin[i]) * k;
}
static void StickToXbox(BYTE x, BYTE y, int deadPercent, SHORT* ox, SHORT* oy) {
    float fx = (x - 127.5f) / 127.5f, fy = -(y - 127.5f) / 127.5f;
    float mag = sqrtf(fx * fx + fy * fy), dead = deadPercent / 100.0f;
    if (mag <= dead || mag < 0.0001f) { *ox = *oy = 0; return; }
    float scale = min(1.0f, (mag - dead) / (1 - dead)) / mag;
    *ox = (SHORT)max(-32767.0f, min(32767.0f, fx * scale * 32767));
    *oy = (SHORT)max(-32767.0f, min(32767.0f, fy * scale * 32767));
}

static BYTE TriggerToXbox(BYTE v, int deadPercent) {
    int dead = deadPercent * 255 / 100;
    if (v <= dead) return 0;
    return (BYTE)((v - dead) * 255 / (255 - dead));
}

// `hold`: buttons used by a shortcut, kept from the game until they're let go.
static XUSB_REPORT ToXbox(Pad* p, const PadState& s, DWORD hold) {
    XUSB_REPORT r{};
    DWORD buttons = s.buttons & ~hold;
    for (const ButtonMap& m : kButtonMap) if (buttons & m.pad) r.wButtons |= m.xbox;
    if (buttons & B_PS) r.wButtons |= XUSB_GAMEPAD_GUIDE;     // PS is always Guide
    if ((buttons & B_TOUCH) && p->set[S_TOUCHBACK]) r.wButtons |= XUSB_GAMEPAD_BACK;
    r.bLeftTrigger = (hold & B_L2) ? 0 : TriggerToXbox(s.l2, p->set[S_DEADT]);
    r.bRightTrigger = (hold & B_R2) ? 0 : TriggerToXbox(s.r2, p->set[S_DEADT]);
    StickToXbox(s.lx, s.ly, p->set[S_DEADL], &r.sThumbLX, &r.sThumbLY);
    StickToXbox(s.rx, s.ry, p->set[S_DEADR], &r.sThumbRX, &r.sThumbRY);
    return r;
}

static DWORD Crc32(const BYTE* data, size_t n, DWORD crc = 0xFFFFFFFF) {
    for (size_t i = 0; i < n; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return crc;
}

// Bluetooth output reports end in a CRC-32 of 0xA2 (the HID "output report" header) + the report.
static void SignReport(BYTE* buf, DWORD len) {
    BYTE seed = 0xA2;
    DWORD crc = ~Crc32(buf, len - 4, Crc32(&seed, 1));
    memcpy(buf + len - 4, &crc, 4);
}

static bool WriteReport(Pad* p, const BYTE* buf, DWORD len, bool fallback);

// audio: 0 none, 1 switch the sound on to the headphone jack, 2 on to the speaker, 3 switch it
// off again, 4 leave it playing (just keep it on; volumes aren't re-sent).
// mic: 0 none, 1 switch the microphone on, 2 keep it on, 3 switch it off.
struct PadOutput { BYTE weak, strong, r, g, b, audio, volume, mic; };

static bool WriteOutput(Pad* p, const PadOutput& o) {
    BYTE buf[128] = {};
    DWORD len;
    if (p->bluetooth) {
        len = 78;
        buf[0] = 0x11; buf[1] = (BYTE)(0xC0 | p->poll); buf[3] = 0x07;
        buf[6] = o.weak; buf[7] = o.strong; buf[8] = o.r; buf[9] = o.g; buf[10] = o.b;
        // Byte 2: 0xA0 while sound plays, 0xA1 while the microphone is on too (or alone).
        bool mic = o.mic == 1 || o.mic == 2, speaker = o.audio == 1 || o.audio == 2 || o.audio == 4;
        if (speaker || mic) buf[2] = mic ? 0xA1 : 0xA0;
        if (o.audio == 1 || o.audio == 2) {     // volumes valid; sound on (0x85 latches it)
            buf[3] |= 0xF0;
            buf[21] = buf[22] = o.audio == 1 ? o.volume : 0;
            buf[23] = mic ? 0x40 : 0;
            buf[24] = o.audio == 2 ? o.volume : 0;
            buf[25] = 0x85;
        } else if (o.audio == 3) {              // sound off (the mic may stay on)
            buf[3] |= mic ? 0xB0 : 0xF0;
            buf[25] = mic ? 0x85 : 0x00;
        }
        if (o.mic == 1) {                       // microphone on, gain 0x40 (its ceiling)
            buf[3] |= 0x40;
            buf[23] = 0x40;
            buf[25] = 0x85;
        } else if (o.mic == 3) {
            buf[3] |= 0x40;
            buf[23] = 0;
        }
        SignReport(buf, len);
    } else {
        len = max((DWORD)32, min((DWORD)p->outLen, (DWORD)sizeof(buf)));
        buf[0] = 0x05; buf[1] = 0x07;
        buf[4] = o.weak; buf[5] = o.strong; buf[6] = o.r; buf[7] = o.g; buf[8] = o.b;
    }
    return WriteReport(p, buf, len, p->bluetooth && !p->audioOn);   // the slow fallback would stall the sound
}

static bool WriteReport(Pad* p, const BYTE* buf, DWORD len, bool fallback) {
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD written = 0;
    bool ok = WriteFile(p->h, buf, len, &written, &ov) != 0;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0) CancelIoEx(p->h, &ov);
        ok = GetOverlappedResult(p->h, &ov, &written, TRUE) != 0;
    }
    CloseHandle(ov.hEvent);
    if (!ok && fallback) ok = HidD_SetOutputReport(p->h, (PVOID)buf, len) != 0;    // over the control channel
    return ok;
}

// ---- Bluetooth

static bool SerialToAddress(const std::wstring& serial, ULONGLONG* address) {
    if (serial.size() != 12) return false;
    wchar_t* end;
    *address = wcstoull(serial.c_str(), &end, 16);
    return *end == 0;
}

static bool DisconnectBluetooth(const Pad* p) {
    ULONGLONG address;
    if (!p->bluetooth || !SerialToAddress(p->serial, &address)) return false;
    BLUETOOTH_FIND_RADIO_PARAMS params{ sizeof(params) };
    HANDLE radio;
    HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
    if (!find) return false;
    bool ok = false;
    do {
        DWORD bytes;
        ok = DeviceIoControl(radio, 0x41000C /* IOCTL_BTH_DISCONNECT_DEVICE */, &address, sizeof(address), nullptr, 0, &bytes, nullptr) || ok;
        CloseHandle(radio);
    } while (BluetoothFindNextRadio(find, &radio));
    BluetoothFindRadioClose(find);
    return ok;
}

// ---- the virtual Xbox pad

static VOID CALLBACK OnXboxNotification(PVIGEM_CLIENT, PVIGEM_TARGET, UCHAR strong, UCHAR weak, UCHAR led, LPVOID user) {
    Pad* p = (Pad*)user;
    p->rumbleStrong = strong;
    p->rumbleWeak = weak;
    SetEvent(p->outputWake);
    if (p->player.exchange(led) != led) PostMessageW(g_tray, WM_PADS, 0, 0);
}

static void SetEmulating(Pad* p, bool on) {
    if (g_noInput) on = false;              // a test run never creates the virtual pad
    if (on == (p->target != nullptr)) return;
    AcquireSRWLockExclusive(&g_vigemLock);
    if (on) {
        PVIGEM_TARGET t = vigem_target_x360_alloc();
        if (VIGEM_SUCCESS(vigem_target_add(g_vigem, t))) {
            vigem_target_x360_register_notification(g_vigem, t, OnXboxNotification, p);
            p->target = t;
        } else {
            vigem_target_free(t);
        }
    } else {
        vigem_target_x360_unregister_notification(p->target);
        vigem_target_remove(g_vigem, p->target);
        vigem_target_free(p->target);
        p->target = nullptr;
        p->player = -1;
        p->rumbleStrong = p->rumbleWeak = 0;
    }
    ReleaseSRWLockExclusive(&g_vigemLock);
    p->emulating = p->target != nullptr;
    PostMessageW(g_tray, WM_PADS, 0, 0);
}

// ---- shortcut actions (on the controller's thread)

static void SendKeys(std::initializer_list<WORD> keys) {
    if (g_noInput) return;
    INPUT in[8] = {};
    int n = 0;
    for (WORD k : keys) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = k; n++; }
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
        in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = *it; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void RunAction(Pad* p, int action, DWORD buttons) {
    if (g_noInput) return;
    switch (action) {
    case A_DISCONNECT:    DisconnectBluetooth(p); break;
    case A_EMULATION:     g_enabled = !g_enabled; RegPut(nullptr, L"Enabled", g_enabled); break;
    case A_PAD_EMULATION: p->set[S_EMULATE] = !p->set[S_EMULATE]; SavePadSettings(p); break;
    case A_LIGHT_NEXT:    p->set[S_LIGHTMODE] = (p->set[S_LIGHTMODE] + 1) % LIGHT_COUNT; SavePadSettings(p); break;
    case A_RAINBOW:
        if (p->set[S_LIGHTMODE] == LIGHT_RAINBOW) {
            int back = p->lastLight;
            p->set[S_LIGHTMODE] = back == LIGHT_RAINBOW ? LIGHT_BATTERY : back;
        } else {
            p->lastLight = p->set[S_LIGHTMODE].load();
            p->set[S_LIGHTMODE] = LIGHT_RAINBOW;
        }
        SavePadSettings(p);
        break;
    case A_SHOW:          PostMessageW(g_tray, WM_COMMAND, ID_MENU_OPEN, 0); break;
    case A_SCREENSHOT:    SendKeys({ VK_LWIN, VK_SNAPSHOT }); break;
    case A_PLAYPAUSE:     SendKeys({ VK_MEDIA_PLAY_PAUSE }); break;
    case A_MUTE:          SendKeys({ VK_VOLUME_MUTE }); break;
    case A_TPMOUSE:       p->set[S_TPMOUSE] = !p->set[S_TPMOUSE]; SavePadSettings(p); break;
    case A_AUDIO:         p->set[S_AUDIO] = !p->set[S_AUDIO]; SavePadSettings(p); break;
    }
    PostMessageW(g_tray, WM_SHORTCUT, action, buttons);
}

// ---- the touchpad as a mouse (on the controller's thread)

struct TouchMouse {
    bool  had[2] = {};
    BYTE  id[2] = {};
    float x[2] = {}, y[2] = {};
    float accX = 0, accY = 0, accV = 0, accH = 0;   // fractions not sent yet
    double cx = 0, cy = 0;              // where we put the cursor (sub-pixel), absolute screen px
    bool  placed = false;
    float velocity = 0;                 // finger speed, smoothed over ~80 ms (touch units a second)
    DWORD up = 0;                       // the MOUSEEVENTF_*UP for a button that's down (held, or a release Windows refused)
    float pressMoved = 0;               // how far the finger slid since the pad was pressed
    // Taps (with "Tap to click"): one or two fingers down briefly with little movement.
    int   tapFingers = 0;
    bool  tapPressed = false;
    ULONGLONG tapAt = 0;
    float tapMoved = 0;
};
static const ULONGLONG kTapMs = 200;
static const float kPressSlide = 40, kTapSlide = 40;     // touch units

// False if Windows refused it: injected input is blocked while an elevated window (Task
// Manager, an installer) has the cursor or focus, so a release can get lost.
static bool MouseInput(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    if (g_noInput) return true;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    return SendInput(1, &in, sizeof(in)) == 1;
}

// A click: down and up. A refused up is kept to be retried, or the button would stay held
// and every move after it would drag.
static void MouseClick(TouchMouse& m, DWORD down, DWORD up) {
    if (MouseInput(down) && !MouseInput(up)) m.up = up;
}

static void TwoFingerButtons(Pad* p, DWORD* down, DWORD* up) {
    bool middle = p->set[S_TPTWO] == TWO_MIDDLE;
    *down = middle ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_RIGHTDOWN;
    *up = middle ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_RIGHTUP;
}

// One finger moves the cursor, two scroll. Pressing the pad is the mouse button, exactly: down
// when pressed, up when let go, and sliding while it's pressed drags (left, or right on the
// right half with split clicks; two fingers: right or middle). With "Tap to click", a quick tap
// clicks too (two fingers: right or middle).
static void UpdateTouchMouse(Pad* p, TouchMouse& m, const PadState& s, DWORD before, ULONGLONG now, float dt) {
    if (!p->set[S_TPMOUSE]) {
        if (m.up && MouseInput(m.up)) m.up = 0;
        m.had[0] = m.had[1] = false;
        m.tapFingers = 0;
        return;
    }
    int fingers = s.touch[0] + s.touch[1];
    // The pad counts as pressed only with a finger on it: its click switch can bounce or stick
    // on its own, and a press the switch never lets go of would otherwise drag forever.
    bool down = (s.buttons & B_TOUCH) && fingers > 0, wasDown = (before & B_TOUCH) && (m.had[0] || m.had[1]);

    // How far the fingers moved since the last report (the same fingers only).
    float moved = 0, dx = 0, dy = 0;
    int same = 0;
    for (int i = 0; i < 2; i++) {
        if (!s.touch[i] || !m.had[i] || m.id[i] != s.tid[i]) continue;
        float ex = (float)(s.tx[i] - m.x[i]), ey = (float)(s.ty[i] - m.y[i]);
        dx += ex; dy += ey; same++;
        moved = max(moved, fabsf(ex) + fabsf(ey));
    }
    if (same) { dx /= same; dy /= same; }

    // The press: the button goes down, and stays down until the pad is let go.
    if (down && !wasDown && !m.up) {
        DWORD dn = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
        int finger = s.touch[0] ? 0 : 1;
        if (fingers >= 2) {
            TwoFingerButtons(p, &dn, &up);
        } else if (p->set[S_TPSPLIT] && s.touch[finger] && s.tx[finger] >= 960) {
            dn = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP;
        }
        m.pressMoved = 0;
        if (MouseInput(dn)) m.up = up;
    }
    if (m.up) m.pressMoved += moved;
    if (!down && m.up && MouseInput(m.up)) m.up = 0;    // released (or retried until Windows takes it)

    if (fingers == 0) { m.velocity = 0; m.placed = false; }

    // Taps.
    if (fingers > 0 && m.tapFingers == 0) {             // fingers landed
        m.tapAt = now;
        m.tapMoved = 0;
        m.tapPressed = false;
    }
    if (fingers > 0) {
        m.tapFingers = max(m.tapFingers, fingers);
        m.tapMoved += moved;
        m.tapPressed = m.tapPressed || down;
    } else if (m.tapFingers) {                          // all lifted
        if (p->set[S_TPTAP] && !m.tapPressed && now - m.tapAt < kTapMs + (m.tapFingers > 1 ? 100 : 0) && m.tapMoved < kTapSlide) {
            DWORD dn = MOUSEEVENTF_LEFTDOWN, up = MOUSEEVENTF_LEFTUP;
            if (m.tapFingers > 1) TwoFingerButtons(p, &dn, &up);
            else if (p->set[S_TPSPLIT] && m.x[0] + m.x[1] > 0 && (m.had[0] ? m.x[0] : m.x[1]) >= 960) {
                dn = MOUSEEVENTF_RIGHTDOWN; up = MOUSEEVENTF_RIGHTUP;
            }
            if (!m.up) MouseClick(m, dn, up);
        }
        m.tapFingers = 0;
    }

    // Moving and scrolling.
    float speed = p->set[S_TPSPEED] / 50.0f;
    // (Not in the first few units after a press: pressing wobbles the finger, and a click
    // shouldn't become a tiny drag.)
    if (same && !(m.up && m.pressMoved < kPressSlide)) {
        if (fingers == 1) {
            // Reports come in bursts over Bluetooth and the touch position doesn't change in
            // every one, so the speed (for the gentle acceleration) is smoothed over time, and
            // the cursor is placed absolutely: Windows' own pointer acceleration would bend
            // the bursty steps again.
            float instant = dt > 0 ? sqrtf(dx * dx + dy * dy) / dt : 0;
            float k = dt > 0 ? 1 - expf(-dt / 0.08f) : 0;
            m.velocity += (min(instant, 20000.0f) - m.velocity) * k;
            float gain = speed * 0.85f * max(0.8f, min(1.8f, 0.8f + m.velocity / 5000));
            // Where the cursor is is read only when a finger lands: Windows applies injected
            // moves a little later, so reading it every report would undo the latest moves.
            if (!m.placed) {
                POINT at;
                GetCursorPos(&at);
                m.cx = at.x;
                m.cy = at.y;
                m.placed = true;
            }
            LONG oldX = (LONG)(m.cx + 0.5), oldY = (LONG)(m.cy + 0.5);
            int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
            int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            m.cx = max((double)vx, min((double)(vx + vw - 1), m.cx + dx * gain));
            m.cy = max((double)vy, min((double)(vy + vh - 1), m.cy + dy * gain));
            LONG nx = (LONG)((m.cx - vx) * 65535.0 / max(1, vw - 1) + 0.5), ny = (LONG)((m.cy - vy) * 65535.0 / max(1, vh - 1) + 0.5);
            if ((LONG)(m.cx + 0.5) != oldX || (LONG)(m.cy + 0.5) != oldY)
                MouseInput(MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK, nx, ny);
        } else if (fingers == 2 && same == 2 && p->set[S_TPSCROLL]) {   // content follows the fingers
            m.accV += dy * 1.6f * speed;
            m.accH -= dx * 1.6f * speed;
            if (fabsf(m.accV) >= 10) { MouseInput(MOUSEEVENTF_WHEEL, 0, 0, (DWORD)(LONG)m.accV); m.accV -= (LONG)m.accV; }
            if (fabsf(m.accH) >= 10) { MouseInput(MOUSEEVENTF_HWHEEL, 0, 0, (DWORD)(LONG)m.accH); m.accH -= (LONG)m.accH; }
        }
    }
    for (int i = 0; i < 2; i++) {
        m.had[i] = s.touch[i];
        m.id[i] = s.tid[i];
        if (s.touch[i]) { m.x[i] = s.tx[i]; m.y[i] = s.ty[i]; }
    }
}

// ---------------------------------------------------------------------------------------
// Sound to the controller over Bluetooth. What the PC plays is captured (WASAPI loopback of
// the default output), resampled to 32 kHz stereo into a ring, and each controller's thread
// takes 512 samples every 16 ms, encodes them as four SBC frames and sends them in a 0x17
// report. Report layouts and SBC settings (32 kHz, 16 blocks, 8 subbands, joint stereo, SNR,
// bitpool 48: 109-byte frames) follow DS4-Sound-Link (MIT, github.com/abh1shekbana/DS4-Sound-Link).
// Over USB the controller is a Windows sound device of its own and needs none of this.

namespace audio {

static const int kRate = 32000, kRing = 1 << 16, kChunk = 512;
static short g_ring[kRing][2];
static std::atomic<ULONGLONG> g_written{0};     // frames written in total
static std::atomic<bool> g_running{false}, g_stop{false};
static std::atomic<long> g_error{0};
static HANDLE g_thread;

// Where the sound comes from: a playback device's id; "" picks DS4Native's own device (a
// playback device with "DualShock" or "DS4Native" in its name); kMirror mirrors the default
// output. Its sound is captured with WASAPI loopback, so any playback device works, including
// a virtual one that plays nowhere else.
static const wchar_t kMirror[] = L"*mirror";
static const long kNoDevice = (long)0x80070490;      // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)
static SRWLOCK g_sourceLock = SRWLOCK_INIT;
static std::wstring g_source;
static std::atomic<bool> g_reopen{false};
static std::wstring g_deviceName;               // what's being played from (capture thread, under the lock)

static const PROPERTYKEY kFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };

struct Device { std::wstring id, name; };

static std::wstring DeviceName(IMMDevice* dev) {
    std::wstring name;
    IPropertyStore* props = nullptr;
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        if (SUCCEEDED(props->GetValue(kFriendlyName, &v)) && v.vt == VT_LPWSTR) name = v.pwszVal;
        PropVariantClear(&v);
        props->Release();
    }
    return name;
}

static std::vector<Device> ListDevices(IMMDeviceEnumerator* en) {
    std::vector<Device> list;
    IMMDeviceCollection* all = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) return list;
    UINT count = 0;
    all->GetCount(&count);
    for (UINT i = 0; i < count; i++) {
        IMMDevice* dev = nullptr;
        if (FAILED(all->Item(i, &dev))) continue;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) { list.push_back({ id, DeviceName(dev) }); CoTaskMemFree(id); }
        dev->Release();
    }
    all->Release();
    return list;
}

// For the UI: the playback devices to choose from.
static std::vector<Device> Devices() {
    std::vector<Device> list;
    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        list = ListDevices(en);
        en->Release();
    }
    return list;
}

static bool OwnDevice(const std::wstring& name) {
    return name.find(L"DualShock") != std::wstring::npos || name.find(L"DS4Native") != std::wstring::npos ||
           name.find(L"DS4-Native") != std::wstring::npos;
}

static std::wstring Source() {
    AcquireSRWLockShared(&g_sourceLock);
    std::wstring s = g_source;
    ReleaseSRWLockShared(&g_sourceLock);
    return s;
}

static void SetSource(const std::wstring& id) {
    AcquireSRWLockExclusive(&g_sourceLock);
    g_source = id;
    ReleaseSRWLockExclusive(&g_sourceLock);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"AudioSource", REG_SZ, id.c_str(), (DWORD)((id.size() + 1) * sizeof(wchar_t)));
    g_reopen = true;
}

static void LoadSource() {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"AudioSource", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) g_source = buf;
}

static std::wstring PlayingFrom() {
    AcquireSRWLockShared(&g_sourceLock);
    std::wstring s = g_deviceName;
    ReleaseSRWLockShared(&g_sourceLock);
    return s;
}

static std::wstring DefaultId(IMMDeviceEnumerator* en) {
    std::wstring id;
    IMMDevice* dev = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) {
        LPWSTR s = nullptr;
        if (SUCCEEDED(dev->GetId(&s))) { id = s; CoTaskMemFree(s); }
        dev->Release();
    }
    return id;
}

static DWORD WINAPI CaptureThread(LPVOID) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);                     // Sleep(5) really 5 ms, not a 15.6 ms tick
    while (!g_stop) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* capture = nullptr;
        WAVEFORMATEX* fmt = nullptr;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        g_reopen = false;
        std::wstring source = Source();
        bool mirror = source == kMirror;
        if (SUCCEEDED(hr)) {
            if (mirror) {
                hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
            } else {
                std::wstring id = source;
                if (id.empty()) {                       // a device named for the controller, else VB-Cable's input
                    std::vector<Device> all = ListDevices(en);
                    for (const Device& d : all) if (OwnDevice(d.name)) { id = d.id; break; }
                    for (const Device& d : all) if (id.empty() && d.name.find(L"CABLE Input") != std::wstring::npos) id = d.id;
                }
                hr = id.empty() ? kNoDevice : en->GetDevice(id.c_str(), &dev);
            }
        }
        AcquireSRWLockExclusive(&g_sourceLock);
        g_deviceName = dev ? DeviceName(dev) : L"";
        ReleaseSRWLockExclusive(&g_sourceLock);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
        bool isFloat = false;
        if (SUCCEEDED(hr)) {
            isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
            if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
                const WAVEFORMATEXTENSIBLE* x = (const WAVEFORMATEXTENSIBLE*)fmt;
                isFloat = x->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
            }
            if (!isFloat && fmt->wBitsPerSample != 16 && fmt->wBitsPerSample != 32) hr = E_NOTIMPL;
        }
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, fmt, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&capture);
        if (SUCCEEDED(hr)) hr = client->Start();
        g_error = SUCCEEDED(hr) ? 0 : hr;
        g_running = SUCCEEDED(hr);
        if (SUCCEEDED(hr)) {
            std::wstring device = DefaultId(en);
            int channels = fmt->nChannels, bits = fmt->wBitsPerSample;
            double step = (double)fmt->nSamplesPerSec / kRate, t = 0;   // resampler: next output, in input samples after prev
            float prev[2] = {}, cur[2];
            ULONGLONG checked = GetTickCount64();
            while (!g_stop) {
                Sleep(5);
                UINT32 packet = 0;
                if (FAILED(capture->GetNextPacketSize(&packet))) break;    // the device went away: open again
                ULONGLONG w = g_written.load();
                while (packet) {
                    BYTE* data;
                    UINT32 frames;
                    DWORD flags;
                    if (FAILED(capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { packet = 0; break; }
                    bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                    for (UINT32 f = 0; f < frames; f++) {
                        for (int c = 0; c < 2; c++) {
                            int ch = min(c, channels - 1);
                            if (silent) cur[c] = 0;
                            else if (isFloat) cur[c] = ((const float*)data)[f * channels + ch] * 32767.0f;
                            else if (bits == 16) cur[c] = ((const short*)data)[f * channels + ch];
                            else cur[c] = ((const int*)data)[f * channels + ch] / 65536.0f;
                        }
                        while (t <= 1) {        // linear interpolation between prev and cur
                            for (int c = 0; c < 2; c++) {
                                float v = prev[c] + (cur[c] - prev[c]) * (float)t;
                                g_ring[w & (kRing - 1)][c] = (short)max(-32768.0f, min(32767.0f, v));
                            }
                            w++;
                            t += step;
                        }
                        t -= 1;
                        prev[0] = cur[0]; prev[1] = cur[1];
                    }
                    capture->ReleaseBuffer(frames);
                    if (FAILED(capture->GetNextPacketSize(&packet))) packet = 0;
                }
                g_written.store(w);
                if (g_reopen) break;                        // another device was picked
                if (mirror && GetTickCount64() - checked > 2000) {     // follow a change of the default output
                    checked = GetTickCount64();
                    if (DefaultId(en) != device) break;
                }
            }
            client->Stop();
        }
        g_running = false;
        if (fmt) CoTaskMemFree(fmt);
        if (capture) capture->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        for (int i = 0; i < 40 && !g_stop && !g_reopen; i++) Sleep(50);    // retry (a device may appear)
    }
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}

// Runs the capture while any controller wants sound (UI thread).
static void Want(bool want) {
    if (want && !g_thread) {
        g_stop = false;
        g_thread = CreateThread(nullptr, 0, CaptureThread, nullptr, 0, nullptr);
    } else if (!want && g_thread) {
        g_stop = true;
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_running = false;
    }
}

// A controller's place in the ring, `lead` frames behind the capture (48 ms to start; each
// dropout adds 16 ms, up to 128 ms). Silence when nothing plays: the packets keep going, or
// the controller's amplifier pops.
struct Reader {
    ULONGLONG pos = 0;
    ULONGLONG lead = 1536;
    bool started = false, playing = false;
    unsigned dropouts = 0;
    void Take(short out[kChunk][2]) {
        ULONGLONG w = g_written.load();
        if (!started || w < pos || w - pos > lead + 4096) { pos = w > lead ? w - lead : 0; started = true; }
        if (w - pos < kChunk) {
            if (playing) {                  // ran dry mid-sound: keep more in hand from now on
                dropouts++;
                lead = min(lead + kChunk, (ULONGLONG)4096);
                pos = w > lead ? w - lead : 0;
            }
            playing = false;
            memset(out, 0, sizeof(short) * 2 * kChunk);
            return;
        }
        playing = true;
        for (int i = 0; i < kChunk; i++, pos++) {
            out[i][0] = g_ring[pos & (kRing - 1)][0];
            out[i][1] = g_ring[pos & (kRing - 1)][1];
        }
    }
};

// SBC encoder for the controller's settings: 128 stereo samples -> one 109-byte frame.
// (Bluetooth A2DP specification, SBC; ported from DS4-Sound-Link's DualShock4SbcEncoder.)
class Sbc {
    static const int kBands = 8, kBlocks = 16, kBitpool = 48, kTaps = 80;
    double history[2][kTaps] = {}, samples[2][kBlocks * kBands] = {};
    int scale[2][kBands] = {}, bits[2][kBands] = {};
    bool joined[kBands] = {};

public:
    static const double* Prototype() {
        static const double proto[kTaps] = {
            0.00000000E+00,  1.56575398E-04,  3.43256425E-04,  5.54620202E-04,  8.23919506E-04,  1.13992507E-03,
            1.47640169E-03,  1.78371725E-03,  2.01182542E-03,  2.10371989E-03,  1.99454554E-03,  1.61656283E-03,
            9.02154502E-04, -1.78805361E-04, -1.64973098E-03, -3.49717454E-03,  5.65949473E-03,  8.02941163E-03,
            1.04584443E-02,  1.27472335E-02,  1.46525263E-02,  1.59045603E-02,  1.62208471E-02,  1.53184106E-02,
            1.29371806E-02,  8.85757540E-03,  2.92408442E-03, -4.91578024E-03, -1.46404076E-02, -2.61098752E-02,
           -3.90751381E-02, -5.31873032E-02,  6.79989431E-02,  8.29847578E-02,  9.75753918E-02,  1.11196689E-01,
            1.23264548E-01,  1.33264415E-01,  1.40753505E-01,  1.45389847E-01,  1.46955068E-01,  1.45389847E-01,
            1.40753505E-01,  1.33264415E-01,  1.23264548E-01,  1.11196689E-01,  9.75753918E-02,  8.29847578E-02,
           -6.79989431E-02, -5.31873032E-02, -3.90751381E-02, -2.61098752E-02, -1.46404076E-02, -4.91578024E-03,
            2.92408442E-03,  8.85757540E-03,  1.29371806E-02,  1.53184106E-02,  1.62208471E-02,  1.59045603E-02,
            1.46525263E-02,  1.27472335E-02,  1.04584443E-02,  8.02941163E-03, -5.65949473E-03, -3.49717454E-03,
           -1.64973098E-03, -1.78805361E-04,  9.02154502E-04,  1.61656283E-03,  1.99454554E-03,  2.10371989E-03,
            2.01182542E-03,  1.78371725E-03,  1.47640169E-03,  1.13992507E-03,  8.23919506E-04,  5.54620202E-04,
            3.43256425E-04,  1.56575398E-04,
        };
        return proto;
    }

private:
    static const double* Matrix() {
        static double m[kBands][kTaps];
        static bool built;
        if (built) return &m[0][0];
        const double* proto = Prototype();
        for (int band = 0; band < kBands; band++)
            for (int tap = 0; tap < kTaps; tap++)
                m[band][tap] = proto[tap] * (((tap / 16) & 1) ? -1.0 : 1.0) *
                               cos((band + 0.5) * (tap - kBands / 2.0) * 3.14159265358979 / kBands);
        built = true;
        return &m[0][0];
    }

    static int ScaleFactor(double maximum) {
        int f = 0;
        while (f < 15 && (1 << (f + 1)) <= maximum) f++;
        return f;
    }

    static void Crc(BYTE& crc, BYTE value) {
        for (int shift = 7; shift >= 0; shift--) {
            bool high = (((value >> shift) & 1) ^ (crc >> 7)) != 0;
            crc = (BYTE)(crc << 1);
            if (high) crc ^= 0x1D;
        }
    }

public:
    static const int kFrameBytes = 109, kSamples = 128;

    void Reset() { memset(history, 0, sizeof(history)); }

    void Encode(const short (*pcm)[2], BYTE* out) {
        const double* matrix = Matrix();
        for (int c = 0; c < 2; c++) {               // analysis filter bank
            for (int block = 0; block < kBlocks; block++) {
                memmove(&history[c][kBands], &history[c][0], sizeof(double) * (kTaps - kBands));
                for (int i = 0; i < kBands; i++) history[c][kBands - 1 - i] = pcm[block * kBands + i][c];
                for (int band = 0; band < kBands; band++) {
                    double v = 0;
                    const double* row = matrix + band * kTaps;
                    for (int tap = 0; tap < kTaps; tap++) v += row[tap] * history[c][tap];
                    samples[c][block * kBands + band] = v;
                }
            }
        }
        for (int band = 0; band < kBands - 1; band++) {     // joint stereo where mid/side is cheaper
            double maxL = 0, maxR = 0, maxSum = 0, maxDiff = 0;
            for (int block = 0; block < kBlocks; block++) {
                double l = samples[0][block * kBands + band], r = samples[1][block * kBands + band];
                maxL = max(maxL, fabs(l)); maxR = max(maxR, fabs(r));
                maxSum = max(maxSum, fabs((l + r) * 0.5)); maxDiff = max(maxDiff, fabs((l - r) * 0.5));
            }
            joined[band] = ScaleFactor(maxSum) + ScaleFactor(maxDiff) < ScaleFactor(maxL) + ScaleFactor(maxR);
            if (!joined[band]) continue;
            for (int block = 0; block < kBlocks; block++) {
                double l = samples[0][block * kBands + band], r = samples[1][block * kBands + band];
                samples[0][block * kBands + band] = (l + r) * 0.5;
                samples[1][block * kBands + band] = (l - r) * 0.5;
            }
        }
        joined[kBands - 1] = false;
        int maxNeed = 0;
        for (int c = 0; c < 2; c++) {
            for (int band = 0; band < kBands; band++) {
                double m = 0;
                for (int block = 0; block < kBlocks; block++) m = max(m, fabs(samples[c][block * kBands + band]));
                scale[c][band] = ScaleFactor(m);
                maxNeed = max(maxNeed, scale[c][band]);
            }
        }
        // SNR bit allocation over both channels.
        int count = 0, slice = 0, bitSlice = maxNeed + 1;
        do {
            bitSlice--;
            count += slice;
            slice = 0;
            for (int c = 0; c < 2; c++)
                for (int band = 0; band < kBands; band++) {
                    int need = scale[c][band];
                    if (need > bitSlice + 1 && need < bitSlice + 16) slice++;
                    else if (need == bitSlice + 1) slice += 2;
                }
        } while (count + slice < kBitpool);
        if (count + slice == kBitpool) { count += slice; bitSlice--; }
        for (int c = 0; c < 2; c++)
            for (int band = 0; band < kBands; band++) {
                int need = scale[c][band];
                bits[c][band] = need < bitSlice + 2 ? 0 : min(need - bitSlice, 16);
            }
        for (int pass = 0; pass < 2; pass++) {
            int c = 0, band = 0;
            while (count < kBitpool && band < kBands) {
                int b = bits[c][band];
                if (pass == 0) {
                    if (b >= 2 && b < 16) { bits[c][band]++; count++; }
                    else if (scale[c][band] == bitSlice + 1 && count + 1 < kBitpool) { bits[c][band] = 2; count += 2; }
                } else if (b < 16) {
                    bits[c][band]++;
                    count++;
                }
                if (++c == 2) { c = 0; band++; }
            }
        }
        // Write the frame.
        memset(out, 0, kFrameBytes);
        int bit = 0;
        auto put = [&](unsigned value, int n) {
            for (int i = n - 1; i >= 0 && bit < kFrameBytes * 8; i--, bit++)
                if ((value >> i) & 1) out[bit >> 3] |= (BYTE)(0x80 >> (bit & 7));
        };
        put(0x9C, 8);                   // sync
        put(0x7F, 8);                   // 32 kHz, 16 blocks, joint stereo, SNR, 8 subbands
        put(kBitpool, 8);
        put(0, 8);                      // CRC, below
        for (int band = 0; band < kBands; band++) put(joined[band] ? 1 : 0, 1);
        for (int c = 0; c < 2; c++)
            for (int band = 0; band < kBands; band++) put(scale[c][band], 4);
        for (int block = 0; block < kBlocks; block++)
            for (int c = 0; c < 2; c++)
                for (int band = 0; band < kBands; band++) {
                    int b = bits[c][band];
                    if (!b) continue;
                    unsigned levels = (1u << b) - 1;
                    double limit = (double)(1 << (scale[c][band] + 1));
                    long long q = (long long)((samples[c][block * kBands + band] / limit + 1.0) * levels * 0.5);
                    put((unsigned)max(0LL, min(q, (long long)levels)), b);
                }
        BYTE crc = 0x0F;
        Crc(crc, out[1]);
        Crc(crc, out[2]);
        for (int i = 4; i <= 12; i++) Crc(crc, out[i]);
        out[3] = crc;
    }
};

}   // namespace audio

// ---------------------------------------------------------------------------------------
// The headset microphone over Bluetooth. While it's on (byte 2 = 0xA1 in the output reports,
// renewed at least every ~4 s, here every 33 ms), the controller sends reports 0x12..0x19:
// the state (if byte 1 bit 7), then from byte 78 (or 3 without state) a frame number, a target
// byte and SBC or mSBC frames, 16 kHz mono. They're decoded and played into a playback device
// (VB-Audio's Hi-Fi Cable by default), whose recording side apps then use as the microphone.
// Layout from PadForge's and hbashton's notes (github.com/hifihedgehog/PadForge/issues/528).

namespace micout {

static const int kRate = 16000, kRing = 1 << 15;
static float g_ring[kRing];
static std::atomic<ULONGLONG> g_written{0};
static std::atomic<bool> g_running{false}, g_stop{false}, g_reopen{false};
static std::atomic<long> g_error{0};
static std::atomic<void*> g_owner{nullptr};    // the one controller whose mic plays
static bool g_dry;                              // testing (DS4NATIVE_MICDRY): decode and count, play nothing
static HANDLE g_thread;
static SRWLOCK g_lock = SRWLOCK_INIT;
static std::wstring g_dest, g_deviceName;       // "" picks Hi-Fi Cable Input

static std::wstring Dest() { AcquireSRWLockShared(&g_lock); std::wstring s = g_dest; ReleaseSRWLockShared(&g_lock); return s; }
static std::wstring PlayingTo() { AcquireSRWLockShared(&g_lock); std::wstring s = g_deviceName; ReleaseSRWLockShared(&g_lock); return s; }

static void SetDest(const std::wstring& id) {
    AcquireSRWLockExclusive(&g_lock);
    g_dest = id;
    ReleaseSRWLockExclusive(&g_lock);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"MicDestination", REG_SZ, id.c_str(), (DWORD)((id.size() + 1) * sizeof(wchar_t)));
    g_reopen = true;
}

static void LoadDest() {
    wchar_t buf[512];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"MicDestination", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) g_dest = buf;
}

static void Push(const float* samples, int n) {
    ULONGLONG w = g_written.load();
    for (int i = 0; i < n; i++) g_ring[(w + i) & (kRing - 1)] = samples[i];
    g_written.store(w + n);
}

static DWORD WINAPI RenderThread(LPVOID) {
    if (g_dry) {
        g_running = true;
        while (!g_stop) Sleep(50);
        g_running = false;
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    timeBeginPeriod(1);
    while (!g_stop) {
        IMMDeviceEnumerator* en = nullptr;
        IMMDevice* dev = nullptr;
        IAudioClient* client = nullptr;
        IAudioRenderClient* render = nullptr;
        WAVEFORMATEX* fmt = nullptr;
        g_reopen = false;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en);
        if (SUCCEEDED(hr)) {
            std::wstring id = Dest();
            if (id.empty()) {
                for (const audio::Device& d : audio::ListDevices(en))
                    if (d.name.find(L"Hi-Fi Cable Input") != std::wstring::npos) { id = d.id; break; }
            }
            hr = id.empty() ? audio::kNoDevice : en->GetDevice(id.c_str(), &dev);
        }
        AcquireSRWLockExclusive(&g_lock);
        g_deviceName = dev ? audio::DeviceName(dev) : L"";
        ReleaseSRWLockExclusive(&g_lock);
        if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
        if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
        bool isFloat = false;
        if (SUCCEEDED(hr)) {
            isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                      (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && ((const WAVEFORMATEXTENSIBLE*)fmt)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
            if (!isFloat && fmt->wBitsPerSample != 16) hr = E_NOTIMPL;
        }
        if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, fmt, nullptr);
        if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&render);
        UINT32 bufferFrames = 0;
        if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufferFrames);
        if (SUCCEEDED(hr)) hr = client->Start();
        g_error = SUCCEEDED(hr) ? 0 : hr;
        g_running = SUCCEEDED(hr);
        if (SUCCEEDED(hr)) {
            int channels = fmt->nChannels;
            double step = (double)kRate / fmt->nSamplesPerSec, at = 0;     // ring position, fractional
            bool started = false;
            while (!g_stop && !g_reopen) {
                Sleep(5);
                UINT32 padding = 0;
                if (FAILED(client->GetCurrentPadding(&padding))) break;
                UINT32 room = bufferFrames - padding;
                if (!room) continue;
                ULONGLONG w = g_written.load();
                // About 60 ms behind the controller; catch up if it falls far behind.
                if (!started || at > (double)w || (double)w - at > 8000) { at = w > 960 ? (double)(w - 960) : 0; started = true; }
                BYTE* data;
                if (FAILED(render->GetBuffer(room, &data))) break;
                for (UINT32 i = 0; i < room; i++) {
                    float v = 0;
                    if (at + 1 < (double)w) {
                        ULONGLONG k = (ULONGLONG)at;
                        float frac = (float)(at - k);
                        v = g_ring[k & (kRing - 1)] * (1 - frac) + g_ring[(k + 1) & (kRing - 1)] * frac;
                        at += step;
                    }
                    for (int c = 0; c < channels; c++) {
                        if (isFloat) ((float*)data)[i * channels + c] = v;
                        else ((short*)data)[i * channels + c] = (short)max(-32768.0f, min(32767.0f, v * 32767));
                    }
                }
                render->ReleaseBuffer(room, 0);
            }
            client->Stop();
        }
        g_running = false;
        if (fmt) CoTaskMemFree(fmt);
        if (render) render->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        if (en) en->Release();
        for (int i = 0; i < 40 && !g_stop && !g_reopen; i++) Sleep(50);
    }
    timeEndPeriod(1);
    CoUninitialize();
    return 0;
}

static void Want(bool want) {
    if (want && !g_thread) {
        g_stop = false;
        g_thread = CreateThread(nullptr, 0, RenderThread, nullptr, 0, nullptr);
    } else if (!want && g_thread) {
        g_stop = true;
        WaitForSingleObject(g_thread, 3000);
        CloseHandle(g_thread);
        g_thread = nullptr;
        g_running = false;
    }
}

}   // namespace micout

// SBC decoder (Bluetooth A2DP specification, SBC; mSBC is SBC with fixed settings): frames of
// 8 subbands, any mode; mono out (stereo is averaged). Samples come out scaled to -1..1.
class SbcDecoder {
    double v[2][160] = {};

    static void Allocate(const int scf[2][8], int bits[2][8], int channels, int mode, bool loudness, int fsIndex, int bitpool) {
        static const int offset8[4][8] = { { -2, 0, 0, 0, 0, 0, 0, 1 }, { -3, 0, 0, 0, 0, 0, 1, 2 },
                                           { -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 } };
        int need[2][8];
        for (int c = 0; c < channels; c++)
            for (int sb = 0; sb < 8; sb++) {
                if (!loudness) { need[c][sb] = scf[c][sb]; continue; }
                if (scf[c][sb] == 0) { need[c][sb] = -5; continue; }
                int l = scf[c][sb] - offset8[fsIndex][sb];
                need[c][sb] = l > 0 ? l / 2 : l;
            }
        // Mono and dual channel allocate per channel; stereo and joint stereo over both.
        int groups = (mode == 0 || mode == 1) ? channels : 1, perGroup = (mode == 0 || mode == 1) ? 1 : channels;
        for (int gidx = 0; gidx < groups; gidx++) {
            int c0 = gidx, cn = perGroup;
            int maxNeed = -100;
            for (int c = c0; c < c0 + cn; c++) for (int sb = 0; sb < 8; sb++) maxNeed = max(maxNeed, need[c][sb]);
            int count = 0, slice = 0, bitSlice = maxNeed + 1;
            do {
                bitSlice--;
                count += slice;
                slice = 0;
                for (int c = c0; c < c0 + cn; c++)
                    for (int sb = 0; sb < 8; sb++) {
                        if (need[c][sb] > bitSlice + 1 && need[c][sb] < bitSlice + 16) slice++;
                        else if (need[c][sb] == bitSlice + 1) slice += 2;
                    }
            } while (count + slice < bitpool);
            if (count + slice == bitpool) { count += slice; bitSlice--; }
            for (int c = c0; c < c0 + cn; c++)
                for (int sb = 0; sb < 8; sb++) bits[c][sb] = need[c][sb] < bitSlice + 2 ? 0 : min(need[c][sb] - bitSlice, 16);
            for (int pass = 0; pass < 2; pass++) {
                int c = c0, sb = 0;
                while (count < bitpool && sb < 8) {
                    if (pass == 0) {
                        if (bits[c][sb] >= 2 && bits[c][sb] < 16) { bits[c][sb]++; count++; }
                        else if (need[c][sb] == bitSlice + 1 && count + 1 < bitpool) { bits[c][sb] = 2; count += 2; }
                    } else if (bits[c][sb] < 16) {
                        bits[c][sb]++;
                        count++;
                    }
                    if (++c == c0 + cn) { c = c0; sb++; }
                }
            }
        }
    }

public:
    // The length of the frame at f (0 if it isn't one).
    static int FrameLength(const BYTE* f, int avail) {
        if (avail < 4) return 0;
        if (f[0] == 0xAD) return 57;
        if (f[0] != 0x9C || !(f[1] & 1)) return 0;
        int blocks = 4 * (((f[1] >> 4) & 3) + 1), mode = (f[1] >> 2) & 3, bitpool = f[2], channels = mode == 0 ? 1 : 2;
        int len = 4 + 4 * 8 * channels / 8;
        if (mode == 0 || mode == 1) len += (blocks * channels * bitpool + 7) / 8;
        else len += ((mode == 3 ? 8 : 0) + blocks * bitpool + 7) / 8;
        return len;
    }

    // Decodes one frame; returns the number of samples written to out (up to 128), 0 if bad.
    int Decode(const BYTE* f, int len, float* out) {
        int fsIndex, blocks, mode, bitpool;
        bool loudness;
        if (f[0] == 0xAD) { fsIndex = 0; blocks = 15; mode = 0; loudness = true; bitpool = 26; }
        else {
            fsIndex = f[1] >> 6; blocks = 4 * (((f[1] >> 4) & 3) + 1); mode = (f[1] >> 2) & 3;
            loudness = !((f[1] >> 1) & 1); bitpool = f[2];
        }
        if (FrameLength(f, len) != len) return 0;
        int channels = mode == 0 ? 1 : 2;
        int bit = 32;
        auto get = [&](int n) {
            unsigned v = 0;
            for (int i = 0; i < n; i++, bit++) {
                if ((bit >> 3) >= len) return v;
                v = (v << 1) | ((f[bit >> 3] >> (7 - (bit & 7))) & 1);
            }
            return v;
        };
        bool join[8] = {};
        if (mode == 3) for (int sb = 0; sb < 8; sb++) join[sb] = get(1) != 0;
        int scf[2][8] = {}, bits[2][8] = {};
        for (int c = 0; c < channels; c++) for (int sb = 0; sb < 8; sb++) scf[c][sb] = (int)get(4);
        Allocate(scf, bits, channels, mode, loudness, fsIndex, bitpool);
        double sample[16][2][8] = {};
        for (int blk = 0; blk < blocks; blk++)
            for (int c = 0; c < channels; c++)
                for (int sb = 0; sb < 8; sb++) {
                    if (!bits[c][sb]) continue;
                    unsigned q = get(bits[c][sb]);
                    double levels = (double)((1u << bits[c][sb]) - 1);
                    sample[blk][c][sb] = (double)(1 << (scf[c][sb] + 1)) * ((q * 2.0 + 1.0) / levels - 1.0);
                }
        if (mode == 3)
            for (int blk = 0; blk < blocks; blk++)
                for (int sb = 0; sb < 8; sb++) {
                    if (!join[sb]) continue;
                    double m = sample[blk][0][sb], s = sample[blk][1][sb];
                    sample[blk][0][sb] = m + s;
                    sample[blk][1][sb] = m - s;
                }
        // Synthesis filter bank.
        static double window[80], matrix[16][8];
        static bool built;
        if (!built) {
            const double* proto = audio::Sbc::Prototype();
            for (int i = 0; i < 80; i++) window[i] = -proto[i] * 8;     // -M x the analysis window
            for (int k = 0; k < 16; k++)
                for (int i = 0; i < 8; i++) matrix[k][i] = cos((i + 0.5) * (k + 4) * 3.14159265358979 / 8);
            built = true;
        }
        for (int blk = 0; blk < blocks; blk++) {
            double mix[8] = {};
            for (int c = 0; c < channels; c++) {
                double* V = v[c];
                memmove(V + 16, V, sizeof(double) * 144);
                for (int k = 0; k < 16; k++) {
                    double sum = 0;
                    for (int i = 0; i < 8; i++) sum += matrix[k][i] * sample[blk][c][i];
                    V[k] = sum;
                }
                double U[80];
                for (int i = 0; i < 5; i++)
                    for (int j = 0; j < 8; j++) { U[i * 16 + j] = V[i * 32 + j]; U[i * 16 + 8 + j] = V[i * 32 + 24 + j]; }
                for (int j = 0; j < 8; j++) {
                    double x = 0;
                    for (int i = 0; i < 10; i++) x += U[j + 8 * i] * window[j + 8 * i];
                    mix[j] += x / channels;
                }
            }
            for (int j = 0; j < 8; j++) out[blk * 8 + j] = (float)(mix[j] / 32768.0);
        }
        return blocks * 8;
    }
};

// The controller's sound: report 0x17 with four SBC frames (16 ms), counted in frames.
struct PadAudio {
    audio::Reader reader;
    audio::Sbc sbc;
    USHORT frame = 0;
    double next = 0;                    // ms (QPC) when the next report is due
};

static double NowMs() {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return c.QuadPart * 1000.0 / freq.QuadPart;
}

static void SendAudio(Pad* p, PadAudio& a) {
    short pcm[audio::kChunk][2];
    a.reader.Take(pcm);
    float level = p->audioMuted ? 0.0f : p->set[S_AUDIOVOL] / 100.0f, gain = level * level;    // a gentle taper
    if (gain < 0.999f)
        for (int i = 0; i < audio::kChunk; i++) { pcm[i][0] = (short)(pcm[i][0] * gain); pcm[i][1] = (short)(pcm[i][1] * gain); }
    BYTE report[462] = {};
    report[0] = 0x17;
    report[1] = (BYTE)(0x40 | p->poll);
    report[2] = p->micOn ? 0xA1 : 0xA0; // keeps the controller sending input while it plays
    report[3] = (BYTE)a.frame;
    report[4] = (BYTE)(a.frame >> 8);
    report[5] = p->set[S_AUDIOOUT] == AUDIO_SPEAKER ? 0x02 : 0x24;
    for (int i = 0; i < 4; i++) a.sbc.Encode(pcm + i * audio::Sbc::kSamples, report + 6 + i * audio::Sbc::kFrameBytes);
    a.frame += 4;
    SignReport(report, sizeof(report));
    if (WriteReport(p, report, sizeof(report), false)) p->audioSent++;
    else p->audioFailed++;
}

// ---- a controller's thread

static std::wstring HexOnly(const wchar_t* s) {
    std::wstring out;
    for (; *s; s++) if (iswxdigit(*s)) out += (wchar_t)towlower(*s);
    return out;
}

static GenericAxis FindAxis(const HIDP_VALUE_CAPS* caps, int count, USHORT page, USHORT usage) {
    GenericAxis a;
    for (int i = 0; i < count; i++) {
        const HIDP_VALUE_CAPS& c = caps[i];
        USHORT lo = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage, hi = c.IsRange ? c.Range.UsageMax : c.NotRange.Usage;
        if (c.UsagePage != page || usage < lo || usage > hi) continue;
        a.page = page; a.usage = usage; a.present = true;
        a.lo = c.LogicalMin; a.hi = c.LogicalMax;
        if (a.hi <= a.lo) { a.lo = 0; a.hi = (1 << min(c.BitSize, (USHORT)16)) - 1; }    // unsigned field
        break;
    }
    return a;
}

static bool OpenGeneric(Pad* p) {
    p->h = CreateFileW(p->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (p->h == INVALID_HANDLE_VALUE)       // some only open for reading
        p->h = CreateFileW(p->path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED, nullptr);
    if (p->h == INVALID_HANDLE_VALUE) { g_openError = GetLastError(); return false; }
    GenericLayout& L = p->layout;
    if (!HidD_GetPreparsedData(p->h, &L.parsed)) return false;
    HIDP_CAPS caps{};
    HidP_GetCaps(L.parsed, &caps);
    HidD_SetNumInputBuffers(p->h, 4);
    p->inLen = caps.InputReportByteLength;
    p->outLen = caps.OutputReportByteLength;
    HIDP_VALUE_CAPS values[64];
    USHORT count = min(caps.NumberInputValueCaps, (USHORT)64);
    HidP_GetValueCaps(HidP_Input, values, &count, L.parsed);
    L.lx = FindAxis(values, count, 1, 0x30);
    L.ly = FindAxis(values, count, 1, 0x31);
    GenericAxis z = FindAxis(values, count, 1, 0x32), rz = FindAxis(values, count, 1, 0x35);
    GenericAxis rxAxis = FindAxis(values, count, 1, 0x33), ryAxis = FindAxis(values, count, 1, 0x34);
    if (z.present && rz.present) {          // right stick on Z / Rz; Rx / Ry (if any) as triggers
        L.rx = z; L.ry = rz;
        L.lt = rxAxis; L.rt = ryAxis;
    } else {
        L.rx = rxAxis; L.ry = ryAxis;
        L.lt = z; L.rt = rz;
    }
    GenericAxis brake = FindAxis(values, count, 2, 0xC5), gas = FindAxis(values, count, 2, 0xC4);
    if (brake.present) L.lt = brake;
    if (gas.present) L.rt = gas;
    L.hat = FindAxis(values, count, 1, 0x39);
    // A battery level, where the HID standard puts one: Generic Device Controls "Battery
    // Strength", or the battery system page's state of charge / remaining capacity.
    static const USHORT kBattery[][2] = { { 0x06, 0x20 }, { 0x85, 0x65 }, { 0x85, 0x66 }, { 0x84, 0x65 }, { 0x84, 0x66 } };
    for (auto& b : kBattery) if (!L.battery.present) L.battery = FindAxis(values, count, b[0], b[1]);
    HIDP_VALUE_CAPS features[64];
    USHORT featureCount = min(caps.NumberFeatureValueCaps, (USHORT)64);
    if (featureCount && HidP_GetValueCaps(HidP_Feature, features, &featureCount, L.parsed) == HIDP_STATUS_SUCCESS) {
        for (auto& b : kBattery) {
            if (L.battery.present || L.batteryFeature.present) break;
            L.batteryFeature = FindAxis(features, featureCount, b[0], b[1]);
            for (int i = 0; i < featureCount && L.batteryFeature.present; i++)
                if (features[i].UsagePage == b[0]) { L.batteryReport = features[i].ReportID; break; }
        }
    } else {
        featureCount = 0;
    }
    // What it reports (Driver status): input values, buttons, feature values.
    wchar_t field[48];
    L.fields = L"inputs";
    for (int i = 0; i < count; i++) {
        swprintf_s(field, L" %02X:%02X", values[i].UsagePage, values[i].IsRange ? values[i].Range.UsageMin : values[i].NotRange.Usage);
        L.fields += field;
    }
    HIDP_BUTTON_CAPS buttonCaps[8];
    USHORT buttonCount = min(caps.NumberInputButtonCaps, (USHORT)8);
    if (buttonCount && HidP_GetButtonCaps(HidP_Input, buttonCaps, &buttonCount, L.parsed) == HIDP_STATUS_SUCCESS)
        for (int i = 0; i < buttonCount; i++) {
            const HIDP_BUTTON_CAPS& b = buttonCaps[i];
            swprintf_s(field, L", buttons %02X:%d-%d", b.UsagePage, b.IsRange ? b.Range.UsageMin : b.NotRange.Usage,
                       b.IsRange ? b.Range.UsageMax : b.NotRange.Usage);
            L.fields += field;
        }
    if (featureCount) {
        L.fields += L"; features";
        for (int i = 0; i < featureCount; i++) {
            swprintf_s(field, L" %02X:%02X(id %d)", features[i].UsagePage, features[i].IsRange ? features[i].Range.UsageMin : features[i].NotRange.Usage,
                       features[i].ReportID);
            L.fields += field;
        }
    }
    L.map = p->vid == 0x2DC8 ? k8BitDoMap : kStandardMap;
    wchar_t serial[64] = L"";
    if (HidD_GetSerialNumberString(p->h, serial, sizeof(serial))) p->serial = HexOnly(serial);
    if (p->serial.size() != 12) {
        wchar_t id[24];
        swprintf_s(id, L"hid%08x", Crc32((const BYTE*)p->instanceId.c_str(), p->instanceId.size() * sizeof(wchar_t)));
        p->serial = id;
    }
    // Over Bluetooth many pads call themselves something generic ("Bluetooth Wireless
    // Controller"): the name Windows paired it under is the better one.
    ULONGLONG address;
    if (p->bluetooth && SerialToAddress(p->serial, &address)) {
        BLUETOOTH_FIND_RADIO_PARAMS params{ sizeof(params) };
        HANDLE radio;
        HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
        if (find) {
            BLUETOOTH_DEVICE_INFO info{ sizeof(info) };
            info.Address.ullLong = address;
            if (BluetoothGetDeviceInfo(radio, &info) == ERROR_SUCCESS && info.szName[0]) p->name = info.szName;
            CloseHandle(radio);
            BluetoothFindRadioClose(find);
        }
    }
    g_openError = 0;
    return true;
}

// A generic pad's report, into the same state a DualShock 4's goes into.
static bool ParseGeneric(Pad* p, BYTE* report, DWORD n, PadState& s) {
    GenericLayout& L = p->layout;
    if (!L.parsed) return false;
    auto value = [&](const GenericAxis& a, BYTE fallback) -> BYTE {
        if (!a.present) return fallback;
        ULONG raw = 0;
        if (HidP_GetUsageValue(HidP_Input, a.page, 0, a.usage, &raw, L.parsed, (PCHAR)report, n) != HIDP_STATUS_SUCCESS) return fallback;
        LONG v = (LONG)raw;
        if (a.lo < 0 && v > a.hi) v -= (LONG)(a.hi - a.lo + 1);     // signed field read unsigned
        double t = (double)(v - a.lo) / (double)(a.hi - a.lo);
        return (BYTE)max(0.0, min(255.0, t * 255.0 + 0.5));
    };
    USAGE pressed[64];
    ULONG count = 64;
    DWORD held = 0;                         // HID button usages 1..32 held
    if (HidP_GetUsages(HidP_Input, 9, 0, pressed, &count, L.parsed, (PCHAR)report, n) == HIDP_STATUS_SUCCESS)
        for (ULONG i = 0; i < count; i++) if (pressed[i] >= 1 && pressed[i] <= 32) held |= 1u << (pressed[i] - 1);
    auto on = [&](USHORT usage) { return usage && (held & (1u << (usage - 1))); };
    const GenericMap& m = L.map;
    DWORD buttons = 0;
    if (on(m.a)) buttons |= B_CROSS;
    if (on(m.b)) buttons |= B_CIRCLE;
    if (on(m.x)) buttons |= B_SQUARE;
    if (on(m.y)) buttons |= B_TRIANGLE;
    if (on(m.lb)) buttons |= B_L1;
    if (on(m.rb)) buttons |= B_R1;
    if (on(m.lt)) buttons |= B_L2;
    if (on(m.rt)) buttons |= B_R2;
    if (on(m.back)) buttons |= B_SHARE;
    if (on(m.start)) buttons |= B_OPTIONS;
    if (on(m.guide)) buttons |= B_PS;
    if (on(m.ls)) buttons |= B_L3;
    if (on(m.rs)) buttons |= B_R3;
    if (L.hat.present) {                    // 8 directions from the bottom of the range; anything else is centred
        ULONG raw = 0;
        if (HidP_GetUsageValue(HidP_Input, 1, 0, 0x39, &raw, L.parsed, (PCHAR)report, n) == HIDP_STATUS_SUCCESS) {
            LONG dir = (LONG)raw - L.hat.lo;
            static const DWORD kHat[8] = { B_UP, B_UP | B_RIGHT, B_RIGHT, B_RIGHT | B_DOWN, B_DOWN, B_DOWN | B_LEFT, B_LEFT, B_LEFT | B_UP };
            if (dir >= 0 && dir < 8 && L.hat.hi - L.hat.lo >= 7) buttons |= kHat[dir];
        }
    }
    s.buttons = buttons;
    s.lx = value(L.lx, 128); s.ly = value(L.ly, 128);
    s.rx = value(L.rx, 128); s.ry = value(L.ry, 128);
    s.l2 = max(value(L.lt, 0), (BYTE)((buttons & B_L2) ? 255 : 0));
    s.r2 = max(value(L.rt, 0), (BYTE)((buttons & B_R2) ? 255 : 0));
    s.imu = false;
    if (L.battery.present) s.battery = (int)(value(L.battery, 0) * 100 / 255);     // else the feature report's, or unknown
    return true;
}

static bool OpenPad(Pad* p) {
    if (p->generic) return OpenGeneric(p);
    wchar_t pollEnv[16];
    if (GetEnvironmentVariableW(L"DS4NATIVE_POLL", pollEnv, 16)) { p->poll = _wtoi(pollEnv); p->pollFixed = true; }   // testing
    p->h = CreateFileW(p->path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (p->h == INVALID_HANDLE_VALUE) {
        g_openError = GetLastError();
        return false;
    }
    PHIDP_PREPARSED_DATA parsed;
    HIDP_CAPS caps{};
    if (!HidD_GetPreparsedData(p->h, &parsed)) return false;
    HidP_GetCaps(parsed, &caps);
    HidD_FreePreparsedData(parsed);
    if (caps.UsagePage != 1 || caps.Usage != 5 || caps.InputReportByteLength < 10) return false;   // not the gamepad
    HidD_SetNumInputBuffers(p->h, 4);       // a short queue: never work through a backlog of old reports
    p->inLen = caps.InputReportByteLength;
    p->outLen = caps.OutputReportByteLength;
    p->featLen = caps.FeatureReportByteLength;
    g_openError = 0;

    std::vector<BYTE> feature(max((int)p->featLen, 64));
    wchar_t serial[64] = L"";
    if (HidD_GetSerialNumberString(p->h, serial, sizeof(serial))) p->serial = HexOnly(serial);
    if (p->bluetooth) {
        feature[0] = 0x05;                  // asking for calibration switches on the extended reports
        HidD_GetFeature(p->h, feature.data(), p->featLen);
    } else if (p->serial.size() != 12) {
        feature[0] = 0x12;                  // the controller's Bluetooth address, backwards
        if (HidD_GetFeature(p->h, feature.data(), p->featLen)) {
            wchar_t mac[16];
            swprintf_s(mac, L"%02x%02x%02x%02x%02x%02x", feature[6], feature[5], feature[4], feature[3], feature[2], feature[1]);
            p->serial = mac;
        }
    }
    if (p->serial.size() != 12) {
        wchar_t id[16];
        swprintf_s(id, L"pad%08x", Crc32((const BYTE*)p->instanceId.c_str(), p->instanceId.size() * sizeof(wchar_t)));
        p->serial = id;
    }
    return true;
}

static bool ClaimSerial(Pad* p) {
    AcquireSRWLockExclusive(&g_serialLock);
    bool claimed = g_serials.insert(p->serial).second;
    ReleaseSRWLockExclusive(&g_serialLock);
    return claimed;
}

static void SetReady(Pad* p, bool ready) {
    if (p->ready.exchange(ready) != ready) PostMessageW(g_tray, WM_PADS, ready, 0);
}

// Mic frames in reports 0x12..0x19: decoded into the mic output (one controller at a time).
// Those reports also carry the sound being sent to the controller (32 kHz stereo SBC, as
// encoded here), so only microphone frames (mSBC, or 16 kHz mono SBC) are taken.
static bool MicFrame(const BYTE* f) {
    if (f[0] == 0xAD) return true;
    return f[0] == 0x9C && (f[1] >> 6) == 0 && ((f[1] >> 2) & 3) == 0;
}

static void TakeMic(Pad* p, const BYTE* b, DWORD n, SbcDecoder& decoder) {
    if (b[0] < 0x12 || b[0] > 0x19 || !(b[2] & 0x80) || !p->micOn) return;
    void* none = nullptr;
    if (micout::g_owner != p && !micout::g_owner.compare_exchange_strong(none, p)) return;
    int length = min((int)n, 78 + (b[0] - 0x11) * 64);
    int block = (b[1] & 0x80) ? 78 : 3;
    if (block + 3 >= length) return;
    p->micTarget = b[block + 2];
    int pos = block + 3, end = length - 4;     // after the frame number and target byte
    float pcm[128];
    while (pos < end) {
        int len = SbcDecoder::FrameLength(b + pos, end - pos);
        if (!len || pos + len > end) break;
        if (!MicFrame(b + pos)) { p->micEcho++; pos += len; continue; }
        int samples = decoder.Decode(b + pos, len, pcm);
        if (samples) { micout::Push(pcm, samples); p->micFrames++; }
        else p->micBad++;
        pos += len;
    }
}
// Everything sent to the controller, on its own thread so reading input never waits on a
// write: rumble and light bar when they change (at most every 10 ms, or 25 ms while the colour
// keeps changing), and the sound reports every 16 ms on a high-resolution timer.
static DWORD WINAPI OutputThread(LPVOID param) {
    Pad* p = (Pad*)param;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    PadOutput sent{};
    bool everSent = false, disarmAudio = false;
    int armedOut = -1, armedVolume = -1;    // what the sound was switched on with (-1: not on)
    bool micArm = false, micDisarm = false; // the microphone's on / off still to be sent
    ULONGLONG lastWrite = 0;
    PadAudio padAudio;
    while (!p->writerStop) {
        double wait = 10;
        if (p->audioOn && armedOut >= 0) wait = max(0.0, min(10.0, padAudio.next - NowMs()));
        if (wait > 0) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)(wait * 10000);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            HANDLE handles[2] = { p->outputWake, timer };
            WaitForMultipleObjects(2, handles, FALSE, 50);
        }
        if (p->writerStop) break;
        // Sound first: a late packet is a gap you hear, a late light-bar colour isn't.
        if (p->audioOn && armedOut >= 0) {
            double ms = NowMs();
            if (ms >= padAudio.next) {
                SendAudio(p, padAudio);
                p->audioDropouts = padAudio.reader.dropouts;
                padAudio.next += 16;
                if (ms - padAudio.next > 64) padAudio.next = ms + 16;  // fell far behind: don't burst
                continue;                       // another packet may be due already
            }
        }
        ULONGLONG now = GetTickCount64();
        AcquireSRWLockShared(&p->lock);
        int battery = p->state.battery;
        bool headsetMic = p->state.mic;
        ReleaseSRWLockShared(&p->lock);

        PadOutput out{};
        if (p->set[S_RUMBLE] && p->emulating) {
            int gain = p->set[S_RUMBLEGAIN];
            out.strong = (BYTE)min(255, p->rumbleStrong * gain / 100);
            out.weak = (BYTE)min(255, p->rumbleWeak * gain / 100);
        }
        if (now < p->testUntil) out.strong = out.weak = 200;
        BYTE rgb[3];
        LightColour(p, battery, rgb);
        int bright = p->set[S_BRIGHT];
        out.r = (BYTE)(rgb[0] * bright / 100); out.g = (BYTE)(rgb[1] * bright / 100); out.b = (BYTE)(rgb[2] * bright / 100);
        // Sound (Bluetooth only): switched on and off with the light bar's report.
        bool wantMic = p->bluetooth && p->ready && p->set[S_MIC] && headsetMic && micout::g_running;
        if (wantMic != p->micOn) {
            p->micOn = wantMic;
            micArm = wantMic;
            micDisarm = !wantMic;
            if (!wantMic && micout::g_owner == p) micout::g_owner = nullptr;
            if (!p->pollFixed) p->poll = wantMic || p->audioOn ? 0 : 4;
        }
        out.mic = p->micOn ? (micArm ? 1 : 2) : micDisarm ? 3 : 0;
        bool wantAudio = p->bluetooth && p->ready && p->set[S_AUDIO] && audio::g_running;
        if (wantAudio != p->audioOn) {
            p->audioOn = wantAudio;
            if (!p->pollFixed) p->poll = wantAudio || p->micOn ? 0 : 4;    // sent with the report below
            if (wantAudio) {
                padAudio.reader.started = false;
                padAudio.sbc.Reset();
                padAudio.next = NowMs() + 20;   // after the report that switches it on
            } else {
                disarmAudio = true;
            }
        }
        // The sound is switched on once (and again only when its output or volume changes);
        // the light bar's reports in between just keep it on.
        int outTo = p->set[S_AUDIOOUT] == AUDIO_SPEAKER ? 2 : 1, volume = 0x4F;     // the Volume setting is applied to the samples
        bool arming = false;
        if (p->audioOn) {
            arming = outTo != armedOut || volume != armedVolume;
            out.audio = arming ? (BYTE)outTo : 4;
            out.volume = (BYTE)volume;
        } else {
            armedOut = armedVolume = -1;
            if (disarmAudio) out.audio = 3;
        }
        int mode = p->set[S_LIGHTMODE];
        bool rumbleChanged = everSent && (out.weak != sent.weak || out.strong != sent.strong);
        ULONGLONG gap = rumbleChanged || arming ? 10 : p->audioOn ? 50 : mode == LIGHT_RAINBOW || mode == LIGHT_CHROMA ? 25 : 10;
        bool keepMic = p->micOn && now - lastWrite >= 33;       // the mic switches itself off without these
        if (p->ready && (!everSent || arming || keepMic || memcmp(&out, &sent, sizeof(out))) && (keepMic || now - lastWrite >= gap)) {
            bool ok = WriteOutput(p, out);
            if (ok && out.audio == 3) disarmAudio = false;
            if (ok && out.mic == 1) micArm = false;
            if (ok && out.mic == 3) micDisarm = false;
            if (ok && arming) {
                armedOut = outTo;
                armedVolume = volume;
                if (padAudio.next < NowMs()) padAudio.next = NowMs() + 4;
            }
            sent = out;
            everSent = true;
            lastWrite = now;
        }
    }
    if (everSent && (sent.strong || sent.weak || p->audioOn || p->micOn)) {     // leave it quiet
        sent.strong = sent.weak = 0;
        if (p->audioOn) sent.audio = 3;
        if (p->micOn) sent.mic = 3;
        WriteOutput(p, sent);
    }
    p->audioOn = false;
    p->micOn = false;
    if (micout::g_owner == p) micout::g_owner = nullptr;
    if (timer) CloseHandle(timer);
    return 0;
}

// ---- the touchpad as a volume slider (Sound tab; only while it isn't the mouse)

struct TouchVolume {
    bool  had[2] = {};
    BYTE  id[2] = {};
    float x[2] = {}, y[2] = {};
    IAudioEndpointVolume* endpoint = nullptr;   // Windows' default output, fetched as a finger lands
    float level = 0;                            // the controller's volume while sliding (finer than 1 %)
    bool  leveled = false;
    ULONGLONG shown = 0;
};

static IAudioEndpointVolume* DefaultVolume() {
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    IAudioEndpointVolume* volume = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void**)&en))) {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &dev))) {
            dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&volume);
            dev->Release();
        }
        en->Release();
    }
    return volume;
}

static void ShowVolume(IAudioEndpointVolume* volume) {
    float level = 0;
    BOOL muted = FALSE;
    volume->GetMasterVolumeLevelScalar(&level);
    volume->GetMute(&muted);
    PostMessageW(g_tray, WM_VOLUME, (WPARAM)(level * 100 + 0.5f), muted);
}

// Sliding right or up turns it up (one finger: about a full slide across for 0..100%; two
// fingers twice as fast); pressing the pad mutes or unmutes.
static void UpdateTouchVolume(Pad* p, TouchVolume& v, const PadState& s, DWORD before, ULONGLONG now) {
    int fingers = s.touch[0] + s.touch[1];
    if (!p->set[S_TPVOLUME] || p->set[S_TPMOUSE] || !fingers) {
        if (v.endpoint) { v.endpoint->Release(); v.endpoint = nullptr; }
        if (v.had[0] || v.had[1]) SavePadSettings(p);      // the controller's volume, once the finger lifts
        v.had[0] = v.had[1] = false;
        v.leveled = false;
        return;
    }
    // While sound goes to the controller (Bluetooth), that's what it controls: the Volume
    // setting on the Sound tab. Otherwise (USB, or no sound to it) Windows' own volume.
    bool own = p->audioOn;
    if (!own && !v.endpoint) v.endpoint = DefaultVolume();
    bool down = (s.buttons & B_TOUCH) != 0, wasDown = (before & B_TOUCH) != 0;
    if (down && !wasDown) {
        if (own) {
            p->audioMuted = !p->audioMuted;
            PostMessageW(g_tray, WM_VOLUME, p->set[S_AUDIOVOL], (p->audioMuted ? 1 : 0) | 2);
        } else if (v.endpoint) {
            BOOL muted = FALSE;
            v.endpoint->GetMute(&muted);
            v.endpoint->SetMute(!muted, nullptr);
            ShowVolume(v.endpoint);
        }
    }
    float dx = 0, dy = 0;
    int same = 0;
    for (int i = 0; i < 2; i++) {
        if (!s.touch[i] || !v.had[i] || v.id[i] != s.tid[i]) continue;
        dx += s.tx[i] - v.x[i];
        dy += s.ty[i] - v.y[i];
        same++;
    }
    if (same && !down) {
        float change = (dx - dy) / same * (fingers >= 2 ? 2.0f : 1.0f) / 1500.0f;
        if (own && change != 0) {
            v.level = max(0.0f, min(1.0f, (v.leveled ? v.level : p->set[S_AUDIOVOL] / 100.0f) + change));
            v.leveled = true;
            int percent = (int)(v.level * 100 + 0.5f);
            if (percent != p->set[S_AUDIOVOL]) {
                p->set[S_AUDIOVOL] = percent;
                if (change > 0) p->audioMuted = false;
                if (now - v.shown >= 40) { PostMessageW(g_tray, WM_VOLUME, percent, (p->audioMuted ? 1 : 0) | 2); v.shown = now; }
            }
        } else if (!own && v.endpoint && change != 0) {
            float level = 0;
            v.endpoint->GetMasterVolumeLevelScalar(&level);
            float next = max(0.0f, min(1.0f, level + change));
            if (next != level) {
                v.endpoint->SetMasterVolumeLevelScalar(next, nullptr);
                if (change > 0) v.endpoint->SetMute(FALSE, nullptr);
                if (now - v.shown >= 40) { ShowVolume(v.endpoint); v.shown = now; }
            }
        }
    }
    for (int i = 0; i < 2; i++) {
        v.had[i] = s.touch[i];
        v.id[i] = s.tid[i];
        if (s.touch[i]) { v.x[i] = s.tx[i]; v.y[i] = s.ty[i]; }
    }
}
// ---- battery life: the trend over this charge, counted in hours the controller was on (as
// in the Magic Mouse app): it starts from a typical drain (a DS4 lasts ~6 hours) or the rate
// learned on earlier charges, and moves to the measured one as the level drops. Kept per
// controller across restarts.

struct BatterySample { float hours; int level; };

struct BatteryLife {
    std::vector<BatterySample> samples;
    double hours = 0;                   // time on, this charge
    double learned = 0;                 // hours per %, from earlier charges (0: none yet)
    ULONGLONG tick = 0;
    bool charging = false;
};
static const double kTypicalHoursPerPercent = 0.06, kPriorWeightHours = 0.5, kSureAfterHours = 0.5;

static void SaveBatteryLife(Pad* p, const BatteryLife& b) {
    std::wstring key = std::wstring(kRegKey) + L"\\" + p->serial;
    static BatterySample none;
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), L"BatteryHistory", REG_BINARY, b.samples.empty() ? &none : b.samples.data(),
                    (DWORD)(b.samples.size() * sizeof(BatterySample)));
}

static void LoadBatteryLife(Pad* p, BatteryLife& b) {
    std::wstring key = std::wstring(kRegKey) + L"\\" + p->serial;
    BatterySample saved[64];
    DWORD size = sizeof(saved);
    if (RegGetValueW(HKEY_CURRENT_USER, key.c_str(), L"BatteryHistory", RRF_RT_REG_BINARY, nullptr, saved, &size) == ERROR_SUCCESS)
        b.samples.assign(saved, saved + size / sizeof(BatterySample));
    if (!b.samples.empty()) b.hours = b.samples.back().hours;
    b.learned = RegGet(p->serial.c_str(), L"BatteryMinutesPerTenPercent", 0) / 600.0;
}

static void UpdateBatteryLife(Pad* p, BatteryLife& b, const PadState& s, ULONGLONG now) {
    if (s.battery < 0 || (b.tick && now - b.tick < 1000)) return;
    double dt = b.tick ? min((now - b.tick) / 3600000.0, 5.0 / 3600) : 0;
    b.tick = now;
    if (s.charging) {                       // a new charge starts the trend again
        if (!b.charging) { b.samples.clear(); b.hours = 0; SaveBatteryLife(p, b); }
        b.charging = true;
        p->minutesLeft = -1;
        return;
    }
    b.charging = false;
    b.hours += dt;
    int level = s.battery;
    if (!b.samples.empty() && level >= b.samples.back().level + 25) { b.samples.clear(); b.hours = 0; }   // charged elsewhere
    if (b.samples.empty() || level != b.samples.back().level || b.hours - b.samples.back().hours >= 0.25) {
        b.samples.push_back({ (float)b.hours, level });
        if (b.samples.size() > 64) b.samples.erase(b.samples.begin());
        double span = b.hours - b.samples.front().hours, drop = b.samples.front().level - level;
        if (span >= 1 && drop >= 20) {      // remembered for the next charges
            b.learned = span / drop;
            RegPut(p->serial.c_str(), L"BatteryMinutesPerTenPercent", (DWORD)(b.learned * 600));
        }
        SaveBatteryLife(p, b);
    }
    double prior = b.learned > 0 ? b.learned : kTypicalHoursPerPercent;
    double span = b.hours - b.samples.front().hours, drop = max(0, b.samples.front().level - level);
    double perHour = (drop + kPriorWeightHours / prior) / (span + kPriorWeightHours);
    p->minutesLeft = (int)(level / perHour * 60);
    p->estimateSure = span >= kSureAfterHours;
}

static DWORD WINAPI PadThread(LPVOID param) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);      // the volume control
    Pad* p = (Pad*)param;
    bool claimed = false;
    if (OpenPad(p)) {
        // Plugged in while on Bluetooth, a controller shows up twice: the second waits its turn.
        while (!(claimed = ClaimSerial(p)) && !p->stop) Sleep(500);
    }
    if (claimed) {
        LoadPadSettings(p);
        OVERLAPPED ov{};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        std::vector<BYTE> buf(p->inLen + 1);
        bool pending = false, failed = false;
        ULONGLONG lastActive = GetTickCount64();
        LARGE_INTEGER freq, lastCount{};
        QueryPerformanceFrequency(&freq);
        PadState state;
        DWORD fired = 0, hold = 0;          // shortcuts held down (by index), buttons kept from the game
        DWORD psHeld = 0;                   // B_PS while it's being kept back
        ULONGLONG psDownAt = 0, guideUntil = 0;
        bool psComboUsed = false;
        const ULONGLONG kPsDelayMs = 250;
        TouchMouse touchMouse;
        TouchVolume touchVolume;
        SbcDecoder micDecoder;
        BatteryLife batteryLife;
        LoadBatteryLife(p, batteryLife);
        ULONGLONG batteryAsked = 0;         // generic pads with the battery in a feature report
        int genericBattery = -1;
        // Link quality: the reports' own counter says how many were sent; the share that
        // arrived, over ~2 s windows.
        ULONGLONG linkStart = GetTickCount64();
        int linkGot = 0, linkSent = 0;
        bool haveCounter = false;
        BYTE lastCounter = 0;
        int crcFails[9] = {};                // per report 0x11..0x19, in a row
        if (!p->dongle) SetReady(p, true);
        p->writerStop = false;
        HANDLE writer = p->generic ? nullptr : CreateThread(nullptr, 0, OutputThread, p, 0, nullptr);
        SetThreadPriority(writer, THREAD_PRIORITY_ABOVE_NORMAL);

        while (!p->stop && !failed) {
            DWORD n = 0;
            bool got = false;
            if (!pending) {
                ResetEvent(ov.hEvent);
                if (ReadFile(p->h, buf.data(), p->inLen, &n, &ov)) got = true;
                else if (GetLastError() == ERROR_IO_PENDING) pending = true;
                else failed = true;
            }
            if (pending && WaitForSingleObject(ov.hEvent, 50) == WAIT_OBJECT_0) {
                pending = false;
                if (GetOverlappedResult(p->h, &ov, &n, FALSE)) got = true;
                else failed = true;
            }
            ULONGLONG now = GetTickCount64();
            bool synced = true;
            PadState next = state;
            // Bluetooth reports end in a CRC-32 of 0xA1 + the report: a corrupted one is dropped, so
            // garbage never turns into button presses or clicks. (If a report kind never matches,
            // its length is guessed wrong: after 50 in a row it's taken unchecked.)
            if (got && p->bluetooth && !p->generic && n > 0 && buf[0] >= 0x11 && buf[0] <= 0x19) {
                DWORD len = buf[0] == 0x19 ? 547 : 78 + (buf[0] - 0x11) * 64;
                int& fails = crcFails[buf[0] - 0x11];
                if (len <= n && fails < 50) {
                    BYTE seed = 0xA1;
                    DWORD crc = ~Crc32(buf.data(), len - 4, Crc32(&seed, 1)), stored;
                    memcpy(&stored, buf.data() + len - 4, 4);
                    if (crc != stored) { fails++; p->crcBad++; got = false; }
                    else fails = 0;
                }
            }
            if (got && n > 0 && g_testMic) {    // testing: keep the first few reports of each kind
                AcquireSRWLockExclusive(&g_dumpLock);
                if (g_dump.size() < 40000 && g_dumpCount[buf[0]]++ < 3) {
                    char line[1200];
                    int at = sprintf_s(line, "report %02X len %lu:", buf[0], n);
                    for (DWORD i = 0; i < min(n, (DWORD)110) && at < 1150; i++) at += sprintf_s(line + at, sizeof(line) - at, " %02X", buf[i]);
                    g_dump += line;
                    g_dump += "\r\n";
                }
                ReleaseSRWLockExclusive(&g_dumpLock);
            }
            if (got && n > 0 && p->bluetooth && !p->generic) TakeMic(p, buf.data(), n, micDecoder);
            if (got && p->bluetooth && !p->generic && buf[0] >= 0x12 && buf[0] <= 0x19 && !(buf[1] & 0x80)) linkGot++;   // sound only
            if (got && n > 0 && (p->generic ? ParseGeneric(p, buf.data(), n, next) : ParseReport(p, buf.data(), n, next, &synced))) {
                if (p->dongle) SetReady(p, synced);
                if (synced) {
                    LARGE_INTEGER count;
                    QueryPerformanceCounter(&count);
                    float dt = lastCount.QuadPart ? min(0.1f, (float)(count.QuadPart - lastCount.QuadPart) / freq.QuadPart) : 0;
                    lastCount = count;
                    UpdateMotion(next, dt, p->motionReset.exchange(false));
                    if (p->generic && p->layout.batteryFeature.present && now - batteryAsked >= 30000) {
                        batteryAsked = now;
                        std::vector<BYTE> feature(512);
                        feature[0] = p->layout.batteryReport;
                        HIDP_CAPS caps{};
                        HidP_GetCaps(p->layout.parsed, &caps);
                        ULONG raw = 0;
                        const GenericAxis& a = p->layout.batteryFeature;
                        if (caps.FeatureReportByteLength && HidD_GetFeature(p->h, feature.data(), caps.FeatureReportByteLength) &&
                            HidP_GetUsageValue(HidP_Feature, a.page, 0, a.usage, &raw, p->layout.parsed, (PCHAR)feature.data(),
                                               caps.FeatureReportByteLength) == HIDP_STATUS_SUCCESS && a.hi > a.lo)
                            genericBattery = (int)max(0L, min(100L, ((LONG)raw - a.lo) * 100 / (a.hi - a.lo)));
                    }
                    if (p->generic && !p->layout.battery.present) next.battery = genericBattery;
                    if (next.imu || (p->generic && next.battery >= 0)) UpdateBatteryLife(p, batteryLife, next, now);
                    if (next.imu) {
                        int step = haveCounter ? ((next.counter - lastCounter) & 63) : 1;
                        linkSent += step ? step : 64;   // 0: a whole lap went missing
                        linkGot++;
                        lastCounter = next.counter;
                        haveCounter = true;
                    }
                    if (now - linkStart >= 2000) {
                        p->reportRate = (int)(linkGot * 1000 / (now - linkStart));
                        p->link = linkSent ? min(100, linkGot * 100 / linkSent) : -1;
                        linkStart = now;
                        linkGot = linkSent = 0;
                    }
                    bool active = next.buttons || next.l2 > 30 || next.r2 > 30 || abs(next.lx - 128) > 40 || abs(next.ly - 128) > 40
                                  || abs(next.rx - 128) > 40 || abs(next.ry - 128) > 40 || next.touch[0];
                    if (active) lastActive = now;

                    // Shortcuts fire once when all their buttons are down.
                    Shortcut list[kMaxShortcuts];
                    int count_ = 0;
                    AcquireSRWLockShared(&g_shortcutLock);
                    for (const Shortcut& s : g_shortcuts) if (count_ < kMaxShortcuts) list[count_++] = s;
                    ReleaseSRWLockShared(&g_shortcutLock);
                    hold &= next.buttons;
                    bool psInCombo = false;
                    for (int i = 0; i < count_; i++) {
                        psInCombo = psInCombo || (list[i].buttons & B_PS);
                        bool down = (next.buttons & list[i].buttons) == list[i].buttons;
                        if (!down) { fired &= ~(1u << i); continue; }
                        if ((fired & (1u << i)) || g_recording) continue;
                        fired |= 1u << i;
                        hold |= list[i].buttons;
                        if (list[i].buttons & B_PS) psComboUsed = true;
                        RunAction(p, (int)list[i].action, list[i].buttons);
                    }
                    // PS starts most combos, and Guide opens the Game Bar: while PS is in a
                    // shortcut, Guide only goes down after PS is held alone for a moment, and a
                    // quick tap that started no combo is sent as a short Guide press.
                    if (next.buttons & B_PS) {
                        if (!psDownAt) { psDownAt = now; psComboUsed = false; }
                    } else if (psDownAt) {
                        if (psInCombo && !psComboUsed && now - psDownAt < kPsDelayMs) guideUntil = now + 80;
                        psDownAt = 0;
                    }
                    psHeld = psInCombo && (next.buttons & B_PS) && now - psDownAt < kPsDelayMs ? B_PS : 0;
                    UpdateTouchMouse(p, touchMouse, next, state.buttons, now, dt);
                    if (!g_noInput) UpdateTouchVolume(p, touchVolume, next, state.buttons, now);
                    // The touchpad click is the mouse's, or mutes (touchpad volume).
                    DWORD touchHold = p->set[S_TPMOUSE] || p->set[S_TPVOLUME] ? B_TOUCH : 0;

                    if (memcmp(&next, &state, sizeof(state))) {
                        state = next;
                        AcquireSRWLockExclusive(&p->lock);
                        p->state = state;
                        ReleaseSRWLockExclusive(&p->lock);
                        p->seq++;
                    }
                    p->lastReport = now;
                    if (p->target) {
                        PadState sent_ = state;
                        if (now < guideUntil) sent_.buttons |= B_PS;
                        vigem_target_x360_update(g_vigem, p->target,
                                                 ToXbox(p, sent_, (hold | psHeld | touchHold) & ~(now < guideUntil ? B_PS : 0)));
                    }
                }
            }

            SetEmulating(p, g_vigem && g_enabled && p->set[S_EMULATE] && p->ready);

            if (p->bluetooth && p->set[S_IDLE] && now - lastActive > kIdleMs) {
                DisconnectBluetooth(p);
                lastActive = now;
            }
        }
        if (pending) {
            CancelIoEx(p->h, &ov);
            DWORD n;
            GetOverlappedResult(p->h, &ov, &n, TRUE);
        }
        SetEmulating(p, false);
        if (touchMouse.up) MouseInput(touchMouse.up);
        p->writerStop = true;               // the output thread stops rumble and sound, then ends
        SetEvent(p->outputWake);
        if (writer) {
            WaitForSingleObject(writer, 3000);
            CloseHandle(writer);
        }
        if (p->layout.parsed) { HidD_FreePreparsedData(p->layout.parsed); p->layout.parsed = nullptr; }
        CloseHandle(ov.hEvent);
        AcquireSRWLockExclusive(&g_serialLock);
        g_serials.erase(p->serial);
        ReleaseSRWLockExclusive(&g_serialLock);
    }
    if (p->h != INVALID_HANDLE_VALUE) CloseHandle(p->h);
    p->h = INVALID_HANDLE_VALUE;
    p->gone = true;
    SetReady(p, false);
    return 0;
}

// ---- finding controllers

struct FoundPad { std::wstring path, instanceId; USHORT pid; bool bluetooth; bool generic; std::wstring name; USHORT vid; };

static std::vector<FoundPad> FindPads() {
    std::vector<FoundPad> found;
    GUID hid;
    HidD_GetHidGuid(&hid);
    HDEVINFO info = SetupDiGetClassDevsW(&hid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) return found;
    SP_DEVICE_INTERFACE_DATA iface{ sizeof(iface) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(info, nullptr, &hid, i, &iface); i++) {
        BYTE buf[2048];
        auto* detail = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        detail->cbSize = sizeof(*detail);
        SP_DEVINFO_DATA dev{ sizeof(dev) };
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, sizeof(buf), nullptr, &dev)) continue;
        std::wstring path = detail->DevicePath;
        std::transform(path.begin(), path.end(), path.begin(), towlower);
        bool usb = path.find(L"vid_054c") != std::wstring::npos, bt = path.find(L"vid&0002054c") != std::wstring::npos;
        static const USHORT kPids[] = { 0x05C4, 0x09CC, 0x0BA0 };
        USHORT pid = 0;
        for (USHORT candidate : kPids) {
            wchar_t tag[16];
            swprintf_s(tag, usb ? L"pid_%04x" : L"pid&%04x", candidate);
            if ((usb || bt) && path.find(tag) != std::wstring::npos) pid = candidate;
        }
        wchar_t id[512] = L"";
        SetupDiGetDeviceInstanceIdW(info, &dev, id, 512, nullptr);
        if (pid) { found.push_back({ path, id, pid, bt, false, L"", 0x054C }); continue; }
        // Any other HID gamepad or joystick. Xbox-type pads (XInput) are left alone: games
        // know them already (their DirectInput mirror has "ig_" in its path).
        if (path.find(L"&ig_") != std::wstring::npos) continue;
        HANDLE h = CreateFileW(detail->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA parsed;
        HIDP_CAPS caps{};
        bool pad = false;
        if (HidD_GetPreparsedData(h, &parsed)) {
            pad = HidP_GetCaps(parsed, &caps) == HIDP_STATUS_SUCCESS && caps.UsagePage == 1 && (caps.Usage == 4 || caps.Usage == 5);
            HidD_FreePreparsedData(parsed);
        }
        wchar_t product[128] = L"";
        HIDD_ATTRIBUTES attrs{ sizeof(attrs) };
        if (pad) { HidD_GetProductString(h, product, sizeof(product)); HidD_GetAttributes(h, &attrs); }
        CloseHandle(h);
        if (!pad) continue;
        std::wstring name = product;
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty()) name = L"Game controller";
        bool overBluetooth = path.find(L"00001124-0000-1000-8000-00805f9b34fb") != std::wstring::npos;
        found.push_back({ path, id, attrs.ProductID, overBluetooth, true, name, attrs.VendorID });
    }
    SetupDiDestroyDeviceInfoList(info);
    return found;
}

static void DestroyPad(Pad* p) {
    p->stop = true;
    if (p->thread) {
        WaitForSingleObject(p->thread, 5000);
        CloseHandle(p->thread);
    }
    if (g_selected == p) g_selected = nullptr;
    if (p->outputWake) CloseHandle(p->outputWake);
    delete p;
}

static Pad* SelectedPad() {
    Pad* first = nullptr;
    for (Pad* p : g_pads) {
        if (!p->ready) continue;
        if (p == g_selected) return p;
        if (!first) first = p;
    }
    return g_selected = first;
}

static bool g_exiting;

// Controllers set to be hidden stay hidden while the app runs, connected or not: hidden only
// once a controller had connected, Steam (or anything else watching for controllers) could
// open it in the moment it appeared and keep it, HidHide or not, and then turn its touchpad
// into a second mouse fighting this one. Remembered across restarts; all shown again on exit.
static std::set<std::wstring> g_keepHidden;
static bool g_keepHiddenLoaded;

static void SaveKeepHidden() {
    std::vector<wchar_t> buf;
    for (const std::wstring& s : g_keepHidden) { buf.insert(buf.end(), s.begin(), s.end()); buf.push_back(0); }
    buf.push_back(0);
    RegSetKeyValueW(HKEY_CURRENT_USER, kRegKey, L"KeepHidden", REG_MULTI_SZ, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)));
}

static void SyncHiding() {
    if (!hidhide::g_ready || g_noInput) return;     // (a test run leaves hiding alone)
    if (!g_keepHiddenLoaded) {
        g_keepHiddenLoaded = true;
        wchar_t buf[8192];
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, kRegKey, L"KeepHidden", RRF_RT_REG_MULTI_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            for (const wchar_t* s = buf; *s; s += wcslen(s) + 1) g_keepHidden.insert(s);
    }
    bool changed = false;
    for (Pad* p : g_pads) {                 // what the connected controllers are set to
        if (!p->ready || p->fake || p->instanceId.empty()) continue;
        if (p->set[S_EMULATE] && p->set[S_HIDE]) changed |= g_keepHidden.insert(p->instanceId).second;
        else changed |= g_keepHidden.erase(p->instanceId) != 0;
    }
    if (changed) SaveKeepHidden();
    std::set<std::wstring> want;
    if (!g_exiting && g_enabled && g_vigem) want = g_keepHidden;
    hidhide::Sync(want);
}

static void ScanPads() {
    std::vector<FoundPad> found = FindPads();
    for (size_t i = 0; i < g_pads.size();) {
        Pad* p = g_pads[i];
        bool present = p->fake;
        for (const FoundPad& f : found) present = present || f.path == p->path;
        if (present && !p->gone) { i++; continue; }
        DestroyPad(p);
        g_pads.erase(g_pads.begin() + i);
    }
    for (const FoundPad& f : found) {
        bool known = false;
        for (Pad* p : g_pads) known = known || p->path == f.path;
        if (known) continue;
        Pad* p = new Pad;
        p->path = f.path;
        p->instanceId = f.instanceId;
        p->pid = f.pid;
        p->bluetooth = f.bluetooth;
        p->generic = f.generic;
        p->vid = f.vid;
        p->dongle = !f.generic && f.pid == 0x0BA0;
        p->name = f.generic ? f.name : f.pid == 0x09CC ? L"DualShock 4 v2" : L"DualShock 4";
        p->thread = CreateThread(nullptr, 0, PadThread, p, 0, nullptr);
        SetThreadPriority(p->thread, THREAD_PRIORITY_ABOVE_NORMAL);
        g_pads.push_back(p);
    }
}

// ---------------------------------------------------------------------------------------
// Settings window: tabs for the controllers on top; under them the header (name, battery,
// master switch, on-top and theme buttons), switches on the left and sliders on the right of
// the 3D controller, and the app's controls in the footer. Layout is in 96-dpi units.

namespace gp = Gdiplus;

static const int kClientW = 1100, kClientH = 700;
static const int kMargin = 24, kSideW = 272, kTabsH = 48, kHeaderH = 76, kFooterH = 72;
static const int kRowH = 58, kSwitchW = 46, kSwitchH = 28, kSliderRowH = 52;
static double g_dpi = 1.0;

static float S(double v) { return (float)(v * g_dpi); }

struct Palette {
    gp::Color bg, card, text, subtext, line, hover, switchOff, segment, segmentOn, body, bodyEdge, shadow;
};
static const Palette kLightPalette = {
    gp::Color(255, 242, 242, 247), gp::Color(215, 255, 255, 255), gp::Color(255, 28, 28, 30),
    gp::Color(255, 110, 110, 115), gp::Color(255, 229, 229, 234), gp::Color(255, 236, 236, 241),
    gp::Color(255, 214, 214, 219), gp::Color(255, 229, 229, 234), gp::Color(255, 255, 255, 255),
    gp::Color(255, 255, 255, 255), gp::Color(255, 209, 209, 214), gp::Color(40, 0, 0, 0),
};
static const Palette kDarkPalette = {
    gp::Color(255, 28, 28, 30), gp::Color(200, 44, 44, 46), gp::Color(255, 242, 242, 247),
    gp::Color(255, 152, 152, 157), gp::Color(255, 58, 58, 60), gp::Color(255, 58, 58, 60),
    gp::Color(255, 72, 72, 74), gp::Color(255, 58, 58, 60), gp::Color(255, 99, 99, 102),
    gp::Color(255, 44, 44, 46), gp::Color(255, 72, 72, 74), gp::Color(90, 0, 0, 0),
};
static gp::Color kAccent(255, 10, 132, 255);      // Windows' accent colour once it's read (LoadAccentColour)
static const gp::Color kGreen(255, 52, 199, 89), kWhite(255, 255, 255, 255);

// Text on the accent colour: white, or near-black on a light accent.
static gp::Color OnAccent() {
    float luma = 0.2126f * kAccent.GetR() + 0.7152f * kAccent.GetG() + 0.0722f * kAccent.GetB();
    return luma > 165 ? gp::Color(255, 28, 28, 30) : kWhite;
}
static const gp::Color kBatteryGreen(255, 52, 199, 89), kBatteryYellow(255, 255, 204, 0), kBatteryRed(255, 255, 59, 48);

static bool g_dark;
static const Palette& Pal() { return g_dark ? kDarkPalette : kLightPalette; }

static gp::Color Mix(gp::Color a, gp::Color b, float t) {
    auto m = [t](BYTE x, BYTE y) { return (BYTE)(x + (y - x) * t + 0.5f); };
    return gp::Color(m(a.GetA(), b.GetA()), m(a.GetR(), b.GetR()), m(a.GetG(), b.GetG()), m(a.GetB(), b.GetB()));
}

static gp::Color WithAlpha(gp::Color c, int alpha) {
    return gp::Color((BYTE)max(0, min(255, alpha)), c.GetR(), c.GetG(), c.GetB());
}

static gp::Color BatteryColor(int level) {
    return level < kBatteryCriticalLevel ? kBatteryRed : level < kBatteryLowLevel ? kBatteryYellow : kBatteryGreen;
}

struct Fonts { gp::Font *title, *body, *caption, *section, *big, *icon, *glyph, *tiny; };
static Fonts g_fonts;

static void CreateFonts() {
    g_fonts.title   = new gp::Font(L"Segoe UI Semibold", S(20), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.body    = new gp::Font(L"Segoe UI", S(14), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.caption = new gp::Font(L"Segoe UI", S(12), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.section = new gp::Font(L"Segoe UI Semibold", S(12), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.big     = new gp::Font(L"Segoe UI Semibold", S(17), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.icon    = new gp::Font(L"Segoe MDL2 Assets", S(15), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.glyph   = new gp::Font(L"Segoe MDL2 Assets", S(10), gp::FontStyleRegular, gp::UnitPixel);
    g_fonts.tiny    = new gp::Font(L"Segoe UI Semibold", S(10), gp::FontStyleRegular, gp::UnitPixel);
}

// ---- drawing helpers

static void RoundPath(gp::GraphicsPath& p, gp::RectF r, float radius) {
    float d = min(radius * 2, min(r.Width, r.Height));
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

static void FillRound(gp::Graphics& g, gp::RectF r, float radius, gp::Color c) {
    gp::GraphicsPath p;
    RoundPath(p, r, radius);
    gp::SolidBrush b(c);
    g.FillPath(&b, &p);
}

static void StrokeRound(gp::Graphics& g, gp::RectF r, float radius, gp::Color c, float width) {
    gp::GraphicsPath p;
    RoundPath(p, r, radius);
    gp::Pen pen(c, width);
    g.DrawPath(&pen, &p);
}

static void Label(gp::Graphics& g, const wchar_t* s, const gp::Font* f, gp::Color c, gp::RectF r,
                  gp::StringAlignment h = gp::StringAlignmentNear, gp::StringAlignment v = gp::StringAlignmentCenter) {
    gp::StringFormat fmt;
    fmt.SetAlignment(h);
    fmt.SetLineAlignment(v);
    fmt.SetFormatFlags(gp::StringFormatFlagsNoWrap);
    fmt.SetTrimming(gp::StringTrimmingEllipsisCharacter);
    gp::SolidBrush b(c);
    g.DrawString(s, -1, f, r, &fmt, &b);
}

// The window's canvas while it's being painted: big glows are blended straight into its
// pixels (a GDI+ path gradient that size costs ~5 ms, this ~1 ms).
static gp::Bitmap*   g_canvas;
static gp::Graphics* g_canvasGraphics;

static bool BlendGlow(float cx, float cy, float rx, float ry, gp::Color c) {
    int w = (int)g_canvas->GetWidth(), h = (int)g_canvas->GetHeight();
    int x0 = max(0, (int)(cx - rx)), x1 = min(w, (int)(cx + rx) + 1), y0 = max(0, (int)(cy - ry)), y1 = min(h, (int)(cy + ry) + 1);
    if (x0 >= x1 || y0 >= y1) return true;
    g_canvasGraphics->Flush(gp::FlushIntentionSync);
    gp::Rect area(x0, y0, x1 - x0, y1 - y0);
    gp::BitmapData data;
    if (g_canvas->LockBits(&area, gp::ImageLockModeRead | gp::ImageLockModeWrite, PixelFormat32bppPARGB, &data) != gp::Ok) return false;
    float alpha = c.GetA() / 255.0f, r = (float)c.GetR(), gr = (float)c.GetG(), b = (float)c.GetB();
    for (int y = y0; y < y1; y++) {
        BYTE* row = (BYTE*)data.Scan0 + (y - y0) * data.Stride;
        float dy = (y + 0.5f - cy) / ry, dy2 = dy * dy;
        if (dy2 >= 1) continue;
        for (int x = x0; x < x1; x++) {
            float dx = (x + 0.5f - cx) / rx, d2 = dx * dx + dy2;
            if (d2 >= 1) continue;
            float a = alpha * (1 - sqrtf(d2)), keep = 1 - a;
            BYTE* px = row + (x - x0) * 4;     // B G R A, premultiplied
            px[0] = (BYTE)(b * a + px[0] * keep);
            px[1] = (BYTE)(gr * a + px[1] * keep);
            px[2] = (BYTE)(r * a + px[2] * keep);
            px[3] = (BYTE)(255 * a + px[3] * keep);
        }
    }
    g_canvas->UnlockBits(&data);
    return true;
}

// Soft glow: colour at the centre fading to transparent at the edge of the ellipse.
static void FillGlow(gp::Graphics& g, float cx, float cy, float rx, float ry, gp::Color c) {
    if (c.GetA() == 0 || rx < 1 || ry < 1) return;
    if (&g == g_canvasGraphics && rx * ry > 2500 && BlendGlow(cx, cy, rx, ry, c)) return;
    gp::GraphicsPath p;
    p.AddEllipse(cx - rx, cy - ry, rx * 2, ry * 2);
    gp::PathGradientBrush b(&p);
    b.SetCenterColor(c);
    gp::Color edge = WithAlpha(c, 0);
    int n = 1;
    b.SetSurroundColors(&edge, &n);
    g.FillPath(&b, &p);
}

// ---- interactive elements (rebuilt by every paint, used for hit testing)

enum ElemKind {
    E_TOGGLE, E_SEGMENT, E_BUTTON, E_PIN, E_THEME, E_THEME_ITEM, E_SLIDER, E_TAB, E_PAD3D,
    E_SC_ROW, E_SC_ACTION, E_SC_ACTIONITEM, E_SC_DELETE, E_TP_TWO, E_TP_TWOITEM, E_TPBUTTON, E_SRC, E_SRCITEM, E_MICDST, E_MICDSTITEM,
};
enum ButtonId { BTN_STATUS, BTN_DISCONNECT, BTN_RUMBLE, BTN_SHORTCUTS, BTN_SC_ADD, BTN_SC_RESET, BTN_SC_DONE, BTN_SC_CANCEL };
struct Elem { ElemKind kind; gp::RectF rc; int id; int sub; };

enum SegmentId { SEG_LIGHT, SEG_LEFT_TAB, SEG_AUDIO_OUT };
static int  g_leftTab;                  // the left column: 0 emulation, 1 touchpad, 2 sound
static bool g_twoMenuOpen;              // the two-finger click menu
static gp::RectF g_twoAnchor;
static bool g_srcMenuOpen;              // the "plays from" menu: Automatic, each playback device, Mirror
static gp::RectF g_srcAnchor;
static std::vector<audio::Device> g_srcDevices;
static bool g_micMenuOpen;              // "mic goes to": Automatic, each playback device
static gp::RectF g_micAnchor;

static std::wstring MicDestLabel() {
    std::wstring dest = micout::Dest();
    if (dest.empty()) return L"Automatic";
    for (const auto& d : g_srcDevices) if (d.id == dest) return d.name;
    std::wstring playing = micout::PlayingTo();
    return playing.empty() ? L"Not connected" : playing;
}

static std::wstring SourceLabel() {
    std::wstring source = audio::Source();
    if (source.empty()) return L"Automatic";
    if (source == audio::kMirror) return L"Mirror the PC";
    for (const auto& d : g_srcDevices) if (d.id == source) return d.name;
    std::wstring playing = audio::PlayingFrom();
    return playing.empty() ? L"Not connected" : playing;
}

// Switch slots: a setting's own index for per-controller switches, then the two app switches.
static const int kSlotStartup = S_COUNT, kSlotEnabled = S_COUNT + 1, kSlotSpout = S_COUNT + 2;

static std::vector<Elem> g_elems;
static int   g_hover = -1, g_pressed = -1;
static bool  g_themeOpen;
static bool  g_startup;
static float g_knob[kSlotSpout + 1];

static int SettingValue(int setting) {
    Pad* p = SelectedPad();
    return p ? p->set[setting].load() : kSettingDefaults[setting];
}

static bool ToggleValue(int slot) {
    if (slot < S_COUNT) return SettingValue(slot) != 0;
    return slot == kSlotEnabled ? g_enabled.load() : slot == kSlotSpout ? g_spoutOn : g_startup;
}

static int AddElem(ElemKind kind, gp::RectF rc, int id, int sub = 0) {
    g_elems.push_back({ kind, rc, id, sub });
    return (int)g_elems.size() - 1;
}

static bool Hovered(int elem) { return elem >= 0 && elem == g_hover; }

// Switches that are on take Windows' accent colour (Settings > Personalisation > Colours).
static gp::Color g_switchOn = kGreen;

static void LoadAccentColour() {
    DWORD abgr = 0, size = sizeof(abgr);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &abgr, &size) == ERROR_SUCCESS) {
        g_switchOn = kAccent = gp::Color(255, (BYTE)abgr, (BYTE)(abgr >> 8), (BYTE)(abgr >> 16));
        return;
    }
    DWORD argb = 0;
    BOOL opaque;
    if (SUCCEEDED(DwmGetColorizationColor(&argb, &opaque))) {
        g_switchOn = kAccent = gp::Color(255, (BYTE)(argb >> 16), (BYTE)(argb >> 8), (BYTE)argb);
    } else {                                // no accent colour: the app's own green and blue
        g_switchOn = kGreen;
        kAccent = gp::Color(255, 10, 132, 255);
    }
}

static void DrawSwitch(gp::Graphics& g, gp::RectF r, float t, bool dim = false) {
    const Palette& p = Pal();
    FillRound(g, r, r.Height / 2, WithAlpha(Mix(p.switchOff, g_switchOn, t), dim ? 110 : 255));
    float d = r.Height - S(4);
    float x = r.X + S(2) + t * (r.Width - d - S(4));
    gp::SolidBrush shadow(gp::Color(50, 0, 0, 0)), knob(WithAlpha(kWhite, dim ? 150 : 255));
    g.FillEllipse(&shadow, x, r.Y + S(3), d, d);
    g.FillEllipse(&knob, x, r.Y + S(2), d, d);
}

// A switch row; without `live` it's drawn dimmed and can't be clicked (no controller).
static void DrawToggleRow(gp::Graphics& g, gp::RectF r, int slot, const wchar_t* title, const wchar_t* detail, bool live = true) {
    const Palette& p = Pal();
    int e = live ? AddElem(E_TOGGLE, r, slot) : -1;
    if (Hovered(e)) FillRound(g, gp::RectF(r.X + S(4), r.Y + S(3), r.Width - S(8), r.Height - S(6)), S(8), p.hover);
    float textW = r.Width - S(16) - S(kSwitchW) - S(28);
    Label(g, title, g_fonts.body, live ? p.text : WithAlpha(p.text, 110), gp::RectF(r.X + S(16), r.Y + S(9), textW, S(20)));
    Label(g, detail, g_fonts.caption, live ? p.subtext : WithAlpha(p.subtext, 110), gp::RectF(r.X + S(16), r.Y + S(30), textW, S(18)));
    float sx = r.X + r.Width - S(16) - S(kSwitchW);
    DrawSwitch(g, gp::RectF(sx, r.Y + (r.Height - S(kSwitchH)) / 2, S(kSwitchW), S(kSwitchH)), g_knob[slot], !live);
}

static float DrawSectionLabel(gp::Graphics& g, float x, float y, float w, const wchar_t* label) {
    Label(g, label, g_fonts.section, Pal().subtext, gp::RectF(x + S(4), y, w, S(20)));
    return y + S(24);
}

// Sliders, dragged with the mouse (see WindowProc).
struct SliderDef { int setting; const wchar_t* label; int lo, hi, step; const wchar_t* unit; };
static const SliderDef kSliders[] = {
    { S_DEADL, L"Left stick dead zone", 0, 40, 1, L"%" },
    { S_DEADR, L"Right stick dead zone", 0, 40, 1, L"%" },
    { S_DEADT, L"Trigger dead zone", 0, 40, 1, L"%" },
    { S_RUMBLEGAIN, L"Rumble strength", 0, 100, 5, L"%" },
    { S_BRIGHT, L"Brightness", 5, 100, 5, L"%" },
    { S_HUE, L"Colour", 0, 359, 1, L"\x00B0" },
    { S_RAINBOW, L"Rainbow speed", 1, 100, 1, L"" },
    { S_TPSPEED, L"Cursor speed", 10, 100, 5, L"%" },
    { S_AUDIOVOL, L"Volume", 0, 100, 5, L"%" },
};
enum { SLIDER_TPSPEED = 7, SLIDER_AUDIOVOL = 8 };

static const wchar_t* ToggleTitle(int setting, bool detail) {
    for (const auto& t : kToggleText) if (t.setting == setting) return detail ? t.detail : t.title;
    return L"";
}

// A row with text on the left and a drop-down button on the right; returns the button.
static gp::RectF DrawDropdownRow(gp::Graphics& g, gp::RectF r, const wchar_t* title, const wchar_t* detail, const wchar_t* value,
                                 ElemKind kind, bool open, bool live);
static const int kSliderCount = (int)ARRAYSIZE(kSliders), kTuningSliders = 3, SLIDER_RUMBLE = 3;
static gp::RectF g_sliderTrack[kSliderCount];
static int g_dragSlider = -1;

static void SetSliderFromX(int id, float x) {
    Pad* p = SelectedPad();
    if (!p) return;
    const SliderDef& d = kSliders[id];
    float t = max(0.0f, min(1.0f, (x - g_sliderTrack[id].X) / max(1.0f, g_sliderTrack[id].Width)));
    int v = d.lo + (int)(t * (d.hi - d.lo) / d.step + 0.5f) * d.step;
    p->set[d.setting] = max(d.lo, min(d.hi, v));
}

static void FillRainbow(gp::Graphics& g, gp::RectF r, int alpha) {
    gp::LinearGradientBrush rainbow(gp::PointF(r.X - 1, 0), gp::PointF(r.X + r.Width + 1, 0), kWhite, kWhite);
    gp::Color stops[7];
    gp::REAL at[7];
    for (int i = 0; i < 7; i++) {
        BYTE rgb[3];
        HsvToRgb(i * 60.0f, 1, 1, rgb);
        stops[i] = gp::Color((BYTE)alpha, rgb[0], rgb[1], rgb[2]);
        at[i] = i / 6.0f;
    }
    rainbow.SetInterpolationColors(stops, at, 7);
    gp::GraphicsPath path;
    RoundPath(path, r, r.Height / 2);
    g.FillPath(&rainbow, &path);
}

static void DrawSliderRow(gp::Graphics& g, gp::RectF r, int id, bool on) {
    const Palette& p = Pal();
    const SliderDef& d = kSliders[id];
    int e = on ? AddElem(E_SLIDER, r, id) : -1;
    int v = SettingValue(d.setting);
    gp::Color text = on ? p.text : WithAlpha(p.text, 110), sub = on ? p.subtext : WithAlpha(p.subtext, 110);
    Label(g, d.label, g_fonts.caption, text, gp::RectF(r.X + S(16), r.Y + S(8), r.Width - S(80), S(18)));
    wchar_t value[24];
    Pad* selected = SelectedPad();
    if (d.setting == S_RAINBOW) swprintf_s(value, L"%.1f s a turn", 100.0 / v);
    else if (d.setting == S_AUDIOVOL && selected && selected->audioMuted) wcscpy_s(value, L"Muted");
    else swprintf_s(value, L"%d%s", v, d.unit);
    Label(g, value, g_fonts.caption, sub, gp::RectF(r.X + r.Width - S(16) - S(90), r.Y + S(8), S(90), S(18)), gp::StringAlignmentFar);
    gp::RectF track(r.X + S(18), r.Y + S(35), r.Width - S(36), S(4));
    g_sliderTrack[id] = track;
    float t = (float)(v - d.lo) / (d.hi - d.lo);
    float kx = track.X + t * track.Width, ky = track.Y + track.Height / 2;
    if (d.setting == S_HUE) {               // the track is the rainbow it picks from
        FillRainbow(g, gp::RectF(track.X, track.Y - S(1), track.Width, track.Height + S(2)), on ? 255 : 90);
    } else {
        FillRound(g, track, S(2), p.switchOff);
        FillRound(g, gp::RectF(track.X, track.Y, kx - track.X, track.Height), S(2), on ? kAccent : p.line);
    }
    float kr = (Hovered(e) || g_dragSlider == id) && on ? S(9) : S(8);
    gp::SolidBrush shadow(gp::Color(50, 0, 0, 0)), knob(on ? kWhite : Mix(kWhite, p.line, 0.5f));
    g.FillEllipse(&shadow, kx - kr, ky - kr + S(1), kr * 2, kr * 2);
    g.FillEllipse(&knob, kx - kr, ky - kr, kr * 2, kr * 2);
}

static void DrawSegmented(gp::Graphics& g, gp::RectF r, int id, const wchar_t* const* labels, int count, int selected, bool live) {
    const Palette& p = Pal();
    FillRound(g, r, S(9), p.segment);
    float w = r.Width / count;
    for (int i = 0; i < count; i++) {
        gp::RectF seg(r.X + w * i, r.Y, w, r.Height);
        int e = live ? AddElem(E_SEGMENT, seg, id, i) : -1;
        gp::RectF pill(seg.X + S(2), seg.Y + S(2), seg.Width - S(4), seg.Height - S(4));
        if (i == selected) {
            FillRound(g, gp::RectF(pill.X, pill.Y + S(1), pill.Width, pill.Height), S(7), p.shadow);
            FillRound(g, pill, S(7), WithAlpha(p.segmentOn, live ? 255 : 120));
        } else if (Hovered(e)) {
            FillRound(g, pill, S(7), WithAlpha(p.segmentOn, 110));
        }
        Label(g, labels[i], i == selected ? g_fonts.section : g_fonts.caption, live ? p.text : WithAlpha(p.text, 110), seg,
              gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    }
}

static void DrawButton(gp::Graphics& g, gp::RectF r, int id, const wchar_t* text, bool primary, bool live = true) {
    const Palette& p = Pal();
    int e = live ? AddElem(E_BUTTON, r, id) : -1;
    gp::Color fill = primary ? kAccent : WithAlpha(p.card, 255);
    if (Hovered(e)) fill = primary ? Mix(kAccent, kWhite, 0.15f) : p.hover;
    if (!live) fill = WithAlpha(fill, primary ? 90 : 120);
    FillRound(g, r, S(8), fill);
    if (!primary) StrokeRound(g, r, S(8), p.line, S(1));
    gp::Color text_ = primary ? OnAccent() : p.text;
    Label(g, text, g_fonts.caption, live ? text_ : WithAlpha(text_, 120), r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

// ---------------------------------------------------------------------------------------
// The 3D controller: a few hundred flat-shaded polygons, painter's-sorted. The model is in
// the same units as a top-down drawing (1000 x 640, y down = towards you), lifted into 3D
// with x right, y up out of the face and z towards you; the pad's tilt and turn come from its
// motion sensors, and dragging spins the view (it keeps spinning and slows down).

static const gp::Color kTouchColors[] = { gp::Color(255, 10, 132, 255), gp::Color(255, 191, 90, 242) };
static const gp::Color kTriangle(255, 48, 209, 158), kCircle(255, 255, 69, 58), kCross(255, 100, 160, 255), kSquare(255, 228, 130, 210);

// ---- the live controller (flat, in the middle)

// Maps the drawing's 1000 x 640 design units into the area.
struct PadLayout {
    float x0, y0, scale;
    float X(float u) const { return x0 + u * scale; }
    float Y(float u) const { return y0 + u * scale; }
    float U(float u) const { return u * scale; }
    gp::RectF R(float x, float y, float w, float h) const { return gp::RectF(X(x), Y(y), U(w), U(h)); }
};

static PadLayout LayoutPad(gp::RectF area) {
    PadLayout L;
    L.scale = min(area.Width / 1000.0f, area.Height / 640.0f);
    L.x0 = area.X + (area.Width - 1000 * L.scale) / 2;
    L.y0 = area.Y + (area.Height - 640 * L.scale) / 2;
    return L;
}

static void PadBodyPath(gp::GraphicsPath& path, const PadLayout& L) {
    static const float pts[][2] = {
        { 250, 118 }, { 500, 108 }, { 750, 118 }, { 868, 150 }, { 938, 300 }, { 978, 480 }, { 940, 596 }, { 868, 606 },
        { 800, 520 }, { 716, 440 }, { 500, 428 }, { 284, 440 }, { 200, 520 }, { 132, 606 }, { 60, 596 }, { 22, 480 },
        { 62, 300 }, { 132, 150 },
    };
    gp::PointF p[ARRAYSIZE(pts)];
    for (int i = 0; i < (int)ARRAYSIZE(pts); i++) p[i] = gp::PointF(L.X(pts[i][0]), L.Y(pts[i][1]));
    path.AddClosedCurve(p, ARRAYSIZE(pts), 0.42f);
}

static void DrawFaceButton(gp::Graphics& g, const PadLayout& L, float cx, float cy, int symbol, gp::Color colour, bool down) {
    const Palette& p = Pal();
    float r = L.U(30);
    gp::RectF rc(L.X(cx) - r, L.Y(cy) - r, r * 2, r * 2);
    gp::SolidBrush fill(down ? colour : Mix(p.body, p.text, 0.10f));
    g.FillEllipse(&fill, rc);
    gp::Pen pen(down ? kWhite : colour, L.U(5));
    pen.SetLineJoin(gp::LineJoinRound);
    pen.SetStartCap(gp::LineCapRound);
    pen.SetEndCap(gp::LineCapRound);
    float s = L.U(13), x = L.X(cx), y = L.Y(cy);
    switch (symbol) {
    case 0: {   // triangle
        gp::PointF tri[3] = { { x, y - s * 1.05f }, { x + s * 1.1f, y + s * 0.8f }, { x - s * 1.1f, y + s * 0.8f } };
        g.DrawPolygon(&pen, tri, 3);
        break;
    }
    case 1: g.DrawEllipse(&pen, x - s, y - s, s * 2, s * 2); break;
    case 2: g.DrawLine(&pen, x - s, y - s, x + s, y + s); g.DrawLine(&pen, x + s, y - s, x - s, y + s); break;
    case 3: g.DrawRectangle(&pen, x - s, y - s, s * 2, s * 2); break;
    }
}

static void DrawStick(gp::Graphics& g, const PadLayout& L, float cx, float cy, BYTE vx, BYTE vy, bool down) {
    const Palette& p = Pal();
    float r = L.U(62);
    gp::SolidBrush well(Mix(p.body, gp::Color(255, 0, 0, 0), g_dark ? 0.45f : 0.12f));
    g.FillEllipse(&well, L.X(cx) - r, L.Y(cy) - r, r * 2, r * 2);
    float fx = (vx - 127.5f) / 127.5f, fy = (vy - 127.5f) / 127.5f;
    float mag = sqrtf(fx * fx + fy * fy);
    if (mag > 1) { fx /= mag; fy /= mag; }
    float x = L.X(cx) + fx * L.U(24), y = L.Y(cy) + fy * L.U(24), cap = L.U(44);
    gp::SolidBrush shadow(gp::Color(60, 0, 0, 0)), top(down ? kAccent : Mix(p.body, p.text, 0.22f));
    g.FillEllipse(&shadow, x - cap, y - cap + L.U(4), cap * 2, cap * 2);
    g.FillEllipse(&top, x - cap, y - cap, cap * 2, cap * 2);
    gp::Pen ring(WithAlpha(p.text, down ? 0 : 50), L.U(4));
    g.DrawEllipse(&ring, x - cap * 0.68f, y - cap * 0.68f, cap * 1.36f, cap * 1.36f);
}

// The body's outline in design units, with outward normals (made once).
struct OutlinePoint { float u, v, nu, nv; };

static const std::vector<OutlinePoint>& DesignOutline() {
    static std::vector<OutlinePoint> pts;
    if (!pts.empty()) return pts;
    PadLayout unit{ 0, 0, 1 };
    gp::GraphicsPath path;
    PadBodyPath(path, unit);
    path.Flatten(nullptr, 0.25f);
    int n = path.GetPointCount();
    std::vector<gp::PointF> f(n);
    path.GetPathPoints(f.data(), n);
    float area = 0;
    for (int i = 0; i < n; i++) area += f[i].X * f[(i + 1) % n].Y - f[(i + 1) % n].X * f[i].Y;
    for (int i = 0; i < n; i++) {
        gp::PointF a = f[(i + n - 1) % n], b = f[(i + 1) % n];
        float du = b.X - a.X, dv = b.Y - a.Y, len = max(1e-4f, sqrtf(du * du + dv * dv));
        float nu = area > 0 ? dv / len : -dv / len, nv = area > 0 ? -du / len : du / len;
        pts.push_back({ f[i].X, f[i].Y, nu, nv });
    }
    return pts;
}

// A shoulder button: a band along the top of the outline between u0 and u1, `offset` design
// units out from it and `width` thick, with rounded ends. Returns its middle (for the label).
// The light edge that sets the pad off the window's background; the stream overlay goes without
// it (over a transparent background it reads as a white outline).
static bool g_overlayDrawing;
static gp::Color PadEdge(const Palette& p) { return g_overlayDrawing ? gp::Color(0, 0, 0, 0) : p.bodyEdge; }

static gp::PointF DrawShoulderBand(gp::Graphics& g, const PadLayout& L, float u0, float u1, float offset, float width,
                                   gp::Color fill, gp::Color edge) {
    std::vector<OutlinePoint> top;
    for (const OutlinePoint& p : DesignOutline()) if (p.u >= u0 && p.u <= u1 && p.v < 260) top.push_back(p);
    std::sort(top.begin(), top.end(), [](const OutlinePoint& a, const OutlinePoint& b) { return a.u < b.u; });
    std::vector<gp::PointF> line;
    for (const OutlinePoint& p : top) line.push_back(gp::PointF(L.X(p.u + p.nu * offset), L.Y(p.v + p.nv * offset)));
    if (line.size() < 2) return gp::PointF();
    gp::Pen border(edge, L.U(width) + S(2)), body(fill, L.U(width));
    for (gp::Pen* pen : { &border, &body }) {
        pen->SetStartCap(gp::LineCapRound);
        pen->SetEndCap(gp::LineCapRound);
        pen->SetLineJoin(gp::LineJoinRound);
        g.DrawLines(pen, line.data(), (int)line.size());
    }
    return line[line.size() / 2];
}

// L1/R1 hug the controller's shoulders; L2/R2 sit just outside them, a little longer, and fill
// in as they're pulled.
static void DrawShoulders(gp::Graphics& g, const PadLayout& L, float u0, float u1, BYTE analog, bool bumper,
                          const wchar_t* bumperName, const wchar_t* triggerName) {
    const Palette& p = Pal();
    float pull = analog / 255.0f;
    gp::PointF t = DrawShoulderBand(g, L, u0 - 25, u1 + 25, 52, 34, Mix(p.body, kAccent, pull), PadEdge(p));
    gp::PointF b = DrawShoulderBand(g, L, u0, u1, 17, 24, bumper ? kAccent : p.body, PadEdge(p));
    Label(g, triggerName, g_fonts.tiny, pull > 0.5f ? OnAccent() : p.subtext, gp::RectF(t.X - S(20), t.Y - S(8), S(40), S(16)),
          gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    Label(g, bumperName, g_fonts.tiny, bumper ? OnAccent() : p.subtext, gp::RectF(b.X - S(20), b.Y - S(8), S(40), S(16)),
          gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}
static float g_split;                   // the touchpad's split line (left / right click), 0 hidden .. 1 shown

static void DrawPad(gp::Graphics& g, const PadLayout& L, const PadState& s, gp::Color light, float opacity) {
    const Palette& p = Pal();
    gp::Color control = Mix(p.body, p.text, 0.10f);
    DWORD b = s.buttons;

    DrawShoulders(g, L, 150, 330, s.l2, (b & B_L1) != 0, L"L1", L"L2");
    DrawShoulders(g, L, 670, 850, s.r2, (b & B_R1) != 0, L"R1", L"R2");

    gp::GraphicsPath body;
    PadBodyPath(body, L);
    {
        gp::Matrix down(1, 0, 0, 1, 0, L.U(8));
        gp::GraphicsPath* shadowPath = body.Clone();
        shadowPath->Transform(&down);
        gp::SolidBrush shadow(p.shadow);
        g.FillPath(&shadow, shadowPath);
        delete shadowPath;
        gp::SolidBrush fill(p.body);
        g.FillPath(&fill, &body);
        if (!g_overlayDrawing) {
            gp::Pen edge(p.bodyEdge, S(1.5));
            g.DrawPath(&edge, &body);
        }
    }

    // Light bar, along the top of the touchpad.
    gp::RectF bar = L.R(400, 122, 200, 10);
    FillGlow(g, bar.X + bar.Width / 2, bar.Y + bar.Height / 2, bar.Width * 0.75f, L.U(46), WithAlpha(light, 120));
    FillRound(g, bar, L.U(5), light.GetA() ? light : control);

    // Touchpad with the fingers on it.
    gp::RectF pad = L.R(330, 142, 340, 176);
    bool click = (b & B_TOUCH) != 0;
    if (click && g_split > 0.5f) {          // split clicks: only the side being clicked lights up
        int finger = s.touch[0] ? 0 : 1;
        bool right = s.touch[finger] && s.tx[finger] >= 960;
        FillRound(g, pad, L.U(22), control);
        gp::GraphicsPath clip;
        RoundPath(clip, pad, L.U(22));
        gp::GraphicsState saved = g.Save();
        g.SetClip(&clip, gp::CombineModeIntersect);
        gp::SolidBrush half(Mix(control, kAccent, 0.55f));
        g.FillRectangle(&half, right ? pad.X + pad.Width / 2 : pad.X, pad.Y, pad.Width / 2, pad.Height);
        g.Restore(saved);
    } else {
        FillRound(g, pad, L.U(22), click ? Mix(control, kAccent, 0.55f) : control);
    }
    StrokeRound(g, pad, L.U(22), p.bodyEdge, S(1));
    if (g_split > 0.01f) {                  // the line grows from the middle when split clicks come on
        float half = pad.Height * 0.42f * g_split, cx = pad.X + pad.Width / 2, cy = pad.Y + pad.Height / 2;
        gp::Pen splitLine(WithAlpha(p.text, (int)(120 * g_split)), S(1.5));
        splitLine.SetStartCap(gp::LineCapRound);
        splitLine.SetEndCap(gp::LineCapRound);
        g.DrawLine(&splitLine, cx, cy - half, cx, cy + half);
    }
    for (int i = 0; i < 2; i++) {
        if (!s.touch[i]) continue;
        float x = pad.X + pad.Width * max(0.04f, min(0.96f, s.tx[i] / 1919.0f));
        float y = pad.Y + pad.Height * max(0.08f, min(0.92f, s.ty[i] / 942.0f));
        FillGlow(g, x, y, L.U(44), L.U(44), WithAlpha(kTouchColors[i], 150));
        gp::SolidBrush dot(kTouchColors[i]);
        g.FillEllipse(&dot, x - L.U(13), y - L.U(13), L.U(26), L.U(26));
    }

    // Share and Options either side of it, PS below.
    FillRound(g, L.R(288, 150, 22, 46), L.U(11), (b & B_SHARE) ? kAccent : control);
    FillRound(g, L.R(690, 150, 22, 46), L.U(11), (b & B_OPTIONS) ? kAccent : control);
    {
        gp::SolidBrush ps((b & B_PS) ? kAccent : control);
        g.FillEllipse(&ps, L.R(476, 366, 48, 48));
    }

    // D-pad.
    static const struct { DWORD bit; float x, y, w, h; } arms[] = {
        { B_UP, 187, 172, 50, 60 }, { B_DOWN, 187, 258, 50, 60 }, { B_LEFT, 139, 220, 60, 50 }, { B_RIGHT, 225, 220, 60, 50 },
    };
    for (const auto& a : arms) FillRound(g, L.R(a.x, a.y, a.w, a.h), L.U(12), (b & a.bit) ? kAccent : control);

    // Face buttons.
    DrawFaceButton(g, L, 788, 177, 0, kTriangle, (b & B_TRIANGLE) != 0);
    DrawFaceButton(g, L, 856, 245, 1, kCircle, (b & B_CIRCLE) != 0);
    DrawFaceButton(g, L, 788, 313, 2, kCross, (b & B_CROSS) != 0);
    DrawFaceButton(g, L, 720, 245, 3, kSquare, (b & B_SQUARE) != 0);

    DrawStick(g, L, 352, 390, s.lx, s.ly, (b & B_L3) != 0);
    DrawStick(g, L, 648, 390, s.rx, s.ry, (b & B_R3) != 0);

    if (opacity < 1) {                      // no controller: wash the drawing out
        gp::RectF all = L.R(0, 0, 1000, 640);
        gp::SolidBrush wash(WithAlpha(p.bg, (int)(255 * (1 - opacity))));
        g.FillRectangle(&wash, all);
    }
}

// ---- ambilight: as on the Magic Mouse app, a pressed button or a finger on the touchpad lights
// the stretch of the controller's outline nearest to it; the light spreads along the edge and
// fades with distance outside it. It's drawn under everything else (the cards dim it).
//
// The outline's geometry is a field at 1/kAmbientStep resolution around the drawing: for each
// cell, the nearest point on the outline and the distance outside it. Each frame the sources
// light the cells, and the field is blended into the window, smoothly interpolated.

static const int   kAmbientStep = 4;
static const float kAmbientReach = 80;          // 96-dpi px outside the outline to fall to 1/e
static const float kAmbientSpread = 190;        // design units along the outline (1 sigma)

enum { GLOW_CONTRAST, GLOW_ACCENT, GLOW_TRIANGLE, GLOW_CIRCLE, GLOW_CROSS, GLOW_SQUARE };
static const struct { DWORD bit; float u, v; int colour; int analog; } kGlowSpots[] = {
    { B_TRIANGLE, 788, 177, GLOW_TRIANGLE, 0 }, { B_CIRCLE, 856, 245, GLOW_CIRCLE, 0 },
    { B_CROSS, 788, 313, GLOW_CROSS, 0 },       { B_SQUARE, 720, 245, GLOW_SQUARE, 0 },
    { B_UP, 212, 200, GLOW_CONTRAST, 0 },       { B_DOWN, 212, 288, GLOW_CONTRAST, 0 },
    { B_LEFT, 169, 245, GLOW_CONTRAST, 0 },     { B_RIGHT, 255, 245, GLOW_CONTRAST, 0 },
    { B_L1, 240, 104, GLOW_CONTRAST, 0 },       { B_R1, 760, 104, GLOW_CONTRAST, 0 },
    { 0, 240, 68, GLOW_ACCENT, 1 },             { 0, 760, 68, GLOW_ACCENT, 2 },          // L2, R2 by how far
    { B_SHARE, 299, 173, GLOW_CONTRAST, 0 },    { B_OPTIONS, 701, 173, GLOW_CONTRAST, 0 },
    { B_PS, 500, 390, GLOW_ACCENT, 0 },         { B_TOUCH, 500, 230, GLOW_ACCENT, 0 },
    { B_L3, 352, 390, GLOW_CONTRAST, 0 },       { B_R3, 648, 390, GLOW_CONTRAST, 0 },
};
static float g_spotLevel[ARRAYSIZE(kGlowSpots)];
static float g_fingerLevel[2], g_fingerU[2], g_fingerV[2];
static float g_ringLevel;               // the whole outline glows: pulsing on a low battery,
static int   g_ringStyle;               // breathing while charging (RING_*)
enum { RING_LOW, RING_CHARGING, RING_FULL };
static gp::Color g_ringColour = kBatteryYellow;

static gp::Color GlowColour(int which) {
    switch (which) {
    case GLOW_ACCENT:   return kAccent;
    case GLOW_TRIANGLE: return kTriangle;
    case GLOW_CIRCLE:   return kCircle;
    case GLOW_CROSS:    return kCross;
    case GLOW_SQUARE:   return kSquare;
    default:            return g_dark ? kWhite : gp::Color(255, 0, 0, 0);
    }
}

// Eases each light towards its button (quick in, slow out) and follows the fingers.
static bool g_touchSplit, g_touchRight;    // split clicks on, and which side the click is on

static void UpdateAmbient(const PadState& s, bool live) {
    g_touchSplit = g_split > 0.5f;
    if (s.buttons & B_TOUCH) {
        int finger = s.touch[0] ? 0 : 1;
        g_touchRight = s.touch[finger] && s.tx[finger] >= 960;
    }
    bool known = live && s.battery >= 0;
    bool charging = known && s.charging, low = known && !s.charging && s.battery < kBatteryLowLevel;
    if (charging) {                     // breathing in the battery's colour; steady green once full
        g_ringStyle = s.battery >= 100 ? RING_FULL : RING_CHARGING;
        g_ringColour = BatteryColor(s.battery);
    } else if (low) {
        g_ringStyle = RING_LOW;
        g_ringColour = BatteryColor(s.battery);
    }
    g_ringLevel += ((charging || low ? 1.0f : 0.0f) - g_ringLevel) * 0.08f;
    if (g_ringLevel < 0.01f) g_ringLevel = 0;
    for (int i = 0; i < (int)ARRAYSIZE(kGlowSpots); i++) {
        const auto& spot = kGlowSpots[i];
        float target = !live ? 0 : spot.analog == 1 ? s.l2 / 255.0f : spot.analog == 2 ? s.r2 / 255.0f
                     : (s.buttons & spot.bit) ? 1.0f : 0.0f;
        float& level = g_spotLevel[i];
        level += (target - level) * (target > level ? 0.45f : 0.12f);
        if (level < 0.01f) level = 0;
    }
    for (int i = 0; i < 2; i++) {
        bool down = live && s.touch[i];
        float u = 330 + 340 * max(0.0f, min(1.0f, s.tx[i] / 1919.0f)), v = 144 + 174 * max(0.0f, min(1.0f, s.ty[i] / 942.0f));
        if (down) {
            if (g_fingerLevel[i] < 0.05f) { g_fingerU[i] = u; g_fingerV[i] = v; }
            g_fingerU[i] += (u - g_fingerU[i]) * 0.5f;
            g_fingerV[i] += (v - g_fingerV[i]) * 0.5f;
            g_fingerLevel[i] += (0.9f - g_fingerLevel[i]) * 0.35f;
        } else {
            g_fingerLevel[i] *= 0.88f;
            if (g_fingerLevel[i] < 0.01f) g_fingerLevel[i] = 0;
        }
    }
}

static bool AmbientActive() {
    for (float level : g_spotLevel) if (level > 0) return true;
    return g_fingerLevel[0] > 0 || g_fingerLevel[1] > 0 || g_ringLevel > 0;
}

struct AmbientField {
    float x0 = -1, y0, scale;           // the layout it was made for
    int ox, oy, cols, rows;             // the field's place in the window (px) and size (cells)
    std::vector<float> fade;            // exp(-distance outside the outline / reach)
};

static void DrawAmbient(gp::Graphics& g, const PadLayout& L, float w, float h, AmbientField& F) {
    if (!AmbientActive() || !g_canvas || &g != g_canvasGraphics) return;

    float reach = S(kAmbientReach);
    if (F.x0 != L.x0 || F.y0 != L.y0 || F.scale != L.scale) {
        F.x0 = L.x0; F.y0 = L.y0; F.scale = L.scale;
        gp::GraphicsPath path;
        PadBodyPath(path, L);
        path.Flatten(nullptr, 0.5f);
        std::vector<gp::PointF> pts(path.GetPointCount());
        path.GetPathPoints(pts.data(), (int)pts.size());
        float margin = reach * 3;
        int x0 = max(0, (int)(L.X(0) - margin)), x1 = min((int)w, (int)(L.X(1000) + margin));
        int y0 = max(0, (int)(L.Y(0) - margin)), y1 = min((int)h, (int)(L.Y(640) + margin));
        F.ox = x0; F.oy = y0;
        F.cols = (x1 - x0) / kAmbientStep + 2;
        F.rows = (y1 - y0) / kAmbientStep + 2;
        F.fade.assign(F.cols * F.rows, 0);
        size_t n = pts.size();
        for (int r = 0; r < F.rows; r++) {
            for (int c = 0; c < F.cols; c++) {
                float px = F.ox + c * kAmbientStep + 0.5f, py = F.oy + r * kAmbientStep + 0.5f;
                float best = 1e30f;
                bool inside = false;
                for (size_t i = 0, j = n - 1; i < n; j = i++) {
                    gp::PointF a = pts[j], b = pts[i];
                    if ((b.Y > py) != (a.Y > py) && px < (a.X - b.X) * (py - b.Y) / (a.Y - b.Y) + b.X) inside = !inside;
                    float ex = b.X - a.X, ey = b.Y - a.Y, len2 = ex * ex + ey * ey;
                    float t = len2 > 0 ? max(0.0f, min(1.0f, ((px - a.X) * ex + (py - a.Y) * ey) / len2)) : 0;
                    float cx = a.X + ex * t, cy = a.Y + ey * t;
                    best = min(best, (px - cx) * (px - cx) + (py - cy) * (py - cy));
                }
                F.fade[r * F.cols + c] = inside ? 1.0f : expf(-sqrtf(best) / reach);
            }
        }
    }

    struct Source { float u, v, strength; float r, gr, b; };
    Source sources[ARRAYSIZE(kGlowSpots) + 2];
    int count = 0;
    for (int i = 0; i < (int)ARRAYSIZE(kGlowSpots); i++) {
        if (!g_spotLevel[i]) continue;
        gp::Color c = GlowColour(kGlowSpots[i].colour);
        float u = kGlowSpots[i].u;
        if (kGlowSpots[i].bit == B_TOUCH && g_touchSplit) u = g_touchRight ? 585.0f : 415.0f;  // just the clicked side
        sources[count++] = { u, kGlowSpots[i].v, g_spotLevel[i], (float)c.GetR(), (float)c.GetG(), (float)c.GetB() };
    }
    for (int i = 0; i < 2; i++) {
        if (!g_fingerLevel[i]) continue;
        gp::Color c = kTouchColors[i];
        sources[count++] = { g_fingerU[i], g_fingerV[i], g_fingerLevel[i], (float)c.GetR(), (float)c.GetG(), (float)c.GetB() };
    }
    // Light per cell: premultiplied colour and alpha.
    static std::vector<float> cr, cg, cb, ca;
    size_t cells = (size_t)F.cols * F.rows;
    cr.assign(cells, 0); cg.assign(cells, 0); cb.assign(cells, 0); ca.assign(cells, 0);
    // Each source lights cells by their own distance from it (smooth everywhere), and the
    // outline's fade keeps the light hugging the edge.
    float sigma = L.U(kAmbientSpread), twoSigma2 = 2 * sigma * sigma;
    float seconds = (GetTickCount64() % 1000000) / 1000.0f, ring;
    if (g_ringStyle == RING_CHARGING)       // a slow breath, about every 3 s
        ring = g_ringLevel * (0.08f + 0.62f * (0.5f - 0.5f * cosf(seconds * 2 * kPi / 3.0f)));
    else if (g_ringStyle == RING_FULL)
        ring = g_ringLevel * 0.35f;
    else                                    // low battery: a quicker pulse
        ring = g_ringLevel * (0.55f + 0.25f * sinf(seconds * 2.2f));
    for (int i = 0; i < count; i++) { sources[i].u = L.X(sources[i].u); sources[i].v = L.Y(sources[i].v); }
    for (int row = 0; row < F.rows; row++) {
        float py = F.oy + row * kAmbientStep + 0.5f;
        for (int col = 0; col < F.cols; col++) {
            size_t k = (size_t)row * F.cols + col;
            if (F.fade[k] < 0.01f) continue;
            float px = F.ox + col * kAmbientStep + 0.5f;
            float sum = 0, r = 0, gr = 0, b = 0;
            for (int i = 0; i < count; i++) {
                float dx = px - sources[i].u, dy = py - sources[i].v;
                float weight = sources[i].strength * expf(-(dx * dx + dy * dy) / twoSigma2);
                sum += weight; r += weight * sources[i].r; gr += weight * sources[i].gr; b += weight * sources[i].b;
            }
            if (ring > 0) {
                sum += ring; r += ring * g_ringColour.GetR(); gr += ring * g_ringColour.GetG(); b += ring * g_ringColour.GetB();
            }
            if (sum < 0.004f) continue;
            float a = min(1.0f, sum) * F.fade[k] * 0.7f;
            ca[k] = a; cr[k] = r / sum * a; cg[k] = gr / sum * a; cb[k] = b / sum * a;
        }
    }
    // Blend into the window, a cell (kAmbientStep square) at a time; dark cells are skipped.
    int x1 = min((int)g_canvas->GetWidth(), F.ox + (F.cols - 1) * kAmbientStep), y1 = min((int)g_canvas->GetHeight(), F.oy + (F.rows - 1) * kAmbientStep);
    if (x1 <= F.ox || y1 <= F.oy) return;
    g_canvasGraphics->Flush(gp::FlushIntentionSync);
    gp::Rect area(F.ox, F.oy, x1 - F.ox, y1 - F.oy);
    gp::BitmapData data;
    if (g_canvas->LockBits(&area, gp::ImageLockModeRead | gp::ImageLockModeWrite, PixelFormat32bppPARGB, &data) != gp::Ok) return;
    const float inv = 1.0f / kAmbientStep;
    for (int row = 0; row + 1 < F.rows; row++) {
        for (int col = 0; col + 1 < F.cols; col++) {
            size_t k = (size_t)row * F.cols + col;
            float a00 = ca[k], a01 = ca[k + 1], a10 = ca[k + F.cols], a11 = ca[k + F.cols + 1];
            if (a00 + a01 + a10 + a11 < 0.004f) continue;
            for (int yy = 0; yy < kAmbientStep; yy++) {
                int y = F.oy + row * kAmbientStep + yy;
                if (y >= y1) break;
                BYTE* line = (BYTE*)data.Scan0 + (y - F.oy) * data.Stride;
                float ty = yy * inv;
                for (int xx = 0; xx < kAmbientStep; xx++) {
                    int x = F.ox + col * kAmbientStep + xx;
                    if (x >= x1) break;
                    float tx = xx * inv;
                    float w00 = (1 - tx) * (1 - ty), w01 = tx * (1 - ty), w10 = (1 - tx) * ty, w11 = tx * ty;
                    float a = a00 * w00 + a01 * w01 + a10 * w10 + a11 * w11;
                    if (a < 0.002f) continue;
                    float r = cr[k] * w00 + cr[k + 1] * w01 + cr[k + F.cols] * w10 + cr[k + F.cols + 1] * w11;
                    float gr = cg[k] * w00 + cg[k + 1] * w01 + cg[k + F.cols] * w10 + cg[k + F.cols + 1] * w11;
                    float b = cb[k] * w00 + cb[k + 1] * w01 + cb[k + F.cols] * w10 + cb[k + F.cols + 1] * w11;
                    BYTE* px = line + (x - F.ox) * 4;
                    float keep = 1 - a;
                    px[0] = (BYTE)min(255.0f, b + px[0] * keep);
                    px[1] = (BYTE)min(255.0f, gr + px[1] * keep);
                    px[2] = (BYTE)min(255.0f, r + px[2] * keep);
                    px[3] = (BYTE)min(255.0f, 255 * a + px[3] * keep);
                }
            }
        }
    }
    g_canvas->UnlockBits(&data);
}

struct V3 { float x, y, z; };
static V3 operator+(V3 a, V3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
static V3 operator-(V3 a, V3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
static V3 operator*(V3 a, float k) { return { a.x * k, a.y * k, a.z * k }; }
static float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static V3 Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
static V3 Normal(V3 a) { float l = sqrtf(Dot(a, a)); return l > 1e-6f ? a * (1 / l) : V3{ 0, 1, 0 }; }

struct M3 { float m[9]; };
static V3 Apply(const M3& r, V3 v) {
    return { r.m[0] * v.x + r.m[1] * v.y + r.m[2] * v.z, r.m[3] * v.x + r.m[4] * v.y + r.m[5] * v.z,
             r.m[6] * v.x + r.m[7] * v.y + r.m[8] * v.z };
}
static M3 operator*(const M3& a, const M3& b) {
    M3 c{};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            for (int k = 0; k < 3; k++) c.m[i * 3 + j] += a.m[i * 3 + k] * b.m[k * 3 + j];
    return c;
}
static M3 RotX(float a) { float c = cosf(a), s = sinf(a); return { 1, 0, 0, 0, c, -s, 0, s, c }; }
static M3 RotY(float a) { float c = cosf(a), s = sinf(a); return { c, 0, s, 0, 1, 0, -s, 0, c }; }

// The rotation turning unit vector `from` onto `to`.
static M3 Align(V3 from, V3 to) {
    V3 v = Cross(from, to);
    float c = Dot(from, to);
    if (c < -0.999f) return RotX(kPi);
    float k = 1 / (1 + c);
    return { 1 - k * (v.y * v.y + v.z * v.z), -v.z + k * v.x * v.y, v.y + k * v.x * v.z,
             v.z + k * v.x * v.y, 1 - k * (v.x * v.x + v.z * v.z), -v.x + k * v.y * v.z,
             -v.y + k * v.x * v.z, v.x + k * v.y * v.z, 1 - k * (v.x * v.x + v.y * v.y) };
}

// Design units (u right, v down) at height y, to model space.
static V3 P(float u, float v, float y) { return { u - 500, y, v - 330 }; }

struct Face {
    std::vector<gp::PointF> pts;
    float depth;
    gp::Color colour;
    float line;         // > 0: a stroke this wide (an open or closed outline), else a fill
    bool closed, top;   // top: the body's top face (its decorations draw right after it)
};

struct Scene {
    M3 r;
    float dist, cx, cy, scale;
    V3 light;
    std::vector<Face> body, deco;
    V3 Cam(V3 m) const { return Apply(r, m); }
    gp::PointF Project(V3 c) const { float k = dist / (dist - c.z); return gp::PointF(cx + c.x * k * scale, cy - c.y * k * scale); }
};

static gp::Color Shade(gp::Color c, float k) {
    auto m = [k](BYTE x) { return (BYTE)max(0.0f, min(255.0f, x * k)); };
    return gp::Color(c.GetA(), m(c.GetR()), m(c.GetG()), m(c.GetB()));
}

// A polygon; `hint` points outwards (picks the normal's sign). Back faces are dropped.
static bool AddPoly(Scene& sc, std::vector<Face>& list, const V3* pts, int n, V3 hint, gp::Color colour,
                    bool emissive = false, float bias = 0, bool top = false) {
    V3 normal{};
    for (int i = 0; i < n; i++) {           // Newell's method
        V3 a = pts[i], b = pts[(i + 1) % n];
        normal.x += (a.y - b.y) * (a.z + b.z);
        normal.y += (a.z - b.z) * (a.x + b.x);
        normal.z += (a.x - b.x) * (a.y + b.y);
    }
    if (Dot(normal, hint) < 0) normal = normal * -1;
    V3 nc = Normal(Apply(sc.r, Normal(normal)));
    Face f;
    f.pts.resize(n);
    V3 centre{};
    float depth = 0;
    for (int i = 0; i < n; i++) {
        V3 c = sc.Cam(pts[i]);
        centre = centre + c;
        depth += c.z;
        f.pts[i] = sc.Project(c);
    }
    centre = centre * (1.0f / n);
    if (Dot(nc, V3{ 0, 0, sc.dist } - centre) <= 0) return false;
    float lambert = Dot(nc, sc.light);
    f.colour = emissive ? colour : Shade(colour, 0.62f + 0.42f * max(0.0f, lambert) - 0.08f * max(0.0f, -lambert));
    f.depth = depth / n + bias;
    f.line = 0;
    f.closed = true;
    f.top = top;
    list.push_back(std::move(f));
    return true;
}

typedef std::vector<gp::PointF> Outline;    // in design units

static Outline Circle(float cu, float cv, float r, int n) {
    Outline o;
    for (int i = 0; i < n; i++) o.push_back(gp::PointF(cu + r * cosf(2 * kPi * i / n), cv + r * sinf(2 * kPi * i / n)));
    return o;
}

static Outline RoundRect(float u, float v, float w, float h, float r) {
    Outline o;
    r = min(r, min(w, h) / 2);
    const float cu[4] = { u + w - r, u + w - r, u + r, u + r }, cv[4] = { v + r, v + h - r, v + h - r, v + r };
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= 5; i++) {
            float a = (-90 + c * 90 + i * 18) * kPi / 180;
            o.push_back(gp::PointF(cu[c] + r * cosf(a), cv[c] + r * sinf(a)));
        }
    }
    return o;
}

// Outward normals (in the u/v plane) of each edge i -> i+1 of a closed outline.
static std::vector<gp::PointF> EdgeNormals(const Outline& o) {
    float area = 0;
    size_t n = o.size();
    for (size_t i = 0; i < n; i++) area += o[i].X * o[(i + 1) % n].Y - o[(i + 1) % n].X * o[i].Y;
    std::vector<gp::PointF> normals(n);
    for (size_t i = 0; i < n; i++) {
        float du = o[(i + 1) % n].X - o[i].X, dv = o[(i + 1) % n].Y - o[i].Y;
        float len = max(1e-4f, sqrtf(du * du + dv * dv));
        normals[i] = area > 0 ? gp::PointF(dv / len, -du / len) : gp::PointF(-dv / len, du / len);
    }
    return normals;
}

static void FlatShape(Scene& sc, const Outline& o, float y, gp::Color colour, bool emissive = false, float bias = 0) {
    std::vector<V3> pts;
    for (const gp::PointF& a : o) pts.push_back(P(a.X, a.Y, y));
    AddPoly(sc, sc.deco, pts.data(), (int)pts.size(), V3{ 0, 1, 0 }, colour, emissive, bias);
}

// The body: the outline at four heights (bevelled top and bottom edges).
static const float kTopY = 38;

struct BodyRings { std::vector<V3> top, up, down, bottom; std::vector<gp::PointF> normals; };

static const BodyRings& Body() {
    static BodyRings rings;
    if (!rings.top.empty()) return rings;
    static const float pts[][2] = {
        { 250, 118 }, { 500, 108 }, { 750, 118 }, { 868, 150 }, { 938, 300 }, { 978, 480 }, { 940, 596 }, { 868, 606 },
        { 800, 520 }, { 716, 440 }, { 500, 428 }, { 284, 440 }, { 200, 520 }, { 132, 606 }, { 60, 596 }, { 22, 480 },
        { 62, 300 }, { 132, 150 },
    };
    gp::PointF curve[ARRAYSIZE(pts)];
    for (int i = 0; i < (int)ARRAYSIZE(pts); i++) curve[i] = gp::PointF(pts[i][0], pts[i][1]);
    gp::GraphicsPath path;
    path.AddClosedCurve(curve, ARRAYSIZE(pts), 0.42f);
    path.Flatten(nullptr, 0.3f);
    int count = path.GetPointCount();
    std::vector<gp::PointF> flat(count);
    path.GetPathPoints(flat.data(), count);
    // Even spacing, so the sides are similar-sized quads.
    float total = 0;
    for (int i = 0; i < count; i++) {
        gp::PointF a = flat[i], b = flat[(i + 1) % count];
        total += sqrtf((b.X - a.X) * (b.X - a.X) + (b.Y - a.Y) * (b.Y - a.Y));
    }
    const int kSides = 120;
    Outline o;
    float step = total / kSides, along = 0, next = 0;
    for (int i = 0; i < count && (int)o.size() < kSides; i++) {
        gp::PointF a = flat[i], b = flat[(i + 1) % count];
        float len = sqrtf((b.X - a.X) * (b.X - a.X) + (b.Y - a.Y) * (b.Y - a.Y));
        while (next <= along + len && (int)o.size() < kSides) {
            float t = len > 0 ? (next - along) / len : 0;
            o.push_back(gp::PointF(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t));
            next += step;
        }
        along += len;
    }
    rings.normals = EdgeNormals(o);
    size_t n = o.size();
    for (size_t i = 0; i < n; i++) {
        gp::PointF na = rings.normals[(i + n - 1) % n], nb = rings.normals[i];
        float nu = na.X + nb.X, nv = na.Y + nb.Y, l = max(1e-4f, sqrtf(nu * nu + nv * nv));
        nu /= l; nv /= l;
        const float inset = 13;
        rings.top.push_back(P(o[i].X - nu * inset, o[i].Y - nv * inset, kTopY));
        rings.up.push_back(P(o[i].X, o[i].Y, 24));
        rings.down.push_back(P(o[i].X, o[i].Y, -24));
        rings.bottom.push_back(P(o[i].X - nu * inset, o[i].Y - nv * inset, -kTopY));
    }
    return rings;
}

static void BuildBody(Scene& sc, gp::Color colour, gp::Color light) {
    const BodyRings& b = Body();
    size_t n = b.top.size();
    for (size_t i = 0; i < n; i++) {
        size_t j = (i + 1) % n;
        V3 out{ b.normals[i].X, 0, b.normals[i].Y };
        V3 bandTop[4] = { b.top[i], b.top[j], b.up[j], b.up[i] };
        V3 side[4] = { b.up[i], b.up[j], b.down[j], b.down[i] };
        V3 bandBottom[4] = { b.down[i], b.down[j], b.bottom[j], b.bottom[i] };
        V3 mid = (b.up[i] + b.up[j]) * 0.5f;
        bool bar = light.GetA() && mid.z < -180 && fabsf(mid.x) < 105;  // the light bar, along the back edge
        AddPoly(sc, sc.body, bandTop, 4, out + V3{ 0, 1, 0 }, colour);
        AddPoly(sc, sc.body, side, 4, out, bar ? light : colour, bar);
        AddPoly(sc, sc.body, bandBottom, 4, out + V3{ 0, -1, 0 }, colour);
    }
    AddPoly(sc, sc.body, b.top.data(), (int)n, V3{ 0, 1, 0 }, colour, false, 0, true);
    AddPoly(sc, sc.body, b.bottom.data(), (int)n, V3{ 0, -1, 0 }, colour);
}

// The preview only shows how the pad is held: its top carries the touchpad and light bar,
// no buttons.
static void BuildSimpleTop(Scene& sc, gp::Color body, gp::Color light) {
    const Palette& p = Pal();
    gp::Color pad = Mix(body, p.text, g_dark ? 0.14f : 0.08f), dark = Mix(body, gp::Color(255, 0, 0, 0), g_dark ? 0.35f : 0.12f);
    FlatShape(sc, RoundRect(372, 129, 256, 8, 4), kTopY + 0.4f, light.GetA() ? light : dark, light.GetA() != 0);
    FlatShape(sc, RoundRect(330, 144, 340, 174, 22), kTopY + 0.4f, pad);
}

static void DrawFaces(gp::Graphics& g, std::vector<Face>& faces) {
    std::sort(faces.begin(), faces.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });
    for (const Face& f : faces) {
        if (f.line > 0) {
            gp::Pen pen(f.colour, f.line);
            pen.SetLineJoin(gp::LineJoinRound);
            pen.SetStartCap(gp::LineCapRound);
            pen.SetEndCap(gp::LineCapRound);
            if (f.closed) g.DrawPolygon(&pen, f.pts.data(), (int)f.pts.size());
            else g.DrawLines(&pen, f.pts.data(), (int)f.pts.size());
            continue;
        }
        gp::SolidBrush brush(f.colour);
        g.FillPolygon(&brush, f.pts.data(), (int)f.pts.size());
        gp::Pen seam(f.colour, 1.0f);       // covers the hairline gaps antialiasing leaves between faces
        g.DrawPolygon(&seam, f.pts.data(), (int)f.pts.size());
    }
}

// The view: a base tilt, then the user's spin, then the pad's own motion.
static const float kViewTilt = 0.38f;       // ~22 degrees: seen from where you hold it, as the real one
static float g_viewYaw, g_viewPitch, g_spinYaw, g_spinPitch;
static bool  g_dragPad, g_viewReset;
static POINT g_dragLast;
static ULONGLONG g_dragTick;

static gp::PointF DrawPad3D(gp::Graphics& g, gp::RectF area, const PadState& s, gp::Color light, bool motion, float opacity) {
    const Palette& p = Pal();
    Scene sc;
    sc.scale = min(area.Width / 1000.0f, area.Height / 640.0f) * 0.94f;
    sc.cx = area.X + area.Width / 2;
    sc.cy = area.Y + area.Height / 2;
    sc.dist = 2600;
    sc.light = Normal(V3{ -0.35f, 0.75f, 0.6f });
    M3 device = RotY(0);
    if (motion) {                           // the raw orientation, and moved by its acceleration
        float w = s.q[0], x = s.q[1], y = s.q[2], z = s.q[3];
        device = { 1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                   2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                   2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y) };
        auto clamp = [](float v) { return max(-1.5f, min(1.5f, v)); };
        sc.cx += clamp(s.lin[0]) * S(40);       // sideways: left and right
        sc.cy -= clamp(s.lin[1]) * S(30);       // up and down
        sc.scale *= 1 + 0.15f * clamp(s.lin[1] + s.lin[2]);    // up, or towards you: closer, bigger
    }
    sc.r = RotX(kViewTilt + g_viewPitch) * RotY(g_viewYaw) * device;

    gp::Color body = g_dark ? gp::Color(255, 70, 70, 76) : gp::Color(255, 238, 238, 242);
    BuildBody(sc, body, light);
    BuildSimpleTop(sc, body, light);

    // The light bar's glow behind everything.
    gp::PointF bar = sc.Project(sc.Cam(P(500, 112, 0)));
    if (light.GetA()) FillGlow(g, bar.X, bar.Y, 240 * sc.scale, 120 * sc.scale, WithAlpha(light, 130));

    // Shadow under the pad.
    gp::PointF floor = sc.Project(sc.Cam(P(500, 360, -kTopY)));
    FillGlow(g, floor.X, floor.Y + 40 * sc.scale, 520 * sc.scale, 200 * sc.scale, gp::Color(g_dark ? 120 : 45, 0, 0, 0));

    std::sort(sc.body.begin(), sc.body.end(), [](const Face& a, const Face& b) { return a.depth < b.depth; });
    for (Face& f : sc.body) {
        std::vector<Face> one(1, f);
        DrawFaces(g, one);
        if (f.top) DrawFaces(g, sc.deco);   // decorations only show while the top face does
    }
    if (opacity < 1) {                      // no controller: wash the drawing out
        gp::SolidBrush wash(WithAlpha(p.bg, (int)(255 * (1 - opacity))));
        g.FillRectangle(&wash, area);
    }
    return bar;
}

static void StepView(float dt) {
    if (g_viewReset) {                      // ease back to straight ahead
        float target = roundf(g_viewYaw / (2 * kPi)) * 2 * kPi;
        float k = 1 - expf(-dt * 8);
        g_viewYaw += (target - g_viewYaw) * k;
        g_viewPitch -= g_viewPitch * k;
        if (fabsf(target - g_viewYaw) < 0.002f && fabsf(g_viewPitch) < 0.002f) {
            g_viewYaw = g_viewPitch = 0;
            g_viewReset = false;
        }
        return;
    }
    if (g_dragPad) return;
    g_viewYaw += g_spinYaw * dt;
    g_viewPitch = max(-1.1f, min(1.1f, g_viewPitch + g_spinPitch * dt));
    float friction = expf(-dt * 1.4f);      // a flick keeps turning and slows down
    g_spinYaw *= friction;
    g_spinPitch *= friction;
    if (fabsf(g_spinYaw) < 0.03f) g_spinYaw = 0;
    if (fabsf(g_spinPitch) < 0.03f) g_spinPitch = 0;
    if (!g_spinPitch) {                     // tipping back level once it stops
        g_viewPitch *= expf(-dt * 1.5f);
        if (fabsf(g_viewPitch) < 0.002f) g_viewPitch = 0;
    }
}

// ---- header pieces

static const wchar_t kGlyphPin[] = L"\xE718", kGlyphPinned[] = L"\xE840", kGlyphSun[] = L"\xE706",
                     kGlyphMoon[] = L"\xE708", kGlyphChevron[] = L"\xE70D", kGlyphCheck[] = L"\xE73E",
                     kGlyphBluetooth[] = L"\xE702", kGlyphUsb[] = L"\xE88E", kGlyphPad[] = L"\xE7FC",
                     kGlyphDelete[] = L"\xE74D", kGlyphRecord[] = L"\xE7C8", kGlyphMouse[] = L"\xE962";
static const wchar_t* const kThemeNames[] = { L"Automatic", L"Light", L"Dark" };

// Under the battery: "Charging", or how long it should last ("Roughly" until it has watched
// this controller drain for a while).
static std::wstring BatteryNote(Pad* pad, const PadState& s) {
    if (!pad || s.battery < 0) return L"";
    if (s.charging) return L"Charging";
    int minutes = pad->minutesLeft;
    if (minutes < 0) return L"";
    const wchar_t* how = pad->estimateSure ? L"About" : L"Roughly";
    wchar_t text[64];
    if (minutes < 60) swprintf_s(text, L"%s %d minutes left", how, max(5, (minutes + 2) / 5 * 5));
    else if (minutes < 90) swprintf_s(text, L"%s 1 hour left", how);
    else swprintf_s(text, L"%s %.0f hours left", how, minutes / 60.0);
    return text;
}

static void DrawBattery(gp::Graphics& g, float centerX, float top, int level, const wchar_t* note) {
    const Palette& p = Pal();
    float w = S(84), h = S(32), cap = S(4);
    gp::RectF r(centerX - (w + cap) / 2, top, w, h);
    gp::Color outline = WithAlpha(p.subtext, 170);
    StrokeRound(g, r, S(10), outline, S(1.6));
    FillRound(g, gp::RectF(r.X + w + S(2), r.Y + h * 0.33f, cap, h * 0.34f), S(1.5), outline);
    if (level >= 0) {
        gp::RectF inner(r.X + S(3), r.Y + S(3), r.Width - S(6), r.Height - S(6));
        FillRound(g, gp::RectF(inner.X, inner.Y, max(S(8), inner.Width * level / 100.0f), inner.Height), S(7),
                  WithAlpha(BatteryColor(level), 210));
    }
    wchar_t text[16];
    if (level >= 0) swprintf_s(text, L"%d%%", level);
    else wcscpy_s(text, L"\x2014");
    Label(g, text, g_fonts.section, p.text, r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    Label(g, note, g_fonts.caption, p.subtext, gp::RectF(centerX - S(130), top + h + S(3), S(260), S(18)), gp::StringAlignmentCenter);
}

// Signal: four bars from the share of the controller's reports that arrive (Bluetooth has
// no signal strength Windows will report, but lost reports are what a weak link means).
static void DrawSignal(gp::Graphics& g, float x, float top, const Pad* pad) {
    if (pad && pad->generic) return;        // (measured from the DualShock 4's own report counter)
    const Palette& p = Pal();
    if (!pad || !pad->bluetooth) return;
    int link = pad->link;
    int bars = link < 0 ? 0 : link >= 98 ? 4 : link >= 93 ? 3 : link >= 80 ? 2 : 1;
    gp::Color on = bars >= 3 ? kGreen : bars == 2 ? kBatteryYellow : kBatteryRed;
    float bw = S(5), gap = S(3), h = S(26), base = top + h;
    for (int i = 0; i < 4; i++) {
        float bh = h * (i + 1) / 4.0f;
        FillRound(g, gp::RectF(x + i * (bw + gap), base - bh, bw, bh), S(1.5), i < bars ? on : WithAlpha(p.subtext, 70));
    }
    wchar_t text[32];
    if (link < 0) wcscpy_s(text, L"\x2014");
    else swprintf_s(text, L"%d%%", link);
    float tx = x + 4 * (bw + gap) + S(4);
    Label(g, text, g_fonts.section, p.text, gp::RectF(tx, top, S(60), h));
    const wchar_t* word = bars == 4 ? L"Strong signal" : bars == 3 ? L"Good signal" : bars == 2 ? L"Weak signal"
                        : bars == 1 ? L"Poor signal" : L"Signal";
    Label(g, word, g_fonts.caption, p.subtext, gp::RectF(x - S(4), top + h + S(9), S(110), S(18)));
}

static void DrawIconButton(gp::Graphics& g, gp::RectF r, ElemKind kind, const wchar_t* glyph, const wchar_t* label, bool on, int id = 0) {
    const Palette& p = Pal();
    int e = AddElem(kind, r, id);
    FillRound(g, r, S(10), Hovered(e) || on ? p.hover : WithAlpha(p.card, 255));
    StrokeRound(g, r, S(10), p.line, S(1));
    gp::Color c = on ? kAccent : p.text;
    if (!label) {
        Label(g, glyph, g_fonts.icon, c, r, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
        return;
    }
    float textX = glyph ? S(34) : S(12);
    if (glyph) Label(g, glyph, g_fonts.icon, c, gp::RectF(r.X + S(10), r.Y, S(20), r.Height), gp::StringAlignmentCenter);
    Label(g, label, g_fonts.caption, p.text, gp::RectF(r.X + textX, r.Y, r.Width - textX - S(24), r.Height));
    Label(g, kGlyphChevron, g_fonts.glyph, p.subtext, gp::RectF(r.X + r.Width - S(26), r.Y, S(16), r.Height), gp::StringAlignmentCenter);
}

static gp::RectF DrawDropdownRow(gp::Graphics& g, gp::RectF r, const wchar_t* title, const wchar_t* detail, const wchar_t* value,
                                 ElemKind kind, bool open, bool live) {
    const Palette& p = Pal();
    float buttonW = S(124);
    gp::RectF button(r.X + r.Width - S(12) - buttonW, r.Y + (r.Height - S(32)) / 2, buttonW, S(32));
    float textW = button.X - r.X - S(24);
    Label(g, title, g_fonts.body, live ? p.text : WithAlpha(p.text, 110), gp::RectF(r.X + S(16), r.Y + S(9), textW, S(20)));
    Label(g, detail, g_fonts.caption, live ? p.subtext : WithAlpha(p.subtext, 110), gp::RectF(r.X + S(16), r.Y + S(30), textW, S(18)));
    if (live) {
        DrawIconButton(g, button, kind, nullptr, value, open);
    } else {
        StrokeRound(g, button, S(10), WithAlpha(p.line, 120), S(1));
        Label(g, value, g_fonts.caption, WithAlpha(p.text, 110), gp::RectF(button.X + S(12), button.Y, button.Width - S(20), button.Height));
    }
    return button;
}

// A row with text on the left and a small segmented choice on the right.
static void DrawChoiceRow(gp::Graphics& g, gp::RectF r, const wchar_t* title, const wchar_t* detail, int id,
                          const wchar_t* const* labels, int count, int selected, bool live) {
    const Palette& p = Pal();
    gp::RectF seg(r.X + r.Width - S(12) - S(136), r.Y + (r.Height - S(30)) / 2, S(136), S(30));
    float textW = seg.X - r.X - S(24);
    Label(g, title, g_fonts.body, live ? p.text : WithAlpha(p.text, 110), gp::RectF(r.X + S(16), r.Y + S(9), textW, S(20)));
    Label(g, detail, g_fonts.caption, live ? p.subtext : WithAlpha(p.subtext, 110), gp::RectF(r.X + S(16), r.Y + S(30), textW, S(18)));
    DrawSegmented(g, seg, id, labels, count, selected, live);
}

// A drop-down list of rows under (or, without room, above) the anchor.
static void DrawMenu(gp::Graphics& g, gp::RectF anchor, float width, float limit, ElemKind kind, int id,
                     const wchar_t* const* names, int count, int selected, const wchar_t* const* glyphs = nullptr) {
    const Palette& p = Pal();
    float rowH = S(34);
    float h = rowH * count + S(8);
    float y = anchor.Y + anchor.Height + S(6);
    if (y + h > limit) y = anchor.Y - S(6) - h;
    gp::RectF menu(anchor.X + anchor.Width - width, y, width, h);
    FillRound(g, gp::RectF(menu.X, menu.Y + S(3), menu.Width, menu.Height), S(12), p.shadow);
    FillRound(g, menu, S(12), WithAlpha(p.card, 255));
    StrokeRound(g, menu, S(12), p.line, S(1));
    for (int i = 0; i < count; i++) {
        gp::RectF row(menu.X + S(4), menu.Y + S(4) + rowH * i, menu.Width - S(8), rowH);
        int e = AddElem(kind, row, id, i);
        if (Hovered(e)) FillRound(g, row, S(8), p.hover);
        float x = row.X + S(12);
        if (glyphs) {
            Label(g, glyphs[i], g_fonts.icon, p.text, gp::RectF(row.X + S(8), row.Y, S(22), row.Height), gp::StringAlignmentCenter);
            x = row.X + S(38);
        }
        Label(g, names[i], g_fonts.body, p.text, gp::RectF(x, row.Y, row.X + row.Width - S(34) - x, row.Height));
        if (i == selected) {
            Label(g, kGlyphCheck, g_fonts.icon, kAccent, gp::RectF(row.X + row.Width - S(30), row.Y, S(22), row.Height),
                  gp::StringAlignmentCenter);
        }
    }
}

static gp::Color PadLight(Pad* p, int battery) {
    if (p->generic) return gp::Color(0, 0, 0, 0);       // no light bar
    BYTE rgb[3];
    LightColour(p, battery, rgb);
    return gp::Color((BYTE)(p->set[S_LIGHTMODE] == LIGHT_OFF ? 0 : 255), rgb[0], rgb[1], rgb[2]);
}

// One tab per connected controller: its light bar colour, name and battery.
static void DrawTabs(gp::Graphics& g, float x, float y, float width, Pad* selected) {
    const Palette& p = Pal();
    std::vector<Pad*> ready;
    for (Pad* pad : g_pads) if (pad->ready) ready.push_back(pad);
    float h = S(34), gap = S(8);
    if (ready.empty()) {
        gp::RectF tab(x, y, S(190), h);
        gp::Pen dash(WithAlpha(p.subtext, 120), S(1));
        dash.SetDashStyle(gp::DashStyleDash);
        gp::GraphicsPath path;
        RoundPath(path, tab, S(10));
        g.DrawPath(&dash, &path);
        Label(g, kGlyphPad, g_fonts.icon, p.subtext, gp::RectF(tab.X + S(10), tab.Y, S(22), h), gp::StringAlignmentCenter);
        Label(g, L"No controller", g_fonts.caption, p.subtext, gp::RectF(tab.X + S(38), tab.Y, tab.Width - S(46), h));
        return;
    }
    float tabW = min(S(210), (width - gap * (ready.size() - 1)) / ready.size());
    for (size_t i = 0; i < ready.size(); i++) {
        Pad* pad = ready[i];
        gp::RectF tab(x + (tabW + gap) * i, y, tabW, h);
        int e = AddElem(E_TAB, tab, (int)i);
        bool on = pad == selected;
        if (on) FillRound(g, gp::RectF(tab.X, tab.Y + S(1), tab.Width, tab.Height), S(10), p.shadow);
        FillRound(g, tab, S(10), on ? p.segmentOn : Hovered(e) ? p.hover : p.card);
        if (!on) StrokeRound(g, tab, S(10), p.line, S(1));
        AcquireSRWLockShared(&pad->lock);
        int battery = pad->state.battery;
        ReleaseSRWLockShared(&pad->lock);
        gp::Color light = PadLight(pad, battery);
        float cy = tab.Y + h / 2;
        if (light.GetA()) FillGlow(g, tab.X + S(18), cy, S(13), S(13), WithAlpha(light, 150));
        gp::SolidBrush dot(light.GetA() ? light : p.subtext);
        g.FillEllipse(&dot, tab.X + S(13), cy - S(5), S(10), S(10));
        wchar_t name[64], level[16] = L"";
        swprintf_s(name, L"%d  \x00B7  %s", (int)i + 1, pad->name.c_str());
        if (battery >= 0) swprintf_s(level, L"%d%%", battery);
        Label(g, name, on ? g_fonts.section : g_fonts.caption, p.text, gp::RectF(tab.X + S(32), tab.Y, tab.Width - S(32) - S(46), h));
        Label(g, level, g_fonts.caption, on ? p.text : p.subtext, gp::RectF(tab.X + tab.Width - S(50), tab.Y, S(40), h),
              gp::StringAlignmentFar);
    }
}

static HICON MakeIcon(int size, gp::Color glow) {
    gp::Bitmap bmp(size, size, PixelFormat32bppARGB);
    {
        gp::Graphics g(&bmp);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.Clear(gp::Color(0, 0, 0, 0));
        float s = (float)size;
        FillGlow(g, s * 0.5f, s * 0.5f, s * 0.5f, s * 0.5f, WithAlpha(glow, 255));
        gp::SolidBrush white(kWhite);
        FillRound(g, gp::RectF(s * 0.10f, s * 0.30f, s * 0.80f, s * 0.34f), s * 0.14f, kWhite);
        g.FillEllipse(&white, s * 0.08f, s * 0.40f, s * 0.34f, s * 0.36f);
        g.FillEllipse(&white, s * 0.58f, s * 0.40f, s * 0.34f, s * 0.36f);
    }
    HICON icon = nullptr;
    bmp.GetHICON(&icon);
    return icon;
}

// ---- the shortcuts panel (a card over the window)

static bool g_panelOpen;
static int  g_actionMenuRow = -1;
static int  g_recordRow = -1;           // the row being re-recorded, -1 a new one
static DWORD g_recordUnion;             // every button held since recording started
static std::wstring g_panelHint;

static void StopRecording() {
    g_recording = false;
    g_recordUnion = 0;
}

static void StartRecording(int row) {
    g_recordRow = row;
    g_recordUnion = 0;
    g_panelHint.clear();
    g_actionMenuRow = -1;
    g_recording = true;
}

// Called every frame while recording: a combo is done when all its buttons are let go.
static void UpdateRecording() {
    if (!g_recording) return;
    DWORD held = 0;
    for (Pad* p : g_pads) {
        if (!p->ready) continue;
        AcquireSRWLockShared(&p->lock);
        held |= p->state.buttons;
        ReleaseSRWLockShared(&p->lock);
    }
    if (held) { g_recordUnion |= held; return; }
    if (!g_recordUnion) return;
    DWORD combo = g_recordUnion;
    g_recordUnion = 0;
    if (BitCount(combo) < 2) {
        g_panelHint = L"Hold at least two buttons together.";
        return;
    }
    AcquireSRWLockExclusive(&g_shortcutLock);
    for (size_t i = 0; i < g_shortcuts.size(); i++) {      // one action per combo
        if (g_shortcuts[i].buttons == combo && (int)i != g_recordRow) {
            g_shortcuts.erase(g_shortcuts.begin() + i);
            if (g_recordRow > (int)i) g_recordRow--;
            break;
        }
    }
    int row;
    if (g_recordRow >= 0 && g_recordRow < (int)g_shortcuts.size()) {
        g_shortcuts[g_recordRow].buttons = combo;
        row = g_recordRow;
    } else {
        g_shortcuts.push_back({ combo, A_DISCONNECT });
        row = (int)g_shortcuts.size() - 1;
    }
    ReleaseSRWLockExclusive(&g_shortcutLock);
    SaveShortcuts();
    StopRecording();
    g_panelHint.clear();
    if (g_recordRow < 0) g_actionMenuRow = row;     // a new one: pick what it does
}

static void DrawShortcutPanel(gp::Graphics& g, float w, float h) {
    const Palette& p = Pal();
    g_elems.clear();                        // only the panel takes clicks
    gp::SolidBrush dim(WithAlpha(p.bg, 190));
    g.FillRectangle(&dim, 0.0f, 0.0f, w, h);

    AcquireSRWLockShared(&g_shortcutLock);
    std::vector<Shortcut> list = g_shortcuts;
    ReleaseSRWLockShared(&g_shortcutLock);
    bool addingNew = g_recording && g_recordRow < 0;
    int rows = (int)list.size() + (addingNew ? 1 : 0);
    float rowH = S(54), cardW = S(640);
    float listH = rowH * max(rows, 1);
    float cardH = S(86) + listH + S(28) + S(52);
    gp::RectF card((w - cardW) / 2, max(S(20), (h - cardH) / 2), cardW, cardH);
    FillRound(g, gp::RectF(card.X, card.Y + S(6), card.Width, card.Height), S(16), p.shadow);
    FillRound(g, card, S(16), WithAlpha(p.card, 255));
    StrokeRound(g, card, S(16), p.line, S(1));

    float x = card.X + S(24), right = card.X + card.Width - S(24);
    Label(g, L"Shortcuts", g_fonts.title, p.text, gp::RectF(x, card.Y + S(18), S(300), S(28)));
    Label(g, L"Hold the buttons together on any controller. Games don't see a combo's buttons while it's held.",
          g_fonts.caption, p.subtext, gp::RectF(x, card.Y + S(48), right - x, S(18)));

    float y = card.Y + S(80);
    gp::Pen line(p.line, S(1));
    gp::RectF menuAnchor;
    int menuAction = 0;
    if (rows == 0) Label(g, L"No shortcuts yet", g_fonts.body, p.subtext, gp::RectF(x, y, S(300), rowH));
    float pulse = 0.5f + 0.5f * sinf(GetTickCount64() / 160.0f);
    for (int i = 0; i < rows; i++) {
        gp::RectF row(card.X + S(12), y, card.Width - S(24), rowH);
        if (i) g.DrawLine(&line, x, y, right, y);
        bool isNew = i == (int)list.size();
        bool recordingThis = g_recording && (isNew || i == g_recordRow);
        gp::RectF chips(row.X, row.Y + S(7), S(300), rowH - S(14));
        if (recordingThis) {
            FillRound(g, chips, S(8), WithAlpha(kAccent, 30 + (int)(30 * pulse)));
            StrokeRound(g, chips, S(8), WithAlpha(kAccent, 120 + (int)(100 * pulse)), S(1.5));
            std::wstring text = g_recordUnion ? ComboText(g_recordUnion) : L"Press the buttons\x2026";
            Label(g, kGlyphRecord, g_fonts.icon, kAccent, gp::RectF(chips.X + S(8), chips.Y, S(22), chips.Height), gp::StringAlignmentCenter);
            Label(g, text.c_str(), g_fonts.body, p.text, gp::RectF(chips.X + S(36), chips.Y, chips.Width - S(44), chips.Height));
            DrawButton(g, gp::RectF(right - S(96), row.Y + S(11), S(96), S(32)), BTN_SC_CANCEL, L"Cancel", false);
        } else {
            int e = g_recording ? -1 : AddElem(E_SC_ROW, chips, i);
            if (Hovered(e)) FillRound(g, chips, S(8), p.hover);
            Label(g, ComboText(list[i].buttons).c_str(), g_fonts.body, p.text, gp::RectF(chips.X + S(12), chips.Y, chips.Width - S(20), chips.Height));
            gp::RectF action(row.X + S(316), row.Y + S(10), S(232), S(34));
            if (!g_recording) {
                DrawIconButton(g, action, E_SC_ACTION, nullptr, kActionNames[list[i].action], g_actionMenuRow == i, i);
                gp::RectF del(right - S(34), row.Y + S(10), S(34), S(34));
                int d = AddElem(E_SC_DELETE, del, i);
                if (Hovered(d)) FillRound(g, del, S(8), p.hover);
                Label(g, kGlyphDelete, g_fonts.icon, Hovered(d) ? kBatteryRed : p.subtext, del, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
            } else {
                Label(g, kActionNames[list[i].action], g_fonts.caption, p.subtext, gp::RectF(action.X + S(12), action.Y, action.Width, action.Height));
            }
            if (g_actionMenuRow == i) { menuAnchor = action; menuAction = (int)list[i].action; }
        }
        y += rowH;
    }
    if (rows) g.DrawLine(&line, x, y, right, y);
    Label(g, g_panelHint.empty() ? (g_recording ? L"Let go of all buttons to finish." : L"Click a combo to record it again.")
                                 : g_panelHint.c_str(),
          g_fonts.caption, g_panelHint.empty() ? p.subtext : kBatteryRed, gp::RectF(x, y + S(4), right - x, S(20)));

    float by = card.Y + card.Height - S(52);
    bool canAdd = !g_recording && (int)list.size() < kMaxShortcuts;
    DrawButton(g, gp::RectF(x, by, S(170), S(34)), BTN_SC_ADD, L"Record a shortcut", true, canAdd);
    DrawButton(g, gp::RectF(right - S(110), by, S(110), S(34)), BTN_SC_DONE, L"Done", true);
    DrawButton(g, gp::RectF(right - S(110) - S(10) - S(150), by, S(150), S(34)), BTN_SC_RESET, L"Reset to default", false, !g_recording);

    if (g_actionMenuRow >= 0 && g_actionMenuRow < (int)list.size() && !g_recording)
        DrawMenu(g, menuAnchor, S(280), h - S(8), E_SC_ACTIONITEM, g_actionMenuRow, kActionNames, A_COUNT, menuAction);
}

// ---- whole window

static std::wstring g_mapText;          // the last press, "Cross -> A", or a shortcut
static ULONGLONG g_mapTick;
static DWORD g_mapButtons;
static Pad* g_mapPad;
static unsigned g_paintedSeq;
static Pad* g_paintedPad;
static const ULONGLONG kMapShowMs = 1500, kSearchDotMs = 400;

static void UpdateTaskbarBattery(bool force);

static void PaintWindow(HWND hwnd, HDC target) {
    UpdateTaskbarBattery(false);
    RECT rc;
    GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    // GDI+ draws much faster into its own premultiplied bitmap than into a device context.
    static gp::Bitmap* canvas;
    if (!canvas || (int)canvas->GetWidth() != rc.right || (int)canvas->GetHeight() != rc.bottom) {
        delete canvas;
        canvas = new gp::Bitmap(rc.right, rc.bottom, PixelFormat32bppPARGB);
    }
    {
        gp::Graphics g(canvas);
        g_canvas = canvas;
        g_canvasGraphics = &g;
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        const Palette& p = Pal();
        g_elems.clear();

        Pad* pad = SelectedPad();
        bool live = pad != nullptr;
        for (int slot = 0; slot <= kSlotSpout; slot++) {
            float goal = ToggleValue(slot) ? 1.0f : 0.0f;
            g_knob[slot] += (goal - g_knob[slot]) * 0.35f;
            if (fabsf(goal - g_knob[slot]) < 0.01f) g_knob[slot] = goal;
        }

        PadState s;
        if (pad) {
            AcquireSRWLockShared(&pad->lock);
            s = pad->state;
            ReleaseSRWLockShared(&pad->lock);
            g_paintedSeq = pad->seq;
        }
        g_paintedPad = pad;
        ULONGLONG now = GetTickCount64();
        static ULONGLONG lastPaint = now;
        StepView(min(0.1f, (now - lastPaint) / 1000.0f));
        lastPaint = now;
        UpdateRecording();
        if (pad != g_mapPad) { g_mapPad = pad; g_mapButtons = s.buttons; g_mapText.clear(); }
        DWORD pressed = s.buttons & ~g_mapButtons;
        g_mapButtons = s.buttons;
        for (const ButtonMap& m : kButtonMap) {
            if (!(pressed & m.pad) || g_recording) continue;
            bool unmapped = m.pad == B_TOUCH && !pad->set[S_TOUCHBACK];
            g_mapText = std::wstring(m.from) + L"  \x2192  " + (unmapped ? L"nothing" : m.to);
            g_mapTick = now;
        }

        // Geometry: settings either side of the controller.
        float w = (float)rc.right, h = (float)rc.bottom, m = S(kMargin);
        float headerY = S(kTabsH);
        float top = headerY + S(kHeaderH), bottom = h - S(kFooterH) - S(12);
        float sideW = S(kSideW);
        float lx = m, rx = w - m - sideW;
        float cxl = lx + sideW + m, cxr = rx - m;
        float bandH = S(150);               // under the controller: the motion preview, then the values
        gp::RectF centre(cxl, top, cxr - cxl, bottom - top - bandH);
        PadLayout layout = LayoutPad(centre);
        gp::RectF preview(layout.X(500) - S(125), layout.Y(612), S(250), S(112));
        int previewElem = AddElem(E_PAD3D, preview, 0);

        // No glow behind the controller, except the ambilight: pressed buttons, fingers on the
        // touchpad, and a pulsing ring when the battery runs low.
        g.Clear(p.bg);
        gp::Color light = pad ? PadLight(pad, s.battery) : gp::Color(0, 0, 0, 0);
        bool emulating = pad && (pad->emulating || pad->fake) && g_enabled;
        float splitGoal = live && pad->set[S_TPMOUSE] && pad->set[S_TPSPLIT] ? 1.0f : 0.0f;
        g_split += (splitGoal - g_split) * 0.18f;
        if (fabsf(splitGoal - g_split) < 0.01f) g_split = splitGoal;
        UpdateAmbient(s, live);
        static AmbientField windowField;
        DrawAmbient(g, layout, w, h, windowField);

        DrawTabs(g, m, S(10), w - 2 * m, pad);

        // Header: name and status; battery in the middle; master switch, on-top and theme.
        const wchar_t* glyph = !pad ? kGlyphBluetooth : pad->bluetooth ? kGlyphBluetooth : kGlyphUsb;
        Label(g, glyph, g_fonts.icon, pad ? kAccent : WithAlpha(kAccent, 160), gp::RectF(m, headerY + S(14), S(22), S(28)),
              gp::StringAlignmentCenter);
        wchar_t title[96], status[200];
        if (pad) {
            wcscpy_s(title, pad->name.c_str());
            const wchar_t* link = pad->dongle ? L"Wireless adapter" : pad->bluetooth ? L"Bluetooth" : L"USB";
            wchar_t player[32] = L"";
            if (pad->player >= 0) swprintf_s(player, L", player %d", pad->player + 1);
            bool hidden = emulating && pad->set[S_HIDE] && hidhide::g_ready && hidhide::g_ours.count(pad->instanceId);
            if (!g_enabled) swprintf_s(status, L"%s   \x00B7   Paused", link);
            else if (!g_vigem) swprintf_s(status, L"%s   \x00B7   ViGEmBus driver missing", link);
            else if (!pad->set[S_EMULATE]) swprintf_s(status, L"%s   \x00B7   Not emulating", link);
            else swprintf_s(status, L"%s   \x00B7   Xbox 360 pad%s%s", link, player, hidden ? L"   \x00B7   Hidden" : L"");
        } else {
            int dots = (int)((now / kSearchDotMs) % 4);
            swprintf_s(title, L"Searching%.*s", dots, L"...");
            DWORD error = g_openError;
            if (error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION)
                wcscpy_s(status, L"A controller is held exclusively by another app");
            else wcscpy_s(status, g_vigem ? L"Turn on or plug in a DualShock 4" : L"ViGEmBus driver not found");
        }
        Label(g, title, g_fonts.title, p.text, gp::RectF(m + S(26), headerY + S(14), S(330), S(28)));
        Label(g, status, g_fonts.caption, p.subtext, gp::RectF(m, headerY + S(42), S(400), S(20)));
        float batteryX = pad && pad->bluetooth && !pad->generic ? w / 2 - S(50) : w / 2;     // the signal goes beside it
        DrawBattery(g, batteryX, headerY + S(22), pad ? s.battery : -1, BatteryNote(pad, s).c_str());
        DrawSignal(g, w / 2 + S(22), headerY + S(25), pad);
        gp::RectF themeBtn(w - m - S(124), headerY + S(20), S(124), S(36));
        gp::RectF pinBtn(themeBtn.X - S(10) - S(36), headerY + S(20), S(36), S(36));
        DrawIconButton(g, pinBtn, E_PIN, g_onTop ? kGlyphPinned : kGlyphPin, nullptr, g_onTop);

        const wchar_t* themeGlyph = g_themeMode == THEME_LIGHT ? kGlyphSun : g_themeMode == THEME_DARK ? kGlyphMoon
                                    : (g_dark ? kGlyphMoon : kGlyphSun);
        DrawIconButton(g, themeBtn, E_THEME, themeGlyph, g_themeMode == THEME_AUTO ? L"Auto" : kThemeNames[g_themeMode], g_themeOpen);

        // Columns, centred vertically on the taller (right) one.
        float rightH = S(24) + S(kSliderRowH) * kTuningSliders + S(20) + S(24) + S(34) + S(10) + S(kSliderRowH) * 3;
        float leftH = S(34) + S(10) + S(kRowH) * kEmulationCount + S(kSliderRowH);    // the Emulation tab, the tallest
        float y0 = top + max(0.0f, (bottom - top - max(leftH, rightH)) / 2);

        // Left column: Emulation, Touchpad and Sound, one at a time.
        static const wchar_t* const leftTabs[] = { L"Emulation", L"Touchpad", L"Sound" };
        DrawSegmented(g, gp::RectF(lx, y0, sideW, S(34)), SEG_LEFT_TAB, leftTabs, 3, g_leftTab, true);
        float y = y0 + S(34) + S(10);
        gp::Pen line(p.line, S(1));
        auto separator = [&](float at) { g.DrawLine(&line, lx + S(16), at, lx + sideW, at); };
        if (g_leftTab == 0) {
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * kEmulationCount + S(kSliderRowH)), S(12), p.card);
            for (int i = 0; i < kEmulationCount; i++) {
                int setting = kEmulationToggles[i];
                if (i) separator(y);
                bool ds4Only = setting == S_RUMBLE || setting == S_MOTION;
                bool usable = live && (setting != S_HIDE || hidhide::g_ready) && (setting != S_IDLE || pad->bluetooth)
                              && !(ds4Only && pad->generic);
                const wchar_t* detail = setting == S_HIDE && !hidhide::g_ready ? L"Needs the HidHide driver"
                                      : ds4Only && pad && pad->generic ? L"Not on this controller"
                                      : setting == S_MOTION && pad && !s.imu ? L"Waiting for motion data"
                                      : ToggleTitle(setting, true);
                const wchar_t* title = setting == S_HIDE && pad && pad->generic ? L"Hide the real pad" : ToggleTitle(setting, false);
                DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), setting, title, detail, usable);
                y += S(kRowH);
                if (setting == S_RUMBLE) {          // its strength, right under it
                    DrawSliderRow(g, gp::RectF(lx, y - S(8), sideW, S(kSliderRowH)), SLIDER_RUMBLE, live && !pad->generic && pad->set[S_RUMBLE]);
                    y += S(kSliderRowH) - S(8);
                }
            }        } else if (g_leftTab == 1) {
            bool touch = live && !pad->generic;     // generic pads have no touchpad
            bool mouse = touch && pad->set[S_TPMOUSE];
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * 5 + S(kSliderRowH)), S(12), p.card);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPMOUSE, ToggleTitle(S_TPMOUSE, false),
                          live && pad->generic ? L"This controller has no touchpad" : ToggleTitle(S_TPMOUSE, true), touch);
            y += S(kRowH); separator(y);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPSPLIT, ToggleTitle(S_TPSPLIT, false),
                          ToggleTitle(S_TPSPLIT, true), mouse);
            y += S(kRowH); separator(y);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPTAP, ToggleTitle(S_TPTAP, false),
                          ToggleTitle(S_TPTAP, true), mouse);
            y += S(kRowH); separator(y);
            g_twoAnchor = DrawDropdownRow(g, gp::RectF(lx, y, sideW, S(kRowH)), L"Two fingers", L"Tap or click",
                                          kTwoFingerNames[SettingValue(S_TPTWO) % TWO_COUNT], E_TP_TWO, g_twoMenuOpen, mouse);
            y += S(kRowH); separator(y);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPSCROLL, ToggleTitle(S_TPSCROLL, false),
                          ToggleTitle(S_TPSCROLL, true), mouse);
            y += S(kRowH); separator(y);
            DrawSliderRow(g, gp::RectF(lx, y, sideW, S(kSliderRowH)), SLIDER_TPSPEED, mouse);
            y += S(kSliderRowH) + S(8);
            Label(g, L"Games don't get the touchpad click then.", g_fonts.caption, p.subtext,
                  gp::RectF(lx + S(4), y, sideW, S(18)));
            if (!mouse) g_twoMenuOpen = false;
        } else {
            bool bt = live && pad->bluetooth && !pad->generic, on = bt && pad->set[S_AUDIO];
            bool mouse = live && pad->set[S_TPMOUSE];
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * 6 + S(kSliderRowH)), S(12), p.card);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_AUDIO, ToggleTitle(S_AUDIO, false),
                          live && !pad->bluetooth ? L"Not needed on USB" : ToggleTitle(S_AUDIO, true), bt);
            y += S(kRowH); separator(y);
            std::wstring from = SourceLabel();
            g_srcAnchor = DrawDropdownRow(g, gp::RectF(lx, y, sideW, S(kRowH)), L"Plays from", L"A playback device",
                                          from.c_str(), E_SRC, g_srcMenuOpen, bt);
            y += S(kRowH); separator(y);
            static const wchar_t* const outputs[] = { L"Jack", L"Speaker" };
            DrawChoiceRow(g, gp::RectF(lx, y, sideW, S(kRowH)), L"Plays on", L"Where to play", SEG_AUDIO_OUT,
                          outputs, 2, SettingValue(S_AUDIOOUT) ? 1 : 0, bt);
            y += S(kRowH); separator(y);
            DrawSliderRow(g, gp::RectF(lx, y, sideW, S(kSliderRowH)), SLIDER_AUDIOVOL, bt);
            y += S(kSliderRowH); separator(y);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPVOLUME, ToggleTitle(S_TPVOLUME, false),
                          mouse ? L"Needs the touchpad mouse off" : ToggleTitle(S_TPVOLUME, true), live && !pad->generic && !mouse);
            y += S(kRowH); separator(y);
            const wchar_t* micDetail = !bt ? (live ? L"On USB it's a Windows mic already" : ToggleTitle(S_MIC, true))
                                     : s.mic ? (pad->set[S_MIC] ? L"Plugged in, on" : L"Plugged in")
                                     : L"No headset mic plugged in";
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_MIC, ToggleTitle(S_MIC, false), micDetail, bt);
            y += S(kRowH); separator(y);
            std::wstring micTo = MicDestLabel();
            g_micAnchor = DrawDropdownRow(g, gp::RectF(lx, y, sideW, S(kRowH)), L"Mic goes to", L"A playback device",
                                          micTo.c_str(), E_MICDST, g_micMenuOpen, bt && pad->set[S_MIC]);
            y += S(kRowH) + S(8);
            wchar_t note[160];
            if (live && !pad->bluetooth) wcscpy_s(note, L"On USB the controller is its own sound\ndevice: pick it in Windows' sound settings.");
            else if (!on) wcscpy_s(note, L"Experimental. Apps that play to the chosen\ndevice play on the controller.");
            else if (audio::g_error == audio::kNoDevice)
                wcscpy_s(note, L"No sound device for it yet: install VB-Cable\nor pick one under \"Plays from\".");
            else if (audio::g_error) swprintf_s(note, L"Couldn't open the sound device (0x%08lX).", (unsigned long)audio::g_error.load());
            else if (!pad->audioOn) wcscpy_s(note, L"Starting\x2026");
            else swprintf_s(note, L"Playing \x00B7 %u reports sent%s", pad->audioSent.load(),
                            pad->audioFailed ? L", some failed" : L"");
            if (bt && pad->set[S_MIC]) {        // the mic's state matters more once it's on
                if (micout::g_error == audio::kNoDevice) wcscpy_s(note, L"Mic: install VB-Audio Hi-Fi Cable, or pick\na device under \"Mic goes to\".");
                else if (micout::g_error) swprintf_s(note, L"Mic: couldn't open the device (0x%08lX).", (unsigned long)micout::g_error.load());
                else if (pad->micOn) swprintf_s(note, L"Mic on \x00B7 %u frames%s", pad->micFrames.load(), pad->micBad ? L", some unreadable" : L"");
            }
            gp::StringFormat fmt;
            gp::SolidBrush brush(p.subtext);
            g.DrawString(note, -1, g_fonts.caption, gp::RectF(lx + S(4), y, sideW, S(40)), &fmt, &brush);
        }

        y = DrawSectionLabel(g, rx, y0, sideW, L"STICKS AND TRIGGERS");
        FillRound(g, gp::RectF(rx, y, sideW, S(kSliderRowH) * kTuningSliders), S(12), p.card);
        for (int i = 0; i < kTuningSliders; i++) {
            if (i) g.DrawLine(&line, rx + S(16), y, rx + sideW, y);
            DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), i, live);
            y += S(kSliderRowH);
        }
        static const wchar_t* const lights[LIGHT_COUNT] = { L"Battery", L"Colour", L"Rainbow", L"Chroma", L"Off" };
        int mode = SettingValue(S_LIGHTMODE);
        y += S(20);
        DrawSectionLabel(g, rx, y, sideW, L"LIGHT BAR");
        bool lightLive = live && !pad->generic;     // generic pads have no light bar
        if (lightLive && mode == LIGHT_CHROMA) {
            int state = g_chromaState;
            gp::Color dot = state == CHROMA_LIVE ? kGreen : state == CHROMA_WAITING || state == CHROMA_STARTING || state == CHROMA_OFF
                          ? kBatteryYellow : kBatteryRed;
            gp::RectF note(rx + S(80), y, sideW - S(84), S(20));
            Label(g, ChromaText(), g_fonts.caption, p.subtext, note, gp::StringAlignmentFar);
            gp::RectF measured;
            g.MeasureString(ChromaText(), -1, g_fonts.caption, gp::PointF(0, 0), &measured);
            gp::SolidBrush dotBrush(dot);
            g.FillEllipse(&dotBrush, note.X + note.Width - measured.Width - S(12), y + S(7), S(7), S(7));
        }
        y += S(24);
        DrawSegmented(g, gp::RectF(rx, y, sideW, S(34)), SEG_LIGHT, lights, LIGHT_COUNT, mode, lightLive);
        y += S(34) + S(10);
        FillRound(g, gp::RectF(rx, y, sideW, S(kSliderRowH) * 3), S(12), p.card);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 4, lightLive && mode != LIGHT_OFF);
        y += S(kSliderRowH);
        g.DrawLine(&line, rx + S(16), y, rx + sideW, y);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 5, lightLive && (mode == LIGHT_COLOUR || mode == LIGHT_CHROMA));
        y += S(kSliderRowH);
        g.DrawLine(&line, rx + S(16), y, rx + sideW, y);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 6, lightLive && mode == LIGHT_RAINBOW);
        // The controller; under it the raw values and the last mapping, and the motion card.
        DrawPad(g, layout, s, light, live ? (emulating ? 1.0f : 0.75f) : 0.4f);
        if (live && !pad->generic) DrawPad3D(g, preview, s, light, pad->set[S_MOTION] && s.imu, 1);   // drag to spin, double-click to level
        (void)previewElem;
        float infoX = cxl, infoW = cxr - cxl, ty = preview.Y + preview.Height + S(4);
        if (pad) {
            wchar_t info[160];
            swprintf_s(info, L"L  %+.2f, %+.2f   \x00B7   R  %+.2f, %+.2f   \x00B7   L2  %d   \x00B7   R2  %d",
                       (s.lx - 127.5) / 127.5, -(s.ly - 127.5) / 127.5, (s.rx - 127.5) / 127.5, -(s.ry - 127.5) / 127.5, s.l2, s.r2);
            Label(g, info, g_fonts.caption, p.subtext, gp::RectF(infoX, ty, infoW, S(18)), gp::StringAlignmentCenter);
            if (!g_mapText.empty() && now - g_mapTick < kMapShowMs) {
                int alpha = (int)min(255.0, (kMapShowMs - (now - g_mapTick)) * 255.0 / 400);
                Label(g, g_mapText.c_str(), g_fonts.big, WithAlpha(p.text, alpha), gp::RectF(infoX, ty + S(20), infoW, S(26)),
                      gp::StringAlignmentCenter);
            }
        } else {
            Label(g, L"Hold PS + Share to pair over Bluetooth, or plug in a cable", g_fonts.caption, p.subtext,
                  gp::RectF(infoX, preview.Y + S(40), infoW, S(18)), gp::StringAlignmentCenter);
        }

        // Footer card: start with Windows, then the app buttons on the right.
        gp::RectF footer(m, h - S(kFooterH), w - 2 * m, S(kFooterH) - S(16));
        FillRound(g, footer, S(12), p.card);
        DrawToggleRow(g, gp::RectF(footer.X, footer.Y, S(250), footer.Height), kSlotStartup, L"Start with Windows", L"Waits in the tray");
        DrawToggleRow(g, gp::RectF(footer.X + S(250), footer.Y, S(250), footer.Height), kSlotSpout, L"Stream overlay",
                      L"Spout2, e.g. for OBS");
        float bx = footer.X + footer.Width - S(12), by = footer.Y + (footer.Height - S(32)) / 2;
        bx -= S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_RUMBLE, L"Test rumble", true, live && !pad->generic);
        bx -= S(10) + S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_SHORTCUTS, L"Shortcuts", false);
        bx -= S(10) + S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_DISCONNECT, L"Disconnect", false, live && pad->bluetooth);
        bx -= S(10) + S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_STATUS, L"Driver status", false);

        if (g_themeOpen) {
            static const wchar_t* glyphs[3];
            glyphs[0] = g_dark ? kGlyphMoon : kGlyphSun; glyphs[1] = kGlyphSun; glyphs[2] = kGlyphMoon;
            DrawMenu(g, themeBtn, S(170), h, E_THEME_ITEM, 0, kThemeNames, 3, g_themeMode, glyphs);
        }
        if (g_twoMenuOpen && g_leftTab == 1)
            DrawMenu(g, g_twoAnchor, S(170), h, E_TP_TWOITEM, 0, kTwoFingerNames, TWO_COUNT, SettingValue(S_TPTWO));
        if (g_micMenuOpen && g_leftTab == 2) {
            std::vector<const wchar_t*> names;
            names.push_back(L"Automatic (Hi-Fi Cable Input)");
            int selected = 0;
            std::wstring dest = micout::Dest();
            for (size_t i = 0; i < g_srcDevices.size(); i++) {
                names.push_back(g_srcDevices[i].name.c_str());
                if (g_srcDevices[i].id == dest) selected = (int)i + 1;
            }
            DrawMenu(g, gp::RectF(lx, g_micAnchor.Y, S(320), g_micAnchor.Height), S(320), h, E_MICDSTITEM, 0, names.data(),
                     (int)names.size(), selected);
        }
        if (g_srcMenuOpen && g_leftTab == 2) {
            std::vector<const wchar_t*> names;
            names.push_back(L"Automatic (DualShock or CABLE Input)");
            int selected = 0;
            std::wstring source = audio::Source();
            for (size_t i = 0; i < g_srcDevices.size(); i++) {
                names.push_back(g_srcDevices[i].name.c_str());
                if (g_srcDevices[i].id == source) selected = (int)i + 1;
            }
            names.push_back(L"Mirror everything the PC plays");
            if (source == audio::kMirror) selected = (int)names.size() - 1;
            DrawMenu(g, gp::RectF(lx, g_srcAnchor.Y, S(320), g_srcAnchor.Height), S(320), h, E_SRCITEM, 0, names.data(),
                     (int)names.size(), selected);
        }
        if (g_panelOpen) DrawShortcutPanel(g, w, h);
        g_canvasGraphics = nullptr;
    }
    gp::Graphics screen(target);
    screen.SetCompositingMode(gp::CompositingModeSourceCopy);
    screen.SetInterpolationMode(gp::InterpolationModeNearestNeighbor);
    screen.DrawImage(canvas, 0, 0, rc.right, rc.bottom);
}

// ---- behaviour

static bool SystemUsesDarkApps() {
    DWORD light = 1, size = sizeof(light);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
    return light == 0;
}

static void ApplyTheme() {
    g_dark = g_themeMode == THEME_DARK || (g_themeMode == THEME_AUTO && SystemUsesDarkApps());
    BOOL dark = g_dark;
    if (FAILED(DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark))))      // DWMWA_USE_IMMERSIVE_DARK_MODE
        DwmSetWindowAttribute(g_window, 19, &dark, sizeof(dark));              // its number before 20H1
    SetWindowPos(g_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    InvalidateRect(g_window, nullptr, FALSE);
}

static void ApplyOnTop() {
    SetWindowPos(g_window, g_onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static std::wstring StatusText() {
    std::wstring text;
    wchar_t line[512];
    text += std::wstring(kAppName) + L" " + kVersion + L"\n\n";
    text += g_vigem ? L"ViGEmBus: connected (virtual Xbox 360 controllers)\n"
                    : L"ViGEmBus: NOT FOUND - install it to emulate Xbox controllers\n";
    if (!hidhide::Installed()) {
        text += L"HidHide: not installed - games will also see the real controller\n";
    } else if (!hidhide::g_ready) {
        text += L"HidHide: installed, but busy (close its configuration window)\n";
    } else {
        swprintf_s(line, L"HidHide: ready, hiding %d controller(s)\n", (int)hidhide::g_ours.size());
        text += line;
    }
    if (g_chromaState == CHROMA_OFF) {
        text += L"Razer Chroma: not in use (pick Chroma under Light bar)\n";
    } else {
        swprintf_s(line, L"Razer Chroma: %s", ChromaText());
        text += line;
        if (g_chromaState == CHROMA_FAILED) {
            swprintf_s(line, L" (error %ld)", g_chromaError.load());
            text += line;
        }
        text += L"\n";
    }
    AcquireSRWLockShared(&g_shortcutLock);
    swprintf_s(line, L"Shortcuts: %d\n\n", (int)g_shortcuts.size());
    ReleaseSRWLockShared(&g_shortcutLock);
    text += line;
    int count = 0;
    for (Pad* p : g_pads) {
        if (!p->ready) continue;
        AcquireSRWLockShared(&p->lock);
        PadState s = p->state;
        ReleaseSRWLockShared(&p->lock);
        swprintf_s(line, L"%d. %s, %s, %s\n     address %s, reports %d/%d bytes\n", ++count, p->name.c_str(),
                   p->dongle ? L"wireless adapter" : p->bluetooth ? L"Bluetooth" : L"USB",
                   p->emulating ? L"emulating" : L"not emulating", p->serial.c_str(), p->inLen, p->outLen);
        text += line;
        if (p->generic) {
            swprintf_s(line, L"     generic gamepad %04X:%04X, %s, battery %s\n     reports: %.300s\n", p->vid, p->pid,
                       p->layout.map.name, p->layout.battery.present ? L"in its reports" : p->layout.batteryFeature.present
                       ? L"in a feature report" : L"not reported", p->layout.fields.c_str());
            text += line;
        }
        if (s.imu) {
            swprintf_s(line, L"     motion: accel %d %d %d, gyro %d %d %d, moving %+.2f %+.2f %+.2f g\n", s.accel[0], s.accel[1],
                       s.accel[2], s.gyro[0], s.gyro[1], s.gyro[2], s.lin[0], s.lin[1], s.lin[2]);
            text += line;
        }
        if (!p->generic) { swprintf_s(line, L"     link: %d%% of reports arrive, %d reports/s (interval setting %d), %u corrupted dropped\n", p->link.load(),
                   p->reportRate.load(), p->poll.load(), p->crcBad.load()); text += line; }
        if (p->set[S_MIC]) {
            swprintf_s(line, L"     mic: %s, headset mic %s, %u frames, %u unreadable, %u echo skipped, target 0x%02X, output %s (0x%08lX)\n",
                       p->micOn ? L"on" : L"off", s.mic ? L"plugged in" : L"not plugged in", p->micFrames.load(),
                       p->micBad.load(), p->micEcho.load(), p->micTarget.load() & 0xFF,
                       micout::g_running ? L"running" : L"stopped", (unsigned long)micout::g_error.load());
            text += line;
        }
        if (p->set[S_AUDIO]) {
            swprintf_s(line, L"     sound: %s, %u reports sent, %u failed, %u dropouts, capture %s (0x%08lX)\n",
                       p->audioOn ? L"on" : L"off", p->audioSent.load(), p->audioFailed.load(), p->audioDropouts.load(),
                       audio::g_running ? L"running" : L"stopped",
                       (unsigned long)audio::g_error.load());
            text += line;
        }
    }
    if (!count) text += L"No controller connected.\n";
    DWORD error = g_openError;
    if (error) {
        swprintf_s(line, L"\nA controller couldn't be opened (error %lu)%s.\n", error,
                   error == ERROR_ACCESS_DENIED || error == ERROR_SHARING_VIOLATION ? L": another app holds it exclusively" : L"");
        text += line;
    }
    if (ProcessRunning(L"DS4Windows.exe"))
        text += L"\nDS4Windows is running too: close it, or every controller gets two Xbox pads.\n";
    return text;
}

static void Refresh();

static void ClosePanel() {
    StopRecording();
    g_actionMenuRow = -1;
    g_panelHint.clear();
    g_panelOpen = false;
}

static void Activate(HWND hwnd, const Elem& e) {
    Pad* pad = SelectedPad();
    switch (e.kind) {
    case E_TOGGLE:
        if (e.id == kSlotSpout) {
            g_spoutOn = !g_spoutOn;
            SaveGlobals();
            ApplySpout();
            return;
        }
        if (e.id == kSlotStartup) {
            SetStartsWithWindows(!g_startup);
            g_startup = StartsWithWindows();
            return;
        }
        if (e.id == kSlotEnabled) g_enabled = !g_enabled;
        else if (pad) pad->set[e.id] = !pad->set[e.id];
        break;
    case E_SEGMENT:
        if (e.id == SEG_LEFT_TAB) { g_leftTab = e.sub; g_twoMenuOpen = g_srcMenuOpen = g_micMenuOpen = false; return; }
        if (!pad) return;
        if (e.id == SEG_AUDIO_OUT) { pad->set[S_AUDIOOUT] = e.sub; break; }
        if (e.sub == LIGHT_RAINBOW && pad->set[S_LIGHTMODE] != LIGHT_RAINBOW) pad->lastLight = pad->set[S_LIGHTMODE].load();
        pad->set[S_LIGHTMODE] = e.sub;
        if (e.sub == LIGHT_CHROMA) {        // picking it again retries a connection that failed
            int state = g_chromaState;
            if (state == CHROMA_FAILED || state == CHROMA_HUNG || state == CHROMA_MISSING) {
                StopChroma();
                g_chromaState = CHROMA_OFF;
            }
        }
        break;
    case E_BUTTON:
        switch (e.id) {
        case BTN_STATUS:     MessageBoxW(hwnd, StatusText().c_str(), kAppName, MB_ICONINFORMATION); break;
        case BTN_RUMBLE:     if (pad) { pad->testUntil = GetTickCount64() + 500; SetEvent(pad->outputWake); } break;
        case BTN_DISCONNECT:
            if (pad && !DisconnectBluetooth(pad)) MessageBoxW(hwnd, L"Couldn't disconnect the controller.", kAppName, MB_ICONWARNING);
            break;
        case BTN_SHORTCUTS:  g_panelOpen = true; g_themeOpen = false; break;
        case BTN_SC_ADD:     StartRecording(-1); break;
        case BTN_SC_CANCEL:  StopRecording(); g_panelHint.clear(); break;
        case BTN_SC_DONE:    ClosePanel(); break;
        case BTN_SC_RESET:
            AcquireSRWLockExclusive(&g_shortcutLock);
            g_shortcuts.assign(1, kDefaultShortcut);
            ReleaseSRWLockExclusive(&g_shortcutLock);
            SaveShortcuts();
            g_actionMenuRow = -1;
            break;
        }
        return;
    case E_SC_ROW:
        StartRecording(e.id);
        return;
    case E_SC_ACTION:
        g_actionMenuRow = g_actionMenuRow == e.id ? -1 : e.id;
        return;
    case E_SC_ACTIONITEM:
        AcquireSRWLockExclusive(&g_shortcutLock);
        if (e.id < (int)g_shortcuts.size()) g_shortcuts[e.id].action = e.sub;
        ReleaseSRWLockExclusive(&g_shortcutLock);
        SaveShortcuts();
        g_actionMenuRow = -1;
        return;
    case E_SC_DELETE:
        AcquireSRWLockExclusive(&g_shortcutLock);
        if (e.id < (int)g_shortcuts.size()) g_shortcuts.erase(g_shortcuts.begin() + e.id);
        ReleaseSRWLockExclusive(&g_shortcutLock);
        SaveShortcuts();
        g_actionMenuRow = -1;
        return;
    case E_PIN:
        g_onTop = !g_onTop;
        ApplyOnTop();
        break;
    case E_THEME:
        g_themeOpen = !g_themeOpen;
        return;
    case E_THEME_ITEM:
        g_themeMode = e.sub;
        g_themeOpen = false;
        ApplyTheme();
        break;
    case E_TP_TWO:
        g_twoMenuOpen = !g_twoMenuOpen;
        return;
    case E_SRC:
        g_srcMenuOpen = !g_srcMenuOpen;
        if (g_srcMenuOpen) g_srcDevices = audio::Devices();
        return;
    case E_MICDST:
        g_micMenuOpen = !g_micMenuOpen;
        if (g_micMenuOpen) g_srcDevices = audio::Devices();
        return;
    case E_MICDSTITEM:
        g_micMenuOpen = false;
        micout::SetDest(e.sub == 0 || e.sub > (int)g_srcDevices.size() ? L"" : g_srcDevices[e.sub - 1].id);
        return;
    case E_SRCITEM:
        g_srcMenuOpen = false;
        if (e.sub == 0) audio::SetSource(L"");
        else if (e.sub <= (int)g_srcDevices.size()) audio::SetSource(g_srcDevices[e.sub - 1].id);
        else audio::SetSource(audio::kMirror);
        return;
    case E_TP_TWOITEM:
        g_twoMenuOpen = false;
        if (!pad) return;
        pad->set[S_TPTWO] = e.sub;
        break;
    case E_TPBUTTON:
        if (!pad) return;
        pad->set[S_TPMOUSE] = !pad->set[S_TPMOUSE];
        break;
    case E_TAB: {
        int index = 0;
        for (Pad* p : g_pads) if (p->ready && index++ == e.id) g_selected = p;
        return;
    }
    default:
        return;
    }
    SaveGlobals();
    if (pad) SavePadSettings(pad);
    Refresh();
}

static int HitTest(LPARAM lp) {
    gp::PointF pt((float)GET_X_LPARAM(lp), (float)GET_Y_LPARAM(lp));
    for (int i = (int)g_elems.size(); i-- > 0;) {       // last drawn is on top
        if (g_elems[i].rc.Contains(pt)) return i;
    }
    return -1;
}

static ElemKind HitKind(int hit) { return hit >= 0 && hit < (int)g_elems.size() ? g_elems[hit].kind : E_TOGGLE; }

// Whether the next frame would differ from the last one.
static bool Animating() {
    Pad* pad = SelectedPad();
    if (pad != g_paintedPad || !pad) return true;       // searching dots
    if (pad->seq != g_paintedSeq) return true;
    if (g_dragPad || g_viewReset || g_spinYaw || g_spinPitch || g_viewPitch || g_recording) return true;
    float splitGoal = pad->set[S_TPMOUSE] && pad->set[S_TPSPLIT] ? 1.0f : 0.0f;
    if (g_split != splitGoal) return true;
    int mode = pad->set[S_LIGHTMODE];
    if (mode == LIGHT_RAINBOW || mode == LIGHT_CHROMA) return true;
    if (AmbientActive() || (g_leftTab == 2 && pad->audioOn)) return true;
    ULONGLONG now = GetTickCount64();
    if (now - g_mapTick < kMapShowMs + 100 || now < pad->testUntil + 100 || pad->rumbleStrong || pad->rumbleWeak) return true;
    for (int slot = 0; slot <= kSlotSpout; slot++) {
        if (g_knob[slot] != (ToggleValue(slot) ? 1.0f : 0.0f)) return true;
    }
    return false;
}

static void ShowWindowFront() {
    g_startup = StartsWithWindows();
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    SetTimer(g_window, 1, 16, nullptr);     // repaint ~60 fps while shown, if anything moved
}

// The window's taskbar button shows the selected controller's battery as its progress bar, in
// the battery's colour (green, yellow under 35 %, red under 15 %); none when it isn't known.
static ITaskbarList3* g_taskbar;
static wchar_t g_taskbarNote[96];           // self-test: what the taskbar was last told
static const UINT WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarButtonCreated");

static void UpdateTaskbarBattery(bool force = false) {
    if (!g_taskbar || !g_window) return;
    int level = -1;
    Pad* p = SelectedPad();
    if (p && p->ready) {
        AcquireSRWLockShared(&p->lock);
        level = p->state.battery;
        ReleaseSRWLockShared(&p->lock);
    }
    TBPFLAG state = level < 0 ? TBPF_NOPROGRESS : level < kBatteryCriticalLevel ? TBPF_ERROR
                  : level < kBatteryLowLevel ? TBPF_PAUSED : TBPF_NORMAL;
    static int lastLevel = -2;
    static TBPFLAG lastState;
    if (!force && level == lastLevel && state == lastState) return;
    lastLevel = level;
    lastState = state;
    HRESULT a = g_taskbar->SetProgressState(g_window, state);
    HRESULT b = level >= 0 ? g_taskbar->SetProgressValue(g_window, max(level, 1), 100) : S_OK;
    swprintf_s(g_taskbarNote, L"Taskbar: battery %d%%, state %d (%s)\n", level, (int)state,
               SUCCEEDED(a) && SUCCEEDED(b) ? L"set" : L"FAILED");
}

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_TASKBARCREATED) {         // (again whenever the button is made anew)
        if (!g_taskbar && FAILED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_taskbar))))
            g_taskbar = nullptr;
        if (g_taskbar && FAILED(g_taskbar->HrInit())) { g_taskbar->Release(); g_taskbar = nullptr; }
        UpdateTaskbarBattery(true);
        return 0;
    }
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        PaintWindow(hwnd, dc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (Animating()) InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_MOUSEMOVE: {
        if (g_dragSlider >= 0) {
            SetSliderFromX(g_dragSlider, (float)GET_X_LPARAM(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        if (g_dragPad) {                    // spin the model; the speed is kept for the flick
            int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
            ULONGLONG now = GetTickCount64();
            float dt = max(0.004f, (now - g_dragTick) / 1000.0f);
            float dyaw = (x - g_dragLast.x) * 0.010f / (float)g_dpi, dpitch = (y - g_dragLast.y) * 0.010f / (float)g_dpi;
            g_viewYaw += dyaw;
            g_viewPitch = max(-1.1f, min(1.1f, g_viewPitch + dpitch));
            g_spinYaw = g_spinYaw * 0.5f + dyaw / dt * 0.5f;
            g_spinPitch = g_spinPitch * 0.5f + dpitch / dt * 0.5f;
            g_dragLast = { x, y };
            g_dragTick = now;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        int hit = HitTest(lp);
        if (hit != g_hover) {
            g_hover = hit;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
            TrackMouseEvent(&tme);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        g_hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            ElemKind kind = HitKind(g_hover);
            SetCursor(LoadCursorW(nullptr, g_hover < 0 ? IDC_ARROW : kind == E_PAD3D ? IDC_SIZEALL : IDC_HAND));
            return TRUE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_LBUTTONDBLCLK:
        if (HitKind(HitTest(lp)) == E_PAD3D && HitTest(lp) >= 0 && !g_panelOpen) {
            g_viewReset = true;
            g_spinYaw = g_spinPitch = 0;
            if (Pad* pad = SelectedPad()) pad->motionReset = true;     // and level the controller again
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        [[fallthrough]];
    case WM_LBUTTONDOWN:
        g_pressed = HitTest(lp);
        SetCapture(hwnd);
        if (g_pressed >= 0 && g_elems[g_pressed].kind == E_SLIDER && !g_themeOpen) {
            g_dragSlider = g_elems[g_pressed].id;       // jump to the press, then follow the drag
            SetSliderFromX(g_dragSlider, (float)GET_X_LPARAM(lp));
            InvalidateRect(hwnd, nullptr, FALSE);
        } else if (g_pressed >= 0 && g_elems[g_pressed].kind == E_PAD3D && !g_themeOpen) {
            g_dragPad = true;
            g_viewReset = false;
            g_spinYaw = g_spinPitch = 0;
            g_dragLast = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            g_dragTick = GetTickCount64();
        }
        return 0;
    case WM_LBUTTONUP: {
        ReleaseCapture();
        InvalidateRect(hwnd, nullptr, FALSE);
        if (g_dragSlider >= 0) {
            g_dragSlider = -1;
            g_pressed = -1;
            if (Pad* pad = SelectedPad()) SavePadSettings(pad);
            return 0;
        }
        if (g_dragPad) {
            g_dragPad = false;
            g_pressed = -1;
            if (GetTickCount64() - g_dragTick > 80) g_spinYaw = g_spinPitch = 0;    // held still before letting go
            return 0;
        }
        int hit = HitTest(lp);
        bool same = hit >= 0 && hit == g_pressed;
        g_pressed = -1;
        // An open menu takes the next click: an item, its button, or anywhere to close.
        if (g_themeOpen && (!same || (g_elems[hit].kind != E_THEME_ITEM && g_elems[hit].kind != E_THEME))) {
            g_themeOpen = false;
            return 0;
        }
        if (g_twoMenuOpen && (!same || (g_elems[hit].kind != E_TP_TWOITEM && g_elems[hit].kind != E_TP_TWO))) {
            g_twoMenuOpen = false;
            return 0;
        }
        if (g_micMenuOpen && (!same || (g_elems[hit].kind != E_MICDSTITEM && g_elems[hit].kind != E_MICDST))) {
            g_micMenuOpen = false;
            return 0;
        }
        if (g_srcMenuOpen && (!same || (g_elems[hit].kind != E_SRCITEM && g_elems[hit].kind != E_SRC))) {
            g_srcMenuOpen = false;
            return 0;
        }
        if (g_actionMenuRow >= 0 && (!same || (g_elems[hit].kind != E_SC_ACTIONITEM && g_elems[hit].kind != E_SC_ACTION))) {
            g_actionMenuRow = -1;
            return 0;
        }
        if (same) Activate(hwnd, g_elems[hit]);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) {
            if (g_themeOpen) g_themeOpen = false;
            else if (g_actionMenuRow >= 0) g_actionMenuRow = -1;
            else if (g_twoMenuOpen) g_twoMenuOpen = false;
            else if (g_srcMenuOpen) g_srcMenuOpen = false;
            else if (g_micMenuOpen) g_micMenuOpen = false;
            else if (g_recording) StopRecording();
            else if (g_panelOpen) ClosePanel();
            else PostMessageW(hwnd, WM_CLOSE, 0, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet")) {
            LoadAccentColour();             // the accent colour may have changed too
            if (g_themeMode == THEME_AUTO) ApplyTheme();
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DWMCOLORIZATIONCOLORCHANGED:
        LoadAccentColour();
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_CAPTURECHANGED:
        if ((HWND)lp != hwnd) {             // capture taken away mid-drag
            if (g_dragSlider >= 0) {
                g_dragSlider = -1;
                if (Pad* pad = SelectedPad()) SavePadSettings(pad);
            }
            g_dragPad = false;
        }
        return 0;
    case WM_CLOSE:          // hide; the app keeps running in the tray
        KillTimer(hwnd, 1);
        g_themeOpen = false;
        ClosePanel();
        ShowWindow(hwnd, SW_HIDE);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------------------
// Tray icon

static UINT  g_taskbarCreated;
static bool  g_noTray;                  // self-test: no icon
static HICON g_trayIcon, g_bigIcon;
static int   g_iconTier = -99;

static void UpdateTray(HWND hwnd, DWORD action) {
    int count = 0, lowest = -1;
    for (Pad* p : g_pads) {
        if (!p->ready) continue;
        count++;
        AcquireSRWLockShared(&p->lock);
        int level = p->state.battery;
        ReleaseSRWLockShared(&p->lock);
        if (level >= 0 && (lowest < 0 || level < lowest)) lowest = level;
    }
    int tier = !count || !g_enabled ? -2 : lowest < 0 ? -1 : lowest < kBatteryCriticalLevel ? 0 : lowest < kBatteryLowLevel ? 1 : 2;
    if (tier != g_iconTier || !g_trayIcon) {
        gp::Color glow = tier == -2 ? gp::Color(255, 142, 142, 147) : tier == -1 ? kAccent : BatteryColor(lowest);
        if (g_trayIcon) DestroyIcon(g_trayIcon);
        if (g_bigIcon) DestroyIcon(g_bigIcon);
        g_trayIcon = MakeIcon(GetSystemMetrics(SM_CXSMICON), glow);
        g_bigIcon = MakeIcon(GetSystemMetrics(SM_CXICON), glow);
        SendMessageW(g_window, WM_SETICON, ICON_BIG, (LPARAM)g_bigIcon);
        SendMessageW(g_window, WM_SETICON, ICON_SMALL, (LPARAM)g_trayIcon);
        g_iconTier = tier;
    }
    if (g_noTray) return;
    NOTIFYICONDATAW nid{ sizeof(nid) };
    nid.hWnd = hwnd;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = g_trayIcon;
    wchar_t battery[32] = L"";
    if (lowest >= 0) swprintf_s(battery, L", battery %d%%", lowest);
    if (!count) swprintf_s(nid.szTip, L"%s - no controller", kAppName);
    else swprintf_s(nid.szTip, L"%s - %d controller%s%s%s", kAppName, count, count == 1 ? L"" : L"s", g_enabled ? L"" : L", paused", battery);
    Shell_NotifyIconW(action, &nid);
}

// After anything changed: selection, hiding, Chroma, tray and window.
// ---- stream overlay: the selected controller with its ambilight, on a transparent
// background, sent 60 times a second as a Spout2 sender ("DS4-Native") that OBS (with the
// Spout2 plugin) and other programs can take in. Keeps going with the window closed.

static const int kOverlayW = 800, kOverlayH = 560;
static const UINT WM_OVERLAY = WM_APP + 6;  // time for the next overlay frame (from the pacing thread)
static spoutDX* g_spout;
static HANDLE g_overlayClock;               // the pacing thread
static std::atomic<bool> g_overlayStop, g_overlayPending;
static LONGLONG g_overlayFrames, g_overlayFirst;  // frames sent, and when the first went (QPC)

static DWORD WINAPI OverlayClock(void*) {
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    LARGE_INTEGER freq, start, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    for (LONGLONG frame = 1; !g_overlayStop; frame++) {
        // frame n is due at start + n/60 s; a late frame doesn't push the rest back
        QueryPerformanceCounter(&now);
        LONGLONG due = start.QuadPart + frame * freq.QuadPart / 60;
        if (due < now.QuadPart - freq.QuadPart / 10) { start = now; frame = 0; continue; }   // fell well behind
        LARGE_INTEGER wait;
        wait.QuadPart = -max(0LL, (due - now.QuadPart) * 10000000 / freq.QuadPart);   // 100 ns units, relative
        SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE);
        WaitForSingleObject(timer, 100);
        if (!g_overlayPending.exchange(true)) PostMessageW(g_tray, WM_OVERLAY, 0, 0);   // one at a time
    }
    CloseHandle(timer);
    return 0;
}

static gp::Bitmap* RenderOverlay() {
    static gp::Bitmap* image;
    if (!image) image = new gp::Bitmap(kOverlayW, kOverlayH, PixelFormat32bppPARGB);
    {
        gp::Graphics og(image);
        og.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        og.SetTextRenderingHint(gp::TextRenderingHintAntiAliasGridFit);
        og.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        og.Clear(gp::Color(0, 0, 0, 0));
        Pad* pad = SelectedPad();
        if (pad) {
            AcquireSRWLockShared(&pad->lock);
            PadState s = pad->state;
            ReleaseSRWLockShared(&pad->lock);
            if (!IsWindowVisible(g_window)) {   // the window animates these while it's open
                UpdateAmbient(s, true);
                float splitGoal = pad->set[S_TPMOUSE] && pad->set[S_TPSPLIT] ? 1.0f : 0.0f;
                g_split += (splitGoal - g_split) * 0.18f;
                if (fabsf(splitGoal - g_split) < 0.01f) g_split = splitGoal;
            }
            PadLayout L = LayoutPad(gp::RectF(40, 60, kOverlayW - 80.0f, kOverlayH - 100.0f));
            g_canvas = image;                   // glows and the ambilight blend straight into it
            g_canvasGraphics = &og;
            g_overlayDrawing = true;
            static AmbientField overlayField;
            DrawAmbient(og, L, (float)kOverlayW, (float)kOverlayH, overlayField);
            DrawPad(og, L, s, PadLight(pad, s.battery), 1);
            g_overlayDrawing = false;
            g_canvasGraphics = nullptr;
            g_canvas = nullptr;
        }
    }
    return image;
}

static void SendOverlay() {
    g_overlayPending = false;
    if (!g_spout) return;
    gp::Bitmap* image = RenderOverlay();
    gp::Rect all(0, 0, kOverlayW, kOverlayH);
    gp::BitmapData data;
    if (image->LockBits(&all, gp::ImageLockModeRead, PixelFormat32bppPARGB, &data) != gp::Ok) return;
    // to straight alpha, which is what receivers take a BGRA sender's pixels as
    static std::vector<BYTE> pixels(kOverlayW * kOverlayH * 4);
    static int unpremultiply[256];          // 65536 / alpha
    if (!unpremultiply[1]) for (int a = 1; a < 256; a++) unpremultiply[a] = (255 * 65536 + a / 2) / a;
    for (int y = 0; y < kOverlayH; y++) {
        const BYTE* in = (const BYTE*)data.Scan0 + y * data.Stride;
        BYTE* out = pixels.data() + y * kOverlayW * 4;
        for (int x = 0; x < kOverlayW; x++, in += 4, out += 4) {
            BYTE a = in[3];
            if (a == 255) { *(DWORD*)out = *(const DWORD*)in; continue; }
            if (a == 0) { *(DWORD*)out = 0; continue; }
            int k = unpremultiply[a];
            out[0] = (BYTE)min(255, (in[0] * k + 32768) >> 16);
            out[1] = (BYTE)min(255, (in[1] * k + 32768) >> 16);
            out[2] = (BYTE)min(255, (in[2] * k + 32768) >> 16);
            out[3] = a;
        }
    }
    image->UnlockBits(&data);
    g_spout->SendImage(pixels.data(), kOverlayW, kOverlayH);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!g_overlayFrames++) g_overlayFirst = now.QuadPart;
}

static void ApplySpout() {
    if (g_spoutOn && !g_spout && (!g_noInput || g_testSpout)) {
        g_spout = new spoutDX;
        if (!g_spout->OpenDirectX11()) { delete g_spout; g_spout = nullptr; return; }
        g_spout->SetSenderName("DS4-Native");
        g_overlayStop = false;
        g_overlayPending = false;
        g_overlayClock = CreateThread(nullptr, 0, OverlayClock, nullptr, 0, nullptr);
    } else if (!g_spoutOn && g_spout) {
        g_overlayStop = true;
        if (g_overlayClock) { WaitForSingleObject(g_overlayClock, 1000); CloseHandle(g_overlayClock); g_overlayClock = nullptr; }
        g_spout->ReleaseSender();
        g_spout->CloseDirectX11();
        delete g_spout;
        g_spout = nullptr;
    }
}

static void Refresh() {
    UpdateTaskbarBattery(false);            // (also while the window is minimised and not painting)
    SelectedPad();
    SyncHiding();
    for (Pad* p : g_pads) {
        if (p->ready && p->set[S_LIGHTMODE] == LIGHT_CHROMA) StartChroma(g_tray);
    }
    bool sound = false;                     // capture the PC's sound while a controller plays it
    for (Pad* p : g_pads) sound = sound || (p->ready && p->bluetooth && !p->fake && p->set[S_AUDIO]);
    audio::Want(sound && !g_exiting);
    bool mic = false;                       // play a controller's mic while one has it on
    for (Pad* p : g_pads) mic = mic || (p->ready && p->bluetooth && !p->fake && p->set[S_MIC]);
    micout::Want(mic && !g_exiting);
    UpdateTray(g_tray, NIM_MODIFY);
    InvalidateRect(g_window, nullptr, FALSE);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_MENU_OPEN, L"Open DS4-Native");

    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_MENU_EXIT, L"Exit");
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);      // so the menu closes when clicking elsewhere
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

static void SelfTestFinish();
static gp::Bitmap* RenderOverlay();
static double g_overlayMs;                  // self-test: the overlay's render time

static LRESULT CALLBACK TrayProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) ShowTrayMenu(hwnd);
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) ShowWindowFront();
        return 0;
    case WM_PADS:
        if (wp) {                           // a controller just became ready: show its tab
            for (Pad* p : g_pads) if (p->ready) g_selected = p;
        }
        Refresh();
        return 0;
    case WM_OVERLAY:                        // the stream overlay's next frame
        SendOverlay();
        return 0;
    case WM_VOLUME: {
        wchar_t text[48];
        const wchar_t* what = (lp & 2) ? L"Controller volume" : L"Volume";
        if (lp & 1) swprintf_s(text, L"%s  muted", what);
        else swprintf_s(text, L"%s  %d%%", what, (int)wp);
        g_mapText = text;
        g_mapTick = GetTickCount64();
        InvalidateRect(g_window, nullptr, FALSE);
        return 0;
    }
    case WM_SHORTCUT:
        g_mapText = ComboText((DWORD)lp) + L"  \x2192  " + kActionNames[min((int)wp, A_COUNT - 1)];
        g_mapTick = GetTickCount64();
        Refresh();
        return 0;
    case WM_CHROMA:
        if (wp == CHROMA_MSG_INIT) {
            g_chromaError = (long)lp;
            if (g_chromaState != CHROMA_LIVE) g_chromaState = lp == 0 ? CHROMA_WAITING : CHROMA_FAILED;
        } else if (wp == CHROMA_MSG_COLOUR) {
            g_chromaColour = (DWORD)lp;
            g_chromaState = CHROMA_LIVE;
        } else if (wp == CHROMA_MSG_STATUS) {
            g_chromaState = lp == 1 ? CHROMA_LIVE : CHROMA_WAITING;
        }
        InvalidateRect(g_window, nullptr, FALSE);
        return 0;
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE || wp == DBT_DEVNODES_CHANGED) SetTimer(hwnd, 3, 300, nullptr);
        return TRUE;
    case WM_TIMER:
        if (wp == 3) KillTimer(hwnd, 3);    // device change, settled
        if (wp == 4) { SelfTestFinish(); return 0; }

        if (!hidhide::g_ready && wp == 2) hidhide::WhitelistSelf();
        if (g_chromaState == CHROMA_STARTING && GetTickCount64() - g_chromaStart > 15000) g_chromaState = CHROMA_HUNG;
        ScanPads();
        Refresh();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_MENU_OPEN:    ShowWindowFront(); break;
        case ID_MENU_ENABLED:
            g_enabled = !g_enabled;
            SaveGlobals();
            Refresh();
            break;
        case ID_MENU_EXIT:    DestroyWindow(hwnd); break;
        }
        return 0;
    case WM_DESTROY:
        if (!g_noTray) {
            NOTIFYICONDATAW nid{ sizeof(nid) };
            nid.hWnd = hwnd;
            nid.uID = 1;
            Shell_NotifyIconW(NIM_DELETE, &nid);
        }
        PostQuitMessage(0);
        return 0;
    default:
        if (msg == g_taskbarCreated) { UpdateTray(hwnd, NIM_ADD); return 0; }  // Explorer restarted
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------------------
// Self-test (DS4NATIVE_SELFTEST=folder): runs hidden for a few seconds, writes the window
// to shot.png and the driver status to status.txt there, and exits. DS4NATIVE_DEMO=1 adds a
// made-up controller so the window can be checked without one; DS4NATIVE_YAW=degrees turns
// the view, DS4NATIVE_PANEL=1 opens the shortcuts panel, DS4NATIVE_LIGHT=mode sets the light.

static std::wstring g_selfTest;

static void AddDemoPad() {
    Pad* p = new Pad;
    p->fake = true;
    p->path = L"demo";
    p->name = L"DualShock 4 v2";
    p->serial = L"demo";
    p->bluetooth = true;
    p->state.buttons = B_CROSS | B_L1 | B_RIGHT;
    p->state.lx = 220; p->state.ly = 60; p->state.r2 = 170;
    p->state.touch[0] = true; p->state.tx[0] = 600; p->state.ty[0] = 400;
    p->state.battery = 62;
    wchar_t level[16];
    if (GetEnvironmentVariableW(L"DS4NATIVE_BATTERY", level, 16)) p->state.battery = _wtoi(level);
    p->state.imu = true;
    float dq[4] = { 0.96f, 0.12f, 0.18f, -0.1f }, dn = sqrtf(dq[0] * dq[0] + dq[1] * dq[1] + dq[2] * dq[2] + dq[3] * dq[3]);
    for (int i = 0; i < 4; i++) p->state.q[i] = dq[i] / dn;
    p->state.oriented = true;
    p->minutesLeft = 260;
    p->link = 96;
    p->player = 0;
    p->ready = true;
    g_pads.push_back(p);
}

static void SelfTestFinish() {
    LARGE_INTEGER sentAt;                   // the overlay's frame rate, before the timings below hold it up
    QueryPerformanceCounter(&sentAt);
    LONGLONG sentFrames = g_overlayFrames;
    RECT rc;
    GetClientRect(g_window, &rc);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < 40; i++) PaintWindow(g_window, dc);     // let the switches settle
    QueryPerformanceCounter(&t1);
    SelectObject(dc, old);
    {
        gp::Bitmap image(bmp, nullptr);
        CLSID png = { 0x557CF406, 0x1A04, 0x11D3, { 0x9A, 0x73, 0x00, 0x00, 0xF8, 0x1E, 0xF3, 0x2E } };
        image.Save((g_selfTest + L"\\shot.png").c_str(), &png);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 40; i++) RenderOverlay();            // the stream overlay's frame too
        QueryPerformanceCounter(&t1);
        g_overlayMs = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 40;
        RenderOverlay()->Save((g_selfTest + L"\\overlay.png").c_str(), &png);
    }
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    double frame = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 40, model, glow;
    {
        gp::Bitmap scratch(rc.right, rc.bottom, PixelFormat32bppPARGB);
        gp::Graphics g(&scratch);
        g.SetSmoothingMode(gp::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(gp::PixelOffsetModeHalf);
        PadState s = g_pads.empty() ? PadState() : g_pads.back()->state;
        gp::RectF area(320, 124, 460, 420);
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 20; i++) DrawPad3D(g, area, s, kGreen, true, 1);
        QueryPerformanceCounter(&t1);
        model = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 20;
        QueryPerformanceCounter(&t0);
        for (int i = 0; i < 20; i++) FillGlow(g, 550, 330, 345, 340, WithAlpha(kGreen, 90));
        QueryPerformanceCounter(&t1);
        glow = (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / 20;
    }
    wchar_t timing[128];
    swprintf_s(timing, L"\nPaint: %.1f ms a frame (3D model %.1f ms, big glow %.1f ms)\n", frame, model, glow);
    std::wstring status = StatusText() + timing + g_taskbarNote;
    if (g_spout) {                          // what a receiver would see
        char name[256];
        unsigned int sw = 0, sh = 0;
        HANDLE share = nullptr;
        DWORD format = 0;
        bool found = g_spout->GetSenderInfo("DS4-Native", sw, sh, share, format);
        wchar_t line[300];
        QueryPerformanceCounter(&t1);
        double sentFor = (sentAt.QuadPart - g_overlayFirst) / (double)f.QuadPart;
        swprintf_s(line, L"Spout: %d sender(s), DS4-Native %s, %ux%u, format %lu, render %.1f ms, %lld frames in %.2f s = %.1f fps\n",
                   g_spout->GetSenderCount(),
                   found ? L"listed" : L"MISSING", sw, sh, format, g_overlayMs, sentFrames, sentFor, sentFrames > 1 ? (sentFrames - 1) / sentFor : 0.0);
        status += line;
        for (int i = 0; i < g_spout->GetSenderCount(); i++)
            if (g_spout->GetSender(i, name)) { status += L"  - "; status += std::wstring(name, name + strlen(name)); status += L"\n"; }
    }
    if (g_testMic) {
        AcquireSRWLockShared(&g_dumpLock);
        status += L"\r\n" + std::wstring(g_dump.begin(), g_dump.end());
        ReleaseSRWLockShared(&g_dumpLock);
    }
    HANDLE file = CreateFileW((g_selfTest + L"\\status.txt").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(file, "\xFF\xFE", 2, &written, nullptr);
        WriteFile(file, status.c_str(), (DWORD)(status.size() * sizeof(wchar_t)), &written, nullptr);
        CloseHandle(file);
    }
    DestroyWindow(g_tray);
}

static HANDLE g_single;     // single-instance mutex

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    if (!wcsncmp(cmdLine, L"/chroma ", 8)) return ChromaHelperMain(cmdLine + 8);
    if (!wcsncmp(cmdLine, L"/sbctest ", 9)) {    // testing: encode a tone, decode it, compare
        const int kFrames = 64, n = kFrames * 128;
        std::vector<short> in(n * 2);
        for (int i = 0; i < n; i++) in[i * 2] = in[i * 2 + 1] = (short)(12000 * sin(2 * 3.14159265 * 1000 * i / 32000.0));
        audio::Sbc enc;
        SbcDecoder dec;
        std::vector<float> out(n);
        int bad = 0;
        for (int f = 0; f < kFrames; f++) {
            BYTE frame[audio::Sbc::kFrameBytes];
            enc.Encode((const short (*)[2])&in[f * 128 * 2], frame);
            if (dec.Decode(frame, audio::Sbc::kFrameBytes, &out[f * 128]) != 128) bad++;
        }
        double best = 0; int bestLag = 0; double gain = 0;
        for (int lag = 0; lag < 200; lag++) {
            double xy = 0, xx = 0, yy = 0;
            for (int i = 1024; i < n - 256; i++) { double x = in[i * 2] / 32768.0, y = out[i + lag]; xy += x * y; xx += x * x; yy += y * y; }
            double c = xy / sqrt(xx * yy + 1e-12);
            if (fabs(c) > fabs(best)) { best = c; bestLag = lag; gain = sqrt(yy / xx); }
        }
        char text[160];
        sprintf_s(text, "bad frames %d, correlation %.4f at lag %d, gain %.3f\n", bad, best, bestLag, gain);
        HANDLE file = CreateFileW(cmdLine + 9, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        DWORD written;
        if (file != INVALID_HANDLE_VALUE) { WriteFile(file, text, (DWORD)strlen(text), &written, nullptr); CloseHandle(file); }
        return 0;
    }
    if (!wcscmp(cmdLine, L"/exit")) {       // (the installer) close a running copy the normal way, so it
        HWND tray = FindWindowW(L"DS4NativeTray", nullptr);    // shows the controllers it hid again
        if (!tray) return 0;
        DWORD pid = 0;
        GetWindowThreadProcessId(tray, &pid);
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(tray, WM_COMMAND, ID_MENU_EXIT, 0);
        if (process) {
            WaitForSingleObject(process, 15000);
            CloseHandle(process);
        }
        return 0;
    }

    wchar_t env[MAX_PATH];
    if (GetEnvironmentVariableW(L"DS4NATIVE_SELFTEST", env, MAX_PATH)) g_selfTest = env;
    bool selfTest = !g_selfTest.empty();
    bool demo = GetEnvironmentVariableW(L"DS4NATIVE_DEMO", env, MAX_PATH) != 0;
    g_testMic = micout::g_dry = selfTest && GetEnvironmentVariableW(L"DS4NATIVE_MICDRY", env, MAX_PATH) != 0;
    g_noInput = selfTest;
    if (selfTest && GetEnvironmentVariableW(L"DS4NATIVE_SPOUT", env, MAX_PATH)) g_testSpout = true;
    if (selfTest) SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);

    g_single = CreateMutexW(nullptr, TRUE, selfTest ? L"Local\\DS4Native.SelfTest" : L"Local\\DS4Native.Tray");
    if (GetLastError() == ERROR_ALREADY_EXISTS || GetLastError() == ERROR_ACCESS_DENIED) {
        HWND tray = FindWindowW(L"DS4NativeTray", nullptr);
        if (tray && !selfTest) PostMessageW(tray, WM_COMMAND, ID_MENU_OPEN, 0);
        return 0;
    }

    SetProcessDPIAware();
    HDC screen = GetDC(nullptr);
    g_dpi = GetDeviceCaps(screen, LOGPIXELSY) / 96.0;
    ReleaseDC(nullptr, screen);
    gp::GdiplusStartupInput gdiplusInput;
    ULONG_PTR gdiplusToken;
    gp::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr);
    CreateFonts();
    LoadAccentColour();

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);     // listing sound devices
    if (!selfTest) MigrateOldSettings();
    LoadGlobals();
    if (g_testSpout) g_spoutOn = true;          // (not saved)
    LoadShortcuts();
    if (!demo) audio::LoadSource();
    if (!demo) micout::LoadDest();
    hidhide::LoadOurs();
    if (!demo) hidhide::WhitelistSelf();    // before opening any controller: it may be hidden already
    else hidhide::g_ready = hidhide::Installed();   // the demo touches nothing, but shows the switch as it would be
    g_vigem = vigem_alloc();
    if (g_vigem && !VIGEM_SUCCESS(vigem_connect(g_vigem))) {
        vigem_free(g_vigem);
        g_vigem = nullptr;
    }
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_noTray = selfTest;

    WNDCLASSW wc{};
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = TrayProc;
    wc.lpszClassName = selfTest ? L"DS4NativeTrayTest" : L"DS4NativeTray";
    RegisterClassW(&wc);
    const wchar_t* trayClass = wc.lpszClassName;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"DS4NativeWindow";
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.style = CS_DBLCLKS;
    RegisterClassW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT size = { 0, 0, (LONG)S(kClientW), (LONG)S(kClientH) };
    AdjustWindowRect(&size, style, FALSE);
    g_window = CreateWindowW(L"DS4NativeWindow", kAppName, style, CW_USEDEFAULT, CW_USEDEFAULT,
                             size.right - size.left, size.bottom - size.top, nullptr, nullptr, inst, nullptr);
    ApplyTheme();
    ApplyOnTop();

    g_tray = CreateWindowW(trayClass, kAppName, 0, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
    UpdateTray(g_tray, NIM_ADD);            // also sets the window's icon

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.dbcc_classguid);
    HDEVNOTIFY notify = RegisterDeviceNotificationW(g_tray, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);

    if (demo) AddDemoPad();
    else ScanPads();
    if (selfTest) {
        if (GetEnvironmentVariableW(L"DS4NATIVE_YAW", env, MAX_PATH)) g_viewYaw = (float)_wtof(env) * kPi / 180;
        if (GetEnvironmentVariableW(L"DS4NATIVE_PANEL", env, MAX_PATH)) { g_panelOpen = true; g_actionMenuRow = _wtoi(env) - 1; }
        if (demo && GetEnvironmentVariableW(L"DS4NATIVE_LIGHT", env, MAX_PATH)) g_pads.back()->set[S_LIGHTMODE] = _wtoi(env);
        if (GetEnvironmentVariableW(L"DS4NATIVE_TAB", env, MAX_PATH)) g_leftTab = _wtoi(env);
        if (demo && g_leftTab == 1) g_pads.back()->set[S_TPMOUSE] = 1;
        if (GetEnvironmentVariableW(L"DS4NATIVE_TASKBAR", env, MAX_PATH))   // a taskbar button, minimised, focus left alone
            ShowWindow(g_window, SW_SHOWMINNOACTIVE);
        if (demo && g_leftTab == 2) {      // the Sound tab as it looks in use
            Pad* d = g_pads.back();
            d->set[S_AUDIO] = 1; d->set[S_TPMOUSE] = 0; d->set[S_TPVOLUME] = 1; d->set[S_MIC] = 1;
            d->state.mic = d->state.phones = true;
        }
    }
    Refresh();
    if (!demo) SetTimer(g_tray, 2, 3000, nullptr);     // rescan: retries and anything the notifications missed
    ApplySpout();                                       // the stream overlay, if it was left on
    if (selfTest) {
        UINT ms = 3000;
        if (GetEnvironmentVariableW(L"DS4NATIVE_SECONDS", env, MAX_PATH)) ms = (UINT)(_wtof(env) * 1000);
        SetTimer(g_tray, 4, ms, nullptr);
    }
    else if (!wcsstr(cmdLine, L"/tray")) ShowWindowFront();     // "Start with Windows" passes /tray

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Stop the controllers' threads, then show the real controllers to games again.
    g_exiting = true;
    if (notify) UnregisterDeviceNotification(notify);
    for (Pad* p : g_pads) p->stop = true;
    while (!g_pads.empty()) {
        DestroyPad(g_pads.back());
        g_pads.pop_back();
    }
    SyncHiding();
    StopChroma();
    g_spoutOn = false;
    ApplySpout();
    audio::Want(false);
    micout::Want(false);
    if (g_vigem) {
        vigem_disconnect(g_vigem);
        vigem_free(g_vigem);
    }
    if (g_single) CloseHandle(g_single);
    gp::GdiplusShutdown(gdiplusToken);
    return 0;
}
