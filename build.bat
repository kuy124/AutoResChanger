@echo off
REM ============================================================
REM  build.bat - double-click to build AutoResChanger.exe
REM
REM  Just run this file. It finds the compiler, builds the exe,
REM  and leaves AutoResChanger.exe next to this script.
REM ============================================================
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM Put common MinGW-w64 locations on PATH if g++ isn't already there.
where g++ >nul 2>nul
if errorlevel 1 (
    for %%D in (
        "C:\msys64\ucrt64\bin"
        "C:\msys64\mingw64\bin"
        "C:\mingw64\bin"
        "C:\mingw32\bin"
        "C:\ProgramData\mingw64\mingw64\bin"
    ) do (
        if exist "%%~D\g++.exe" set "PATH=%%~D;!PATH!"
    )
)

where g++ >nul 2>nul
if errorlevel 1 (
    echo.
    echo   ERROR: g++ not found.
    echo.
    echo   Install MSYS2 from https://www.msys2.org/ then run:
    echo       pacman -S mingw-w64-ucrt-x86_64-gcc
    echo.
    echo   Or add an existing MinGW-w64 bin folder to your PATH.
    echo.
    pause
    exit /b 1
)

echo Building resources...
windres resource.rc -O coff -o resource.res
if errorlevel 1 ( echo   windres failed. & pause & exit /b 1 )

echo Compiling AutoResChanger.exe...
g++ -O2 -s -Wall -Wextra -Wpedantic ^
    -Wno-unknown-pragmas -Wno-missing-field-initializers ^
    main.cpp resource.res -o AutoResChanger.exe ^
    -mwindows -municode ^
    -luser32 -lgdi32 -lshell32 -lcomdlg32 -ladvapi32 ^
    -ldwmapi -luxtheme -lole32 -lcomctl32
if errorlevel 1 ( echo   Compile failed. & pause & exit /b 1 )

del resource.res >nul 2>nul

echo.
echo   ========================================
echo     Build succeeded: AutoResChanger.exe
echo   ========================================
echo.
pause
exit /b 0
