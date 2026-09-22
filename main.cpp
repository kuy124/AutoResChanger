#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <set>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>
#include <algorithm>
#include <fstream>
#include <ctime>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "dwmapi.lib")

// ==========================================
// DATA STRUCTURES & PRESETS
// ==========================================
struct AppProfile {
    std::wstring exePath;
    std::wstring exeName;
    std::wstring displayDev; 
    int targetW;
    int targetH;
    int targetHz;
    int delaySec; 
    bool restore;
    bool enabled;
};

struct ResTemplate {
    std::wstring name;
    int w;
    int h;
};

struct ModeEntry {
    int w;
    int h;
    int hz;
};

const std::vector<ResTemplate> g_Presets = {
    { L"Custom (Manual Input)", 0, 0 },
    { L"3840 x 2160 (4K UHD 16:9)", 3840, 2160 },
    { L"2560 x 1440 (QHD 16:9)", 2560, 1440 },
    { L"1920 x 1080 (FHD 16:9)", 1920, 1080 },
    { L"1600 x 900 (HD+ 16:9)", 1600, 900 },
    { L"1440 x 900 (WXGA+ 16:10)", 1440, 900 },
    { L"1366 x 768 (HD 16:9)", 1366, 768 },
    { L"1280 x 1024 (SXGA 5:4)", 1280, 1024 },
    { L"1280 x 800 (WXGA 16:10)", 1280, 800 },
    { L"1280 x 720 (HD 16:9)", 1280, 720 },
    { L"1024 x 768 (XGA 4:3)", 1024, 768 },
    { L"800 x 600 (SVGA 4:3)", 800, 600 },
    { L"640 x 480 (VGA 4:3)", 640, 480 }
};

// The profile list is owned exclusively by the UI thread. The monitor thread
// never touches g_Profiles directly; instead the UI thread publishes an
// immutable snapshot that the worker reads. This eliminates the data race and
// the dangling-pointer bug that occurred when the vector reallocated.
std::vector<AppProfile> g_Profiles;
std::mutex g_SnapshotMutex;
std::shared_ptr<const std::vector<AppProfile>> g_ProfileSnapshot;

// NOTE: g_OriginalDevMode / g_ResChanged / g_ProcessFoundTime / g_ActiveIndex
// are touched only by the monitor thread, except for the explicit
// restore-on-exit path which is serialized via g_RestoreMutex.
DEVMODEW g_OriginalDevMode;
bool g_ResChanged = false;
std::atomic<bool> g_MonitorRunning(true);
std::wstring g_IniPath;
std::atomic<ULONGLONG> g_ProcessFoundTime(0);
std::atomic<int> g_ActiveIndex(-1); // index into the *current snapshot*, or -1
AppProfile g_ActiveProfileCopy;    // value copy of the active profile (never dangles)
std::mutex g_RestoreMutex;         // serializes restore between UI + monitor threads
bool g_IgnoreEditChange = false;
HINSTANCE g_hInst = NULL; // Stores instance handle for loading the baked-in icon
HANDLE g_hInstanceMutex = NULL; // Single-instance guard
HICON g_hAppIcon = NULL; // Owned icon handle, freed on exit
bool g_DarkMode = false;       // UI theme preference
bool g_LoggingEnabled = false; // Persisted logging preference

// UI Handles
HWND g_hMain, g_hList, g_hExe, g_hDisplayCombo, g_hW, g_hH, g_hHz, g_hDelay;
HWND g_hRestore, g_hEnable, g_hStartup, g_hPresetCombo, g_hSupportedCombo;
HWND g_hListLabel, g_hExeLabel, g_hBtnBrowse, g_hDisplayLabel, g_hPresetLabel;
HWND g_hWLabel, g_hHLabel, g_hHzLabel, g_hDelayLabel, g_hSaveBtn, g_hDelBtn, g_hLaunchBtn, g_hTestBtn;
HWND g_hSupportedLabel, g_hLogChk, g_hDarkChk;

#define WM_TRAYICON (WM_APP + 1)
NOTIFYICONDATAW g_Nid = {};
std::vector<std::wstring> g_MonitorDevices;
std::wstring g_LogPath;
std::vector<ModeEntry> g_SupportedModesCache;

// ==========================================
// LOGGING
// ==========================================
void LogMsg(const std::wstring& text) {
    if (!g_LoggingEnabled || g_LogPath.empty()) return;
    std::wofstream f(g_LogPath.c_str(), std::ios::app);
    if (!f.is_open()) return;
    time_t t = time(nullptr);
    tm tmv;
    localtime_s(&tmv, &t);
    WCHAR stamp[32];
    wcsftime(stamp, 32, L"%Y-%m-%d %H:%M:%S", &tmv);
    f << L"[" << stamp << L"] " << text << L"\n";
}

// ==========================================
// UTILITY & REGISTRY FUNCTIONS
// ==========================================
std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool IsProcessRunning(const std::wstring& processName) {
    if (processName.empty()) return false;
    bool exists = false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, processName.c_str()) == 0) {
                exists = true; break;
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return exists;
}

// Returns the set of running executable names (lower-cased) in a single pass.
// The monitor thread uses one snapshot per tick instead of one per profile,
// which keeps the detection cheap even with many profiles.
std::set<std::wstring> GetRunningProcessNames() {
    std::set<std::wstring> names;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return names;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            std::wstring n = entry.szExeFile;
            for (auto& c : n) c = (WCHAR)towlower(c);
            names.insert(n);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return names;
}

std::wstring ToLowerCopy(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = (WCHAR)towlower(c);
    return r;
}

// Republish an immutable snapshot of the profile list for the monitor thread.
// Must be called from the UI thread whenever g_Profiles changes.
void PublishSnapshot() {
    auto snap = std::make_shared<std::vector<AppProfile>>(g_Profiles);
    std::lock_guard<std::mutex> lock(g_SnapshotMutex);
    g_ProfileSnapshot = std::move(snap);
}

bool GetRunAtStartup() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        WCHAR path[MAX_PATH];
        DWORD size = sizeof(path);
        bool exists = (RegQueryValueExW(hKey, L"AutoResChanger", NULL, NULL, (LPBYTE)path, &size) == ERROR_SUCCESS);
        RegCloseKey(hKey);
        return exists;
    }
    return false;
}

