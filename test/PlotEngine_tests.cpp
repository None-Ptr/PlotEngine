// =====================================================================
// PlotEngine_tests.cpp - 单元 + 集成测试套件 (针对 PlotEngine.cpp)
//
// 设计: 通过 #include "PlotEngine.cpp" 把引擎所有 static 函数与全局状态
//       (S / VARS / g_script / g_ip) 拉进同一个翻译单元, 从而在不修改引擎
//       源码的前提下直接对内部逻辑做白盒测试。引擎自身的 main 被重命名为
//       plotengine_main 以避免冲突。
//
// 编译: 见 build_tests.bat / build_tests.sh
// 运行: 从仓库根目录执行 test/PlotEngine_tests(.exe)
//       (需能找到 test_full.txt 等脚本, 故在仓库根目录运行)
// =====================================================================

// ---- 1. 重命名引擎 main, 然后包含引擎源码 ----
#define main plotengine_main
#include "../PlotEngine.cpp"
#undef main

#include <iostream>
#include <sstream>
#include <algorithm>

// ============================================================
// 极简测试框架 (无第三方依赖)
// ============================================================
static int g_pass = 0;
static int g_fail = 0;
static int g_groups = 0;

template <typename T>
static std::string dbgVal(const T& v) {
    std::ostringstream os; os << v; return os.str();
}
template <>
std::string dbgVal(const std::string& v) { return "\"" + v + "\""; }
// ftxui::Color 不可流式输出, 提供特化仅用于失败信息
template <>
std::string dbgVal(const ftxui::Color&) { return "Color"; }

#define GROUP(name) do { g_groups++; std::cout << "\n=== " << (name) << " ===" << std::endl; } while(0)

#define CHECK(cond) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; std::cout << "  [FAIL] " << __FILE__ << ":" << __LINE__ \
        << "  " << #cond << std::endl; } \
} while(0)

#define CHECK_EQ(a, b) do { \
    auto _a = (a); auto _b = (b); \
    if (_a == _b) { g_pass++; } \
    else { g_fail++; std::cout << "  [FAIL] " << __FILE__ << ":" << __LINE__ \
        << "  " << #a << " == " << #b \
        << "  (got " << dbgVal(_a) << ", want " << dbgVal(_b) << ")" << std::endl; } \
} while(0)

static bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

// ============================================================
// 集成测试执行器: 模拟无头运行脚本
//   - CHOICE: 从 choices 队列取序号 (默认选 1)
//   - INPUT : 从 inputs  队列取字符串 (默认 "x")
//   - 自动刷完打字机效果, 处理 userLoad 的 loadResumeIp 跳转
// ============================================================
static void flushTW() {
    if (S.waitType && S.typeLine >= 0) {
        Panel* pp = &S.panels[S.typePanel.empty() ? S.curPanel : S.typePanel];
        if (pp && S.typeLine < (int)pp->lines.size())
            S.typePos = visibleCharCount(pp->lines[S.typeLine]);
    }
    S.waitType = false; S.typeLine = -1; S.sleepUntil = 0;
}

static void runScript(std::vector<int>& choices, std::vector<std::string>& inputs) {
    g_ip = 0; S.typeSpeed = 0; S.run = true; S.loadResumeIp = -1;
    int guard = 500000;
    while (g_ip < (int)g_script.size() && S.run && guard-- > 0) {
        if (S.loadResumeIp >= 0) { g_ip = S.loadResumeIp; S.loadResumeIp = -1; }
        if (S.waiting) {
            if (S.lastEvent == "CHOICE") {
                int c = choices.empty() ? 1 : choices.front();
                if (!choices.empty()) choices.erase(choices.begin());
                VARS.setI("choice", c);
                S.choices.clear(); S.waiting = false; S.lastEvent = "";
                flushTW(); continue;
            } else if (S.lastEvent == "INPUT") {
                std::string v = inputs.empty() ? std::string("x") : inputs.front();
                if (!inputs.empty()) inputs.erase(inputs.begin());
                VARS.setS(S.inputVar, v);
                S.inputBuf.clear(); S.inputPos = 0; S.waiting = false; S.lastEvent = "";
                flushTW(); continue;
            }
        }
        bool jumped = false;
        execCmd(g_script[g_ip], jumped);
        flushTW();
        if (!jumped) g_ip++;
    }
}

static void freshState() {
    resetGameState();
    S.run = true;
    S.curPanel = "main";
    S.panels["main"] = Panel();
    S.typeSpeed = 0;
}

static std::string mainText() {
    std::string o;
    for (auto& l : S.panels["main"].lines) o += l + "\n";
    return o;
}

// ============================================================
// 单元测试: 工具函数
// ============================================================
static void test_utils() {
    GROUP("Unit: 工具函数 (tr/low/up/splitTok/splitBy/stripQuote/itoa2)");

    CHECK_EQ(tr("   hi  "), "hi");
    CHECK_EQ(tr(""), "");
    CHECK_EQ(tr("\t\n x \r"), "x");

    CHECK_EQ(low("AbC"), "abc");
    CHECK_EQ(up("AbC"), "ABC");

    auto t1 = splitTok("a b c");
    CHECK_EQ(t1.size(), 3u);
    CHECK_EQ(t1[0], "a"); CHECK_EQ(t1[2], "c");

    auto t2 = splitTok("\"hello world\" x");
    CHECK_EQ(t2.size(), 2u);
    CHECK_EQ(t2[0], "\"hello world\"");
    CHECK_EQ(t2[1], "x");

    auto t3 = splitTok("say 'a b' c");
    CHECK_EQ(t3.size(), 3u);
    CHECK_EQ(t3[1], "'a b'");

    auto t4 = splitBy("a,b,c", ',');
    CHECK_EQ(t4.size(), 3u);
    CHECK_EQ(t4[0], "a"); CHECK_EQ(t4[2], "c");

    auto t5 = splitBy("solo", ',');
    CHECK_EQ(t5.size(), 1u);
    CHECK_EQ(t5[0], "solo");

    CHECK_EQ(stripQuote("\"abc\""), "abc");
    CHECK_EQ(stripQuote("'abc'"), "abc");
    CHECK_EQ(stripQuote("abc"), "abc");           // 无引号原样返回
    CHECK_EQ(stripQuote("\"ab"), "\"ab");         // 不成对, 原样返回

    CHECK_EQ(itoa2(0), "0");
    CHECK_EQ(itoa2(12345), "12345");
    CHECK_EQ(itoa2(-7), "-7");
    CHECK_EQ(atoiS("-42"), -42);
    CHECK_EQ(atoiS("100"), 100);
}

