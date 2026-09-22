# build.ps1 - one-shot build script for AutoRes Changer (MinGW-w64 / MSYS2).
#
# Usage:
#   .\build.ps1              # release build
#   .\build.ps1 -Debug       # debug build with symbols
#   .\build.ps1 -Clean       # remove build artifacts first
#
# Requires g++ and windres on PATH (e.g. MSYS2 UCRT64: C:\msys64\ucrt64\bin).

param(
    [switch]$Debug,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
Set-Location -LiteralPath $PSScriptRoot

if ($Clean) {
    Remove-Item -Force -ErrorAction SilentlyContinue `
        AutoResChanger.exe, resource.res, baseline.exe
    Write-Host "Cleaned build artifacts." -ForegroundColor Yellow
}

# Make sure the MinGW toolchain is reachable even if it's not on PATH.
if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    foreach ($p in @("C:\msys64\ucrt64\bin", "C:\msys64\mingw64\bin")) {
        if (Test-Path (Join-Path $p "g++.exe")) {
            $env:PATH = "$p;$env:PATH"
            break
        }
    }
}

if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
    throw "g++ not found. Install MSYS2 (UCRT64) or add MinGW-w64 to PATH."
}

$cxxflags = if ($Debug) {
    @("-O0", "-g")
} else {
    @("-O2", "-s")
}
$warn = @("-Wall", "-Wextra", "-Wpedantic", "-Wno-unknown-pragmas", "-Wno-missing-field-initializers")
$libs = @("-luser32", "-lgdi32", "-lshell32", "-lcomdlg32", "-ladvapi32", "-ldwmapi", "-luxtheme", "-lole32", "-lcomctl32")

Write-Host "Compiling resources..." -ForegroundColor Cyan
& windres resource.rc -O coff -o resource.res
if ($LASTEXITCODE -ne 0) { throw "windres failed." }

Write-Host "Compiling AutoResChanger.exe..." -ForegroundColor Cyan
& g++ @cxxflags @warn main.cpp resource.res -o AutoResChanger.exe -mwindows -municode @libs
if ($LASTEXITCODE -ne 0) { throw "g++ failed." }

$size = [math]::Round((Get-Item AutoResChanger.exe).Length / 1KB)
Write-Host "Build succeeded: AutoResChanger.exe ($size KB)" -ForegroundColor Green
