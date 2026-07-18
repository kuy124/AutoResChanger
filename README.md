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
1. Open your terminal in the folder where your files are located.
2. Run the resource compiler to prepare your custom icon:
   ```bash
   windres resource.rc -O coff -o resource.res
   ```
3. Run the C++ compiler to build the final executable:
   ```bash
   g++ -O2 main.cpp resource.res -o AutoResChanger.exe -mwindows -municode -luser32 -lgdi32 -lshell32 -lcomdlg32 -ladvapi32
   ```

<hr>

## How to Use

Managing your custom application profiles is straightforward and handled entirely through a clean, resizable graphical interface.

### Creating a Profile
1. Launch `AutoResChanger.exe`.
2. Click the **`...`** browse button next to the **Executable Path** field and select the `.exe` file of the game or program you want to configure.
3. Select which screen you want to modify from the **Target Display** drop-down list (supports multi-monitor setups).
4. Choose a pre-defined layout from the **Resolution Template / Presets** drop-down menu, or type custom dimensions manually into the **Width**, **Height**, and **Hz** input fields.
5. Click **Save Profile**. Your new configuration will appear in the left-hand profile list.

### Testing Modes Safely
If you want to verify whether a custom resolution or refresh rate is supported by your monitor before saving it:
1. Input your target dimensions or select an existing profile.
2. Click the **Test Display Settings** button.
3. Your screen will temporarily transition to the selected mode. 
4. A prompt will appear on your screen. Clicking **OK** or waiting will safely revert your monitor to its original desktop settings.

---

## Understanding Key Features

AutoRes Changer includes several features to accommodate complex setups and game behaviors:

* <span style="color:#2980b9"><b>Multi-Monitor Routing:</b></span> Instead of changing settings globally, you can assign target resolutions to specific displays. The utility reads your active hardware configuration to target individual monitors cleanly.
* <span style="color:#27ae60"><b>Startup Delay (Grace Period):</b></span> Some games load an initial splash screen or configuration launcher before launching the actual game window. Setting a **Delay** (in seconds) tells the utility to wait until the primary game window is fully loaded before executing the resolution override.
* <span style="color:#d35400"><b>Auto-Start on Boot:</b></span> Checking **Start automatically with Windows** registers the application in your local user workspace. Upon system boot, it launches silently in the background and rests minimized in your system tray without interrupting you.

---

## Background Behavior & Reversion Safety

* **Minimizing to Tray:** Closing the configuration window via the standard close button does not exit the utility. It hides the interface to the system tray so that monitoring remains active. To restore the window, simply double-click the system tray icon near your clock.
* **Emergency Reversion:** The display modifications are applied using standard Windows dynamic sessions (`CDS_FULLSCREEN`). This design choice means that your custom resolutions are not permanently written to your Windows registry. If a game crashes or your system restarts unexpectedly, Windows will natively restore your default desktop resolution automatically.

---

## Maintenance & Removal

### Managing Configurations
Your application profiles are saved cleanly in a standard configuration file on your system.
* You can find your saved settings at: `%APPDATA%\AutoResChanger\config.ini`
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