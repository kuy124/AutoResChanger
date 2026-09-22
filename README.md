<div align="center">
  <h1>AutoRes Changer</h1>
  <p>
    <b>A lightweight utility designed to automatically manage display resolutions and refresh rates for specific applications on Windows.</b>
  </p>
</div>

When launching older games, classic emulators, or modern competitive shooters, you often want your monitor to temporarily switch to a specific resolution or refresh rate. AutoRes Changer handles this process automatically in the background. It instantly detects when your configured program starts, changes the display settings, and safely restores your desktop to its original state the moment you close the program.

To ensure the lowest possible resource footprint on your system, AutoRes Changer is built entirely with native C++ and Win32 APIs. It does not run on bloated background frameworks, consuming less than 1 MB of RAM while resting silently in your system tray.

<hr>

## Quick Setup Guide

Follow these steps to set up or compile AutoRes Changer on your computer.

### Option A: Download Pre-compiled Packages
You do not need to compile the code manually. You can download the standalone executable immediately:
1. Go to the **Release** section on this repository page.
2. Download the latest `AutoResChanger.exe` file.
3. Place it anywhere on your computer and run it. No installation is required.

### Option B: Compile from Source
If you prefer to compile the application yourself using a lightweight compiler like MSYS2/MinGW-w64:

#### Step 1: Prepare the Files
1. Create a folder on your computer named `AutoResChanger`.
2. Save your C++ source code as `main.cpp` inside this folder.
3. Save your resource script as `resource.rc` inside this folder.
4. Place any custom icon file you want to use inside the folder and name it `app.ico`.

#### Step 2: Compile the Program
Pick whichever build method you prefer.

**Easiest: double-click `build.bat`.** It auto-detects the MinGW toolchain, compiles the resource and the program, and leaves `AutoResChanger.exe` next to the script — no terminal needed. If the compiler isn't found it prints exactly what to install.

**PowerShell script.** `build.ps1` does the same with a few extra options:
```powershell
.\build.ps1              # release build
.\build.ps1 -Debug       # debug build with symbols
.\build.ps1 -Clean       # remove build artifacts first
```

**CMake.** A `CMakeLists.txt` is provided for both MinGW-w64 and MSVC:
```bash
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build
ctest --test-dir build --output-on-failure   # run unit tests
```

**Manual (MinGW-w64).** Run the resource compiler, then the C++ compiler. Note that `-municode` and linking `resource.res` are required so the Unicode resource APIs resolve correctly:
```bash
windres resource.rc -O coff -o resource.res
g++ -O2 -Wall -Wextra main.cpp resource.res -o AutoResChanger.exe -mwindows -municode -luser32 -lgdi32 -lshell32 -lcomdlg32 -ladvapi32 -ldwmapi -luxtheme -lole32 -lcomctl32
```

A GitHub Actions workflow (`.github/workflows/build.yml`) automatically builds the executable and runs both the unit and GUI tests on every push, then attaches the binary to a release whenever you push a `v*` tag.

---

## Testing

AutoRes Changer ships with two layers of automated tests:

* **Headless unit tests** (`tests/test_main.cpp`) cover the pure logic: path parsing, config save/load round-trips, atomic-write cleanliness, mode enumeration, and process detection. They compile with `-DTESTING`, which excludes the GUI. Run them via `ctest` (see above) or:
  ```powershell
  .\tests\run_tests.ps1
  ```
* **GUI end-to-end tests** (`tests/gui_save_test.ps1`) drive the real window through Win32 messages to prove the full Save/Delete/validation flow writes the correct `config.ini` with no leftover temp files:
  ```powershell
  .\tests\run_tests.ps1 -Gui
  ```

<hr>

## How to Use

Managing your custom application profiles is straightforward and handled entirely through a clean, resizable graphical interface.

### Creating a Profile
1. Launch `AutoResChanger.exe`.
2. Click the **`...`** browse button next to the **Executable Path** field and select the `.exe` file of the game or program you want to configure. You can also simply **drag and drop** an `.exe` from Explorer onto the window.
3. Select which screen you want to modify from the **Target Display** drop-down list (supports multi-monitor setups).
4. Pick a resolution in one of two ways:
   * **Supported Modes** — a live list read directly from the selected monitor, so every entry is guaranteed to be supported.
   * **Resolution Template / Presets** — a curated list of common resolutions, or type custom dimensions manually into the **Width**, **Height**, and **Hz** input fields.