// ============================================================
// 单元测试: 编码 (hex / base64 / xor)
// ============================================================
static void test_codec() {
    GROUP("Unit: 编码 (toHex/fromHex, toBase64/fromBase64, xorCrypt)");

    // hex 往返
    std::vector<std::string> hexSamples = {"", "A", "AB", "abc", "Hello, World!", "\x00\x01\x02\xff"};
    for (auto& s : hexSamples) CHECK_EQ(fromHex(toHex(s)), s);

    // base64 往返 (覆盖 0/1/2/3 字节尾块)
    std::vector<std::string> b64Samples = {"", "f", "fo", "foo", "foob", "fooba", "foobar",
                                            "hello world", "A\0B\0C", "任意中文测试"};
    for (auto& s : b64Samples) CHECK_EQ(fromBase64(toBase64(s)), s);

    // xor 往返 + 密钥循环
    std::string key = "PLOTENGINE_SECRET_KEY_2024";
    std::vector<std::string> xorSamples = {"", "x", "secret data 12345", "longer than the key string!!"};
    for (auto& s : xorSamples) CHECK_EQ(xorCrypt(xorCrypt(s, key), key), s);

    // 非对称: xor 后不应等于原文 (非空时)
    CHECK(xorCrypt("plaintext", key) != "plaintext");
}

// ============================================================
// 单元测试: 变量系统 VarSystem
// ============================================================
static void test_varsystem() {
    GROUP("Unit: 变量系统 VarSystem");

    VarSystem v;
    v.setI("hp", 100);
    v.setS("name", "hero");
    CHECK_EQ(v.getI("hp"), 100);
    CHECK_EQ(v.getS("hp"), "100");
    CHECK_EQ(v.getS("name"), "hero");
    CHECK(v.has("hp"));
    CHECK(!v.has("missing"));

    // 字符串变量以数字形式读取 -> atoll
    v.setS("num", "42");
    CHECK_EQ(v.getI("num"), 42);

    // 整型变量以字符串形式读取
    v.setI("lv", 7);
    CHECK_EQ(v.getS("lv"), "7");

    // resolve: 多变量插值
    v.setI("a", 1); v.setI("b", 2); v.setI("c", 3);
    CHECK_EQ(v.resolve("[$a] [$b] [$c]"), "[1] [2] [3]");
    CHECK_EQ(v.resolve("$a,$b,$c"), "1,2,3");
    CHECK_EQ(v.resolve("no vars here"), "no vars here");

    // serialize / deserialize 往返
    v.setI("x", 99); v.setS("y", "zz");
    std::string ser = v.serialize();
    VarSystem v2;
    v2.deserialize(ser);
    CHECK_EQ(v2.getI("x"), 99);
    CHECK_EQ(v2.getS("y"), "zz");
    CHECK_EQ(v2.getI("hp"), 100);
}

// ============================================================
// 单元测试: resolveStr
// ============================================================
static void test_resolvestr() {
    GROUP("Unit: resolveStr (字符串解析/变量/引号)");

    VARS.vs.clear();
    VARS.setI("a", 1); VARS.setI("b", 2); VARS.setI("c", 3);

    CHECK_EQ(resolveStr("\"hello\""), "hello");          // 纯引号字面量
    CHECK_EQ(resolveStr("$a $b $c"), "1 2 3");           // $变量插值
    CHECK_EQ(resolveStr("a $b c"), "a 2 c");             // 无 $ 前缀的单词按字面量处理
    CHECK_EQ(resolveStr("$a+$b"), "1+2");
    CHECK_EQ(resolveStr("plain text"), "plain text");
}

// ============================================================
// 单元测试: evalExpr (表达式求值)
// ============================================================
static void test_evalexpr() {
    GROUP("Unit: evalExpr (算术/优先级/括号/变量/边界)");

    // 纯数字字面量
    CHECK_EQ(evalExpr("42"), 42);
    CHECK_EQ(evalExpr("-5"), -5);
    CHECK_EQ(evalExpr("  1 + 2  "), 3);                   // 空格处理

    // 运算符优先级
    CHECK_EQ(evalExpr("1+2*3"), 7);
    CHECK_EQ(evalExpr("(1+2)*3"), 9);
    CHECK_EQ(evalExpr("2*(3+4)"), 14);
    CHECK_EQ(evalExpr("2+3*4-1"), 13);
    CHECK_EQ(evalExpr("(2+3)*(4+1)"), 25);
    CHECK_EQ(evalExpr("((((1+1))))"), 2);                 // 深层括号
    CHECK_EQ(evalExpr("10/3"), 3);                        // 整数除法
    CHECK_EQ(evalExpr("10%3"), 1);                        // 取模

    // 变量参与
    VARS.vs.clear();
    VARS.setI("a", 10); VARS.setI("b", 3);
    CHECK_EQ(evalExpr("a+b"), 13);
    CHECK_EQ(evalExpr("a*b"), 30);
    CHECK_EQ(evalExpr("a/b"), 3);
    CHECK_EQ(evalExpr("a%b"), 1);
    CHECK_EQ(evalExpr("1+2*3"), 7);
    CHECK_EQ(evalExpr("(1+2)*3"), 9);
    CHECK_EQ(evalExpr("2*(a+b)"), 26);

    // 边界 / 异常输入
    CHECK_EQ(evalExpr(""), 0);                            // 空表达式
    CHECK_EQ(evalExpr("undefined_var"), 0);              // 未知变量 -> 0
    CHECK_EQ(evalExpr("10/0"), 0);                        // 除零 -> 0 (安全)
    CHECK_EQ(evalExpr("10%0"), 0);                        // 取模零 -> 0 (安全)
}

