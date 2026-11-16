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
#include <atomic>
#include <string>
#include <vector>
#include <set>
#include <algorithm>
#include <cmath>
#include <ViGEm/Client.h>

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
static const wchar_t kVersion[] = L"1.0.0";
static const wchar_t kRegKey[]  = L"Software\\DS4Native";
static const wchar_t kRunKey[]  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const UINT WM_TRAY     = WM_APP + 1;
static const UINT WM_PADS     = WM_APP + 2;     // a controller came, went or changed (from its thread)
static const UINT WM_SHORTCUT = WM_APP + 3;     // a combo fired: wParam action, lParam buttons
static const UINT WM_CHROMA   = WM_APP + 4;     // from the Chroma helper: wParam what, lParam value
enum TrayMenuId { ID_MENU_OPEN = 400, ID_MENU_ENABLED, ID_MENU_EXIT };
static const float kPi = 3.14159265f;

// ---------------------------------------------------------------------------------------
// Settings: one int per setting, per controller

enum Setting {
    S_EMULATE, S_HIDE, S_RUMBLE, S_TOUCHBACK, S_PSGUIDE, S_IDLE, S_MOTION,    // switches, in card order
    S_LIGHTMODE, S_HUE, S_BRIGHT, S_DEADL, S_DEADR, S_DEADT, S_RUMBLEGAIN, S_RAINBOW,
    S_TPMOUSE, S_TPSPLIT, S_TPTWO, S_TPSCROLL, S_TPSPEED,       // the touchpad as a mouse
    S_AUDIO, S_AUDIOOUT, S_AUDIOVOL,                            // sound to the controller (Bluetooth)
    S_TPTAP,
    S_COUNT
};
enum LightMode { LIGHT_BATTERY, LIGHT_COLOUR, LIGHT_RAINBOW, LIGHT_CHROMA, LIGHT_OFF, LIGHT_COUNT };
enum TwoFinger { TWO_RIGHT, TWO_MIDDLE, TWO_COUNT };
enum AudioOut { AUDIO_JACK, AUDIO_SPEAKER };
static const wchar_t* const kSettingNames[S_COUNT] = {
    L"Emulate", L"Hide", L"Rumble", L"TouchpadBack", L"PsGuide", L"IdleSleep", L"Motion",
    L"Light", L"Hue", L"Brightness", L"DeadLeft", L"DeadRight", L"DeadTrigger", L"RumbleGain", L"RainbowSpeed",
    L"TouchMouse", L"TouchSplit", L"TouchTwoFinger", L"TouchScroll", L"TouchSpeed",
    L"Audio", L"AudioOut", L"AudioVolume", L"TouchTap",
};
static const int kSettingDefaults[S_COUNT] = { 1, 1, 1, 1, 1, 0, 1, LIGHT_BATTERY, 210, 40, 8, 8, 0, 100, 25,
                                               0, 1, TWO_RIGHT, 1, 50, 0, AUDIO_JACK, 70, 0 };
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
};
static const int kEmulationToggles[] = { S_EMULATE, S_HIDE, S_RUMBLE, S_TOUCHBACK, S_PSGUIDE, S_IDLE, S_MOTION };
static const int kEmulationCount = (int)ARRAYSIZE(kEmulationToggles);
static const ULONGLONG kIdleMs = 10 * 60 * 1000;

static std::atomic<bool> g_enabled{true};   // the master switch
enum ThemeMode { THEME_AUTO, THEME_LIGHT, THEME_DARK };
static int  g_themeMode = THEME_AUTO;
static bool g_onTop;

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
    g_enabled = RegGet(nullptr, L"Enabled", 1) != 0;
    g_themeMode = (int)min(RegGet(nullptr, L"Theme", THEME_AUTO), (DWORD)THEME_DARK);
    g_onTop = RegGet(nullptr, L"OnTop", 0) != 0;
}

static void SaveGlobals() {
    RegPut(nullptr, L"Enabled", g_enabled);
    RegPut(nullptr, L"Theme", g_themeMode);
    RegPut(nullptr, L"OnTop", g_onTop);
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
static const GUID kChromaSampleAppId = { 0x8fb61199, 0x44f5, 0x4c53, { 0xa7, 0x06, 0x74, 0x73, 0x73, 0xa2, 0x17, 0x96 } };

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
    GUID id = kChromaSampleAppId;
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
    bool  imu = false;                  // the motion sensors are in the reports
    short gyro[3] = {}, accel[3] = {};  // raw: ~16.4 per deg/s, ~8192 per g
    float up[3] = { 0, 1, 0 };          // gravity in the pad's frame, smoothed (x right, y up, z towards you)
    float yaw = 0;                      // turn from the gyro, drifting back to 0
    BYTE  tid[2] = {};                  // touch ids: a new id is a new finger
    BYTE  counter = 0;                  // the report's own count (6 bits): gaps are lost reports
};

