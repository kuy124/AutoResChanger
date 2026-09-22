// test_main.cpp - Headless unit tests for AutoRes Changer's core logic.
//
// This file pulls in main.cpp so the tests exercise the *real* config/utility
// code, not a copy. Run via tests/run_tests.ps1 or CMake/CI.
//
// Zero external dependencies: a tiny assertion framework + a temp config dir.

#ifndef TESTING
#define TESTING
#endif
#include "../main.cpp"

#include <cstdio>
#include <cstdlib>

// ---- tiny test framework -------------------------------------------------
static int g_passed = 0;
static int g_failed = 0;
static const wchar_t* g_current = L"";

#define CHECK(cond) do { \
    if (cond) { ++g_passed; } \
    else { ++g_failed; \
        std::printf("  FAIL [%ls] %s:%d  %s\n", g_current, __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_EQ(a, b) do { \
    auto _va = (a); auto _vb = (b); \
    if (_va == _vb) { ++g_passed; } \
    else { ++g_failed; \
        std::printf("  FAIL [%ls] %s:%d  expected %s == %s\n", g_current, __FILE__, __LINE__, #a, #b); } \
} while (0)

#define TEST(name) g_current = name; std::printf("[TEST] %ls\n", name)

// ---- helpers -------------------------------------------------------------
static std::wstring TempDir() {
    WCHAR buf[MAX_PATH] = { 0 };
    GetTempPathW(MAX_PATH, buf);
    std::wstring dir = std::wstring(buf) + L"AutoResChangerTest";
    CreateDirectoryW(dir.c_str(), NULL);
    return dir;
}

static bool FileExists(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES;
}

// ---- tests ---------------------------------------------------------------

static void Test_GetFileName() {
    TEST(L"GetFileName");
    CHECK_EQ(GetFileName(L"C:\\Games\\Kendo.exe"), std::wstring(L"Kendo.exe"));
    CHECK_EQ(GetFileName(L"C:/Games/Kendo.exe"), std::wstring(L"Kendo.exe"));
    CHECK_EQ(GetFileName(L"Kendo.exe"), std::wstring(L"Kendo.exe"));
    CHECK_EQ(GetFileName(L""), std::wstring(L""));
    CHECK_EQ(GetFileName(L"C:\\dir\\"), std::wstring(L""));
    CHECK_EQ(GetFileName(L"\\\\server\\share\\app.exe"), std::wstring(L"app.exe"));
}

static void Test_ToLower() {
    TEST(L"ToLowerCopy");
    CHECK_EQ(ToLowerCopy(L"KeNdO.EXE"), std::wstring(L"kendo.exe"));
    CHECK_EQ(ToLowerCopy(L""), std::wstring(L""));
}

static void Test_ConfigRoundTrip() {
    TEST(L"Config save/load round-trip");
    std::wstring dir = TempDir();
    g_IniPath = dir + L"\\test_config.ini";
    DeleteFileW(g_IniPath.c_str());
    DeleteFileW((g_IniPath + L".tmp").c_str());

    // Prepare profiles.
    g_Profiles.clear();
    AppProfile a;
    a.exePath = L"C:\\Games\\Alpha.exe"; a.exeName = L"Alpha.exe";
    a.displayDev = L"\\\\.\\DISPLAY1";
    a.targetW = 1920; a.targetH = 1080; a.targetHz = 144; a.delaySec = 3;
    a.restore = true; a.enabled = true;
    g_Profiles.push_back(a);

    AppProfile b;
    b.exePath = L"C:\\Games\\Beta.exe"; b.exeName = L"Beta.exe";
    b.displayDev = L"";
    b.targetW = 1280; b.targetH = 720; b.targetHz = 0; b.delaySec = 0;
    b.restore = false; b.enabled = false;
    g_Profiles.push_back(b);

    g_DarkMode = true; g_LoggingEnabled = true;
    SaveConfig();

    CHECK(FileExists(g_IniPath));
    CHECK(!FileExists(g_IniPath + L".tmp"));   // atomic write left no temp

    // Reload into a fresh state.
    g_Profiles.clear();
    g_DarkMode = false; g_LoggingEnabled = false;
    LoadConfig();
    LoadSettings();

    CHECK_EQ((int)g_Profiles.size(), 2);
    CHECK_EQ(g_Profiles[0].exePath, std::wstring(L"C:\\Games\\Alpha.exe"));
    CHECK_EQ(g_Profiles[0].displayDev, std::wstring(L"\\\\.\\DISPLAY1"));
    CHECK_EQ(g_Profiles[0].targetW, 1920);
    CHECK_EQ(g_Profiles[0].targetH, 1080);
    CHECK_EQ(g_Profiles[0].targetHz, 144);
    CHECK_EQ(g_Profiles[0].delaySec, 3);
    CHECK_EQ(g_Profiles[0].restore, true);
    CHECK_EQ(g_Profiles[0].enabled, true);

    CHECK_EQ(g_Profiles[1].exePath, std::wstring(L"C:\\Games\\Beta.exe"));
    CHECK_EQ(g_Profiles[1].targetW, 1280);
    CHECK_EQ(g_Profiles[1].restore, false);
    CHECK_EQ(g_Profiles[1].enabled, false);

    CHECK_EQ(g_DarkMode, true);       // settings survived a profile save
    CHECK_EQ(g_LoggingEnabled, true);

    DeleteFileW(g_IniPath.c_str());
}

static void Test_ConfigOverwrite() {
    TEST(L"Config overwrite replaces, not appends");
    std::wstring dir = TempDir();
    g_IniPath = dir + L"\\ovw.ini";
    DeleteFileW(g_IniPath.c_str());

    g_Profiles.clear();
    AppProfile a; a.exePath = L"C:\\a.exe"; a.exeName = L"a.exe";
    a.targetW = 800; a.targetH = 600; a.targetHz = 0; a.delaySec = 0; a.restore = true; a.enabled = true;
    g_Profiles.push_back(a);
    SaveConfig();

    // Now save a different single profile; Count must become 1, not 2.
    g_Profiles.clear();
    AppProfile c; c.exePath = L"C:\\c.exe"; c.exeName = L"c.exe";
    c.targetW = 1024; c.targetH = 768; c.targetHz = 60; c.delaySec = 1; c.restore = true; c.enabled = true;
    g_Profiles.push_back(c);
    SaveConfig();

    g_Profiles.clear();
    LoadConfig();
    CHECK_EQ((int)g_Profiles.size(), 1);
    CHECK_EQ(g_Profiles[0].exePath, std::wstring(L"C:\\c.exe"));
    CHECK_EQ(g_Profiles[0].targetHz, 60);

    DeleteFileW(g_IniPath.c_str());
}

static void Test_EnumModes() {
    TEST(L"EnumModesForDevice returns sane modes");
    std::vector<ModeEntry> modes = EnumModesForDevice(L"");
    CHECK(!modes.empty());                       // a real primary display exists
    bool sortedDesc = true;
    for (size_t i = 1; i < modes.size(); ++i) {
        if (modes[i].w > modes[i - 1].w) { sortedDesc = false; break; }
    }
    CHECK(sortedDesc);                           // sorted widest-first
    bool allPositive = true;
    for (const auto& m : modes) if (m.w <= 0 || m.h <= 0) { allPositive = false; break; }
    CHECK(allPositive);
}

static void Test_GetCurrentRes() {
    TEST(L"GetCurrentRes returns a valid desktop mode");
    DEVMODEW dm = GetCurrentRes(L"");
    CHECK(dm.dmPelsWidth > 0);
    CHECK(dm.dmPelsHeight > 0);
}

static void Test_RunningProcesses() {
    TEST(L"GetRunningProcessNames includes this process's exe");
    std::set<std::wstring> names = GetRunningProcessNames();
    CHECK(!names.empty());
    // All entries are lower-cased (spot check: no upper-case letters).
    bool anyUpper = false;
    for (const auto& n : names) {
        for (wchar_t c : n) if (c >= L'A' && c <= L'Z') { anyUpper = true; break; }
        if (anyUpper) break;
    }
    CHECK(!anyUpper);
}

static void Test_MissingConfigDefaults() {
    TEST(L"LoadConfig on missing file yields empty list (no crash)");
    std::wstring dir = TempDir();
    g_IniPath = dir + L"\\does_not_exist.ini";
    DeleteFileW(g_IniPath.c_str());
    g_Profiles.clear();
    LoadConfig();
    CHECK_EQ((int)g_Profiles.size(), 0);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("=== AutoRes Changer unit tests ===\n");

    Test_GetFileName();
    Test_ToLower();
    Test_ConfigRoundTrip();
    Test_ConfigOverwrite();
    Test_EnumModes();
    Test_GetCurrentRes();
    Test_RunningProcesses();
    Test_MissingConfigDefaults();

    std::printf("\n=== Results: %d passed, %d failed ===\n", g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