// ============================================================
// 单元测试: parseRich (富文本标签解析)
// ============================================================
static void test_rich() {
    GROUP("Unit: parseRich (富文本解析)");

    auto seg0 = parseRich("plain");
    CHECK_EQ(seg0.size(), 1u);
    CHECK(!seg0[0].second.b);

    auto seg1 = parseRich("[b]hi[/]");
    CHECK_EQ(seg1.size(), 1u);
    CHECK(seg1[0].second.b);
    CHECK_EQ(seg1[0].first, "hi");

    auto seg2 = parseRich("[fg:red]x[/]");
    CHECK_EQ(seg2[0].second.fg, Color::Red);

    auto seg3 = parseRich("[u]under[/]");
    CHECK(seg3[0].second.u);

    auto seg4 = parseRich("a\\[b\\]");                    // 转义: 字面 [b]
    CHECK_EQ(seg4.size(), 1u);
    CHECK_EQ(seg4[0].first, "a[b]");

    auto seg5 = parseRich("before [i]mid[/] after");
    CHECK_EQ(seg5.size(), 3u);
    CHECK(seg5[1].second.i);
    CHECK_EQ(seg5[0].first, "before ");
    CHECK_EQ(seg5[2].first, " after");
}

// ============================================================
// 单元测试: findLabelIdx (标签定位, 4 种格式)
// ============================================================
static void test_labels() {
    GROUP("Unit: findLabelIdx (标签格式)");

    g_script = {":lab1", "SAY a", ":lab2:", "SAY b", "::lab3", "SAY c", "lab4:", "SAY d"};
    g_ip = 0;
    CHECK_EQ(findLabelIdx("lab1", 0), 0);
    CHECK_EQ(findLabelIdx("lab2", 0), 2);
    CHECK_EQ(findLabelIdx("lab3", 0), 4);
    CHECK_EQ(findLabelIdx("lab4", 0), 6);

    // 从指定位置向后查找
    CHECK_EQ(findLabelIdx("lab4", 3), 6);
    CHECK_EQ(findLabelIdx("lab1", 3), -1);                // 只向后找 -> 找不到
}

// ============================================================
// 单元测试: visibleCharCount / visibleSubstr (打字机截取)
// ============================================================
static void test_visible() {
    GROUP("Unit: visibleCharCount / visibleSubstr");

    CHECK_EQ(visibleCharCount("abc"), 3);
    CHECK_EQ(visibleCharCount("[b]x[/]"), 1);            // 标签不计可见字符
    CHECK_EQ(visibleCharCount("a\\[b\\]"), 4);           // 转义计入

    CHECK_EQ(visibleSubstr("abcdef", 3), "abc");
    CHECK_EQ(visibleSubstr("[b]hello[/]", 5), "[b]hello"); // 标签整体保留, 截 5 可见字符
    CHECK_EQ(visibleSubstr("abc", 100), "abc");          // 超过长度
}

// ============================================================
// 集成测试: SET / ADD / SUB / INC / DEC / RANDOM
// ============================================================
static void test_vars_flow() {
    GROUP("Integration: SET/ADD/SUB/INC/DEC/RANDOM");

    freshState();
    std::vector<std::string> scr = {
        "SET hp = 100",
        "SET name = 英雄",
        "SET strlit = \"hello world\"",
        "SET neg = -5",
        "SET empty_str = \"\"",
        "ADD hp 50",
        "SUB hp 30",
        "SET z = 0",
        "INC z 5",
        "DEC z 2",
        "RANDOM 1 100 r1",
        "SAY hp=[$hp] name=[$name] str=[$strlit] neg=[$neg] empty=[$empty_str]",
        "SAY z=$z"
    };
    g_script = scr;
    std::vector<int> ch; std::vector<std::string> inp;
    runScript(ch, inp);

    CHECK_EQ(VARS.getI("hp"), 120);          // 100+50-30
    CHECK_EQ(VARS.getI("z"), 3);             // 0+5-2
    CHECK_EQ(VARS.getS("name"), "英雄");
    CHECK_EQ(VARS.getS("strlit"), "hello world");
    CHECK_EQ(VARS.getI("neg"), -5);
    CHECK_EQ(VARS.getS("empty_str"), "");
    // RANDOM 结果应在 [1,100]
    long long r1 = VARS.getI("r1");
    CHECK(r1 >= 1 && r1 <= 100);

    std::string out = mainText();
    CHECK(contains(out, "120"));             // hp=[120] 中的数值
    CHECK(contains(out, "英雄"));            // name=[英雄]
    CHECK(contains(out, "hello world"));     // str=[hello world]
    CHECK(contains(out, "z=3"));
}