struct Pad {
    std::wstring path, instanceId, serial, name;
    USHORT pid = 0;
    bool   bluetooth = false, dongle = false, fake = false;
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
    std::atomic<int> link{-1};          // % of the controller's reports that arrived (last ~2 s), -1 unknown
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
    if (b[0] == 0x11 && n >= 46) { d = b + 2; full = true; }               // Bluetooth, extended
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
static void UpdateMotion(PadState& s, float dt) {
    if (!s.imu || dt <= 0) return;
    float a[3] = { (float)s.accel[0], (float)s.accel[1], (float)s.accel[2] };
    float len = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (len > 2000) {
        float k = 1 - expf(-dt / 0.06f), n = 0;
        for (int i = 0; i < 3; i++) { s.up[i] += (a[i] / len - s.up[i]) * k; n += s.up[i] * s.up[i]; }
        n = sqrtf(max(n, 1e-6f));
        for (int i = 0; i < 3; i++) s.up[i] /= n;
    }
    s.yaw += s.gyro[1] / 16.4f * (kPi / 180) * dt;
    s.yaw *= expf(-dt / 1.5f);
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
    if ((buttons & B_PS) && p->set[S_PSGUIDE]) r.wButtons |= XUSB_GAMEPAD_GUIDE;
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
struct PadOutput { BYTE weak, strong, r, g, b, audio, volume; };

static bool WriteOutput(Pad* p, const PadOutput& o) {
    BYTE buf[128] = {};
    DWORD len;
    if (p->bluetooth) {
        len = 78;
        buf[0] = 0x11; buf[1] = (BYTE)(0xC0 | p->poll); buf[3] = 0x07;
        buf[6] = o.weak; buf[7] = o.strong; buf[8] = o.r; buf[9] = o.g; buf[10] = o.b;
        if (o.audio == 4) {
            buf[2] = 0xA0;
        } else if (o.audio) {               // volumes valid; sound on (0xA0, 0x85) or off
            buf[3] |= 0xF0;
            bool on = o.audio != 3;
            buf[2] = on ? 0xA0 : 0x00;
            buf[21] = buf[22] = o.audio == 1 ? o.volume : 0;
            buf[24] = o.audio == 2 ? o.volume : 0;
            buf[25] = on ? 0x85 : 0x00;
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
    INPUT in[8] = {};
    int n = 0;
    for (WORD k : keys) { in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = k; n++; }
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
        in[n].type = INPUT_KEYBOARD; in[n].ki.wVk = *it; in[n].ki.dwFlags = KEYEVENTF_KEYUP; n++;
    }
    SendInput(n, in, sizeof(INPUT));
}

static void RunAction(Pad* p, int action, DWORD buttons) {
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
    DWORD up = 0;                       // the MOUSEEVENTF_*UP for a button a pad click is holding
    // A press of the pad is only a click if the finger stays put: it's pending until it's let go
    // quickly or held still (then it clicks), or the finger slides (then it was navigating).
    bool  pending = false, cancelled = false;
    DWORD pendDown = 0, pendUp = 0;
    ULONGLONG pressAt = 0;
    float pressMoved = 0;
    // Taps (with "Tap to click"): one or two fingers down briefly with little movement.
    int   tapFingers = 0;
    bool  tapPressed = false;
    ULONGLONG tapAt = 0;
    float tapMoved = 0;
};
static const ULONGLONG kPressHoldMs = 250, kTapMs = 200;
static const float kPressSlide = 40, kTapSlide = 40;     // touch units

static void MouseInput(DWORD flags, LONG dx = 0, LONG dy = 0, DWORD data = 0) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dx = dx;
    in.mi.dy = dy;
    in.mi.dwFlags = flags;
    in.mi.mouseData = data;
    SendInput(1, &in, sizeof(in));
}

static void TwoFingerButtons(Pad* p, DWORD* down, DWORD* up) {
    bool middle = p->set[S_TPTWO] == TWO_MIDDLE;
    *down = middle ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_RIGHTDOWN;
    *up = middle ? MOUSEEVENTF_MIDDLEUP : MOUSEEVENTF_RIGHTUP;
}

// One finger moves the cursor, two scroll. Pressing the pad and keeping the finger still
// clicks (left, or right on the right half with split clicks; with two fingers, right or
// middle); a press that slides is navigation and doesn't click. With "Tap to click", a quick
// tap clicks too (two fingers: right or middle).
static void UpdateTouchMouse(Pad* p, TouchMouse& m, const PadState& s, DWORD before, ULONGLONG now, float dt) {
    if (!p->set[S_TPMOUSE]) {
        if (m.up) { MouseInput(m.up); m.up = 0; }
        m.had[0] = m.had[1] = m.pending = false;
        m.tapFingers = 0;
        return;
    }
    int fingers = s.touch[0] + s.touch[1];
    bool down = (s.buttons & B_TOUCH) != 0, wasDown = (before & B_TOUCH) != 0;

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

    // The press: which button it would be, and whether it becomes a click.
    if (down && !wasDown && !m.up) {
        m.pendDown = MOUSEEVENTF_LEFTDOWN; m.pendUp = MOUSEEVENTF_LEFTUP;
        int finger = s.touch[0] ? 0 : 1;
        if (fingers >= 2) {
            TwoFingerButtons(p, &m.pendDown, &m.pendUp);
        } else if (p->set[S_TPSPLIT] && s.touch[finger] && s.tx[finger] >= 960) {
            m.pendDown = MOUSEEVENTF_RIGHTDOWN; m.pendUp = MOUSEEVENTF_RIGHTUP;
        }
        m.pending = true;
        m.cancelled = false;
        m.pressAt = now;
        m.pressMoved = 0;
    }
    if (m.pending) {
        m.pressMoved += moved;
        if (m.pressMoved >= kPressSlide) {              // sliding: navigating, not clicking
            m.pending = false;
            m.cancelled = true;
        } else if (!down) {                             // let go quickly without sliding: a click
            MouseInput(m.pendDown);
            MouseInput(m.pendUp);
            m.pending = false;
        } else if (now - m.pressAt >= kPressHoldMs) {   // held still: the button stays down (drag)
            MouseInput(m.pendDown);
            m.up = m.pendUp;
            m.pending = false;
        }
    }
    if (!down && m.up) {
        MouseInput(m.up);
        m.up = 0;
    }

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
            MouseInput(dn);
            MouseInput(up);
        }
        m.tapFingers = 0;
    }

