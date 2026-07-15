@echo off
REM PlotEngine static-link build, no external MinGW runtime DLLs
REM Output plotengine.exe depends only on Windows system DLLs KERNEL32/msvcrt, distributable standalone
REM Requires g++ MinGW 11 plus static libgcc/libstdc++/winpthread in PATH
setlocal
cd /d %~dp0

echo === PlotEngine static build ===

where g++ >nul 2>nul
if errorlevel 1 (
    echo Error: g++ not found in PATH
    exit /b 1
)

if not exist ftxui_amalgamation.hpp (
    echo ftxui_amalgamation.hpp missing, regenerating...
    python tools\amalgamate_ftxui.py || exit /b 1
)

REM -static forces static link of libgcc/libstdc++, removing external MinGW DLL deps
REM FTXUI is inlined via amalgamation header; threads use Win32 (mingw_std_threads.hpp), no winpthread needed
g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE -static PlotEngine.cpp -o plotengine.exe
if errorlevel 1 (
    echo *** Build failed ***
    exit /b 1
)

echo === Build success: plotengine.exe static no external MinGW DLL ===
exit /b 0
