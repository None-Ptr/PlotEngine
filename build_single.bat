@echo off
REM PlotEngine single-file build (FTXUI bundled via ftxui_amalgamation.hpp)
REM Requires: g++ (MinGW 11+) in PATH
setlocal
cd /d %~dp0

echo === PlotEngine single-file build ===

where g++ >nul 2>nul
if errorlevel 1 (
    echo Error: g++ not found in PATH
    exit /b 1
)

if not exist ftxui_amalgamation.hpp (
    echo ftxui_amalgamation.hpp missing, regenerating...
    python tools\amalgamate_ftxui.py || exit /b 1
)

g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE PlotEngine.cpp -o plotengine.exe
if errorlevel 1 (
    echo *** Build failed ***
    exit /b 1
)

echo === Build success: plotengine.exe ===
exit /b 0