// ============================================================
// 集成测试: IF 运算符 (块 + 单行) 与嵌套 IF
// ============================================================
static void test_if() {
    GROUP("Integration: IF 运算符 + 嵌套");

    freshState();
    g_script = {
        "SET n = 42",
        "IF n == 42 SAY OP_EQ_PASS",
        "IF n != 0 SAY OP_NE_PASS",
        "IF n > 10 SAY OP_GT_PASS",
        "IF n < 100 SAY OP_LT_PASS",
        "IF n >= 42 SAY OP_GE_PASS",
        "IF n <= 42 SAY OP_LE_PASS",
        "IF n == 100 SAY OP_EQ_FAIL",
        "SET s = \"abc\"",
        "IF s == \"abc\" SAY STR_EQ_PASS",
        "IF s != \"xyz\" SAY STR_NE_PASS",
        // 块 IF
        "SET score = 85",
        "IF score > 60",
        "SAY 块-及格",
        "ELSE",
        "SAY 块-不及格",
        "ENDIF",
        "SET x = 10",
        "IF x > 100",
        "SAY 块2-真",
        "ELSE",
        "SAY 块2-ELSE",
        "ENDIF",
        // 嵌套 IF
        "SET outer = 1",
        "SET inner = 1",
        "IF outer == 1",
        "SAY 外层真",
        "IF inner == 1",
        "SAY 内层真",
        "ELSE",
        "SAY 内层假",
        "ENDIF",
        "ELSE",
        "SAY 外层假",
        "ENDIF"
    };
    std::vector<int> ch; std::vector<std::string> inp;
    runScript(ch, inp);

    std::string out = mainText();
    CHECK(contains(out, "OP_EQ_PASS"));
    CHECK(contains(out, "OP_NE_PASS"));
    CHECK(contains(out, "OP_GT_PASS"));
    CHECK(contains(out, "OP_LT_PASS"));
    CHECK(contains(out, "OP_GE_PASS"));
    CHECK(contains(out, "OP_LE_PASS"));
    CHECK(!contains(out, "OP_EQ_FAIL"));   // 不应出现
    CHECK(contains(out, "STR_EQ_PASS"));
    CHECK(contains(out, "STR_NE_PASS"));
    CHECK(contains(out, "块-及格"));
    CHECK(!contains(out, "块-不及格"));
    CHECK(!contains(out, "块2-真"));
    CHECK(contains(out, "块2-ELSE"));
    CHECK(contains(out, "外层真"));
    CHECK(contains(out, "内层真"));
    CHECK(!contains(out, "内层假"));
    CHECK(!contains(out, "外层假"));
}

// ============================================================
// 集成测试: 内联 ELSE <动作> (回归: IF 成立时 else 内联动作不应执行)
// ============================================================
static void test_else_inline() {
    GROUP("Integration: 内联 ELSE <动作> (IF 成立不误执行 else)");

    // 1) IF 成立 -> 仅 THEN 内联动作, else 内联动作被跳过
    freshState();
    g_script = {
        "SET n = 42",
        "IF n == 42 SAY THEN_HIT",
        "ELSE SAY ELSE_HIT",
        "ENDIF"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out, "THEN_HIT"));
        CHECK(!contains(out, "ELSE_HIT"));   // 回归点: 修复前会误执行
    }

    // 2) IF 不成立 -> 执行 ELSE 内联动作
    freshState();
    g_script = {
        "SET n = 0",
        "IF n == 42 SAY THEN_HIT",
        "ELSE SAY ELSE_HIT",
        "ENDIF"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(!contains(out, "THEN_HIT"));
        CHECK(contains(out, "ELSE_HIT"));
    }

    // 3) 块 IF 内嵌套内联 ELSE: IF 成立走 THEN, 内层内联 ELSE 仍按自身条件判定
    freshState();
    g_script = {
        "SET a = 1",
        "SET b = 0",
        "IF a == 1",
        "SAY A_HIT",
        "IF b == 1 SAY B_HIT",
        "ELSE SAY B_MISS",
        "ENDIF",
        "ELSE",
        "SAY A_MISS",
        "ENDIF"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out, "A_HIT"));
        CHECK(!contains(out, "A_MISS"));
        CHECK(!contains(out, "B_HIT"));     // b==0 -> 内层 else
        CHECK(contains(out, "B_MISS"));
    }

    // 4) 块 IF + 独立 ELSE 行 (无内联动作): 行为应与原先一致
    freshState();
    g_script = {
        "SET n = 42",
        "IF n == 42",
        "SAY T_BLOCK",
        "ELSE",
        "SAY F_BLOCK",
        "ENDIF"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out, "T_BLOCK"));
        CHECK(!contains(out, "F_BLOCK"));
    }
}

// ============================================================
// 集成测试: GOTO 多种格式 + CALL
// ============================================================
static void test_flow() {
    GROUP("Integration: GOTO 格式 + CALL");

    freshState();
    g_script = {
        "GOTO t12_y",
        "SAY SHOULD_SKIP",
        ":t12_y:",
        "SAY X2",
        "GOTO :t12_z",
        "::t12_z",
        "SAY X3",
        "GOTO ::t12_w",
        "::t12_w:",
        "SAY X4",
        // CALL 不返回 (子例程自行 GOTO 终点)
        "CALL sub",
        "SAY AFTER_CALL",
        ":sub",
        "SAY IN_SUB",
        "GOTO AFTER_CALL"
    };
    std::vector<int> ch; std::vector<std::string> inp;
    runScript(ch, inp);

    std::string out = mainText();
    CHECK(!contains(out, "SHOULD_SKIP"));   // 被 GOTO 跳过
    CHECK(contains(out, "X2"));
    CHECK(contains(out, "X3"));
    CHECK(contains(out, "X4"));
    CHECK(contains(out, "IN_SUB"));
}

