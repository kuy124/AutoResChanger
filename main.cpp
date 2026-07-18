#pragma comment(linker,"\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")

#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <commctrl.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>

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

std::vector<AppProfile> g_Profiles;
AppProfile* g_ActiveProfile = nullptr;
DEVMODEW g_OriginalDevMode;
bool g_ResChanged = false;
std::atomic<bool> g_MonitorRunning(true);
std::wstring g_IniPath;
ULONGLONG g_ProcessFoundTime = 0;
bool g_IgnoreEditChange = false;
HINSTANCE g_hInst = NULL; // Stores instance handle for loading the baked-in icon

// UI Handles
HWND g_hMain, g_hList, g_hExe, g_hDisplayCombo, g_hW, g_hH, g_hHz, g_hDelay;
HWND g_hRestore, g_hEnable, g_hStartup, g_hPresetCombo;
HWND g_hListLabel, g_hExeLabel, g_hBtnBrowse, g_hDisplayLabel, g_hPresetLabel;
HWND g_hWLabel, g_hHLabel, g_hHzLabel, g_hDelayLabel, g_hSaveBtn, g_hDelBtn, g_hLaunchBtn, g_hTestBtn;

#define WM_TRAYICON (WM_APP + 1)
NOTIFYICONDATAW g_Nid = {};
std::vector<std::wstring> g_MonitorDevices;

