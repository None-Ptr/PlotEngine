# PlotEngine

PlotEngine 是一款轻量级交互叙事引擎，基于 [FTXUI 4.1.1](https://arthursonzogni.github.io/FTXUI/) 开发，仅一个 `.cpp` 文件即可编译运行。可用类似汇编的简单脚本指令编写带有富文本渲染、动态效果、多面板布局、存档读档的文字冒险游戏。

## 关于 `PlotEngine_release.cpp`：

FTXUI 全项目基于 `c++ 17` 开发，所以本项目也用需要 `c++ 17` 以上版本编译。但为了在机房编译运行与传播，本项目提供了 `c++ 11` 版本的 `PlotEngine_release.cpp` 文件，同时为了防止有些版本的 `gcc` 不兼容线程，所以编写了 `cxx11_compat.hpp` 和 `mingw_std_threads.hpp`。

---
## 关于编译

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

---
## 本仓库文件说明

| 文件 | 说明 |
|------|------|
| `PlotEngine.cpp` | 主程序（内联 FTXUI，直接编译） |
| `ftxui_amalgamation.hpp` | FTXUI 合并头（构建必需，由 `tools/amalgamate_ftxui.py` 生成） |
| `FTXUI` | FTXUI 4.1.1 源码 |
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

## 关于脚本指令

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

## 关于快捷键

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