void SetRunAtStartup(bool enable) {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_ALL_ACCESS, &hKey) == ERROR_SUCCESS) {
        if (enable) {
            WCHAR path[MAX_PATH];
            GetModuleFileNameW(NULL, path, MAX_PATH);
            // Properly quote path and add the silent flag so it starts minimized in the background
            std::wstring cmd = L"\"" + std::wstring(path) + L"\" -silent";
            RegSetValueExW(hKey, L"AutoResChanger", 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.length() + 1) * sizeof(WCHAR)));
        } else {
            RegDeleteValueW(hKey, L"AutoResChanger");
        }
        RegCloseKey(hKey);
    }
}

// ==========================================
// CONFIGURATION MANAGEMENT
// ==========================================
void InitConfigPath() {
    WCHAR path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, path))) {
        std::wstring dir = std::wstring(path) + L"\\AutoResChanger";
        CreateDirectoryW(dir.c_str(), NULL);
        g_IniPath = dir + L"\\config.ini";
        g_LogPath = dir + L"\\log.txt";
    }
}

void LoadSettings() {
    g_DarkMode = GetPrivateProfileIntW(L"Settings", L"DarkMode", 0, g_IniPath.c_str()) != 0;
    g_LoggingEnabled = GetPrivateProfileIntW(L"Settings", L"Logging", 0, g_IniPath.c_str()) != 0;
}

void SaveSettings() {
    WritePrivateProfileStringW(L"Settings", L"DarkMode", g_DarkMode ? L"1" : L"0", g_IniPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"Logging", g_LoggingEnabled ? L"1" : L"0", g_IniPath.c_str());
}

void LoadConfig() {
    g_Profiles.clear();
    int count = GetPrivateProfileIntW(L"Settings", L"Count", 0, g_IniPath.c_str());
    for (int i = 0; i < count; ++i) {
        std::wstring section = L"Profile_" + std::to_wstring(i);
        WCHAR exe[MAX_PATH], dev[64];
        GetPrivateProfileStringW(section.c_str(), L"Exe", L"", exe, MAX_PATH, g_IniPath.c_str());
        GetPrivateProfileStringW(section.c_str(), L"Device", L"", dev, 64, g_IniPath.c_str());
        
        if (wcslen(exe) > 0) {
            AppProfile p;
            p.exePath = exe;
            p.exeName = GetFileName(p.exePath);
            p.displayDev = dev;
            p.targetW = GetPrivateProfileIntW(section.c_str(), L"W", 1920, g_IniPath.c_str());
            p.targetH = GetPrivateProfileIntW(section.c_str(), L"H", 1080, g_IniPath.c_str());
            p.targetHz = GetPrivateProfileIntW(section.c_str(), L"Hz", 0, g_IniPath.c_str());
            p.delaySec = GetPrivateProfileIntW(section.c_str(), L"Delay", 0, g_IniPath.c_str());
            p.restore = GetPrivateProfileIntW(section.c_str(), L"Restore", 1, g_IniPath.c_str()) != 0;
            p.enabled = GetPrivateProfileIntW(section.c_str(), L"Enabled", 1, g_IniPath.c_str()) != 0;
            g_Profiles.push_back(p);
        }
    }
}

void SaveConfig() {
    if (g_IniPath.empty()) return;

    // Write to a temporary file first, then atomically replace the real one so
    // a crash or power loss mid-save cannot corrupt config.ini.
    std::wstring tmpPath = g_IniPath + L".tmp";
    DeleteFileW(tmpPath.c_str());

    WritePrivateProfileStringW(L"Settings", L"Count", std::to_wstring(g_Profiles.size()).c_str(), tmpPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"DarkMode", g_DarkMode ? L"1" : L"0", tmpPath.c_str());
    WritePrivateProfileStringW(L"Settings", L"Logging", g_LoggingEnabled ? L"1" : L"0", tmpPath.c_str());
    
    for (size_t i = 0; i < g_Profiles.size(); ++i) {
        std::wstring sec = L"Profile_" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), L"Exe", g_Profiles[i].exePath.c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Device", g_Profiles[i].displayDev.c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"W", std::to_wstring(g_Profiles[i].targetW).c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"H", std::to_wstring(g_Profiles[i].targetH).c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Hz", std::to_wstring(g_Profiles[i].targetHz).c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Delay", std::to_wstring(g_Profiles[i].delaySec).c_str(), tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Restore", g_Profiles[i].restore ? L"1" : L"0", tmpPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Enabled", g_Profiles[i].enabled ? L"1" : L"0", tmpPath.c_str());
    }
    // Flush the file to disk before swapping it in.
    WritePrivateProfileStringW(NULL, NULL, NULL, tmpPath.c_str());

    if (!MoveFileExW(tmpPath.c_str(), g_IniPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        // Fallback: if the atomic replace failed, try a plain replace.
        DeleteFileW(g_IniPath.c_str());
        MoveFileW(tmpPath.c_str(), g_IniPath.c_str());
    }
}

// ==========================================
// DISPLAY MANAGEMENT
// ==========================================
void PopulateMonitors() {
    g_MonitorDevices.clear();
    DISPLAY_DEVICEW dd = { sizeof(dd) };
    DWORD devNum = 0;
    SendMessage(g_hDisplayCombo, CB_RESETCONTENT, 0, 0);
    SendMessage(g_hDisplayCombo, CB_ADDSTRING, 0, (LPARAM)L"Primary Monitor (Default)");
    g_MonitorDevices.push_back(L"");
    
    while (EnumDisplayDevicesW(NULL, devNum, &dd, 0)) {
        if (dd.StateFlags & DISPLAY_DEVICE_ACTIVE) {
            std::wstring displayStr = std::wstring(dd.DeviceString) + L" (" + dd.DeviceName + L")";
            SendMessage(g_hDisplayCombo, CB_ADDSTRING, 0, (LPARAM)displayStr.c_str());
            g_MonitorDevices.push_back(dd.DeviceName);
        }
        devNum++;
    }
}

DEVMODEW GetCurrentRes(const std::wstring& devName) {
    DEVMODEW dm = { 0 }; dm.dmSize = sizeof(dm);
    EnumDisplaySettingsExW(devName.empty() ? NULL : devName.c_str(), ENUM_CURRENT_SETTINGS, &dm, 0);
    return dm;
}