// ==========================================
// UTILITY & REGISTRY FUNCTIONS
// ==========================================
std::wstring GetFileName(const std::wstring& path) {
    size_t pos = path.find_last_of(L"\\/");
    return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool IsProcessRunning(const std::wstring& processName) {
    bool exists = false;
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
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
    }
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
    WritePrivateProfileStringW(NULL, NULL, NULL, g_IniPath.c_str()); 
    DeleteFileW(g_IniPath.c_str());
    
    WritePrivateProfileStringW(L"Settings", L"Count", std::to_wstring(g_Profiles.size()).c_str(), g_IniPath.c_str());
    
    for (size_t i = 0; i < g_Profiles.size(); ++i) {
        std::wstring sec = L"Profile_" + std::to_wstring(i);
        WritePrivateProfileStringW(sec.c_str(), L"Exe", g_Profiles[i].exePath.c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Device", g_Profiles[i].displayDev.c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"W", std::to_wstring(g_Profiles[i].targetW).c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"H", std::to_wstring(g_Profiles[i].targetH).c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Hz", std::to_wstring(g_Profiles[i].targetHz).c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Delay", std::to_wstring(g_Profiles[i].delaySec).c_str(), g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Restore", g_Profiles[i].restore ? L"1" : L"0", g_IniPath.c_str());
        WritePrivateProfileStringW(sec.c_str(), L"Enabled", g_Profiles[i].enabled ? L"1" : L"0", g_IniPath.c_str());
    }
    WritePrivateProfileStringW(NULL, NULL, NULL, g_IniPath.c_str()); 
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

// ==========================================
// BACKGROUND THREAD
// ==========================================
void MonitorLoop() {
    while (g_MonitorRunning) {
        if (g_ActiveProfile != nullptr) {
            if (!IsProcessRunning(g_ActiveProfile->exeName)) {
                if (g_ActiveProfile->restore && g_ResChanged) {
                    RestoreRes(g_ActiveProfile->displayDev, g_OriginalDevMode);
                }
                g_ActiveProfile = nullptr; g_ResChanged = false; g_ProcessFoundTime = 0;
            }
        } else {
            for (auto& p : g_Profiles) {
                if (p.enabled && IsProcessRunning(p.exeName)) {
                    ULONGLONG now = GetTickCount64();
                    if (g_ProcessFoundTime == 0) g_ProcessFoundTime = now;

                    if ((now - g_ProcessFoundTime) >= (ULONGLONG)(p.delaySec * 1000)) {
                        g_OriginalDevMode = GetCurrentRes(p.displayDev);
                        if (ChangeRes(p.displayDev, p.targetW, p.targetH, p.targetHz) == DISP_CHANGE_SUCCESSFUL) {
                            g_ResChanged = true;
                        }
                        g_ActiveProfile = &p;
                        break;
                    }
                }
            }
        }
        Sleep(2000);
    }
}

// ==========================================
// UI MANAGEMENT
// ==========================================
void RefreshList(int selectIndex = -1) {
    SendMessage(g_hList, LB_RESETCONTENT, 0, 0);
    for (const auto& p : g_Profiles) {
        std::wstring display = p.exeName + L" (" + std::to_wstring(p.targetW) + L"x" + std::to_wstring(p.targetH) + L")";
        SendMessage(g_hList, LB_ADDSTRING, 0, (LPARAM)display.c_str());
    }
    if (selectIndex >= 0 && selectIndex < g_Profiles.size()) {
        SendMessage(g_hList, LB_SETCURSEL, selectIndex, 0);
    }
}

void SelectProfile(int index) {
    if (index >= 0 && index < g_Profiles.size()) {
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

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            // Left Panel
            g_hListLabel = CreateWindowW(L"STATIC", L"Profiles:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hList = CreateWindowW(L"LISTBOX", NULL, WS_VISIBLE | WS_CHILD | WS_BORDER | LBS_NOTIFY | WS_VSCROLL, 0, 0, 0, 0, hwnd, (HMENU)100, NULL, NULL);

            // Right Panel
            g_hExeLabel = CreateWindowW(L"STATIC", L"Executable Path:", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hExe = CreateWindowW(L"EDIT", L"", WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hBtnBrowse = CreateWindowW(L"BUTTON", L"...", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, (HMENU)101, NULL, NULL);

            g_hDisplayLabel = CreateWindowW(L"STATIC", L"Target Display (Monitor):", WS_VISIBLE | WS_CHILD, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);
            g_hDisplayCombo = CreateWindowW(L"COMBOBOX", NULL, WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, hwnd, NULL, NULL, NULL);

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

            for (const auto& preset : g_Presets) {
                SendMessage(g_hPresetCombo, CB_ADDSTRING, 0, (LPARAM)preset.name.c_str());
            }
            SendMessage(g_hPresetCombo, CB_SETCURSEL, 0, 0);

            HFONT hFont = CreateFontW(15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, ANSI_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            EnumChildWindows(hwnd, SetFontCallback, (LPARAM)hFont);

            // Tray Icon Setup (Loads baked-in icon '1' instead of IDI_APPLICATION)
            g_Nid.cbSize = sizeof(NOTIFYICONDATAW);
            g_Nid.hWnd = hwnd;
            g_Nid.uID = 1;
            g_Nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
            g_Nid.uCallbackMessage = WM_TRAYICON;
            g_Nid.hIcon = LoadIconW(g_hInst, MAKEINTRESOURCE(1));
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

            MoveWindow(g_hDisplayLabel, rightX, 60, rightW, 20, TRUE);
            MoveWindow(g_hDisplayCombo, rightX, 80, rightW, 150, TRUE);

            MoveWindow(g_hPresetLabel, rightX, 110, rightW, 20, TRUE);
            MoveWindow(g_hPresetCombo, rightX, 130, rightW, 150, TRUE);

            int colW = rightW / 4;
            MoveWindow(g_hWLabel, rightX, 160, colW - 5, 20, TRUE);
            MoveWindow(g_hW, rightX, 180, colW - 5, 22, TRUE);

            MoveWindow(g_hHLabel, rightX + colW, 160, colW - 5, 20, TRUE);
            MoveWindow(g_hH, rightX + colW, 180, colW - 5, 22, TRUE);

            MoveWindow(g_hHzLabel, rightX + colW * 2, 160, colW - 5, 20, TRUE);
            MoveWindow(g_hHz, rightX + colW * 2, 180, colW - 5, 22, TRUE);

            MoveWindow(g_hDelayLabel, rightX + colW * 3, 160, colW - 5, 20, TRUE);
            MoveWindow(g_hDelay, rightX + colW * 3, 180, colW - 5, 22, TRUE);

            MoveWindow(g_hRestore, rightX, 215, rightW, 20, TRUE);
            MoveWindow(g_hEnable, rightX, 235, rightW, 20, TRUE);

            int btnW = rightW / 3;
            MoveWindow(g_hSaveBtn, rightX, 270, btnW - 5, 30, TRUE);
            MoveWindow(g_hDelBtn, rightX + btnW, 270, btnW - 5, 30, TRUE);
            MoveWindow(g_hLaunchBtn, rightX + btnW * 2, 270, btnW - 5, 30, TRUE);

            MoveWindow(g_hTestBtn, rightX, 310, rightW, 30, TRUE);
            MoveWindow(g_hStartup, rightX, h - 35, rightW, 20, TRUE);
            break;
        }
        case WM_GETMINMAXINFO: {
            MINMAXINFO* mmi = (MINMAXINFO*)lParam;
            mmi->ptMinTrackSize.x = 550; 
            mmi->ptMinTrackSize.y = 440; 
            return 0;
        }
        case WM_COMMAND: {
            int wmId = LOWORD(wParam);
            if (wmId == 100 && HIWORD(wParam) == LBN_SELCHANGE) {
                SelectProfile(SendMessage(g_hList, LB_GETCURSEL, 0, 0));
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
                WCHAR exe[MAX_PATH], w[16], h[16], hz[16], del[16];
                GetWindowTextW(g_hExe, exe, MAX_PATH); if (wcslen(exe) == 0) break;
                GetWindowTextW(g_hW, w, 16); GetWindowTextW(g_hH, h, 16); 
                GetWindowTextW(g_hHz, hz, 16); GetWindowTextW(g_hDelay, del, 16);

                AppProfile p;
                p.exePath = exe; p.exeName = GetFileName(p.exePath);
                p.targetW = _wtoi(w); p.targetH = _wtoi(h); p.targetHz = _wtoi(hz); p.delaySec = _wtoi(del);
                p.restore = SendMessage(g_hRestore, BM_GETCHECK, 0, 0) == BST_CHECKED;
                p.enabled = SendMessage(g_hEnable, BM_GETCHECK, 0, 0) == BST_CHECKED;

                int comboIdx = SendMessage(g_hDisplayCombo, CB_GETCURSEL, 0, 0);
                p.displayDev = (comboIdx > 0 && comboIdx < g_MonitorDevices.size()) ? g_MonitorDevices[comboIdx] : L"";

                int targetIdx = -1;
                bool found = false;
                for (size_t i = 0; i < g_Profiles.size(); ++i) {
                    if (_wcsicmp(g_Profiles[i].exePath.c_str(), p.exePath.c_str()) == 0) { 
                        g_Profiles[i] = p; found = true; targetIdx = i; break; 
                    }
                }
                if (!found) {
                    g_Profiles.push_back(p);
                    targetIdx = g_Profiles.size() - 1;
                }
                
                SaveConfig(); 
                RefreshList(targetIdx); 
            } else if (wmId == 103) { // Delete
                int idx = SendMessage(g_hList, LB_GETCURSEL, 0, 0);
                if (idx != LB_ERR) { g_Profiles.erase(g_Profiles.begin() + idx); SaveConfig(); RefreshList(); SetWindowTextW(g_hExe, L""); }
            } else if (wmId == 104) { // Test
                WCHAR w[16], h[16], hz[16];
                GetWindowTextW(g_hW, w, 16); GetWindowTextW(g_hH, h, 16); GetWindowTextW(g_hHz, hz, 16);
                int idx = SendMessage(g_hDisplayCombo, CB_GETCURSEL, 0, 0);
                std::wstring dev = (idx > 0 && idx < g_MonitorDevices.size()) ? g_MonitorDevices[idx] : L"";
                
                DEVMODEW orig = GetCurrentRes(dev);
                if (ChangeRes(dev, _wtoi(w), _wtoi(h), _wtoi(hz)) == DISP_CHANGE_SUCCESSFUL) {
                    MessageBoxW(hwnd, L"Resolution applied! Press OK to revert.", L"Test", MB_OK | MB_ICONINFORMATION);
                    RestoreRes(dev, orig);
                } else MessageBoxW(hwnd, L"Monitor does not support this mode.", L"Error", MB_OK | MB_ICONERROR);
            } else if (wmId == 105) { // Launch App
                WCHAR exe[MAX_PATH]; GetWindowTextW(g_hExe, exe, MAX_PATH);
                if (wcslen(exe) > 0) ShellExecuteW(NULL, L"open", exe, NULL, NULL, SW_SHOWNORMAL);
            } else if (wmId == 106) { // Start with Windows
                SetRunAtStartup(SendMessage(g_hStartup, BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (wmId == 107 && HIWORD(wParam) == CBN_SELCHANGE) { // Preset changed
                int idx = SendMessage(g_hPresetCombo, CB_GETCURSEL, 0, 0);
                if (idx > 0 && idx < g_Presets.size()) {
                    g_IgnoreEditChange = true;
                    SetWindowTextW(g_hW, std::to_wstring(g_Presets[idx].w).c_str());
                    SetWindowTextW(g_hH, std::to_wstring(g_Presets[idx].h).c_str());
                    g_IgnoreEditChange = false;
                }
            } else if ((wmId == 108 || wmId == 109) && HIWORD(wParam) == EN_CHANGE) { 
                if (!g_IgnoreEditChange) {
                    SendMessage(g_hPresetCombo, CB_SETCURSEL, 0, 0); 
                }
            } else if (wmId == 200) { 
                DestroyWindow(hwnd);
            }
            break;
        }
        case WM_TRAYICON: {
            if (lParam == WM_LBUTTONDBLCLK) { ShowWindow(hwnd, SW_RESTORE); SetForegroundWindow(hwnd); }
            else if (lParam == WM_RBUTTONUP) {
                POINT pt; GetCursorPos(&pt); HMENU hMenu = CreatePopupMenu();
                InsertMenuW(hMenu, 0, MF_BYPOSITION | MF_STRING, 200, L"Exit AutoRes Changer");
                SetForegroundWindow(hwnd); TrackPopupMenu(hMenu, TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, NULL);
                DestroyMenu(hMenu);
            }
            break;
        }
        case WM_CLOSE:
            ShowWindow(hwnd, SW_HIDE);
            g_Nid.uFlags = NIF_INFO; wcscpy_s(g_Nid.szInfoTitle, L"AutoRes Changer");
            wcscpy_s(g_Nid.szInfo, L"Running in background. Right-click tray icon to exit.");
            Shell_NotifyIconW(NIM_MODIFY, &g_Nid);
            return 0; 
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_Nid); PostQuitMessage(0); break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ==========================================
// APPLICATION ENTRY
// ==========================================
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPWSTR lpCmdLine, int nCmdShow) {
    SetProcessDPIAware(); 
    g_hInst = hInstance; // Store instance for resource retrieval
    InitConfigPath(); 
    LoadConfig();

    WNDCLASSW wc = {0}; 
    wc.lpfnWndProc = WndProc; 
    wc.hInstance = hInstance;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW); 
    wc.lpszClassName = L"AutoResChangerClass";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); 
    wc.hIcon = LoadIconW(hInstance, MAKEINTRESOURCE(1)); // Loads custom icon for window/taskbar
    RegisterClassW(&wc);

    g_hMain = CreateWindowW(L"AutoResChangerClass", L"AutoRes Changer", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 750, 480, NULL, NULL, hInstance, NULL);

    PopulateMonitors(); RefreshList();
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

    MSG msg; while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    g_MonitorRunning = false; monitorThread.join();
    return 0;
}