// ============================================================
// 集成测试: CHOICE 分支 (选不同序号)
// ============================================================
static void test_choice() {
    GROUP("Integration: CHOICE 分支");

    freshState();
    g_script = {
        "CHOICE 走A 走B 走C",
        "IF choice == 1 GOTO a",
        "IF choice == 2 GOTO b",
        "IF choice == 3 GOTO c",
        "GOTO end",
        ":a",
        "SAY BRANCH_A",
        "GOTO end",
        ":b",
        "SAY BRANCH_B",
        "GOTO end",
        ":c",
        "SAY BRANCH_C",
        ":end",
        "SAY CHOICE_DONE"
    };
    // 依次测试三种选择
    for (int pick = 1; pick <= 3; pick++) {
        freshState();
        g_script = {  // 重新设置 (上面已修改 g_script 不影响, 这里重建)
            "CHOICE 走A 走B 走C",
            "IF choice == 1 GOTO a",
            "IF choice == 2 GOTO b",
            "IF choice == 3 GOTO c",
            "GOTO end",
            ":a", "SAY BRANCH_A", "GOTO end",
            ":b", "SAY BRANCH_B", "GOTO end",
            ":c", "SAY BRANCH_C", ":end", "SAY CHOICE_DONE"
        };
        std::vector<int> ch = { pick };
        std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out, "CHOICE_DONE"));
        if (pick == 1) CHECK(contains(out, "BRANCH_A"));
        if (pick == 2) CHECK(contains(out, "BRANCH_B"));
        if (pick == 3) CHECK(contains(out, "BRANCH_C"));
    }
}

// ============================================================
// 集成测试: INPUT 接收输入
// ============================================================
static void test_input() {
    GROUP("Integration: INPUT 接收输入");

    freshState();
    g_script = {
        "INPUT hero_name",
        "SAY 收到: $hero_name",
        "INPUT short 5",
        "SAY 短输入: $short"
    };
    std::vector<int> ch;
    std::vector<std::string> inp = { "Alice", "Bob12" };
    runScript(ch, inp);

    std::string out = mainText();
    CHECK(contains(out, "收到: Alice"));
    CHECK(contains(out, "短输入: Bob12"));
    CHECK_EQ(VARS.getS("hero_name"), "Alice");
    CHECK_EQ(VARS.getS("short"), "Bob12");
}

// ============================================================
// 集成测试: PANEL 系统 (new/switch/hide/show/clear)
// ============================================================
static void test_panel() {
    GROUP("Integration: PANEL 系统");

    freshState();
    g_script = {
        "PANELNEW sidebar",
        "PANEL sidebar",
        "SAY 侧栏内容",
        "PANEL main",
        "SAY 回到主",
        "HIDE sidebar",
        "SHOW sidebar",
        "CLEAR sidebar",
        "PANEL sidebar",
        "SAY sidebar已清空",
        "PANEL main",
        "SAY PANEL_DONE"
    };
    std::vector<int> ch; std::vector<std::string> inp;
    runScript(ch, inp);

    CHECK(S.panels.count("sidebar") == 1);
    CHECK(S.panels.count("main") == 1);
    std::string mainOut;
    for (auto& l : S.panels["main"].lines) mainOut += l + "\n";
    CHECK(contains(mainOut, "回到主"));
    CHECK(contains(mainOut, "PANEL_DONE"));
    std::string sideOut;
    for (auto& l : S.panels["sidebar"].lines) sideOut += l + "\n";
    CHECK(contains(sideOut, "sidebar已清空"));
    CHECK(!contains(sideOut, "侧栏内容"));   // CLEAR 已清空
}

// ============================================================
// 集成测试: SAVE / LOAD 变量持久化
// ============================================================
static void test_saveload() {
    GROUP("Integration: SAVE / LOAD 持久化");

    const char* fn = "test_save_tmp.txt";
    std::remove(fn);

    freshState();
    VARS.setI("hp", 88);
    VARS.setS("hero", "Bob");
    VARS.setI("gold", 500);
    CHECK(userSave(fn));

    // 重置后再载入
    freshState();
    CHECK_EQ(VARS.getI("hp"), 0);
    CHECK(userLoad(fn, false));                 // 脚本 LOAD 模式: 不跳转
    CHECK_EQ(VARS.getI("hp"), 88);
    CHECK_EQ(VARS.getS("hero"), "Bob");
    CHECK_EQ(VARS.getI("gold"), 500);

    // 用户主动读档 (resume 模式) 应设定 loadResumeIp
    freshState();
    VARS.setI("hp", 1);
    userSave(fn);
    S.loadResumeIp = -1;
    userLoad(fn, true);
    CHECK(S.loadResumeIp >= 0);                 // 应被设置以跳回存档点下一行

    // 读档失败处理: 文件不存在
    CHECK(!userLoad("no_such_file_xyz.txt", false));

    std::remove(fn);
}