// Enumerates every distinct (w,h,hz) mode the given display advertises. This
// powers the "Supported Modes" dropdown so the user never has to guess.
std::vector<ModeEntry> EnumModesForDevice(const std::wstring& devName) {
    std::vector<ModeEntry> modes;
    std::set<long long> seen; // key = w<<40 | h<<20 | hz-ish, packed loosely
    DEVMODEW dm = { 0 }; dm.dmSize = sizeof(dm);
    DWORD i = 0;
    while (EnumDisplaySettingsExW(devName.empty() ? NULL : devName.c_str(), i, &dm, 0)) {
        long long key = ((long long)dm.dmPelsWidth << 32) ^ ((long long)dm.dmPelsHeight << 16) ^ (long long)dm.dmDisplayFrequency;
        if (seen.insert(key).second) {
            modes.push_back({ (int)dm.dmPelsWidth, (int)dm.dmPelsHeight, (int)dm.dmDisplayFrequency });
        }
        i++;
    }
    // Sort by resolution desc, then refresh rate desc.
    std::sort(modes.begin(), modes.end(), [](const ModeEntry& a, const ModeEntry& b) {
        if (a.w != b.w) return a.w > b.w;
        if (a.h != b.h) return a.h > b.h;
        return a.hz > b.hz;
    });
    return modes;
}

// Rebuilds the Supported Modes dropdown for the currently selected display.
void RefreshSupportedModes() {
    if (!g_hSupportedCombo) return;
    SendMessage(g_hSupportedCombo, CB_RESETCONTENT, 0, 0);

    int idx = (int)SendMessage(g_hDisplayCombo, CB_GETCURSEL, 0, 0);
    std::wstring dev = (idx > 0 && idx < (int)g_MonitorDevices.size()) ? g_MonitorDevices[idx] : L"";

    std::vector<ModeEntry> modes = EnumModesForDevice(dev);
    SendMessage(g_hSupportedCombo, CB_ADDSTRING, 0, (LPARAM)L"Pick a listed mode...");
    for (const auto& m : modes) {
        std::wstring label = std::to_wstring(m.w) + L" x " + std::to_wstring(m.h);
        if (m.hz > 0) label += L"  @ " + std::to_wstring(m.hz) + L" Hz";
        SendMessage(g_hSupportedCombo, CB_ADDSTRING, 0, (LPARAM)label.c_str());
    }
    g_SupportedModesCache = modes;   // index 0 == placeholder, so cache index = combo index-1
    SendMessage(g_hSupportedCombo, CB_SETCURSEL, 0, 0);
}

// Extracts the icon for a given executable and returns a small HICON (caller
// owns it) or NULL on failure.
HICON ExtractExeIcon(const std::wstring& path, int cx, int cy) {
    if (path.empty()) return NULL;
    SHFILEINFOW sfi = { 0 };
    UINT flags = SHGFI_ICON | SHGFI_SMALLICON;
    DWORD_PTR r = SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), flags);
    if (!r || !sfi.hIcon) return NULL;
    // Resize to the requested dimensions for crisper list rendering.
    HICON resized = (HICON)CopyImage(sfi.hIcon, IMAGE_ICON, cx, cy, LR_COPYFROMRESOURCE);
    DestroyIcon(sfi.hIcon);
    return resized ? resized : NULL;
}

int ChangeRes(const std::wstring& devName, int w, int h, int hz) {
    DEVMODEW dm = GetCurrentRes(devName);
    dm.dmPelsWidth = w; dm.dmPelsHeight = h;
    dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
    if (hz > 0) { dm.dmDisplayFrequency = hz; dm.dmFields |= DM_DISPLAYFREQUENCY; }
    return ChangeDisplaySettingsExW(devName.empty() ? NULL : devName.c_str(), &dm, NULL, CDS_FULLSCREEN, NULL);
}

void RestoreRes(const std::wstring& devName, DEVMODEW& dm) {
    ChangeDisplaySettingsExW(devName.empty() ? NULL : devName.c_str(), &dm, NULL, CDS_FULLSCREEN, NULL);
}

// Restore the resolution if a profile change is currently active. Safe to call
// from either thread; the restore itself is serialized by g_RestoreMutex.
// Returns true if a restore was performed.
bool RestoreActiveIfChanged() {
    std::lock_guard<std::mutex> lock(g_RestoreMutex);
    if (g_ActiveIndex >= 0 && g_ResChanged) {
        RestoreRes(g_ActiveProfileCopy.displayDev, g_OriginalDevMode);
        g_ResChanged = false;
        g_ActiveIndex = -1;
        g_ProcessFoundTime = 0;
        return true;
    }
    return false;
}

// Clears the active-state bookkeeping without touching the display. Used when
// the profile was set to not restore, or the change never succeeded.
void ClearActiveState() {
    std::lock_guard<std::mutex> lock(g_RestoreMutex);
    g_ResChanged = false;
    g_ActiveIndex = -1;
    g_ProcessFoundTime = 0;
}

// ==========================================
// BACKGROUND THREAD
// ==========================================
void MonitorLoop() {
    // How long to wait between checks. We poll more often while idle so a game
    // is detected quickly, but back off a little while watching a running
    // process (nothing can change until it exits).
    const DWORD kIdleIntervalMs = 1000;
    const DWORD kWatchIntervalMs = 2000;

    while (g_MonitorRunning) {
        DWORD sleepMs = kIdleIntervalMs;

        // Grab the latest immutable snapshot (cheap shared_ptr copy).
        std::shared_ptr<const std::vector<AppProfile>> snap;
        {
            std::lock_guard<std::mutex> lock(g_SnapshotMutex);
            snap = g_ProfileSnapshot;
        }

        if (g_ActiveIndex >= 0) {
            sleepMs = kWatchIntervalMs;
            // An active profile is being watched. If it was removed from the
            // config, or its process exited, restore and stop watching.
            bool stillConfigured = (snap && g_ActiveIndex < (int)snap->size());
            bool processGone = !stillConfigured || !IsProcessRunning(g_ActiveProfileCopy.exeName);
            if (processGone) {
                if (g_ActiveProfileCopy.restore) {
                    if (RestoreActiveIfChanged())
                        LogMsg(L"Restored resolution (process exited): " + g_ActiveProfileCopy.exeName);
                } else {
                    ClearActiveState();
                }
            }
        } else if (snap && !snap->empty()) {
            std::set<std::wstring> running = GetRunningProcessNames();
            for (size_t i = 0; i < snap->size(); ++i) {
                const AppProfile& p = (*snap)[i];
                if (p.enabled && running.count(ToLowerCopy(p.exeName))) {
                    ULONGLONG now = GetTickCount64();
                    if (g_ProcessFoundTime == 0) {
                        g_ProcessFoundTime = now;
                        LogMsg(L"Detected process: " + p.exeName);
                    }

                    if ((now - g_ProcessFoundTime) >= (ULONGLONG)(p.delaySec * 1000)) {
                        DEVMODEW orig = GetCurrentRes(p.displayDev);
                        int result = ChangeRes(p.displayDev, p.targetW, p.targetH, p.targetHz);
                        // Publish the new active state under the restore lock so
                        // the UI thread's RestoreActiveIfChanged() sees a
                        // consistent (dev mode, active index, changed flag) set.
                        {
                            std::lock_guard<std::mutex> lock(g_RestoreMutex);
                            g_OriginalDevMode = orig;
                            g_ResChanged = (result == DISP_CHANGE_SUCCESSFUL);
                            g_ActiveProfileCopy = p;   // value copy -> never dangles
                            g_ActiveIndex = (int)i;
                        }
                        if (result == DISP_CHANGE_SUCCESSFUL) {
                            LogMsg(L"Applied " + std::to_wstring(p.targetW) + L"x" + std::to_wstring(p.targetH) +
                                   L"@" + std::to_wstring(p.targetHz) + L"Hz for " + p.exeName);
                        } else {
                            LogMsg(L"FAILED to apply mode for " + p.exeName + L" (code " + std::to_wstring(result) + L")");
                        }
                        break;
                    }
                }
            }
        }

        // Interruptible sleep: check the shutdown flag frequently so exit is fast.
        DWORD waited = 0;
        while (waited < sleepMs && g_MonitorRunning) {
            Sleep(200);
            waited += 200;
        }
    }
}

