@echo off
REM PlotEngine 测试套件构建 + 运行 (Windows / MinGW)
setlocal
cd /d %~dp0\..

where g++ >nul 2>nul
if errorlevel 1 (
    echo Error: g++ not found in PATH
    exit /b 1
)

echo === 构建测试套件 ===
g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE test/PlotEngine_tests.cpp -o test/PlotEngine_tests.exe -lpthread
if errorlevel 1 (
    echo *** 构建失败 ***
    exit /b 1
)

echo === 运行测试 (需在仓库根目录, 以便找到 test_full.txt) ===
test\PlotEngine_tests.exe
set rc=%errorlevel%
echo === 退出码: %rc% ===
exit /b %rc%
