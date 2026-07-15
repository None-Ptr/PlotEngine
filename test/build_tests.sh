#!/usr/bin/env bash
# PlotEngine 测试套件构建 + 运行 (Linux / macOS)
set -e
cd "$(dirname "$0")/.."

echo "=== 构建测试套件 ==="
g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 test/PlotEngine_tests.cpp -o test/PlotEngine_tests -lpthread

echo "=== 运行测试 (在仓库根目录, 以便找到 test_full.txt) ==="
./test/PlotEngine_tests
rc=$?
echo "=== 退出码: $rc ==="
exit $rc