// ==========================================
// UI MANAGEMENT
// ==========================================
// Icons are owned per rendered list row and re-created on refresh.
std::vector<HICON> g_ListItemIcons;

void RefreshList(int selectIndex = -1) {
    for (HICON ic : g_ListItemIcons) if (ic) DestroyIcon(ic);
    g_ListItemIcons.clear();

    SendMessage(g_hList, LB_RESETCONTENT, 0, 0);
    for (const auto& p : g_Profiles) {
        std::wstring display = p.exeName + L" (" + std::to_wstring(p.targetW) + L"x" + std::to_wstring(p.targetH) + L")";
        int idx = (int)SendMessage(g_hList, LB_ADDSTRING, 0, (LPARAM)display.c_str());
        HICON ic = ExtractExeIcon(p.exePath, 16, 16);
        g_ListItemIcons.push_back(ic);
        SendMessage(g_hList, LB_SETITEMDATA, idx, (LPARAM)ic);
    }
    if (selectIndex >= 0 && selectIndex < (int)g_Profiles.size()) {
        SendMessage(g_hList, LB_SETCURSEL, selectIndex, 0);
    }
}

void SelectProfile(int index) {
    if (index >= 0 && index < (int)g_Profiles.size()) {
        const auto& p = g_Profiles[index];
        SetWindowTextW(g_hExe, p.exePath.c_str());

        g_IgnoreEditChange = true;
        SetWindowTextW(g_hW, std::to_wstring(p.targetW).c_str());
        SetWindowTextW(g_hH, std::to_wstring(p.targetH).c_str());
        g_IgnoreEditChange = false;

        SetWindowTextW(g_hHz, std::to_wstring(p.targetHz).c_str());
        SetWindowTextW(g_hDelay, std::to_wstring(p.delaySec).c_str());
        SendMessage(g_hRestore, BM_SETCHECK, p.restore ? BST_CHECKED : BST_UNCHECKED, 0);
        SendMessage(g_hEnable, BM_SETCHECK, p.enabled ? BST_CHECKED : BST_UNCHECKED, 0);

        SendMessage(g_hDisplayCombo, CB_SETCURSEL, 0, 0);
        for (size_t i = 0; i < g_MonitorDevices.size(); ++i) {
            if (g_MonitorDevices[i] == p.displayDev) {
                SendMessage(g_hDisplayCombo, CB_SETCURSEL, i, 0); break;
            }
        }

        int presetIdx = 0; 
        for (size_t i = 1; i < g_Presets.size(); ++i) {
            if (g_Presets[i].w == p.targetW && g_Presets[i].h == p.targetH) {
                presetIdx = i; break;
            }
        }
        SendMessage(g_hPresetCombo, CB_SETCURSEL, presetIdx, 0);
    }
}

BOOL CALLBACK SetFontCallback(HWND hwndChild, LPARAM lParam) {
    SendMessage(hwndChild, WM_SETFONT, lParam, TRUE);
    return TRUE;
}

// ==========================================
// THEME (DARK MODE)
// ==========================================
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

HBRUSH g_hDarkBrush = NULL;   // Owned background brush for dark mode
HBRUSH g_hEditBrush = NULL;   // Owned edit-field background brush