5. Click **Save Profile**. Your new configuration will appear in the left-hand profile list (with the target app's own icon).

### Testing Modes Safely
If you want to verify whether a custom resolution or refresh rate is supported by your monitor before saving it:
1. Input your target dimensions or select an existing profile.
2. Click the **Test Display Settings** button. The button becomes **Keep This Mode** and a countdown starts.
3. Your screen temporarily transitions to the selected mode.
4. Click **Keep This Mode** to confirm, or simply do nothing — after 15 seconds the utility automatically reverts to your original desktop settings. This guarantees you can always recover even if the tested mode renders your display unreadable.

> Validation feedback (missing path, invalid resolution, unsupported mode) is shown in a non-blocking status line beneath the buttons rather than a pop-up dialog, so the interface never traps you.

---

## Understanding Key Features

AutoRes Changer includes several features to accommodate complex setups and game behaviors:

* <span style="color:#2980b9"><b>Multi-Monitor Routing:</b></span> Instead of changing settings globally, you can assign target resolutions to specific displays. The utility reads your active hardware configuration to target individual monitors cleanly.
* <span style="color:#16a085"><b>Live Supported Modes:</b></span> The **Supported Modes** drop-down is populated on the fly from your selected monitor via `EnumDisplaySettings`, so you can see exactly which resolutions and refresh rates your hardware actually offers before saving.
* <span style="color:#27ae60"><b>Startup Delay (Grace Period):</b></span> Some games load an initial splash screen or configuration launcher before launching the actual game window. Setting a **Delay** (in seconds) tells the utility to wait until the primary game window is fully loaded before executing the resolution override.
* <span style="color:#8e44ad"><b>Input Validation:</b></span> Width and Height are validated before a profile is saved or tested, preventing accidental zero-sized or malformed modes.
* <span style="color:#c0392b"><b>Single Instance:</b></span> Only one copy of AutoRes Changer can run at a time, so launching it twice never leaves duplicate tray icons or competing monitors fighting over your display settings.
* <span style="color:#d35400"><b>Auto-Start on Boot:</b></span> Checking **Start automatically with Windows** registers the application in your local user workspace. Upon system boot, it launches silently in the background and rests minimized in your system tray without interrupting you.
* <span style="color:#2c3e50"><b>Dark Mode &amp; Diagnostic Logging:</b></span> Toggle a dark UI theme and an optional diagnostic log (written to `%APPDATA%\AutoResChanger\log.txt`) that records each detection, mode change, and failure — handy when troubleshooting a stubborn game.
* <span style="color:#7f8c8d"><b>Per-Monitor DPI Aware:</b></span> Uses the modern per-monitor-v2 DPI context so the interface stays crisp on mixed-DPI and high-refresh setups.

---

## Background Behavior & Reversion Safety

* **Minimizing to Tray:** Closing the configuration window via the standard close button does not exit the utility. It hides the interface to the system tray so that monitoring remains active. To restore the window, simply double-click the system tray icon near your clock.
* **Tray Quick Actions:** Right-clicking the tray icon offers **Open window**, **Restore resolution now** (instantly reverts if a profile change is currently active, enabled only when one is), **Open config folder**, and **Exit AutoRes Changer**.
* **Guaranteed Restoration:** If a profile is active (i.e. your resolution is currently changed) and you exit the utility — or log off / shut down Windows — the original desktop resolution is restored automatically first. You will never be left stuck at a game resolution after closing AutoRes Changer.
* **Emergency Reversion:** The display modifications are applied using standard Windows dynamic sessions (`CDS_FULLSCREEN`). This design choice means that your custom resolutions are not permanently written to your Windows registry. If a game crashes or your system restarts unexpectedly, Windows will natively restore your default desktop resolution automatically.

---

## Maintenance & Removal

### Managing Configurations
Your application profiles are saved cleanly in a standard configuration file on your system.
* You can find your saved settings at: `%APPDATA%\AutoResChanger\config.ini`
* An optional diagnostic log (when **Enable diagnostic logging** is checked) is written to `%APPDATA%\AutoResChanger\log.txt`.
* To clear all profiles or start fresh, you can simply delete the `config.ini` file or the parent `AutoResChanger` directory inside your AppData folder.

### Complete Removal
Because the utility is entirely portable, uninstallation is clean:
1. Open the program interface and uncheck the **Start automatically with Windows** option to remove the startup registration from your registry.
2. Right-click the system tray icon and select **Exit AutoRes Changer** to stop the background monitor.
3. Delete the folder containing `AutoResChanger.exe`. No leftover application files or secondary system configurations will remain on your computer.

<hr>

<details>
  <summary><b>License</b> <i>(Click to expand)</i></summary>
  <br>
  <p>This project is open-source and distributed under the <strong>MIT License</strong>.</p>
</details>