// ============================================================
// 集成测试: 边界 / 异常流程 (不崩溃, 行为可预期)
// ============================================================
static void test_boundary() {
    GROUP("Integration: 边界 / 异常流程");

    // 1) GOTO 不存在的标签 -> 不跳转, 继续执行下一行, 不崩溃
    freshState();
    g_script = { "GOTO nowhere_label", "SAY REACHED_AFTER_BAD_GOTO" };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    CHECK(contains(mainText(), "REACHED_AFTER_BAD_GOTO"));

    // 2) 畸形 IF (参数不足) -> 忽略, 继续执行
    freshState();
    g_script = { "IF x >", "SAY AFTER_BAD_IF" };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    CHECK(contains(mainText(), "AFTER_BAD_IF"));

    // 3) 空脚本 -> 立即结束, 不崩溃
    freshState();
    g_script = {};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    CHECK(true);   // 能走到这里即未崩溃

    // 4) 未知命令 -> 当作 SAY 输出
    freshState();
    g_script = { "FOOBAR this is unknown" };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    CHECK(contains(mainText(), "this is unknown"));

    // 5) 注释 / 空行被忽略
    freshState();
    g_script = { "; comment", "", "  # another", "SAY AFTER_COMMENTS" };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    CHECK(contains(mainText(), "AFTER_COMMENTS"));

    // 6) RANDOM 区间反转 (a>b) -> 自动交换, 结果仍合法
    freshState();
    g_script = { "RANDOM 100 1 rswap" };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
    }
    long long rs = VARS.getI("rswap");
    CHECK(rs >= 1 && rs <= 100);

    // 7) INPUT 最大长度限制被正确解析
    freshState();
    g_script = { "INPUT short 5", "SAY maxset" };
    {
        std::vector<int> ch; std::vector<std::string> inp = { "abcdefghij" };
        runScript(ch, inp);
    }
    CHECK_EQ(S.inputMax, 5);
    CHECK_EQ(VARS.getS("short"), "abcdefghij");   // 模拟器不强制截断, 仅验证参数解析

    // 8) 深层嵌套括号表达式不崩溃
    freshState();
    CHECK_EQ(evalExpr("((((((1+1))))))"), 2);
}

// ============================================================
// 集成测试: 完整测试脚本 test_full.txt (端到端)
// ============================================================
static void test_full() {
    GROUP("Integration: 完整测试 test_full.txt (端到端)");

    freshState();
    if (!loadScript("test_full.txt")) {
        std::cout << "  [SKIP] test_full.txt 未找到 (请在仓库根目录运行测试)" << std::endl;
        return;
    }
    std::vector<int> ch;            // CHOICE 默认选 1
    std::vector<std::string> inp;   // INPUT 默认 "x"
    runScript(ch, inp);

    std::string out = mainText();
    // 各段起始/结束标记
    CHECK(contains(out, "T0_START"));
    CHECK(contains(out, "T1_START"));
    CHECK(contains(out, "T18_START"));   // 脚本 T18 段起始标记 (无 T18_DONE)
    // CHOICE 选 1 -> 分支 A
    CHECK(contains(out, "BRANCH_A"));
    // 运算符全部通过
    CHECK(contains(out, "OP_EQ_PASS"));
    CHECK(contains(out, "STR_EQ_PASS"));
    // 多面板就绪
    CHECK(contains(out, "T19-全部面板就绪"));
    CHECK(contains(out, "[T19_DONE]"));
    // 富文本转义: 反斜杠转义方括号, 输出中保留字面量 (含反斜杠)
    CHECK(contains(out, "转义方括号"));

    // 表达式求值结果 (与脚本 T4 一致)
    CHECK_EQ(VARS.getI("g"), 120);   // 100+50-30
    CHECK_EQ(VARS.getI("z"), 3);     // 0+5-2
    CHECK(contains(out, "s=13"));    // T4 输出 s=13 (T6 将 s 重新赋值为字符串)
    CHECK_EQ(VARS.getI("p"), 30);    // 10*3
    CHECK_EQ(VARS.getI("d"), 3);     // 10/3
    CHECK_EQ(VARS.getI("m"), 1);     // 10%3
    CHECK_EQ(VARS.getI("e"), 7);     // 1+2*3
    CHECK_EQ(VARS.getI("f"), 9);     // (1+2)*3
    CHECK_EQ(VARS.getI("h"), 26);    // 2*(10+3)
    CHECK_EQ(VARS.getI("hp"), 100);
}

// ============================================================
// 单元测试: joinRemain (参数拼接/去引号)
// ============================================================
static void test_joinremain() {
    GROUP("Unit: joinRemain (参数拼接/去引号)");
    std::vector<std::string> v1 = {"SAY","hello","world"};
    CHECK_EQ(joinRemain(v1,1), "hello world");
    std::vector<std::string> v2 = {"SET","x","\"a b\""};
    CHECK_EQ(joinRemain(v2,2), "a b");          // 去引号
    std::vector<std::string> v3 = {"X","only"};
    CHECK_EQ(joinRemain(v3,1), "only");
    std::vector<std::string> v4 = {"X"};
    CHECK_EQ(joinRemain(v4,1), "");             // 越界 -> 空
}

// ============================================================
// 单元测试: evalExpr 边缘 (连续运算/变量/括号)
// ============================================================
static void test_evalexpr_edge() {
    GROUP("Unit: evalExpr 边缘 (连续运算/变量/括号)");
    VARS.vs.clear();
    VARS.setI("p", 10); VARS.setI("q", 3);
    CHECK_EQ(evalExpr("1 + 2 + 3"), 6);
    CHECK_EQ(evalExpr("10 - 3 - 2"), 5);
    CHECK_EQ(evalExpr("p + q"), 13);
    CHECK_EQ(evalExpr("p * q + 1"), 31);
    CHECK_EQ(evalExpr("(p + q) * 2"), 26);
    CHECK_EQ(evalExpr("2 * (3 + 4) - 1"), 13);
    CHECK_EQ(evalExpr("100 % 7"), 2);
    CHECK_EQ(evalExpr(""), 0);
    CHECK_EQ(evalExpr("   "), 0);
}