// Applies the Windows immersive dark title bar. The rest of the dark styling is
// handled in WM_CTLCOLOR* using the brushes above.
void ApplyTheme(HWND hwnd) {
    BOOL dark = g_DarkMode ? TRUE : FALSE;
    // Older builds honour attribute 19, newer ones 20; try both.
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)))) {
        const DWORD alt = 19;
        DwmSetWindowAttribute(hwnd, alt, &dark, sizeof(dark));
    }
    if (!g_hDarkBrush) g_hDarkBrush = CreateSolidBrush(RGB(32, 32, 32));
    if (!g_hEditBrush) g_hEditBrush = CreateSolidBrush(RGB(45, 45, 45));
    InvalidateRect(hwnd, NULL, TRUE);
    RedrawWindow(hwnd, NULL, NULL, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

// ==========================================
// TEST-MODE AUTO-REVERT
// ==========================================
// When the user tests a display mode we apply it, then arm a countdown. If the
// user does not confirm within the grace period we automatically restore the
// previous mode. This is the safety net that the README always promised but
// the original blocking MessageBox never delivered.
#define TIMER_TEST_REVERT 1
#define TEST_REVERT_SECONDS 15

DEVMODEW g_TestOriginal;
std::wstring g_TestDevice;
bool g_TestActive = false;

void CancelTestTimer(HWND hwnd) {
    if (g_TestActive) {
        KillTimer(hwnd, TIMER_TEST_REVERT);
        g_TestActive = false;
    }
}

// Returns true and writes the value when the field holds a positive integer.
bool ReadPositiveInt(HWND hEdit, int minVal, int maxVal, int& out) {
    WCHAR buf[16] = { 0 };
    GetWindowTextW(hEdit, buf, 16);
    if (wcslen(buf) == 0) return false;
    int v = _wtoi(buf);
    if (v < minVal || v > maxVal) return false;
    out = v;
    return true;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Left Panel
            g_hListLabel = CreateWindowW(L"STATIC", L"Profiles:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hList = CreateWindowW(L"LISTBOX", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT, 0, 0, 0, 0, hwnd, (HMENU)100, NULL, NULL);

            // Right Panel
            g_hExeLabel = CreateWindowW(L"STATIC", L"Executable Path:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hExe = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hBtnBrowse = CreateWindowW(L"BUTTON", L"...", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)101, NULL, NULL);

            g_hDisplayLabel = CreateWindowW(L"STATIC", L"Target Display (Monitor):", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hDisplayCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)110, NULL, NULL);

            g_hSupportedLabel = CreateWindowW(L"STATIC", L"Supported Modes (live from monitor):", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hSupportedCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)111, NULL, NULL);

            g_hPresetLabel = CreateWindowW(L"STATIC", L"Resolution Template / Presets:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hPresetCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)107, NULL, NULL);

            g_hWLabel = CreateWindowW(L"STATIC", L"Width:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hW = CreateWindowW(L"EDIT", L"1920", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)108, NULL, NULL);

            g_hHLabel = CreateWindowW(L"STATIC", L"Height:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hH = CreateWindowW(L"EDIT", L"1080", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, (HMENU)109, NULL, NULL);

            g_hHzLabel = CreateWindowW(L"STATIC", L"Hz (0=Def):", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hHz = CreateWindowW(L"EDIT", L"0", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);

            g_hDelayLabel = CreateWindowW(L"STATIC", L"Delay (sec):", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hDelay = CreateWindowW(L"EDIT", L"0", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_NUMBER, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);

            g_hRestore = CreateWindowW(L"BUTTON", L"Restore resolution on exit", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hEnable = CreateWindowW(L"BUTTON", L"Enable this profile", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            SendMessage(g_hRestore, BM_SETCHECK, BST_CHECKED, 0);
            SendMessage(g_hEnable, BM_SETCHECK, BST_CHECKED, 0);

            g_hSaveBtn = CreateWindowW(L"BUTTON", L"Save Profile", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)102, NULL, NULL);
            g_hDelBtn = CreateWindowW(L"BUTTON", L"Delete", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)103, NULL, NULL);
            g_hLaunchBtn = CreateWindowW(L"BUTTON", L"Launch App", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)105, NULL, NULL);
            g_hTestBtn = CreateWindowW(L"BUTTON", L"Test Display Settings", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)104, NULL, NULL);

            g_hStartup = CreateWindowW(L"BUTTON", L"Start automatically with Windows (Minimized)", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)106, NULL, NULL);
            if (GetRunAtStartup()) SendMessage(g_hStartup, BM_SETCHECK, BST_CHECKED, 0);

            g_hLogChk = CreateWindowW(L"BUTTON", L"Enable diagnostic logging", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)112, NULL, NULL);
            g_hDarkChk = CreateWindowW(L"BUTTON", L"Dark mode", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX, 0, 0, 0, 0, hwnd, (HMENU)113, NULL, NULL);
            if (g_LoggingEnabled) SendMessage(g_hLogChk, BM_SETCHECK, BST_CHECKED, 0);
            if (g_DarkMode) SendMessage(g_hDarkChk, BM_SETCHECK, BST_CHECKED, 0);

            DragAcceptFiles(hwnd, TRUE);

            for (const auto& preset : g_Presets) {
                SendMessage(g_hPresetCombo, CB_ADDSTRING, 0, (LPARAM)preset.name.c_str());
            }
            SendMessage(g_hPresetCombo, CB_SETCURSEL, 0, 0);
            RefreshSupportedModes();

            HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            EnumChildWindows(hwnd, SetFontCallback, (LPARAM)hFont);

            // Tray Icon Setup (loads the baked-in icon '1' at the correct small
            // size so it renders crisply in the notification area).
            g_hAppIcon = (HICON)LoadImageW(g_hInst, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                           GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON),
                                           LR_DEFAULTCOLOR);
            if (!g_hAppIcon) g_hAppIcon = LoadIconW(NULL, IDI_APPLICATION);

            g_Nid.cbSize = sizeof(NOTIFYICONDATAW);
            g_Nid.hWnd = hwnd;
            g_Nid.uID = 1;
            g_Nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_Nid.uCallbackMessage = WM_TRAYICON;
            g_Nid.hIcon = g_hAppIcon;
            wcscpy_s(g_Nid.szTip, L"AutoRes Changer");
            Shell_NotifyIconW(NIM_ADD, &g_Nid);
            break;
        }
        case WM_SIZE: {
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (w == 0 || h == 0) break; // Minimized

            int listWidth = w * 33 / 100; 
            if (listWidth < 180) listWidth = 180;
            if (listWidth > 260) listWidth = 260;

            int rightX = listWidth + 20;
            int rightW = w - rightX - 10;
            if (rightW < 220) rightW = 220;

            MoveWindow(g_hListLabel, 10, 10, listWidth, 20, TRUE);
            MoveWindow(g_hList, 10, 30, listWidth, h - 70, TRUE);

            MoveWindow(g_hExeLabel, rightX, 10, rightW, 20, TRUE);
            MoveWindow(g_hExe, rightX, 30, rightW - 35, 22, TRUE);
            MoveWindow(g_hBtnBrowse, rightX + rightW - 30, 30, 30, 22, TRUE);

            MoveWindow(g_hDisplayLabel, rightX, 58, rightW, 18, TRUE);
            MoveWindow(g_hDisplayCombo, rightX, 76, rightW, 150, TRUE);

            MoveWindow(g_hSupportedLabel, rightX, 104, rightW, 18, TRUE);
            MoveWindow(g_hSupportedCombo, rightX, 122, rightW, 150, TRUE);

            MoveWindow(g_hPresetLabel, rightX, 150, rightW, 18, TRUE);
            MoveWindow(g_hPresetCombo, rightX, 168, rightW, 150, TRUE);

            int colW = rightW / 4;
            MoveWindow(g_hWLabel, rightX, 196, colW - 5, 18, TRUE);
            MoveWindow(g_hW, rightX, 214, colW - 5, 22, TRUE);

            MoveWindow(g_hHLabel, rightX + colW, 196, colW - 5, 18, TRUE);
            MoveWindow(g_hH, rightX + colW, 214, colW - 5, 22, TRUE);

            MoveWindow(g_hHzLabel, rightX + colW * 2, 196, colW - 5, 18, TRUE);
            MoveWindow(g_hHz, rightX + colW * 2, 214, colW - 5, 22, TRUE);

            MoveWindow(g_hDelayLabel, rightX + colW * 3, 196, colW - 5, 18, TRUE);
            MoveWindow(g_hDelay, rightX + colW * 3, 214, colW - 5, 22, TRUE);

            MoveWindow(g_hRestore, rightX, 244, rightW, 20, TRUE);
            MoveWindow(g_hEnable, rightX, 264, rightW, 20, TRUE);

            int btnW = rightW / 3;
            MoveWindow(g_hSaveBtn, rightX, 292, btnW - 5, 30, TRUE);
            MoveWindow(g_hDelBtn, rightX + btnW, 292, btnW - 5, 30, TRUE);
            MoveWindow(g_hLaunchBtn, rightX + btnW * 2, 292, btnW - 5, 30, TRUE);

            MoveWindow(g_hTestBtn, rightX, 326, rightW, 30, TRUE);

            // Bottom row of options.
            int optW = rightW / 2;
            MoveWindow(g_hLogChk, rightX, h - 58, optW - 5, 20, TRUE);
            MoveWindow(g_hDarkChk, rightX + optW, h - 58, optW - 5, 20, TRUE);
            MoveWindow(g_hStartup, rightX, h - 34, rightW, 20, TRUE);
            break;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 580; 
            mmi->ptMinTrackSize.y = 500; 
            return 0;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN: {
            if (g_DarkMode && g_hDarkBrush) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, RGB(230, 230, 230));
                SetBkColor(hdc, RGB(32, 32, 32));
                return (LRESULT)g_hDarkBrush;
            }
            break;
        }
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            if (g_DarkMode && g_hEditBrush) {
                HDC hdc = (HDC)wParam;
                SetTextColor(hdc, RGB(230, 230, 230));
                SetBkColor(hdc, RGB(45, 45, 45));
                return (LRESULT)g_hEditBrush;
            }
            break;
        }
        case WM_ERASEBKGND: {
            if (g_DarkMode && g_hDarkBrush) {
                RECT rc; GetClientRect(hwnd, &rc);
                FillRect((HDC)wParam, &rc, g_hDarkBrush);
                return 1;
            }
            break;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == 100 && HIWORD(wParam) == LBN_SELCHANGE) {
                SelectProfile(SendMessage(g_hList, LB_GETCURSEL, 0, 0));
            } else if (wmId == 110 && HIWORD(wParam) == CBN_SELCHANGE) { // Display changed
                RefreshSupportedModes();
            } else if (wmId == 111 && HIWORD(wParam) == CBN_SELCHANGE) { // Supported mode picked
                int sel = (int)SendMessage(g_hSupportedCombo, CB_GETCURSEL, 0, 0);
                int mi = sel - 1; // combo index 0 is the placeholder
                if (mi >= 0 && mi < (int)g_SupportedModesCache.size()) {
                    const ModeEntry& m = g_SupportedModesCache[mi];
                    g_IgnoreEditChange = true;
                    SetWindowTextW(g_hW, std::to_wstring(m.w).c_str());
                    SetWindowTextW(g_hH, std::to_wstring(m.h).c_str());
                    SetWindowTextW(g_hHz, std::to_wstring(m.hz > 0 ? m.hz : 0).c_str());
                    g_IgnoreEditChange = false;
                    SendMessage(g_hPresetCombo, CB_SETCURSEL, 0, 0);
                }
            } else if (wmId == 101) { // Browse
                WCHAR szFile[MAX_PATH] = { 0 };
                OPENFILENAMEW ofn = { 0 };
                ofn.lStructSize = sizeof(ofn);
                ofn.hwndOwner = hwnd;
                ofn.lpstrFile = szFile;
                ofn.nMaxFile = MAX_PATH;
                ofn.lpstrFilter = L"Executables\0*.exe\0All\0*.*\0";
                ofn.lpstrDefExt = L"exe";
                ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST;
                if (GetOpenFileNameW(&ofn)) SetWindowTextW(g_hExe, szFile);
            } else if (wmId == 102) { // Save
                WCHAR exe[MAX_PATH];
                GetWindowTextW(g_hExe, exe, MAX_PATH);
                if (wcslen(exe) == 0) {
                    MessageBoxW(hwnd, L"Select an executable path first.", L"Missing Path", MB_OK | MB_ICONWARNING);
                    break;
                }
                int w = 0, h = 0;
                if (!ReadPositiveInt(g_hW, 1, 32767, w) || !ReadPositiveInt(g_hH, 1, 32767, h)) {
                    MessageBoxW(hwnd, L"Width and Height must be positive numbers (1-32767).", L"Invalid Resolution", MB_OK | MB_ICONWARNING);
                    break;
                }
                WCHAR hzbuf[16] = { 0 }, delbuf[16] = { 0 };
                GetWindowTextW(g_hHz, hzbuf, 16);
                GetWindowTextW(g_hDelay, delbuf, 16);
                int hz = _wtoi(hzbuf); if (hz < 0) hz = 0;
                int delSec = _wtoi(delbuf); if (delSec < 0) delSec = 0;

                AppProfile p;
                p.exePath = exe; p.exeName = GetFileName(p.exePath);
                p.targetW = w; p.targetH = h; p.targetHz = hz; p.delaySec = delSec;
                p.restore = SendMessage(g_hRestore, BM_GETCHECK, 0, 0) == BST_CHECKED;
                p.enabled = SendMessage(g_hEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;

                int comboIdx = SendMessage(g_hDisplayCombo, CB_GETCURSEL, 0, 0);
                p.displayDev = (comboIdx > 0 && comboIdx < (int)g_MonitorDevices.size()) ? g_MonitorDevices[comboIdx] : L"";

                int targetIdx = -1;
                bool found = false;
                for (size_t i = 0; i < g_Profiles.size(); ++i) {
                    if (_wcsicmp(g_Profiles[i].exePath.c_str(), p.exePath.c_str()) == 0) { 
                        g_Profiles[i] = p; found = true; targetIdx = (int)i; break; 
                    }
                }
                if (!found) {
                    g_Profiles.push_back(p);
                    targetIdx = (int)g_Profiles.size() - 1;
                }
                
                SaveConfig();
                PublishSnapshot();
                RefreshList(targetIdx);
            } else if (wmId == 103) { // Delete
                int idx = SendMessage(g_hList, LB_GETCURSEL, 0, 0);
                if (idx != LB_ERR) { g_Profiles.erase(g_Profiles.begin() + idx); SaveConfig(); PublishSnapshot(); RefreshList(); SetWindowTextW(g_hExe, L""); }
            } else if (wmId == 104) { // Test
                int w = 0, h = 0, hz = 0;
                if (!ReadPositiveInt(g_hW, 1, 32767, w) || !ReadPositiveInt(g_hH, 1, 32767, h)) {
                    MessageBoxW(hwnd, L"Enter a valid Width and Height before testing.", L"Invalid Input", MB_OK | MB_ICONWARNING);
                    break;
                }
                // Hz is optional: blank or 0 means "use the driver default".
                {
                    WCHAR hzbuf[16] = { 0 };
                    GetWindowTextW(g_hHz, hzbuf, 16);
                    hz = _wtoi(hzbuf);
                    if (hz < 0) hz = 0;
                }
                int idx = SendMessage(g_hDisplayCombo, CB_GETCURSEL, 0, 0);
                std::wstring dev = (idx > 0 && idx < (int)g_MonitorDevices.size()) ? g_MonitorDevices[idx] : L"";

                DEVMODEW orig = GetCurrentRes(dev);
                if (ChangeRes(dev, w, h, hz) == DISP_CHANGE_SUCCESSFUL) {
                    g_TestOriginal = orig;
                    g_TestDevice = dev;
                    g_TestActive = true;
                    SetTimer(hwnd, TIMER_TEST_REVERT, (UINT)TEST_REVERT_SECONDS * 1000, NULL);
                    std::wstring msg = L"Resolution applied. Click OK to keep it, or it will "
                                       L"automatically revert in " + std::to_wstring(TEST_REVERT_SECONDS) + L" seconds.";
                    MessageBoxW(hwnd, msg.c_str(), L"Testing Display Settings", MB_OK | MB_ICONINFORMATION);
                    // If the user pressed OK within the window, keep the mode.
                    if (g_TestActive) CancelTestTimer(hwnd);
                } else {
                    MessageBoxW(hwnd, L"Monitor does not support this mode.", L"Error", MB_OK | MB_ICONERROR);
                }
            } else if (wmId == 105) { // Launch App
                WCHAR exe[MAX_PATH]; GetWindowTextW(g_hExe, exe, MAX_PATH);
                if (wcslen(exe) > 0) ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
            } else if (wmId == 106) { // Start with Windows
                SetRunAtStartup(SendMessage(g_hStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (wmId == 107 && HIWORD(wParam) == CBN_SELCHANGE) { // Preset changed
                int idx = SendMessage(g_hPresetCombo, CB_GETCURSEL, 0, 0);
                if (idx > 0 && idx < (int)g_Presets.size()) {
                    g_IgnoreEditChange = true;
                    SetWindowTextW(g_hW, std::to_wstring(g_Presets[idx].w).c_str());
                    SetWindowTextW(g_hH, std::to_wstring(g_Presets[idx].h).c_str());
                    g_IgnoreEditChange = false;
                }
            } else if ((wmId == 108 || wmId == 109) && HIWORD(wParam) == EN_CHANGE) { 
                if (!g_IgnoreEditChange) {
                    SendMessage(g_hPresetCombo, CB_SETCURSEL, 0, 0); 
                }
            } else if (wmId == 112) { // Logging toggle
                g_LoggingEnabled = (SendMessage(g_hLogChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveSettings();
                if (g_LoggingEnabled) LogMsg(L"Diagnostic logging enabled.");
            } else if (wmId == 113) { // Dark mode toggle
                g_DarkMode = (SendMessage(g_hDarkChk, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveSettings();
                ApplyTheme(hwnd);
            } else if (wmId == 201) { // Tray: open window
                ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd);
            } else if (wmId == 202) { // Tray: open config folder
                std::wstring dir = g_IniPath.substr(0, g_IniPath.find_last_of(L'\\'));
                if (!dir.empty()) ShellExecuteW(NULL, L"open", dir.c_str(), NULL, NULL, SW_SHOWNORMAL);
            } else if (wmId == 203) { // Tray: restore resolution now
                if (RestoreActiveIfChanged()) LogMsg(L"User restored resolution from tray.");
            } else if (wmId == 200) { 
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == TIMER_TEST_REVERT && g_TestActive) {
                RestoreRes(g_TestDevice, g_TestOriginal);
                g_TestActive = false;
                KillTimer(hwnd, TIMER_TEST_REVERT);
            }
            break;
        }
        case WM_MEASUREITEM: {
            MEASUREITEMSTRUCT* mis = (MEASUREITEMSTRUCT*)lParam;
            if (mis->CtlID == 100) { mis->itemHeight = 22; return TRUE; }
            break;
        }
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* dis = (DRAWITEMSTRUCT*)lParam;
            if (dis->CtlID != 100) break;
            if ((int)dis->itemID < 0) return TRUE; // empty list

            WCHAR text[512] = { 0 };
            SendMessageW(g_hList, LB_GETTEXT, dis->itemID, (LPARAM)text);

            bool selected = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = selected ? GetSysColor(COLOR_HIGHLIGHT)
                                   : (g_DarkMode ? RGB(32, 32, 32) : GetSysColor(COLOR_WINDOW));
            COLORREF fg = selected ? GetSysColor(COLOR_HIGHLIGHTTEXT)
                                   : (g_DarkMode ? RGB(230, 230, 230) : GetSysColor(COLOR_WINDOWTEXT));

            HBRUSH bgb = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgb);
            DeleteObject(bgb);

            HICON ic = (HICON)SendMessage(g_hList, LB_GETITEMDATA, dis->itemID, 0);
            int textX = dis->rcItem.left + 4;
            if (ic) {
                DrawIconEx(dis->hDC, dis->rcItem.left + 3, dis->rcItem.top + 3, ic, 16, 16, 0, NULL, DI_NORMAL);
                textX = dis->rcItem.left + 24;
            }
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, fg);
            RECT tr = dis->rcItem; tr.left = textX;
            DrawTextW(dis->hDC, text, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
            return TRUE;
        }
        case WM_DISPLAYCHANGE: {
            // A monitor was added/removed or a mode changed out from under us.
            // Re-enumerate displays and refresh the supported-mode list without
            // disturbing the user's current field entries.
            PopulateMonitors();
            RefreshSupportedModes();
            LogMsg(L"WM_DISPLAYCHANGE received; refreshed monitor list.");
            break;
        }
        case WM_DROPFILES: {
            HDROP hDrop = (HDROP)wParam;
            UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, NULL, 0);
            if (count > 0) {
                WCHAR file[MAX_PATH] = { 0 };
                DragQueryFileW(hDrop, 0, file, MAX_PATH);
                std::wstring path = file;
                std::wstring lower = ToLowerCopy(path);
                if (lower.size() >= 4 && lower.substr(lower.size() - 4) == L".exe") {
                    SetWindowTextW(g_hExe, path.c_str());
                    LogMsg(L"Dropped executable: " + path);
                } else {
                    MessageBoxW(hwnd, L"Please drop an .exe file.", L"Unsupported File", MB_OK | MB_ICONINFORMATION);
                }
            }
            DragFinish(hDrop);
            break;
        }
        case WM_TRAYICON: {
            if (lParam == WM_LBUTTONDBLCLK) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
            else if (lParam == WM_RBUTTONUP) {
                POINT pt; GetCursorPos(&pt); HMENU hMenu = CreatePopupMenu();
                bool active = (g_ActiveIndex >= 0);
                InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING | MF_GRAYED, 0, L"AutoRes Changer");
                InsertMenuW(hMenu, 1, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                InsertMenuW(hMenu, 2, MF_BYPOSITION | MF_STRING, 201, L"Open window");
                InsertMenuW(hMenu, 3, MF_BYPOSITION | MF_STRING | (active ? MF_ENABLED : MF_GRAYED),
                            203, L"Restore resolution now");
                InsertMenuW(hMenu, 4, MF_BYPOSITION | MF_STRING, 202, L"Open config folder");
                InsertMenuW(hMenu, 5, MF_BYPOSITION | MF_SEPARATOR, 0, NULL);
                InsertMenuW(hMenu, 6, MF_BYPOSITION | MF_STRING, 200, L"Exit AutoRes Changer");
                SetForegroundWindow(hwnd);
                TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;
        }
        case WM_CLOSE: {
            // Keep the icon/message flags intact and just add the balloon.
            NOTIFYICONDATAW nid = g_Nid;
            nid.uFlags = NIF_INFO;
            wcscpy_s(nid.szInfoTitle, L"AutoRes Changer");
            wcscpy_s(nid.szInfo, L"Running in background. Right-click tray icon to exit.");
            nid.dwInfoFlags = NIIF_INFO;
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        case WM_ENDSESSION:
        case WM_DESTROY: {
            // On logoff/shutdown (WM_ENDSESSION) or normal exit, make sure the
            // desktop is restored if we are the ones who changed it.
            CancelTestTimer(hwnd);
            if (msg == WM_ENDSESSION) {
                RestoreActiveIfChanged();
                break;
            }
            Shell_NotifyIconW(NIM_DELETE, &g_Nid);
            RestoreActiveIfChanged();
            if (g_hAppIcon) { DestroyIcon(g_hAppIcon); g_hAppIcon = NULL; }
            for (HICON ic : g_ListItemIcons) if (ic) DestroyIcon(ic);
            g_ListItemIcons.clear();
            if (g_hDarkBrush) { DeleteObject(g_hDarkBrush); g_hDarkBrush = NULL; }
            if (g_hEditBrush) { DeleteObject(g_hEditBrush); g_hEditBrush = NULL; }
            if (g_hInstanceMutex) { CloseHandle(g_hInstanceMutex); g_hInstanceMutex = NULL; }
            PostQuitMessage(0);
            break;
        }
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ==========================================
// DPI AWARENESS
// ==========================================
// Prefer the modern per-monitor-v2 context (Win10 1703+) and gracefully fall
// back for older Windows builds. This keeps the UI crisp on mixed-DPI setups.
void EnableBestDpiAwareness() {
    typedef BOOL(WINAPI* SetCtxFn)(HANDLE);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32) {
        SetCtxFn setCtx = reinterpret_cast<SetCtxFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setCtx) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
            if (setCtx((HANDLE)-4)) return;
        }
        // Fallback: framework/legacy shcore API.
        typedef HRESULT(WINAPI* SetShcoreFn)(int);
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        if (shcore) {
            SetShcoreFn setShcore = reinterpret_cast<SetShcoreFn>(
                reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
            if (setShcore && SUCCEEDED(setShcore(2 /* PROCESS_PER_MONITOR_DPI_AWARE */))) return;
        }
    }
    SetProcessDPIAware();
}

// ==========================================
// APPLICATION ENTRY
// ==========================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance;
    EnableBestDpiAwareness();
    g_hInst = hInstance; // Store instance for resource retrieval

    // Single-instance guard: a second launch should not create a duplicate tray
    // icon or a competing monitor thread. If already running, just exit.
    g_hInstanceMutex = CreateMutexW(NULL, TRUE, L"AutoResChanger_SingleInstance");
    if (g_hInstanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_hInstanceMutex);
        g_hInstanceMutex = NULL;
        return 0;
    }

    InitConfigPath(); 
    LoadConfig();
    LoadSettings();
    PublishSnapshot();

    WNDCLASSW wc = {0}; 
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); 
    wc.lpszClassName = L"AutoResChangerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); 
    wc.hIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                 0, 0, LR_DEFAULTSIZE | LR_DEFAULTCOLOR);
    if (!wc.hIcon) wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    RegisterClassW(&wc);

    g_hMain = CreateWindowW(L"AutoResChangerClass", L"AutoRes Changer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 780, 560, NULL, NULL, hInstance, NULL);

    ApplyTheme(g_hMain);
    PopulateMonitors(); RefreshList(); RefreshSupportedModes();
    std::thread monitorThread(MonitorLoop);

    // Parse command line case-insensitively
    bool startSilent = false;
    std::wstring cmdLine(lpCmdLine);
    for (size_t i = 0; i < cmdLine.length(); ++i) {
        if (cmdLine[i] >= L'A' && cmdLine[i] <= L'Z') {
            cmdLine[i] = cmdLine[i] - L'A' + L'a';
        }
    }
    if (cmdLine.find(L"silent") != std::wstring::npos || cmdLine.find(L"min") != std::wstring::npos) {
        startSilent = true;
    }

    // Hide on boot if requested silently, or if the system launched it in a minimized/hidden state
    if (startSilent || nCmdShow == SW_HIDE || nCmdShow == SW_MINIMIZE || nCmdShow == SW_SHOWMINIMIZED || nCmdShow == SW_SHOWMINNOACTIVE) {
        ShowWindow(g_hMain, SW_HIDE);
    } else {
        ShowWindow(g_hMain, nCmdShow);
        UpdateWindow(g_hMain);
    }

    MSG msg;
    BOOL bRet;
    while ((bRet = GetMessage(&msg, NULL, 0, 0)) != 0) {
        if (bRet == -1) break; // GetMessage error
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    g_MonitorRunning = false;
    if (monitorThread.joinable()) monitorThread.join();
    if (g_hInstanceMutex) { CloseHandle(g_hInstanceMutex); g_hInstanceMutex = NULL; }
    return (int)msg.wParam;
}