# gui_save_test.ps1 - End-to-end GUI test for AutoRes Changer.
#
# Drives the real window via Win32 messages (no interactive input) to verify the
# full Save path: set the executable path, click "Save Profile", and assert the
# resolved config.ini is written atomically with the expected profile.
#
# Requires the app to be built at .\AutoResChanger.exe (repo root).
# Exits 0 on success, 1 on any failure. Designed to run in CI.

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath (Join-Path $PSScriptRoot "..")

$exePath = Join-Path (Get-Location) "AutoResChanger.exe"
if (-not (Test-Path $exePath)) { Write-Error "AutoResChanger.exe not found. Build first."; exit 1 }

$cfgDir = Join-Path $env:APPDATA "AutoResChanger"
$cfg    = Join-Path $cfgDir "config.ini"
$tmp    = "$cfg.tmp"

# --- Win32 interop (single type definition reused by all tests) ---
Add-Type -TypeDefinition @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class Gui {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string win);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  // Overload: SendMessage with a marshaled string argument (used for WM_SETTEXT).
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageStr(IntPtr h, uint msg, IntPtr wp, string lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder sb, int max);
  [DllImport("user32.dll")] public static extern bool EnumChildWindows(IntPtr parent, EnumProc cb, IntPtr lp);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h, StringBuilder sb, int max);
  public delegate bool EnumProc(IntPtr h, IntPtr lp);

  public const uint BM_CLICK = 0x00F5;
  public const uint LB_GETCOUNT = 0x018B;
  public const uint WM_SETTEXT = 0x000C;

  // IMPORTANT: use WM_SETTEXT (marshaled by Windows) rather than SetWindowText.
  // SetWindowText across processes only updates the cached caption; the owning
  // process reading via WM_GETTEXT would still see the old (empty) buffer.
  public static void SetText(IntPtr h, string t) { SendMessageStr(h, WM_SETTEXT, IntPtr.Zero, t); }

  public static IntPtr ListBox = IntPtr.Zero;
  public static bool GrabList(IntPtr h, IntPtr lp) {
    var sb = new StringBuilder(32); GetClassName(h, sb, 32);
    if (sb.ToString() == "ListBox") ListBox = h;
    return true;
  }
  public static string Text(IntPtr h) { var sb = new StringBuilder(512); GetWindowText(h, sb, 512); return sb.ToString(); }
}
"@

function Get-ListCount([IntPtr]$h) { [int][Gui]::SendMessage($h, [Gui]::LB_GETCOUNT, [IntPtr]::Zero, [IntPtr]::Zero) }

function Start-App {
  Remove-Item $cfg, $tmp -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $exePath -PassThru
  Start-Sleep -Seconds 3
  $h = [Gui]::FindWindow("AutoResChangerClass", "AutoRes Changer")
  if ($h -eq [IntPtr]::Zero) { throw "Main window not found." }
  return @{ Proc = $p; Hwnd = $h }
}

# Like Start-App but keeps the existing config.ini (used to seed a profile).
function Start-App2 {
  Remove-Item $tmp -ErrorAction SilentlyContinue
  $p = Start-Process -FilePath $exePath -PassThru
  Start-Sleep -Seconds 3
  $h = [Gui]::FindWindow("AutoResChangerClass", "AutoRes Changer")
  if ($h -eq [IntPtr]::Zero) { throw "Main window not found." }
  return @{ Proc = $p; Hwnd = $h }
}

function Stop-App($app) { if ($app -and -not $app.Proc.HasExited) { $app.Proc | Stop-Process -Force }; Start-Sleep -Milliseconds 600 }

$failures = 0
function Assert($cond, $name) {
  if ($cond) { Write-Host "  PASS: $name" -ForegroundColor Green }
  else { Write-Host "  FAIL: $name" -ForegroundColor Red; $script:failures++ }
}

Write-Host "== Test 1: Save with empty exe path shows inline warning, writes nothing ==" -ForegroundColor Cyan
$app = Start-App
try {
  [Gui]::EnumChildWindows($app.Hwnd, [Gui+EnumProc]{ param($h,$l) [Gui]::GrabList($h,$l) }, [IntPtr]::Zero) | Out-Null
  $save = [Gui]::GetDlgItem($app.Hwnd, 102)
  [Gui]::SendMessage($save, [Gui]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 700
  $status = [Gui]::Text([Gui]::GetDlgItem($app.Hwnd, 114))
  Assert ($status -match "executable path") "inline warning shown ('$status')"
  Assert (-not (Test-Path $cfg)) "no config written on invalid input"
} finally { Stop-App $app }

Write-Host "== Test 2: Valid Save writes config and adds a list row ==" -ForegroundColor Cyan
$app = Start-App
try {
  [Gui]::EnumChildWindows($app.Hwnd, [Gui+EnumProc]{ param($h,$l) [Gui]::GrabList($h,$l) }, [IntPtr]::Zero) | Out-Null
  $before = Get-ListCount([Gui]::ListBox)
  [Gui]::SetText([Gui]::GetDlgItem($app.Hwnd, 115), "C:\Windows\System32\notepad.exe")
  [Gui]::SetText([Gui]::GetDlgItem($app.Hwnd, 108), "1280")
  [Gui]::SetText([Gui]::GetDlgItem($app.Hwnd, 109), "720")
  $save = [Gui]::GetDlgItem($app.Hwnd, 102)
  [Gui]::SendMessage($save, [Gui]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 900
  $after = Get-ListCount([Gui]::ListBox)
  Assert ((Test-Path $cfg)) "config.ini created"
  Assert (-not (Test-Path $tmp)) "no leftover .tmp file"
  $content = if (Test-Path $cfg) { Get-Content $cfg -Raw } else { "" }
  Assert ($content -match "notepad") "config contains saved exe"
  Assert ($content -match "W=1280") "config contains width 1280"
  Assert ($content -match "H=720") "config contains height 720"
  Assert ($after -eq ($before + 1)) "list row added ($before -> $after)"
} finally { Stop-App $app }

Write-Host "== Test 3: Delete removes the profile ==" -ForegroundColor Cyan
# Seed a config with one profile so Delete has something to operate on.
New-Item -ItemType Directory -Force -Path $cfgDir | Out-Null
@"
[Settings]
Count=1
DarkMode=0
Logging=0
[Profile_0]
Exe=C:\Windows\System32\notepad.exe
Device=
W=1280
H=720
Hz=0
Delay=0
Restore=1
Enabled=1
"@ | Set-Content -Encoding ASCII $cfg
$app = Start-App2   # start without wiping our seeded config
try {
  [Gui]::EnumChildWindows($app.Hwnd, [Gui+EnumProc]{ param($h,$l) [Gui]::GrabList($h,$l) }, [IntPtr]::Zero) | Out-Null
  # Select the first list item, then click Delete.
  [Gui]::SendMessage([Gui]::ListBox, 0x0186, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null  # LB_SETCURSEL 0
  $del = [Gui]::GetDlgItem($app.Hwnd, 103)
  [Gui]::SendMessage($del, [Gui]::BM_CLICK, [IntPtr]::Zero, [IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 700
  $content = if (Test-Path $cfg) { Get-Content $cfg -Raw } else { "" }
  Assert ($content -notmatch "notepad") "profile removed from config"
  Assert ((Get-ListCount([Gui]::ListBox)) -eq 0) "list emptied"
} finally { Stop-App $app }

if ($failures -eq 0) { Write-Host "`nALL GUI TESTS PASSED" -ForegroundColor Green; exit 0 }
else { Write-Host "`n$failures GUI TEST(S) FAILED" -ForegroundColor Red; exit 1 }