    // Moving and scrolling (not while a press is still deciding).
    float speed = p->set[S_TPSPEED] / 50.0f;
    if (same && !m.pending) {
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

    static const double* Matrix() {
        static double m[kBands][kTaps];
        static bool built;
        if (built) return &m[0][0];
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
    BYTE report[462] = {};
    report[0] = 0x17;
    report[1] = (BYTE)(0x40 | p->poll);
    report[2] = 0xA0;                   // keeps the controller sending input while it plays
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

static bool OpenPad(Pad* p) {
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
        bool wantAudio = p->bluetooth && p->ready && p->set[S_AUDIO] && audio::g_running;
        if (wantAudio != p->audioOn) {
            p->audioOn = wantAudio;
            if (!p->pollFixed) p->poll = wantAudio ? 0 : 4;    // sent with the report below
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
        int outTo = p->set[S_AUDIOOUT] == AUDIO_SPEAKER ? 2 : 1, volume = p->set[S_AUDIOVOL] * 0x4F / 100;
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
        if (p->ready && (!everSent || arming || memcmp(&out, &sent, sizeof(out))) && now - lastWrite >= gap) {
            bool ok = WriteOutput(p, out);
            if (ok && out.audio == 3) disarmAudio = false;
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
    if (everSent && (sent.strong || sent.weak || p->audioOn)) {     // leave it quiet
        sent.strong = sent.weak = 0;
        if (p->audioOn) sent.audio = 3;
        WriteOutput(p, sent);
    }
    p->audioOn = false;
    if (timer) CloseHandle(timer);
    return 0;
}

static DWORD WINAPI PadThread(LPVOID param) {
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
        // Link quality: the reports' own counter says how many were sent; the share that
        // arrived, over ~2 s windows.
        ULONGLONG linkStart = GetTickCount64();
        int linkGot = 0, linkSent = 0;
        bool haveCounter = false;
        BYTE lastCounter = 0;
        if (!p->dongle) SetReady(p, true);
        p->writerStop = false;
        HANDLE writer = CreateThread(nullptr, 0, OutputThread, p, 0, nullptr);
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
            if (got && n > 0 && ParseReport(p, buf.data(), n, next, &synced)) {
                if (p->dongle) SetReady(p, synced);
                if (synced) {
                    LARGE_INTEGER count;
                    QueryPerformanceCounter(&count);
                    float dt = lastCount.QuadPart ? min(0.1f, (float)(count.QuadPart - lastCount.QuadPart) / freq.QuadPart) : 0;
                    lastCount = count;
                    UpdateMotion(next, dt);
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
                    DWORD touchHold = p->set[S_TPMOUSE] ? B_TOUCH : 0;     // the touchpad click is the mouse's

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
        WaitForSingleObject(writer, 3000);
        CloseHandle(writer);
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

struct FoundPad { std::wstring path, instanceId; USHORT pid; bool bluetooth; };

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
        if (!usb && !bt) continue;
        static const USHORT kPids[] = { 0x05C4, 0x09CC, 0x0BA0 };
        USHORT pid = 0;
        for (USHORT candidate : kPids) {
            wchar_t tag[16];
            swprintf_s(tag, usb ? L"pid_%04x" : L"pid&%04x", candidate);
            if (path.find(tag) != std::wstring::npos) pid = candidate;
        }
        if (!pid) continue;
        wchar_t id[512] = L"";
        SetupDiGetDeviceInstanceIdW(info, &dev, id, 512, nullptr);
        found.push_back({ path, id, pid, bt });
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

static void SyncHiding() {
    if (!hidhide::g_ready) return;
    std::set<std::wstring> want;
    if (!g_exiting && g_enabled && g_vigem) {
        for (Pad* p : g_pads) {
            if (p->ready && !p->fake && p->set[S_EMULATE] && p->set[S_HIDE] && !p->instanceId.empty()) want.insert(p->instanceId);
        }
    }
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
        p->dongle = f.pid == 0x0BA0;
        p->name = f.pid == 0x09CC ? L"DualShock 4 v2" : L"DualShock 4";
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
static const gp::Color kAccent(255, 10, 132, 255), kGreen(255, 52, 199, 89), kWhite(255, 255, 255, 255);
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
    E_SC_ROW, E_SC_ACTION, E_SC_ACTIONITEM, E_SC_DELETE, E_TP_TWO, E_TP_TWOITEM, E_TPBUTTON, E_SRC, E_SRCITEM,
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

static std::wstring SourceLabel() {
    std::wstring source = audio::Source();
    if (source.empty()) return L"Automatic";
    if (source == audio::kMirror) return L"Mirror the PC";
    for (const auto& d : g_srcDevices) if (d.id == source) return d.name;
    std::wstring playing = audio::PlayingFrom();
    return playing.empty() ? L"Not connected" : playing;
}

// Switch slots: a setting's own index for per-controller switches, then the two app switches.
static const int kSlotStartup = S_COUNT, kSlotEnabled = S_COUNT + 1;

static std::vector<Elem> g_elems;
static int   g_hover = -1, g_pressed = -1;
static bool  g_themeOpen;
static bool  g_startup;
static float g_knob[kSlotEnabled + 1];

static int SettingValue(int setting) {
    Pad* p = SelectedPad();
    return p ? p->set[setting].load() : kSettingDefaults[setting];
}

static bool ToggleValue(int slot) {
    if (slot < S_COUNT) return SettingValue(slot) != 0;
    return slot == kSlotEnabled ? g_enabled.load() : g_startup;
}

static int AddElem(ElemKind kind, gp::RectF rc, int id, int sub = 0) {
    g_elems.push_back({ kind, rc, id, sub });
    return (int)g_elems.size() - 1;
}

static bool Hovered(int elem) { return elem >= 0 && elem == g_hover; }

static void DrawSwitch(gp::Graphics& g, gp::RectF r, float t, bool dim = false) {
    const Palette& p = Pal();
    FillRound(g, r, r.Height / 2, WithAlpha(Mix(p.switchOff, kGreen, t), dim ? 110 : 255));
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
static const int kSliderCount = (int)ARRAYSIZE(kSliders), kTuningSliders = 4;
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
    if (d.setting == S_RAINBOW) swprintf_s(value, L"%.1f s a turn", 100.0 / v);
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
    gp::Color text_ = primary ? kWhite : p.text;
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

static void DrawTrigger(gp::Graphics& g, const PadLayout& L, float x, BYTE analog, bool l1, const wchar_t* l1Name, const wchar_t* l2Name) {
    const Palette& p = Pal();
    gp::RectF t2 = L.R(x, 34, 150, 62), t1 = L.R(x - 12, 100, 174, 44);
    FillRound(g, t2, L.U(16), p.body);
    if (analog) {
        gp::GraphicsPath clip;
        RoundPath(clip, t2, L.U(16));
        gp::GraphicsState saved = g.Save();
        g.SetClip(&clip, gp::CombineModeIntersect);
        float hgt = t2.Height * analog / 255.0f;
        gp::SolidBrush fill(kAccent);
        g.FillRectangle(&fill, t2.X, t2.Y + t2.Height - hgt, t2.Width, hgt);
        g.Restore(saved);
    }
    StrokeRound(g, t2, L.U(16), p.bodyEdge, S(1));
    Label(g, l2Name, g_fonts.tiny, analog > 128 ? kWhite : p.subtext, t2, gp::StringAlignmentCenter, gp::StringAlignmentCenter);
    FillRound(g, t1, L.U(14), l1 ? kAccent : p.body);
    StrokeRound(g, t1, L.U(14), p.bodyEdge, S(1));
    Label(g, l1Name, g_fonts.tiny, l1 ? kWhite : p.subtext, gp::RectF(t1.X, t1.Y, t1.Width, t1.Height * 0.62f),
          gp::StringAlignmentCenter, gp::StringAlignmentCenter);
}

static void DrawPad(gp::Graphics& g, const PadLayout& L, const PadState& s, gp::Color light, float opacity) {
    const Palette& p = Pal();
    gp::Color control = Mix(p.body, p.text, 0.10f);
    DWORD b = s.buttons;

    DrawTrigger(g, L, 160, s.l2, (b & B_L1) != 0, L"L1", L"L2");
    DrawTrigger(g, L, 690, s.r2, (b & B_R1) != 0, L"R1", L"R2");

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
        gp::Pen edge(p.bodyEdge, S(1.5));
        g.DrawPath(&edge, &body);
    }

    // Light bar, along the top of the touchpad.
    gp::RectF bar = L.R(400, 122, 200, 10);
    FillGlow(g, bar.X + bar.Width / 2, bar.Y + bar.Height / 2, bar.Width * 0.75f, L.U(46), WithAlpha(light, 120));
    FillRound(g, bar, L.U(5), light.GetA() ? light : control);

    // Touchpad with the fingers on it.
    gp::RectF pad = L.R(330, 142, 340, 176);
    FillRound(g, pad, L.U(22), (b & B_TOUCH) ? Mix(control, kAccent, 0.55f) : control);
    StrokeRound(g, pad, L.U(22), p.bodyEdge, S(1));
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
    { B_L1, 247, 122, GLOW_CONTRAST, 0 },       { B_R1, 777, 122, GLOW_CONTRAST, 0 },
    { 0, 235, 65, GLOW_ACCENT, 1 },             { 0, 765, 65, GLOW_ACCENT, 2 },          // L2, R2 by how far
    { B_SHARE, 299, 173, GLOW_CONTRAST, 0 },    { B_OPTIONS, 701, 173, GLOW_CONTRAST, 0 },
    { B_PS, 500, 390, GLOW_ACCENT, 0 },         { B_TOUCH, 500, 230, GLOW_ACCENT, 0 },
    { B_L3, 352, 390, GLOW_CONTRAST, 0 },       { B_R3, 648, 390, GLOW_CONTRAST, 0 },
};
static float g_spotLevel[ARRAYSIZE(kGlowSpots)];
static float g_fingerLevel[2], g_fingerU[2], g_fingerV[2];

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
static void UpdateAmbient(const PadState& s, bool live) {
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
    return g_fingerLevel[0] > 0 || g_fingerLevel[1] > 0;
}

struct AmbientField {
    float x0 = -1, y0, scale;           // the layout it was made for
    int ox, oy, cols, rows;             // the field's place in the window (px) and size (cells)
    std::vector<float> fade;            // exp(-distance outside the outline / reach)
};

static void DrawAmbient(gp::Graphics& g, const PadLayout& L, float w, float h) {
    if (!AmbientActive() || !g_canvas || &g != g_canvasGraphics) return;
    static AmbientField F;
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
        sources[count++] = { kGlowSpots[i].u, kGlowSpots[i].v, g_spotLevel[i], (float)c.GetR(), (float)c.GetG(), (float)c.GetB() };
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
static const float kViewTilt = 1.95f;       // ~112 degrees: seen from above the back, light bar showing
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
    if (motion) {
        V3 up = Normal(V3{ s.up[0], s.up[1], s.up[2] });
        device = RotY(s.yaw) * Align(up, V3{ 0, 1, 0 });
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

// The small 3D preview in the corner: tilt and turn from the motion sensors, and the force
// (in g) the accelerometer feels. Drag it to spin; double-click to straighten it.
static void DrawMotionCard(gp::Graphics& g, gp::RectF card, const PadState& s, gp::Color light, Pad* pad, bool hover) {
    const Palette& p = Pal();
    FillRound(g, card, S(12), hover ? Mix(WithAlpha(p.card, 255), p.hover, 0.5f) : p.card);
    Label(g, L"MOTION", g_fonts.tiny, p.subtext, gp::RectF(card.X + S(10), card.Y + S(5), S(80), S(14)));
    bool motion = pad && pad->set[S_MOTION] && s.imu;
    gp::Graphics* canvas = g_canvasGraphics;   // keep the glows inside the card (the fast path ignores clipping)
    g_canvasGraphics = nullptr;
    gp::GraphicsState saved = g.Save();
    gp::GraphicsPath clip;
    RoundPath(clip, card, S(12));
    g.SetClip(&clip, gp::CombineModeIntersect);
    DrawPad3D(g, gp::RectF(card.X + S(6), card.Y + S(14), card.Width - S(12), card.Height - S(36)), s, light, motion, 1);
    g.Restore(saved);
    g_canvasGraphics = canvas;
    wchar_t text[96];
    if (motion) {
        float force = sqrtf((float)s.accel[0] * s.accel[0] + (float)s.accel[1] * s.accel[1] + (float)s.accel[2] * s.accel[2]) / 8192;
        float tilt = acosf(max(-1.0f, min(1.0f, s.up[1]))) * 180 / kPi;
        swprintf_s(text, L"%.2f g   \x00B7   tilt %.0f\x00B0   \x00B7   turn %+.0f\x00B0", force, tilt, s.yaw * 180 / kPi);
    } else {
        wcscpy_s(text, !pad ? L"No controller" : !s.imu ? L"No motion data yet" : L"3D motion is off");
    }
    Label(g, text, g_fonts.caption, p.subtext, gp::RectF(card.X + S(6), card.Y + card.Height - S(22), card.Width - S(12), S(18)),
          gp::StringAlignmentCenter);
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

static void PaintWindow(HWND hwnd, HDC target) {
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
        for (int slot = 0; slot <= kSlotEnabled; slot++) {
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
            bool unmapped = (m.pad == B_PS && !pad->set[S_PSGUIDE]) || (m.pad == B_TOUCH && !pad->set[S_TOUCHBACK]);
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
        float bandH = S(128);               // under the controller: values, and the motion card in the corner
        gp::RectF centre(cxl, top, cxr - cxl, bottom - top - bandH);
        PadLayout layout = LayoutPad(centre);
        gp::RectF preview(cxr - S(184), centre.Y + centre.Height + S(4), S(184), bandH - S(8));
        int previewElem = AddElem(E_PAD3D, preview, 0);

        // The light bar's colour glows behind the controller; rumble makes it swell.
        g.Clear(p.bg);
        gp::Color light = pad ? PadLight(pad, s.battery) : gp::Color(0, 0, 0, 0);
        bool emulating = pad && (pad->emulating || pad->fake) && g_enabled;
        if (pad && light.GetA()) {
            float rumble = max(pad->rumbleStrong.load(), pad->rumbleWeak.load()) / 255.0f;
            if (now < pad->testUntil) rumble = 0.8f;
            float pulse = 1 + 0.10f * rumble * sinf(now / 28.0f);
            int alpha = (int)((g_dark ? 90 : 70) * (emulating ? 1.0f : 0.45f) + 70 * rumble);
            FillGlow(g, layout.X(500), layout.Y(330), layout.U(700) * pulse, layout.U(520) * pulse, WithAlpha(light, alpha));
        }
        UpdateAmbient(s, live);
        DrawAmbient(g, layout, w, h);

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
        float batteryX = pad && pad->bluetooth ? w / 2 - S(50) : w / 2;     // the signal goes beside it
        DrawBattery(g, batteryX, headerY + S(22), pad ? s.battery : -1, pad && s.charging ? L"Charging" : L"");
        DrawSignal(g, w / 2 + S(22), headerY + S(25), pad);
        gp::RectF themeBtn(w - m - S(124), headerY + S(20), S(124), S(36));
        gp::RectF pinBtn(themeBtn.X - S(10) - S(36), headerY + S(20), S(36), S(36));
        DrawIconButton(g, pinBtn, E_PIN, g_onTop ? kGlyphPinned : kGlyphPin, nullptr, g_onTop);
        gp::RectF mouseBtn(pinBtn.X - S(10) - S(36), headerY + S(20), S(36), S(36));     // touchpad mouse on / off
        DrawIconButton(g, mouseBtn, E_TPBUTTON, kGlyphMouse, nullptr, pad && pad->set[S_TPMOUSE]);
        gp::RectF master(mouseBtn.X - S(14) - S(140), headerY + S(20), S(140), S(36));
        if (Hovered(AddElem(E_TOGGLE, master, kSlotEnabled))) FillRound(g, master, S(10), p.hover);
        Label(g, L"Emulation", g_fonts.body, p.text, gp::RectF(master.X + S(12), master.Y, S(76), master.Height));
        DrawSwitch(g, gp::RectF(master.X + master.Width - S(10) - S(kSwitchW), master.Y + (master.Height - S(kSwitchH)) / 2,
                                S(kSwitchW), S(kSwitchH)), g_knob[kSlotEnabled]);
        const wchar_t* themeGlyph = g_themeMode == THEME_LIGHT ? kGlyphSun : g_themeMode == THEME_DARK ? kGlyphMoon
                                    : (g_dark ? kGlyphMoon : kGlyphSun);
        DrawIconButton(g, themeBtn, E_THEME, themeGlyph, g_themeMode == THEME_AUTO ? L"Auto" : kThemeNames[g_themeMode], g_themeOpen);

        // Columns, centred vertically on the taller (right) one.
        float rightH = S(24) + S(kSliderRowH) * kTuningSliders + S(20) + S(24) + S(34) + S(10) + S(kSliderRowH) * 3;
        float y0 = top + max(0.0f, (bottom - top - rightH) / 2);

        // Left column: Emulation, Touchpad and Sound, one at a time.
        static const wchar_t* const leftTabs[] = { L"Emulation", L"Touchpad", L"Sound" };
        DrawSegmented(g, gp::RectF(lx, y0, sideW, S(34)), SEG_LEFT_TAB, leftTabs, 3, g_leftTab, true);
        float y = y0 + S(34) + S(10);
        gp::Pen line(p.line, S(1));
        auto separator = [&](float at) { g.DrawLine(&line, lx + S(16), at, lx + sideW, at); };
        if (g_leftTab == 0) {
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * kEmulationCount), S(12), p.card);
            for (int i = 0; i < kEmulationCount; i++) {
                int setting = kEmulationToggles[i];
                float ry = y + S(kRowH) * i;
                if (i) separator(ry);
                bool usable = live && (setting != S_HIDE || hidhide::g_ready) && (setting != S_IDLE || pad->bluetooth)
                              && (setting != S_TOUCHBACK || !pad->set[S_TPMOUSE]);
                const wchar_t* detail = setting == S_HIDE && !hidhide::g_ready ? L"Needs the HidHide driver"
                                      : setting == S_MOTION && pad && !s.imu ? L"Waiting for motion data"
                                      : setting == S_TOUCHBACK && pad && pad->set[S_TPMOUSE] ? L"The touchpad is a mouse now"
                                      : ToggleTitle(setting, true);
                DrawToggleRow(g, gp::RectF(lx, ry, sideW, S(kRowH)), setting, ToggleTitle(setting, false), detail, usable);
            }
        } else if (g_leftTab == 1) {
            bool mouse = live && pad->set[S_TPMOUSE];
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * 5 + S(kSliderRowH)), S(12), p.card);
            DrawToggleRow(g, gp::RectF(lx, y, sideW, S(kRowH)), S_TPMOUSE, ToggleTitle(S_TPMOUSE, false), ToggleTitle(S_TPMOUSE, true), live);
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
            bool bt = live && pad->bluetooth, on = bt && pad->set[S_AUDIO];
            FillRound(g, gp::RectF(lx, y, sideW, S(kRowH) * 3 + S(kSliderRowH)), S(12), p.card);
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
            y += S(kSliderRowH) + S(8);
            wchar_t note[160];
            if (live && !pad->bluetooth) wcscpy_s(note, L"On USB the controller is its own sound\ndevice: pick it in Windows' sound settings.");
            else if (!on) wcscpy_s(note, L"Experimental. Apps that play to the chosen\ndevice play on the controller.");
            else if (audio::g_error == audio::kNoDevice)
                wcscpy_s(note, L"No sound device for it yet: install VB-Cable\nor pick one under \"Plays from\".");
            else if (audio::g_error) swprintf_s(note, L"Couldn't open the sound device (0x%08lX).", (unsigned long)audio::g_error.load());
            else if (!pad->audioOn) wcscpy_s(note, L"Starting\x2026");
            else swprintf_s(note, L"Playing \x00B7 %u reports sent%s", pad->audioSent.load(),
                            pad->audioFailed ? L", some failed" : L"");
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
        if (live && mode == LIGHT_CHROMA) {
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
        DrawSegmented(g, gp::RectF(rx, y, sideW, S(34)), SEG_LIGHT, lights, LIGHT_COUNT, mode, live);
        y += S(34) + S(10);
        FillRound(g, gp::RectF(rx, y, sideW, S(kSliderRowH) * 3), S(12), p.card);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 4, live && mode != LIGHT_OFF);
        y += S(kSliderRowH);
        g.DrawLine(&line, rx + S(16), y, rx + sideW, y);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 5, live && (mode == LIGHT_COLOUR || mode == LIGHT_CHROMA));
        y += S(kSliderRowH);
        g.DrawLine(&line, rx + S(16), y, rx + sideW, y);
        DrawSliderRow(g, gp::RectF(rx, y, sideW, S(kSliderRowH)), 6, live && mode == LIGHT_RAINBOW);

        // The controller; under it the raw values and the last mapping, and the motion card.
        DrawPad(g, layout, s, light, live ? (emulating ? 1.0f : 0.75f) : 0.4f);
        DrawMotionCard(g, preview, s, light, pad, Hovered(previewElem));
        float infoX = cxl, infoW = preview.X - S(12) - cxl, ty = preview.Y + S(12);
        if (pad) {
            wchar_t info[160];
            swprintf_s(info, L"L  %+.2f, %+.2f   \x00B7   R  %+.2f, %+.2f", (s.lx - 127.5) / 127.5, -(s.ly - 127.5) / 127.5,
                       (s.rx - 127.5) / 127.5, -(s.ry - 127.5) / 127.5);
            Label(g, info, g_fonts.caption, p.subtext, gp::RectF(infoX, ty, infoW, S(18)), gp::StringAlignmentCenter);
            swprintf_s(info, L"L2  %d   \x00B7   R2  %d", s.l2, s.r2);
            Label(g, info, g_fonts.caption, p.subtext, gp::RectF(infoX, ty + S(18), infoW, S(18)), gp::StringAlignmentCenter);
            if (!g_mapText.empty() && now - g_mapTick < kMapShowMs) {
                int alpha = (int)min(255.0, (kMapShowMs - (now - g_mapTick)) * 255.0 / 400);
                Label(g, g_mapText.c_str(), g_fonts.big, WithAlpha(p.text, alpha), gp::RectF(infoX, ty + S(46), infoW, S(26)),
                      gp::StringAlignmentCenter);
            }
        } else {
            Label(g, L"Hold PS + Share to pair over Bluetooth, or plug in a cable", g_fonts.caption, p.subtext,
                  gp::RectF(infoX, ty + S(18), infoW, S(18)), gp::StringAlignmentCenter);
        }

        // Footer card: start with Windows, then the app buttons on the right.
        gp::RectF footer(m, h - S(kFooterH), w - 2 * m, S(kFooterH) - S(16));
        FillRound(g, footer, S(12), p.card);
        DrawToggleRow(g, gp::RectF(footer.X, footer.Y, S(300), footer.Height), kSlotStartup, L"Start with Windows", L"Waits in the tray");
        float bx = footer.X + footer.Width - S(12), by = footer.Y + (footer.Height - S(32)) / 2;
        bx -= S(124);
        DrawButton(g, gp::RectF(bx, by, S(124), S(32)), BTN_RUMBLE, L"Test rumble", true, live);
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
        if (s.imu) {
            swprintf_s(line, L"     motion: accel %d %d %d, gyro %d %d %d, up %.2f %.2f %.2f, yaw %.2f\n", s.accel[0], s.accel[1],
                       s.accel[2], s.gyro[0], s.gyro[1], s.gyro[2], s.up[0], s.up[1], s.up[2], s.yaw);
            text += line;
        }
        swprintf_s(line, L"     link: %d%% of reports arrive, %d reports/s (interval setting %d)\n", p->link.load(),
                   p->reportRate.load(), p->poll.load());
        text += line;
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
        if (e.id == kSlotStartup) {
            SetStartsWithWindows(!g_startup);
            g_startup = StartsWithWindows();
            return;
        }
        if (e.id == kSlotEnabled) g_enabled = !g_enabled;
        else if (pad) pad->set[e.id] = !pad->set[e.id];
        break;
    case E_SEGMENT:
        if (e.id == SEG_LEFT_TAB) { g_leftTab = e.sub; g_twoMenuOpen = g_srcMenuOpen = false; return; }
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
    int mode = pad->set[S_LIGHTMODE];
    if (mode == LIGHT_RAINBOW || mode == LIGHT_CHROMA) return true;
    if (AmbientActive() || (g_leftTab == 2 && pad->audioOn)) return true;
    ULONGLONG now = GetTickCount64();
    if (now - g_mapTick < kMapShowMs + 100 || now < pad->testUntil + 100 || pad->rumbleStrong || pad->rumbleWeak) return true;
    for (int slot = 0; slot <= kSlotEnabled; slot++) {
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

static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
            else if (g_recording) StopRecording();
            else if (g_panelOpen) ClosePanel();
            else PostMessageW(hwnd, WM_CLOSE, 0, 0);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_SETTINGCHANGE:
        if (lp && !lstrcmpiW((LPCWSTR)lp, L"ImmersiveColorSet") && g_themeMode == THEME_AUTO) ApplyTheme();
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
static void Refresh() {
    SelectedPad();
    SyncHiding();
    for (Pad* p : g_pads) {
        if (p->ready && p->set[S_LIGHTMODE] == LIGHT_CHROMA) StartChroma(g_tray);
    }
    bool sound = false;                     // capture the PC's sound while a controller plays it
    for (Pad* p : g_pads) sound = sound || (p->ready && p->bluetooth && !p->fake && p->set[S_AUDIO]);
    audio::Want(sound && !g_exiting);
    UpdateTray(g_tray, NIM_MODIFY);
    InvalidateRect(g_window, nullptr, FALSE);
}

static void ShowTrayMenu(HWND hwnd) {
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, ID_MENU_OPEN, L"Open DS4-Native");
    AppendMenuW(m, MF_STRING | (g_enabled ? MF_CHECKED : 0), ID_MENU_ENABLED, L"Emulation");
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
    p->state.imu = true;
    V3 up = Normal(V3{ 0.22f, 1, -0.18f });
    p->state.up[0] = up.x; p->state.up[1] = up.y; p->state.up[2] = up.z;
    p->state.yaw = 0.35f;
    p->link = 96;
    p->player = 0;
    p->ready = true;
    g_pads.push_back(p);
}

static void SelfTestFinish() {
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
    std::wstring status = StatusText() + timing;
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

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);     // listing sound devices
    if (!selfTest) MigrateOldSettings();
    LoadGlobals();
    LoadShortcuts();
    audio::LoadSource();
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
        if (demo && g_leftTab) { g_pads.back()->set[S_TPMOUSE] = 1; g_pads.back()->set[S_AUDIO] = 1; }
    }
    Refresh();
    if (!demo) SetTimer(g_tray, 2, 3000, nullptr);     // rescan: retries and anything the notifications missed
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
    audio::Want(false);
    if (g_vigem) {
        vigem_disconnect(g_vigem);
        vigem_free(g_vigem);
    }
    if (g_single) CloseHandle(g_single);
    gp::GdiplusShutdown(gdiplusToken);
    return 0;
}
