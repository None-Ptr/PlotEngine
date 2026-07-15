# PlotEngine

> 单文件 C++17 文字冒险游戏引擎，开箱即编译；静态构建后零外部依赖，可独立分发。

PlotEngine 是一款轻量级交互叙事引擎，内置 [FTXUI 4.1.1](https://arthursonzogni.github.io/FTXUI/)（MIT 协议），仅一个 `.cpp` 文件即可编译运行。创作者可用简单脚本指令编写带有富文本渲染、动态效果、多面板布局、存档读档的文字冒险游戏。

---

## 快速开始

```bash
# Windows (MinGW)
g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE PlotEngine.cpp -o plotengine.exe -lpthread
plotengine.exe demo.txt

# Linux/macOS
g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 PlotEngine.cpp -o plotengine -lpthread
./plotengine demo.txt
```

或用批处理（Windows）：
```cmd
build_single.bat     # 动态链接, 依赖 MinGW 运行时 DLL
build_static.bat     # 静态链接, 无外置 DLL, 可独立分发
```

> 编译器要求：**g++ 11+**（C++17）。`ftxui_amalgamation.hpp` 由 `tools/amalgamate_ftxui.py` 从 `FTXUI-4.1.1/` 生成；若缺失，构建脚本会自动重新生成。

---

## 测试

`test/PlotEngine_tests.cpp` 是一套**单元 + 集成测试套件**，通过 `#include "PlotEngine.cpp"`
把引擎内部 `static` 函数与全局状态拉入同一翻译单元，在不改动引擎源码的前提下做白盒测试。
覆盖工具函数、编解码、变量系统、字符串解析、表达式求值、富文本解析、标签查找、可见字符、
变量/流程/分支/输入/面板/存档 等核心业务逻辑，以及边界与异常流程（非法标签、畸形指令、空脚本、未知命令等）。

当前共 **25 个测试组**（单元 + 集成 + 端到端）。新增覆盖：

- **IF/ELSE 边缘**：块 IF 内含单行 IF 时 ELSE 分支不误跳过、单行 IF 内联 `ELSE`（`IF x==1 SAY A ELSE SAY B`）、5 层深层嵌套。
- **未覆盖命令**：`SAY+`(不换行)、`NARRATE`、`CLEARALL`、`CURSOR/GOTOXY`、`BORDER`、`LOG`、`WAIT`、`TYPESPEED`、`SHAKE`、`BLINK`、`EFFECT/FX`、`EXIT`、`REM/ //`、`HELP`。
- **SET 表达式 / RANDOM 默认变量 / ADD-SUB 表达式增量**。
- **USERSAVE+USERLOAD 恢复** 与 **RUN/CHAIN 切换剧本**。
- **边界扩展**：CHOICE 越界序号、INPUT 默认上限、孤立 ELSE。
- **evalExpr 连续运算**（左结合修正）、`joinRemain` 单元。

### 测试中发现并修复的引擎缺陷

1. **`evalExpr` 结合律错误**：`10 - 3 - 2` 旧实现得 `9`（右结合），已修正为左结合得 `5`；`10/3/2` 等同理。
2. **块 IF 内嵌单行 IF 时跳过深度错误**：块 IF 不成立时，内部单行 IF 会让跳过循环误增深度，导致 `ELSE` 分支（乃至后续语句）被整体跳过。已通过 `isBlockIfLine` 仅对块 IF 计数作用域深度修复。
3. **单行 IF 内联 ELSE 未解析**：`IF x==1 SAY A ELSE SAY B` 旧实现把整串 `ELSE SAY B` 当作 SAY 文本输出。已在 IF 处理器中拆分 `THEN`/`ELSE` 动作分别执行。

### 运行步骤

```bash
# Windows (MinGW) —— 自动切到仓库根目录, 以便找到 test_full.txt
test\build_tests.bat

# Linux / macOS
bash test/build_tests.sh

# 或手动编译 (必须在仓库根目录执行, 否则找不到 test_full.txt)
g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE test/PlotEngine_tests.cpp -o test/PlotEngine_tests.exe -lpthread
test/PlotEngine_tests.exe
```

### 验证标准

- 输出末行形如 `通过: N   失败: 0   测试组: M` 且退出码为 `0` 即表示全部通过。
- 若 `失败 > 0`，会逐条打印 `[FAIL] 文件:行号 表达式`，据此定位。
- 引擎自身也内置自测：`plotengine.exe --selftest demo.txt`（退出码 0 为通过）。

---

## 特性

| 特性 | 说明 |
|------|------|
| **单文件** | 所有逻辑 + FTXUI 4.1.1 合并为一个 `.cpp`，一行命令编译 |
| **富文本** | 颜色/加粗/下划线/反显/闪烁，`[red]` `[bg:blue]` `[b]` 等标签 |
| **变量系统** | 整数与字符串，脚本中 `$var` 自动替换 |
| **多面板** | 多个独立文字区域，各自维护光标与内容 |
| **动态效果** | 打字机、闪烁、震屏，可调节速度 |
| **完整指令** | `SAY`/`IF`/`CHOICE`/`INPUT`/`GOTO`/`CALL` 等 |
| **加密存档** | XOR + Base64，用户存档(F5) / 脚本存档(SAVE) 双模式 |
| **流程图覆盖** | F 键弹出脚本流程图，浏览剧本结构 |
| **调试模式** | F2 显示 IP、变量、事件 |

---

## 文件指南

| 文件 | 说明 |
|------|------|
| `PlotEngine.cpp` | 主程序（内联 FTXUI，直接编译） |
| `ftxui_amalgamation.hpp` | FTXUI 合并头（构建必需，由 `tools/amalgamate_ftxui.py` 生成） |
| `FTXUI-4.1.1/` | FTXUI 4.1.1 源码（供升级用） |
| `build_single.bat` | 动态链接构建（依赖 MinGW 运行时 DLL） |
| `build_static.bat` | 静态链接构建（无外置 DLL，可独立分发） |
| `plotengine.exe` | 编译产物（当前为 `build_static.bat` 生成的静态版） |
| `demo.txt` | 自检 / 快速体验示例剧本（`--selftest` 使用） |
| `example.txt` | 完整功能演示剧本（覆盖全部指令） |
| `test_expr.txt` / `test_full.txt` / `test_input.txt` / `test_load.txt` / `test_save.txt` | 手动回归测试剧本（可 `plotengine.exe test_full.txt` 运行） |
| `test/` | 单元 + 集成测试套件（`PlotEngine_tests.cpp` 等） |
| `tools/amalgamate_ftxui.py` | 从 FTXUI 源码生成合并头 |
| `README.md` | 本文档 |

---

## 脚本指令速查

> 别名用 `/` 分隔；`[arg]` 可选；未知命令自动当作 `SAY` 处理（裸文本行可直接写）。

### 基础输出

| 指令 | 说明 |
|------|------|
| `SAY <text>` / `S` / `PRINT` / `P` | 显示文本（打字机效果） |
| `SAY+ <text>` / `S+` | 即时显示（无打字机） |
| `NARRATE <text>` / `N` / `LN` | 灰色旁白 |
| `BORDER` / `B` | 面板顶部/底部加 `=` 分隔线 |
| `<裸文本行>` | 自动当作 `SAY` |

### 富文本标签

```
[b]       加粗
[u]       下划线
[i]       反显
[blink]   闪烁
[red]     前景色（black/red/green/yellow/blue/magenta/cyan/white/gray + Light）
[bg:blue] 背景色
[fg:cyan] 前景色（显式）
[reset] / [/] / [0]  重置样式
\[ \] \\  转义
```

示例：`SAY [b][fg:cyan]═══ 标题 ═══[/][/]`

### 变量

| 指令 | 说明 |
|------|------|
| `SET <name> <value>` | 设置变量（自动判断：数字/字符串/表达式） |
| `ADD <name> <n>` / `INC` | 整数加法 |
| `SUB <name> <n>` / `DEC` | 整数减法 |
| `RANDOM a b [var]` / `RAND` | 随机整数 `[a,b]`，存入 `random`（可选 `[var]`） |
| `$name` / `${name}` | 引用变量值；`${...}` 用于紧随字母数字 |
| `# comment` / `; comment` | 注释 |

### 流程控制

| 指令 | 说明 |
|------|------|
| `name:` / `:name:` / `:name` / `::name:` | 标签定义 |
| `IF <var> <op> <val> <action>` | 单行条件（一行完成） |
| `IF ... ELSE ... ENDIF` | 块条件，支持嵌套 |
| `GOTO <label>` / `JUMP` | 跳转到标签 |
| `CALL <label>` / `GOSUB` | 跳转（单层，无返回栈） |
| `EXIT` / `QUIT` / `END` | 退出引擎 |
| `REM` / `//` | 注释（整行跳过） |

示例：
```
SET hp = 100
IF hp > 50 SAY 你还很健康
IF choice == 1
  SAY 你选了第一个
ELSE
  SAY 你没选第一个
ENDIF
```

### 用户交互

| 指令 | 说明 |
|------|------|
| `CHOICE <opt1> <opt2> ...` / `MENU` / `CHOICES` | 选项菜单，结果写入 `choice` |
| `INPUT [var] [max]` / `ASK` | 用户输入到变量（默认 `input`，最大 64 字符） |
| `WAIT <ms>` / `SLEEP` / `PAUSE` | 暂停（默认 500ms） |

### 面板系统

| 指令 | 说明 |
|------|------|
| `PANEL <name>` / `USE` / `SWITCH` | 切换当前面板 |
| `PANELNEW <name>` / `PN` | 新建面板 |
| `HIDE <name>` / `SHOW <name>` | 隐藏/显示面板 |
| `CLEAR [name]` / `CLS` | 清空面板 |
| `CLEARALL` | 清空所有面板 |
| `LOG <text>` | 写入 `log` 面板（自动创建） |
| `CURSOR [col] [row]` / `GOTOXY` | 设置光标位置 |

> 默认只有 `main` 面板；`log` 面板在首次 `LOG` 时自动创建。

### 动态效果

| 指令 | 说明 |
|------|------|
| `TYPESPEED <ms>` / `TYPERATE` | 打字机速度（默认 12ms/字，0=即时）；空格/回车跳过 |
| `SHAKE <n> <ms>` | 震屏（强度 默认 2，持续 默认 500ms） |
| `BLINK <step> <ms>` | 闪烁（步长 默认 300ms，持续 默认 2000ms） |
| `BEEP` / `SOUND` | 终端响铃 |

### 存档与脚本切换

| 指令 | 说明 |
|------|------|
| `SAVE [file]` | 脚本存档（XOR+Base64），不记位置 |
| `LOAD [file]` | 脚本读档（恢复变量） |
| `USERSAVE [file]` / `F5` | 用户存档（记位置，供 USERLOAD 跳回） |
| `USERLOAD [file]` / `F9` | 用户读档（跳回上次存档点下一行） |
| `RUN <file>` / `CHAIN` | 切换脚本（清空状态） |
| `DEBUG` | 切换调试条 |
| `HELP` / `?` / `H` | 输出指令速查 |

---

## 快捷键

| 键 | 功能 |
|----|------|
| `Esc` / `Q` | 退出 |
| `F2` / `` ` `` | 切换调试条 |
| `F` | 流程图覆盖 |
| `F5` | 立即存档 |
| `F9` | 立即读档（跳回上次 F5 位置） |
| `1` - `9` | 选择选项 |
| `↑` / `↓` | 切换选项 / 流程图滚动 |
| `Enter` / `Return` | 确认 / 跳过打字机 |
| `Space` | 跳过打字机 |
| `Backspace` | 删除字符（输入模式） |

---

## 升级 FTXUI

```bash
python tools/amalgamate_ftxui.py   # 从 FTXUI-4.1.1/ 重新生成 ftxui_amalgamation.hpp
```

> 合并头生成后，`PlotEngine.cpp` 通过 `#include "ftxui_amalgamation.hpp"` 直接内联全部 FTXUI 代码，无需单独编译 FTXUI 源码或额外链接库。

---

## 许可

FTXUI 遵循 MIT License（Copyright (c) 2019 Arthur Sonzogni，完整协议见 `FTXUI-4.1.1/LICENSE`）。