// ============================================================
// 集成测试: IF/ELSE 边缘场景 (P0 回归)
// ============================================================
static void test_if_edge() {
    GROUP("Integration: IF/ELSE 边缘 (块内单行IF / 内联ELSE / 深层嵌套)");

    // 1) 块 IF 不成立时, 内部含单行 IF -> ELSE 分支必须仍执行
    //    (回归: 旧实现因单行 IF 误增深度, 会跳过 ELSE)
    freshState();
    g_script = {
        "SET a = 0",
        "IF a == 1",
        "SET b = 1",
        "IF c == 2 SAY INNER_HIT",
        "SET d = 2",
        "ELSE",
        "SET e = 3",
        "ENDIF",
        "SAY DONE"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(!contains(out,"INNER_HIT"));   // 单行 IF 条件不成立, 不输出
        CHECK(contains(out,"DONE"));          // 正常结束
        CHECK_EQ(VARS.getI("e"), 3);          // 关键: ELSE 分支应执行, e=3
        CHECK_EQ(VARS.getI("b"), 0);          // 块 IF 失败, b 未设置
        CHECK_EQ(VARS.getI("d"), 0);          // 块 IF 失败, d 未设置
    }

    // 2) 块 IF 成立时, 内部含单行 IF(条件成立) -> 执行内联动作
    freshState();
    g_script = {
        "SET a = 1",
        "SET c = 2",
        "IF a == 1",
        "SET b = 1",
        "IF c == 2 SAY INNER_HIT",
        "ELSE",
        "SET e = 3",
        "ENDIF",
        "SAY DONE"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out,"INNER_HIT"));     // 单行 IF 成立
        CHECK(contains(out,"DONE"));
        CHECK_EQ(VARS.getI("b"), 1);
        CHECK_EQ(VARS.getI("e"), 0);          // ELSE 未执行
    }

    // 3) 单行 IF 内联 ELSE: 成立走 THEN, 不成立走 ELSE
    freshState();
    g_script = {
        "SET x = 1",
        "IF x == 1 SAY T_HIT ELSE SAY F_HIT",
        "SET y = 0",
        "IF y == 1 SAY T2_HIT ELSE SAY F2_HIT"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out,"T_HIT"));
        CHECK(!contains(out,"F_HIT"));
        CHECK(!contains(out,"T2_HIT"));
        CHECK(contains(out,"F2_HIT"));
    }

    // 4) 深层嵌套块 IF (5 层) 全部成立
    freshState();
    g_script = {
        "SET l1 = 1","SET l2 = 1","SET l3 = 1","SET l4 = 1","SET l5 = 1",
        "IF l1 == 1",
        "IF l2 == 1",
        "IF l3 == 1",
        "IF l4 == 1",
        "IF l5 == 1",
        "SAY DEEP_HIT",
        "ENDIF","ENDIF","ENDIF","ENDIF","ENDIF",
        "SAY DONE"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"DEEP_HIT"));
        CHECK(contains(mainText(),"DONE"));
    }
}

// ============================================================
// 集成测试: 未覆盖命令 (P1)
// ============================================================
static void test_commands_misc() {
    GROUP("Integration: 未覆盖命令 (SAY+/NARRATE/CLEARALL/CURSOR/BORDER/LOG/WAIT/TYPESPEED/SHAKE/BLINK/EFFECT/EXIT/REM/HELP)");

    // SAY+ 不换行 (同行追加)
    freshState();
    g_script = {"SAY+ Hello","SAY+ World","SAY NewLine"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        std::string out = mainText();
        CHECK(contains(out,"HelloWorld"));   // 两行 SAY+ 合并
        CHECK(contains(out,"NewLine"));
    }

    // NARRATE 旁白
    freshState();
    g_script = {"NARRATE 旁白文本"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"旁白文本"));
    }

    // CLEARALL 清空所有面板
    freshState();
    g_script = {"SAY m1","PANELNEW side","PANEL side","SAY s1","PANEL main","CLEARALL","SAY after"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(!S.panels["main"].lines.empty());  // after 仍在
        CHECK(S.panels["side"].lines.empty());    // side 被清空
    }

    // CURSOR / GOTOXY
    freshState();
    g_script = {"CURSOR 7 3"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK_EQ(S.panels[S.curPanel].curCol, 7);
        CHECK_EQ(S.panels[S.curPanel].cur, 3);
    }

    // BORDER
    freshState();
    g_script = {"SAY x","BORDER","SAY y"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK_EQ((int)S.panels[S.curPanel].lines.size(), 4); // x + 2 border + y
    }

    // LOG
    freshState();
    g_script = {"LOG 日志内容"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(S.panels.count("log")==1);
        std::string lo;
        for(auto&l:S.panels["log"].lines)lo+=l;
        CHECK(contains(lo,"日志内容"));
    }

    // WAIT / TYPESPEED / SHAKE / BLINK / EFFECT (状态标志)
    // 注: runScript 每步调用 flushTW 会清零 sleepUntil, 故 WAIT 用 execCmd 直接验证
    freshState();
    {
        bool j=false; execCmd("WAIT 100", j);
        CHECK(S.sleepUntil>0);   // 命令执行后立即检查, 未被 flushTW 清零
    }
    freshState();
    g_script = {"TYPESPEED 30","SHAKE 3 500","BLINK 300 2000","EFFECT main shake 800"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK_EQ(S.typeSpeed, 30);
        CHECK(S.shaking);
        CHECK(S.blinking);
        CHECK_EQ((int)S.panels[S.curPanel].fx.size(), 1);
        CHECK_EQ(S.panels[S.curPanel].fx[0].kind, "shake");
        CHECK_EQ(S.panels[S.curPanel].fx[0].dur, 800);
    }

    // EXIT 停止运行
    freshState();
    g_script = {"SAY before_exit","EXIT","SAY after_exit"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"before_exit"));
        CHECK(!contains(mainText(),"after_exit"));  // EXIT 后停止
    }

    // REM / // 注释被忽略
    freshState();
    g_script = {"REM 这是注释","// 另一注释","SAY real"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"real"));
        CHECK_EQ((int)S.panels[S.curPanel].lines.size(), 1);
    }

    // HELP 输出指令集
    freshState();
    g_script = {"HELP"};
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK((int)S.panels[S.curPanel].lines.size() > 5);
    }
}

