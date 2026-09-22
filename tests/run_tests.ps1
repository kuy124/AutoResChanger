# run_tests.ps1 - Build and run the AutoRes Changer unit + GUI tests.
#
# Usage:
#   .\tests\run_tests.ps1            # unit tests only
#   .\tests\run_tests.ps1 -Gui       # unit tests + GUI end-to-end tests
#
# Exit code is non-zero if any test fails. Suitable for CI.

param([switch]$Gui)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location -LiteralPath $root

# Ensure MinGW toolchain is reachable.
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    foreach ($p in @("C:\msys64\ucrt64\bin", "C:\msys64\mingw64\bin")) {
        if (Test-Path (Join-Path $p "g++.exe")) { $env:PATH = "$p;$env:PATH"; break }
    }
}
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) { throw "g++ not found." }

$libs = @("-luser32","-lgdi32","-lshell32","-lcomdlg32","-ladvapi32","-ldwmapi","-luxtheme","-lole32","-lcomctl32")

Write-Host "== Building unit test runner ==" -ForegroundColor Cyan
& g++ -O2 -std=c++17 -Wall -Wextra -Wno-unknown-pragmas -Wno-missing-field-initializers `
    tests/test_main.cpp -o tests/test_runner.exe @libs
if ($LASTEXITCODE -ne 0) { throw "Unit test build failed." }

Write-Host "== Running unit tests ==" -ForegroundColor Cyan
& .\tests\test_runner.exe
$unitExit = $LASTEXITCODE

$guiExit = 0
if ($Gui) {
    # The GUI test needs the built app.
    if (-not (Test-Path ".\AutoResChanger.exe")) {
        Write-Host "== Building AutoResChanger.exe for GUI test ==" -ForegroundColor Cyan
        & windres resource.rc -O coff -o resource.res
        & g++ -O2 -s main.cpp resource.res -o AutoResChanger.exe -mwindows -municode @libs
    }
    Write-Host "== Running GUI end-to-end tests ==" -ForegroundColor Cyan
    & powershell -ExecutionPolicy Bypass -File .\tests\gui_save_test.ps1
    $guiExit = $LASTEXITCODE
}

if ($unitExit -ne 0 -or $guiExit -ne 0) {
    Write-Host "TESTS FAILED (unit=$unitExit gui=$guiExit)" -ForegroundColor Red
    exit 1
}
Write-Host "ALL TESTS PASSED" -ForegroundColor Green
exit 0