// ============================================================
// 集成测试: SET 表达式 / RANDOM 默认变量 / ADD-SUB 表达式 (P1)
// ============================================================
static void test_set_expr() {
    GROUP("Integration: SET 表达式 / RANDOM 默认变量 / ADD-SUB 表达式");

    freshState();
    g_script = {
        "SET hp = 100",
        "ADD hp (10+5)",       // 表达式增量
        "SUB hp 3",
        "RANDOM 1 10",         // 默认写入 'random'
        "SET name = \"hero\"",
        "SET sum = hp + 5",
        "SAY hp=$hp name=$name sum=$sum"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK_EQ(VARS.getI("hp"), 112);   // 100+15-3
        CHECK_EQ(VARS.getS("name"), "hero");
        CHECK_EQ(VARS.getI("sum"), 117);  // 112+5
        long long rnd = VARS.getI("random");
        CHECK(rnd>=1 && rnd<=10);
    }
}

// ============================================================
// 集成测试: USERSAVE/USERLOAD 恢复 + RUN/CHAIN 切换剧本 (P1)
// ============================================================
static void test_saveload_resume() {
    GROUP("Integration: USERSAVE/USERLOAD 恢复 + RUN/CHAIN 切换剧本");

    const char* fn = "test_usave_tmp.txt";
    std::remove(fn);

    // USERSAVE: 记录存档点, 之后 EXIT 避免回环
    freshState();
    VARS.setI("hp", 77);
    g_script = {
        "SET hp = 77",
        std::string("USERSAVE ") + fn,
        "EXIT",
        "SET hp = 0"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(), "存档成功"));
    }

    // USERLOAD: 跳回存档点下一行, 变量还原
    freshState();
    VARS.setI("hp", 0);
    g_script = {
        "SET hp = 0",
        std::string("USERLOAD ") + fn,
        "SAY DONE hp=$hp"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(), "读档成功"));
        CHECK(contains(mainText(), "DONE hp=77"));
        CHECK_EQ(VARS.getI("hp"), 77);
    }

    // RUN/CHAIN: 加载并切换剧本, 原后续行不再执行
    {
        std::ofstream tf("test_run_chain_tmp.txt");
        tf << "SET chained = 1\n";
        tf << "SAY CHAINED_OK\n";
        tf.close();
        freshState();
        g_script = {"SET hp = 5", "RUN test_run_chain_tmp.txt", "SAY AFTER_RUN"};
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(), "CHAINED_OK"));
        CHECK(!contains(mainText(), "AFTER_RUN"));  // 剧本已替换
        std::remove("test_run_chain_tmp.txt");
    }

    std::remove(fn);
}

// ============================================================
// 集成测试: 边界/异常扩展 (P2)
// ============================================================
static void test_boundary2() {
    GROUP("Integration: 边界/异常 (CHOICE越界, INPUT默认上限, 孤立ELSE)");

    // CHOICE 越界序号: 不崩溃, 走默认分支
    freshState();
    g_script = {
        "CHOICE A B C",
        "IF choice == 1 GOTO a",
        "IF choice == 2 GOTO b",
        "GOTO end",
        ":a","SAY BR_A","GOTO end",
        ":b","SAY BR_B","GOTO end",
        ":end","SAY CHOICE_DONE"
    };
    {
        std::vector<int> ch = {99};  // 越界
        std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"CHOICE_DONE"));
        CHECK(!contains(mainText(),"BR_A"));
        CHECK(!contains(mainText(),"BR_B"));
    }

    // INPUT 无上限参数 -> 默认 64
    freshState();
    g_script = {"INPUT name","SAY got=$name"};
    {
        std::vector<int> ch; std::vector<std::string> inp = {"zoe"};
        runScript(ch, inp);
        CHECK_EQ(S.inputMax, 64);
        CHECK_EQ(VARS.getS("name"), "zoe");
    }

    // 孤立 ELSE (无配对 IF) -> 安全忽略, 不崩溃
    freshState();
    g_script = {
        "SET a = 0",
        "IF a == 1",
        "SAY T",
        "ELSE",
        "SAY E1",
        "ENDIF",
        "ELSE",
        "SAY AFTER_ORPHAN"
    };
    {
        std::vector<int> ch; std::vector<std::string> inp;
        runScript(ch, inp);
        CHECK(contains(mainText(),"E1"));
        CHECK(contains(mainText(),"AFTER_ORPHAN"));
    }
}

// ============================================================
// 主入口
// ============================================================
int main() {
    std::cout << "PlotEngine 测试套件" << std::endl;

    test_utils();
    test_codec();
    test_varsystem();
    test_resolvestr();
    test_evalexpr();
    test_evalexpr_edge();
    test_joinremain();
    test_rich();
    test_labels();
    test_visible();

    test_vars_flow();
    test_if();
    test_if_edge();
    test_else_inline();
    test_set_expr();
    test_flow();
    test_choice();
    test_input();
    test_panel();
    test_saveload();
    test_saveload_resume();
    test_commands_misc();
    test_boundary();
    test_boundary2();
    test_full();

    std::cout << "\n========================================" << std::endl;
    std::cout << "  通过: " << g_pass << "   失败: " << g_fail
              << "   测试组: " << g_groups << std::endl;
    std::cout << "========================================" << std::endl;

    return g_fail == 0 ? 0 : 1;
}
