// =====================================================================
// PlotEngine.cpp - 单文件 C++17 文字冒险游戏引擎 (内置 FTXUI 4.1.1)
//
// 单文件编译运行 (无需 FTXUI 源码目录, 只依赖 ftxui_amalgamation.hpp):
//   g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 PlotEngine.cpp -o plotengine.exe -lpthread   (Windows/MinGW)
//   g++ -std=c++17 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 PlotEngine.cpp -o plotengine   -lpthread     (Linux)
// 或直接执行:  build_single.bat
//
// FTXUI 由 ftxui_amalgamation.hpp 合并提供, 遵循 MIT License,
// Copyright (c) 2019 Arthur Sonzogni (完整协议见 FTXUI-4.1.1/LICENSE)。
// 合并文件可用 tools/amalgamate_ftxui.py 重新生成。
// =====================================================================
#include "mingw_std_threads.hpp"   // Win32 线程支撑 (std::mutex/thread/condition_variable)
#include "ftxui_amalgamation.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>
using namespace ftxui;
using std::string;
using std::vector;
using std::map;
using std::pair;
using std::function;
#define E Element

// ============================================================
// 调试日志
// ============================================================
static std::ofstream g_log;
static bool g_logOn=false;
static void logOpen(){
    if(!g_log.is_open()){
        g_log.open("plotengine.log",std::ios::out|std::ios::trunc);
        g_logOn=g_log.is_open();
    }
}
static void dlog(const string& s){
    if(!g_logOn)return;
    g_log<<s<<std::endl;
    g_log.flush();
}
#define DLOG(x) do{logOpen();dlog(x);}while(0)

// ============================================================
// 工具函数
// ============================================================
static inline string tr(const string& s){
    size_t a=s.find_first_not_of(" \t\r\n");
    if(a==string::npos)return "";
    size_t b=s.find_last_not_of(" \t\r\n");
    return s.substr(a,b-a+1);
}
static inline string low(string s){for(auto&c:s)c=tolower((unsigned char)c);return s;}
static inline string up(string s){for(auto&c:s)c=toupper((unsigned char)c);return s;}
static vector<string> splitTok(const string& s){
    vector<string> r;string t;int q=0;char qc=0;
    for(size_t i=0;i<s.size();i++){
        char c=s[i];
        if(q){t+=c;if(c==qc)q=0;}
        else if(c=='"'||c=='\''){q=1;qc=c;t+=c;}
        else if(c==' '||c=='\t'){if(!t.empty()){r.push_back(t);t.clear();}}
        else t+=c;
    }
    if(!t.empty())r.push_back(t);
    return r;
}
static vector<string> splitBy(const string& s,char d){
    vector<string> r;string t;
    for(char c:s){if(c==d){r.push_back(t);t.clear();}else t+=c;}
    r.push_back(t);
    return r;
}
static string stripQuote(string s){
    if(s.size()>=2&&((s.front()=='"'&&s.back()=='"')||(s.front()=='\''&&s.back()=='\'')))
        return s.substr(1,s.size()-2);
    return s;
}
static string joinRemain(const vector<string>& v,size_t from){
    string o;for(size_t i=from;i<v.size();i++){if(i>from)o+=' ';o+=v[i];}
    return stripQuote(o);
}
static string toHex(const string& s){
    static const char*H="0123456789ABCDEF";
    string o;o.reserve(s.size()*2);
    for(unsigned char c:s){o+=H[c>>4];o+=H[c&15];}
    return o;
}
static string fromHex(const string& s){
    string o;o.reserve(s.size()/2);
    auto hv=[](char c)->int{return (c>='0'&&c<='9')?c-'0':(c>='A'&&c<='F')?c-'A'+10:(c>='a'&&c<='f')?c-'a'+10:0;};
    for(size_t i=0;i+1<s.size();i+=2)o+=(char)((hv(s[i])<<4)|hv(s[i+1]));
    return o;
}
// Base64 编解码: 使用 A-Z a-z 0-9 + / = (64 种可见字符,存档可读)
static const char* B64="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static string toBase64(const string& s){
    string o;o.reserve(((s.size()+2)/3)*4);
    for(size_t i=0;i<s.size();i+=3){
        unsigned a=(unsigned char)s[i];
        unsigned b=(i+1<s.size())?(unsigned char)s[i+1]:0;
        unsigned c=(i+2<s.size())?(unsigned char)s[i+2]:0;
        o+=B64[(a>>2)&63];
        o+=B64[((a&3)<<4)|(b>>4)];
        o+=(i+1<s.size())?B64[((b&15)<<2)|(c>>6)]:'=';
        o+=(i+2<s.size())?B64[c&63]:'=';
    }
    return o;
}
static string fromBase64(const string& s){
    auto iv=[](char ch)->int{
        for(int i=0;i<64;i++)if(B64[i]==ch)return i;
        if(ch=='='||ch=='\n'||ch=='\r')return -1;return 0;
    };
    string o;o.reserve((s.size()/4)*3);
    int buf=0,bits=0;
    for(char ch:s){
        int v=iv(ch);
        if(v<0)continue;
        buf=(buf<<6)|v;bits+=6;
        if(bits>=8){bits-=8;o+=(char)((buf>>bits)&0xFF);}
    }
    return o;
}
static string xorCrypt(const string& s,const string& k){
    string o;o.reserve(s.size());
    for(size_t i=0;i<s.size();i++)o+=s[i]^k[i%k.size()];
    return o;
}
static string itoa2(long long n){char b[40];sprintf(b,"%lld",n);return b;}
static string nowStr(){time_t t=time(nullptr);char b[64];strftime(b,sizeof(b),"%H:%M:%S",localtime(&t));return b;}
static long long nowMs(){
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static int atoiS(const string& s){return atoi(s.c_str());}
static Color parseColor(const string& s){
    string t=low(s);
    if(t=="black")return Color::Black;if(t=="red")return Color::Red;if(t=="green")return Color::Green;
    if(t=="yellow")return Color::Yellow;if(t=="blue")return Color::Blue;if(t=="magenta")return Color::Magenta;
    if(t=="cyan")return Color::Cyan;if(t=="white")return Color::White;    if(t=="gray"||t=="grey")return Color::GrayDark;
    if(t=="grayl"||t=="greyl")return Color::GrayLight;
    if(t=="redl"||t=="pink")return Color::RedLight;if(t=="greenl")return Color::GreenLight;
    if(t=="yellowl"||t=="orange")return Color::YellowLight;if(t=="bluel")return Color::BlueLight;
    if(t=="magental"||t=="purple")return Color::MagentaLight;if(t=="cyanl")return Color::CyanLight;
    if(t=="default")return Color::Default;
    return Color::Default;
}

// ============================================================
// 变量系统
// ============================================================
struct Var{string name;long long iv=0;string sv;bool isInt=true;};
class VarSystem{
public:
    vector<Var> vs;
    Var* find(const string& n){for(auto&v:vs)if(v.name==n)return&v;return nullptr;}
    const Var* find(const string& n)const{for(auto&v:vs)if(v.name==n)return&v;return nullptr;}
    void setI(const string& n,long long v){
        Var* p=find(n);
        if(p){p->iv=v;p->isInt=true;}
        else{Var x;x.name=n;x.iv=v;x.isInt=true;vs.push_back(x);}
    }
    void setS(const string& n,const string& v){
        Var* p=find(n);
        if(p){p->sv=v;p->isInt=false;}
        else{Var x;x.name=n;x.sv=v;x.isInt=false;vs.push_back(x);}
    }
    long long getI(const string& n)const{
        const Var*p=find(n);return p?(p->isInt?p->iv:atoll(p->sv.c_str())):0;
    }
    string getS(const string& n)const{
        const Var*p=find(n);
        if(!p)return"";
        return p->isInt?itoa2(p->iv):p->sv;
    }
    bool has(const string& n)const{return find(n)!=nullptr;}
    string resolve(const string& t)const{
        string o;o.reserve(t.size());
        for(size_t i=0;i<t.size();){
            if(t[i]=='$'&&i+1<t.size()){
                size_t j=i+1;
                while(j<t.size()&&(isalnum((unsigned char)t[j])||t[j]=='_'))j++;
                if(j>i+1){
                    string nm=t.substr(i+1,j-i-1);
                    o+=getS(nm);i=j;continue;
                }
            }
            o+=t[i++];
        }
        return o;
    }
    string serialize()const{
        string o;
        for(auto&v:vs){
            o+=v.name;o+="=";o+=(v.isInt?"I":"S");o+="=";
            o+=(v.isInt?std::to_string((long long)v.iv):v.sv);
            o+="\n";
        }
        return o;
    }
    void deserialize(const string& d){
        vs.clear();
        for(auto&l:splitBy(d,'\n')){
            if(l.empty())continue;
            size_t p1=l.find('=');if(p1==string::npos)continue;
            size_t p2=l.find('=',p1+1);if(p2==string::npos)continue;
            Var v;v.name=l.substr(0,p1);
            string typ=l.substr(p1+1,p2-p1-1);
            string val=l.substr(p2+1);
            v.isInt=(typ=="I");
            if(v.isInt)v.iv=atoll(val.c_str());else v.sv=val;
            vs.push_back(v);
        }
    }
    string dump()const{
        string o;
        for(auto&v:vs){o+="  $"+v.name+" = "+(v.isInt?itoa2(v.iv):v.sv)+" ["+(v.isInt?"int":"str")+"]\n";}
        return o;
    }
};
static VarSystem VARS;

// ============================================================
// 效果与面板
// ============================================================
struct Effect{string kind;int dur=500;int elapsed=0;int a=0;string text;};
struct Panel{
    string name;
    vector<string> lines;
    vector<Effect> fx;
    int cur=0;
    int curCol=0;
    bool visible=true;
    int scroll=0;        // 面板内容滚动偏移(行)
    bool follow=true;    // 是否自动跟随底部(新内容自动可见)
    void clear(){lines.clear();fx.clear();cur=0;curCol=0;scroll=0;follow=true;}
    void add(const string& l){if(lines.size()>=2000)lines.erase(lines.begin());lines.push_back(l);cur=(int)lines.size()-1;curCol=0; if(follow)scroll=(int)lines.size();}
    void addTop(const string& l){lines.insert(lines.begin(),l);cur=0;}
};

// ============================================================
// 全局状态
// ============================================================
struct State{
    // 引擎运行开关
    bool run=true;          // false 时主循环退出
    bool debug=false;       // F2 / ` 切换; 显示调试条

    // 等待中
    bool waiting=false;     // 等待玩家输入(CHOICE/INPUT)
    bool waitType=false;    // 打字机进行中

    // 动画
    bool shaking=false;     // 面板震动
    bool blinking=false;    // 面板闪烁

    // 打字机
    int typeSpeed=12;       // ms per char, 0 = instant (默认 12ms ~80字/秒)
    int typePos=0,typeLine=-1;
    long long typeT0=0;     // 打字机起始时间戳(用于批量推进)

    // 震动
    int shakeDur=0,shakeInt=2,shakeOff=0,shakeT0=0;

    // 闪烁
    int blinkDur=0,blinkStep=200,blinkOn=true,blinkT0=0;

    // 面板
    string curPanel="main";
    string typePanel="";    // 打字机所属面板(doSay 时记录)

    // INPUT
    int inputPos=0;         // INPUT 光标位置
    int inputMax=64;        // INPUT 最大长度
    string inputBuf,inputVar="input";

    // CHOICE
    vector<string> choices;
    int choiceIdx=0;

    // 流程控制
    int loadResumeIp=-1;    // 读档后跳转的 IP (用户主动 LOAD 时设为 g_ip+1, 脚本 LOAD 不设)
    std::vector<char> ifTaken;  // IF/ENDIF 配对栈: 1=该 IF 的 THEN 分支已执行(命中), 0=未命中
    int sleepUntil=0;       // 暂停时间戳 (ms)
    int curLine=0;          // 最近一次 execCmd 的 IP

    // 调试事件
    string lastKey,lastMouse,lastEvent;

    // 面板集合
    map<string,Panel> panels;
    Panel& p(const string& n=""){string k=n.empty()?curPanel:n;return panels[k];}

    // 流程图覆盖层 (仅用户按 F 触发, 脚本不可主动调)
    bool showFlow=false;    // 是否显示流程图浮层
    int  flowScroll=0;      // 流程图滚动偏移(行)
};
static State S;
static int g_termH=24;   // 终端高度(行), 由 Renderer 每帧更新, 用于估算面板可见行数
static vector<string> g_script;
static int g_ip=0;
static string SCRIPT_FILE="demo.txt";
static string SAVE_FILE="save.txt";
static const char* ENC_KEY="PLOTENGINE_SECRET_KEY_2024";

// ============================================================
// 流程图: 数据结构 + 全局状态
//   - 仅用于 F 键触发的覆盖层显示
//   - 切换脚本 (RUN/CHAIN) 时清空重建
//   - 解析/渲染实现见下方"流程图: 解析 + 渲染"段
// ============================================================
enum FlowKind{
    FK_NOP=0,    // 注释/空行/标签
    FK_SAY,      // SAY/NARRATE/PRINT
    FK_BRANCH,   // IF (条件)
    FK_MERGE,    // ENDIF
    FK_GOTO,     // GOTO/JUMP
    FK_CALL,     // CALL/GOSUB
    FK_CHOICE,   // CHOICE/MENU
    FK_INPUT,    // INPUT/ASK
    FK_RUN,      // RUN/CHAIN (切脚本)
    FX_EXIT      // EXIT/QUIT/END
};
struct FlowNode{
    int ip=-1;                          // 节点起始 IP (源行号, 标签占首行)
    int ipEnd=-1;                       // 节点结束 IP (块语句的 ENDIF/ELSE 行; 普通节点 == ip)
    string label;                       // 标签名(若本行是标签定义)
    FlowKind kind=FK_NOP;
    string text;                        // 描述文本 (SAY 摘要 / 条件 / 目标 label)
    vector<int> succ;                   // 后继节点 IP (主流程: ip+1, 跳转: 目标)
    vector<int> succFalse;              // 条件为假时的后继 (仅 BRANCH 用)
    bool isCurrent=false;               // 当前 IP 命中此节点
};
static vector<FlowNode> g_flow;
static bool g_flowDirty=true;           // 脚本变化后置 true 重建
static int g_flowCurrentIdx=-1;         // 当前 IP 命中节点的下标(渲染时高亮)

// ============================================================
// 富文本解析
//   标签: [b] [u] [i] [blink] [red] [bg:blue] [reset]
//   颜色: black/red/green/yellow/blue/magenta/cyan/white/gray + Light 后缀
//   转义: \[ \] \\
// ============================================================
struct StyleState{
    Color fg=Color::Default;
    Color bg=Color::Default;
    bool b=false,u=false,i=false,bl=false;
};
static vector<pair<string,StyleState>> parseRich(const string& raw){
    vector<pair<string,StyleState>> segs;StyleState cur;string buf;
    auto flush=[&](){
        if(!buf.empty()){segs.push_back({buf,cur});buf.clear();}
    };
    for(size_t i=0;i<raw.size();){
        if(raw[i]=='\\'&&i+1<raw.size()){
            // 转义: 跳过反斜杠,加入下一个字符
            if(raw[i+1]=='n')buf+='\n';
            else buf+=raw[i+1];
            i+=2;continue;
        }
        if(raw[i]=='['){
            size_t j=raw.find(']',i+1);
            if(j!=string::npos){
                flush();
                string tag=low(raw.substr(i+1,j-i-1));
                if(tag=="b"||tag=="bold")cur.b=true;
                else if(tag=="u"||tag=="under")cur.u=true;
                else if(tag=="i"||tag=="inv"||tag=="invert")cur.i=true;
                else if(tag=="bl"||tag=="blink")cur.bl=true;
                else if(tag=="reset"||tag=="/"||tag=="0")cur=StyleState();
                else if(tag.rfind("fg:",0)==0)cur.fg=parseColor(tag.substr(3));
                else if(tag.rfind("bg:",0)==0)cur.bg=parseColor(tag.substr(3));
                else if(tag.rfind("color:",0)==0)cur.fg=parseColor(tag.substr(6));
                else{Color c=parseColor(tag);if(c!=Color::Default)cur.fg=c;}
                i=j+1;continue;
            }
        }
        if(raw[i]=='\n'){flush();buf+='\n';flush();i++;continue;}
        buf+=raw[i++];
    }
    flush();
    return segs;
}
static Element renderStyledStr(const string& txt,const StyleState& st){
    Element e=text(txt);
    if(st.b)e=e|bold;
    if(st.u)e=e|underlined;
    if(st.i)e=e|inverted;
    if(st.bl)e=e|blink;
    if(st.fg!=Color::Default)e=e|color(st.fg);
    if(st.bg!=Color::Default)e=e|bgcolor(st.bg);
    return e;
}
static Element richLine(const string& raw){
    string r=VARS.resolve(raw);
    auto segs=parseRich(r);
    if(segs.empty())return text("");
    // 处理多行(允许 \n 切分)
    Elements lines;Elements cur;string curTxt;StyleState curSt;
    auto pushCur=[&](){
        if(!curTxt.empty())cur.push_back(renderStyledStr(curTxt,curSt));
    };
    auto pushLine=[&](){
        pushCur();
        if(!cur.empty()){lines.push_back(hbox(cur));cur.clear();curTxt.clear();}
    };
    for(auto&sg:segs){
        const string&t=sg.first;const StyleState&st=sg.second;
        size_t i=0;
        while(i<t.size()){
            size_t p=t.find('\n',i);
            if(p==string::npos){
                string sub=t.substr(i);
                if(!sub.empty()){
                    if(st.fg!=curSt.fg||st.bg!=curSt.bg||st.b!=curSt.b||st.u!=curSt.u||st.i!=curSt.i||st.bl!=curSt.bl){
                        pushCur();curTxt.clear();
                        curSt=st;
                    }
                    curTxt+=sub;pushCur();curTxt.clear();
                }
                break;
            }else{
                string sub=t.substr(i,p-i);
                if(!sub.empty()){
                    if(st.fg!=curSt.fg||st.bg!=curSt.bg||st.b!=curSt.b||st.u!=curSt.u||st.i!=curSt.i||st.bl!=curSt.bl){
                        pushCur();curTxt.clear();
                        curSt=st;
                    }
                    curTxt+=sub;pushCur();
                }
                pushLine();
                i=p+1;
            }
        }
    }
    pushLine();
    if(lines.empty())return text("");
    if(lines.size()==1)return lines[0];
    return vbox(lines);
}

// 打字机效果: 截取前 N 个"可见"字符
static int visibleCharCount(const string& s){
    int n=0;
    for(size_t i=0;i<s.size();i++){
        if(s[i]=='\\'&&i+1<s.size()){i++;n++;continue;}
        if(s[i]=='['){
            size_t j=s.find(']',i+1);
            if(j!=string::npos){i=j;continue;}
        }
        if(s[i]!='\n')n++;
    }
    return n;
}
static string visibleSubstr(const string& s,int maxChars){
    if(maxChars<0)maxChars=0;
    string o;int n=0;
    for(size_t i=0;i<s.size()&&n<maxChars;i++){
        if(s[i]=='\\'&&i+1<s.size()){
            if(s[i+1]=='n')o+='\n';else o+=s[i+1];
            i++;n++;continue;
        }
        if(s[i]=='['){
            size_t j=s.find(']',i+1);
            if(j!=string::npos){
                o+=s.substr(i,j-i+1);
                i=j;continue;
            }
        }
        if(s[i]!='\n'){o+=s[i];n++;}
    }
    return o;
}
static Element richLineAnim(const string& raw,int maxChars){
    string r=VARS.resolve(raw);
    return richLine(visibleSubstr(r,maxChars));
}

// ============================================================
// 渲染面板
// ============================================================
static Element renderPanelContent(Panel&p){
    Elements ls;
    int total=(int)p.lines.size();
    // 估算面板正文可见行数(标题/分隔线/边框/输入栏等固定开销)
    int avail=g_termH-8; if(avail<1)avail=1;
    int top=p.scroll;
    if(top<0)top=0;
    int maxTop=0; if(total>avail)maxTop=total-avail;  // 最多滚到末屏首行
    if(top>maxTop)top=maxTop;
    p.scroll=top;   // 回写, 防止 CatchEvent 计算越界
    p.follow=(top>=maxTop);  // 在底部则保持自动跟随, 否则视为用户正在查看历史
    for(int i=top;i<total;i++){
        if(S.waitType&&i==S.typeLine&&S.typeLine>=0){
            ls.push_back(richLineAnim(p.lines[i],S.typePos));
        }else{
            ls.push_back(richLine(p.lines[i]));
        }
    }
    if(ls.empty())ls.push_back(text(" ")|dim);
    return vbox(ls);
}
static Element renderAll(){
    Elements panelEls;
    vector<string> names;
    for(auto&kv:S.panels)if(kv.second.visible)names.push_back(kv.first);
    sort(names.begin(),names.end());
    for(auto&nm:names){
        Panel&p=S.panels[nm];
        bool isCur=(nm==S.curPanel);
        Element title=text(" "+nm+" ")|bold|color(isCur?Color::Yellow:Color::GrayLight);
        Element body=renderPanelContent(p);
        int avail=g_termH-8; if(avail<1)avail=1;
        body=body|size(HEIGHT, EQUAL, avail);   // 固定视口高度, 溢出裁切(避免 flex 撑大布局导致滚动失效)
        Element bx=vbox({title,separator(),body})|border;
        if(isCur)bx=bx|color(Color::Yellow);
        panelEls.push_back(bx|flex);
    }
    if(panelEls.empty()){Panel pp;S.panels["main"]=pp;return text(" (no panel) ")|border;}
    if(panelEls.size()==1)return panelEls[0];
    return hbox(panelEls)|flex;
}

// 面板内容滚动: 上下键(及 PgUp/PgDn/Home/End)滚动"所有可见面板"内容.
// 始终生效(含 CHOICE/INPUT 期间), 让 UP/DOWN 在任何状态下都能翻看面板历史;
// 选项用数字键 1-9 + 回车选择, 输入框用 ←/→ 移光标, 互不冲突.
// (流程图 showFlow 期间由专门分支处理, 此处跳过)
static bool handlePanelScroll(Event ev){
    if(S.showFlow)return false;
    bool isScrollKey=(ev==Event::ArrowUp||ev==Event::ArrowDown||
                      ev==Event::PageUp||ev==Event::PageDown||
                      ev==Event::Home||ev==Event::End);
    if(!isScrollKey)return false;
    // INPUT 模式下 Home/End 留给输入框移动光标(见 CatchEvent 的 INPUT 分支), 不滚面板;
    // 其余滚动键(↑/↓/PgUp/PgDn)在 INPUT 期间仍滚面板, 与 ←/→ 移光标互不冲突
    if(S.waiting && S.lastEvent=="INPUT" && (ev==Event::Home||ev==Event::End))return false;
    for(auto&kv:S.panels){
        if(!kv.second.visible)continue;
        Panel&p=kv.second;
        if(ev==Event::ArrowUp){ if(p.scroll>0)p.scroll--; p.follow=false; }
        else if(ev==Event::ArrowDown){ p.scroll++; }
        else if(ev==Event::PageUp){ p.scroll-=10; if(p.scroll<0)p.scroll=0; p.follow=false; }
        else if(ev==Event::PageDown){ p.scroll+=10; }
        else if(ev==Event::Home){ p.scroll=0; p.follow=false; }
        else if(ev==Event::End){ p.scroll=(int)p.lines.size(); p.follow=true; }
    }
    return true;
}
static Element renderInputBox(){
    if(!S.choices.empty()){
        Elements es;
        for(size_t i=0;i<S.choices.size();i++){
            string ch=itoa2(i+1)+". "+S.choices[i];
            Element e=text(ch);
            if((int)i==S.choiceIdx)e=e|color(Color::Black)|bgcolor(Color::White)|bold;
            es.push_back(e);
        }
        return vbox({text(" [选择] ")|bold|color(Color::Yellow),vbox(es)})|border;
    }
    if(S.waiting&&S.lastEvent=="INPUT"){
        // 显示当前输入缓冲,光标位置用反显下划线
        string pre=S.inputBuf.substr(0,S.inputPos);
        string cur=(S.inputPos<(int)S.inputBuf.size())?string(1,S.inputBuf[S.inputPos]):"";
        string post=(S.inputPos<(int)S.inputBuf.size())?S.inputBuf.substr(S.inputPos+1):"";
        Elements es;
        es.push_back(text(" > ")|bold|color(Color::Green));
        es.push_back(text(pre));
        if(!cur.empty())es.push_back(text(cur)|inverted|underlined);
        es.push_back(text("_")|blink);
        es.push_back(text(post));
        es.push_back(text(" [回车确认] ")|dim);
        return hbox(es)|border;
    }
    return text("");
}
static Element renderDebugBar(){
    if(!S.debug)return text("");
    Elements dbg;
    dbg.push_back(text(" DEBUG  脚本:"+SCRIPT_FILE+"  行:"+itoa2(S.curLine)+"  IP:"+itoa2(g_ip)+
                       "  面板:"+S.curPanel+"  F2切换 ")|bold|color(Color::Red));
    dbg.push_back(separator());
    dbg.push_back(text(" 事件:["+S.lastKey+"] mouse:["+S.lastMouse+"]"));
    dbg.push_back(text(" 变量:"));
    dbg.push_back(paragraph(VARS.dump())|color(Color::GrayLight));
    dbg.push_back(separator());
    return vbox(dbg)|border|color(Color::Red);
}

// ============================================================
// 指令执行
// ============================================================
static int findLabelIdx(const string& lbl,int from=0){
    // 支持 :label / ::label / label: 三种写法
    string tgt1=lbl+":";
    string tgt2=":"+lbl+":";
    string tgt3="::"+lbl+":";
    for(int i=from;i<(int)g_script.size();i++){
        string t=tr(g_script[i]);
        if(t==tgt1||t==tgt2||t==tgt3||t=="::"+lbl||t==":"+lbl)return i;
    }
    return -1;
}
static int evalExpr(const string& e){
    string t=tr(e);
    if(t.empty())return 0;
    // 剥掉最外层配对的括号, 使 ((1+1)) 等纯括号表达式也能求值
    while(t.size()>=2 && t.front()=='(' && t.back()==')'){
        int d=0;bool balanced=true;
        for(size_t i=0;i<t.size();i++){
            if(t[i]=='(')d++;
            else if(t[i]==')'){d--; if(d==0 && i!=(t.size()-1)){balanced=false;break;}}
        }
        if(balanced && d==0)t=t.substr(1,t.size()-2);
        else break;
    }
    if(VARS.has(t))return(int)VARS.getI(t);
    // 仅当整个字符串都是数字字面量时直接返回,否则交给运算符解析
    {
        bool allNum=true;
        for(size_t i=0;i<t.size();i++){
            char c=t[i];
            if(i==0&&(c=='-'||c=='+'))continue;
            if(!isdigit((unsigned char)c)){allNum=false;break;}
        }
        if(allNum)return atoiS(t);
    }
    // 优先级: 先找最低优先级运算符
    int paren=0;int minPrio=99;int opPos=-1;char op=0;
    for(size_t i=0;i<t.size();i++){
        char c=t[i];
        if(c=='('){paren++;continue;}
        if(c==')'){paren--;continue;}
        if(paren>0)continue;
        int p=99;
        if(c=='*'||c=='/'||c=='%')p=2;
        else if(c=='+'||c=='-')p=1;
        if(p<=minPrio){minPrio=p;opPos=(int)i;op=c;}
    }
    if(opPos<0)return 0;
    // 切分后 trim 再递归,避免空格导致 atoll 解析失败
    string lt=tr(t.substr(0,opPos));
    string rt=tr(t.substr(opPos+1));
    // 去除最外层配对括号 (1+2) -> 1+2
    auto stripParen=[](string s)->string{
        while(s.size()>=2&&s.front()=='('&&s.back()==')'){
            int d=0;bool ok=true;
            for(size_t i=0;i<s.size();i++){
                if(s[i]=='(')d++;
                else if(s[i]==')'){d--;if(d==0&&i!=s.size()-1){ok=false;break;}}
            }
            if(ok)s=s.substr(1,s.size()-2);else break;
        }
        return s;
    };
    long long a=evalExpr(stripParen(lt));
    long long b=evalExpr(stripParen(rt));
    if(op=='+')return(int)(a+b);
    if(op=='-')return(int)(a-b);
    if(op=='*')return(int)(a*b);
    if(op=='/')return b?int(a/b):0;
    if(op=='%')return b?int(a%b):0;
    return 0;
}
static string resolveStr(const string& e){
    string t=tr(e);
    if(t.empty())return "";
    if(t.front()=='"'||t.front()=='\'')return stripQuote(t);
    string o;
    auto parts=splitTok(t);
    for(size_t i=0;i<parts.size();i++){
        auto&p=parts[i];
        if(i>0)o+=' ';
        if(p.size()>=2&&p.front()=='"'&&p.back()=='"')o+=stripQuote(p);
        else o+=VARS.resolve(p);
    }
    return o;
}
static void doSay(const string& text,bool newline=true){
    Panel&p=S.panels[S.curPanel];
    if(!newline){
        if(p.lines.empty())p.add("");
        if(!p.lines.empty())p.lines.back()+=text;
    }else p.add(text);
    S.waitType=true;S.typeLine=(int)p.lines.size()-1;S.typePos=0;
    S.typeT0=nowMs();   // 记录起始时间,用于批量推进
    S.typePanel=S.curPanel;  // 记录打字机所属面板,避免多面板时取错行
}
// 立即完成当前打字机行(用户主动跳过时使用)
static void flushTypewriter(){
    if(S.waitType&&S.typeLine>=0){
        Panel* pp=&S.panels[S.typePanel.empty()?S.curPanel:S.typePanel];
        if(pp&&S.typeLine<(int)pp->lines.size()){
            S.typePos=visibleCharCount(pp->lines[S.typeLine]);
        }
    }
    S.waitType=false;S.typeLine=-1;
    S.sleepUntil=0;
}
// 解析面板名参数: 若空返回 S.curPanel (用于 CLEAR/EFFECT/CURSOR/HIDE/SHOW/LOG 等)
static inline string argPanel(const string& name){
    return name.empty() ? S.curPanel : name;
}
// ============================================================
// 流程图: 解析 + 渲染
// ============================================================

// 提取标签(若本行是标签定义,返回规范名;否则空)
//   支持: name: / :name / :name: / ::name / ::name:
static string flowLabelOf(const string& line){
    string t=tr(line);
    if(t.empty()||t.find(' ')!=string::npos)return "";   // 标签行无空格
    if(t.back()==':')return t.substr(0,t.size()-1);
    if(t.front()==':'){
        string body=t.substr(1);
        if(!body.empty()&&body.back()==':')body.pop_back();
        return body;
    }
    return "";
}

// 在 g_flow 中按 IP 找节点下标
static int flowIdxByIp(int ip){
    for(int i=0;i<(int)g_flow.size();i++)if(g_flow[i].ip==ip)return i;
    return -1;
}

// 从标签名找节点下标(找首个 ip==标签行的节点)
static int flowIdxByLabel(const string& lbl){
    for(int i=0;i<(int)g_flow.size();i++){
        if(!g_flow[i].label.empty()&&g_flow[i].label==lbl)return i;
    }
    return -1;
}

// 扫描 g_script 建图 (一次性, 缓存到 g_flow)
// 工具: 从 "goto xxx" / "call xxx" 提取标签
static string pp_to_label(const string& s){
    size_t sp=s.find(' ');
    if(sp==string::npos)return s;
    return s.substr(sp+1);
}

// 扫描 g_script 建图 (一次性, 缓存到 g_flow)
// 思路: 略过空行/注释/标签占位, 把 "命令行 + 紧随的标签行" 合并成一个节点
//       块语句 (IF ... ENDIF) 算一个节点, 占据从 IF 行到 ENDIF 行的范围
static void buildFlow(){
    g_flow.clear();
    g_flowCurrentIdx=-1;
    if(g_script.empty())return;

    // 第一遍: 收集所有标签行号 + 命令行
    int n=(int)g_script.size();
    for(int i=0;i<n;i++){
        string t=tr(g_script[i]);
        if(t.empty()||t[0]=='#'||t[0]==';')continue;

        string lbl=flowLabelOf(t);
        if(!lbl.empty()){
            FlowNode nd;
            nd.ip=i;
            nd.ipEnd=i;
            nd.label=lbl;
            nd.kind=FK_NOP;
            nd.text=":"+lbl;
            g_flow.push_back(nd);
            continue;
        }
        auto pp=splitTok(t);
        if(pp.empty())continue;
        string cmd=up(pp[0]);
        if(cmd=="REM"||cmd=="//")continue;

        FlowNode nd;
        nd.ip=i;
        nd.ipEnd=i;
        if(cmd=="SAY"||cmd=="S"||cmd=="PRINT"||cmd=="P"||cmd=="SAY+"||cmd=="S+"){
            nd.kind=FK_SAY;
            string s=joinRemain(pp,1);
            if(s.size()>28)s=s.substr(0,26)+"..";
            nd.text=s;
        }else if(cmd=="NARRATE"||cmd=="N"||cmd=="LN"){
            nd.kind=FK_SAY;
            string s=joinRemain(pp,1);
            if(s.size()>24)s=s.substr(0,22)+"..";
            nd.text="(旁白) "+s;
        }else if(cmd=="IF"||cmd=="IFE"){
            nd.kind=FK_BRANCH;
            string cond;
            for(size_t k=1;k<pp.size();k++){
                if(k>1)cond+=" ";
                cond+=pp[k];
            }
            if(cond.size()>26)cond=cond.substr(0,24)+"..";
            nd.text="if "+cond;
            int depth=1;
            int j=i+1;
            for(;j<n;j++){
                string u=tr(g_script[j]);auto qq=splitTok(u);
                if(qq.empty())continue;
                string cc=up(qq[0]);
                if(cc=="IF"||cc=="IFE")depth++;
                else if(cc=="ENDIF"){depth--;if(depth==0)break;}
            }
            nd.ipEnd=j;
        }else if(cmd=="GOTO"||cmd=="JUMP"){
            nd.kind=FK_GOTO;
            nd.text="goto "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="CALL"||cmd=="GOSUB"){
            nd.kind=FK_CALL;
            nd.text="call "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="CHOICE"||cmd=="MENU"||cmd=="CHOICES"){
            nd.kind=FK_CHOICE;
            int nopt=(int)pp.size()-1;
            if(nopt<0)nopt=0;
            nd.text="choice ["+itoa2(nopt)+"]";
        }else if(cmd=="INPUT"||cmd=="ASK"){
            nd.kind=FK_INPUT;
            nd.text="input"+(pp.size()>=2?(" ->"+pp[1]):"");
        }else if(cmd=="RUN"||cmd=="CHAIN"){
            nd.kind=FK_RUN;
            nd.text="run "+(pp.size()>=2?pp[1]:"?");
        }else if(cmd=="EXIT"||cmd=="QUIT"||cmd=="END"){
            nd.kind=FX_EXIT;
            nd.text="exit";
        }else{
            nd.kind=FK_SAY;
            string s=joinRemain(pp,0);
            if(s.size()>28)s=s.substr(0,26)+"..";
            nd.text=s;
        }
        g_flow.push_back(nd);
    }

    // 第二遍: 补边
    for(int i=0;i<(int)g_flow.size();i++){
        FlowNode& nd=g_flow[i];
        int defaultSucc=-1;
        for(int k=i+1;k<(int)g_flow.size();k++){
            if(g_flow[k].kind!=FK_NOP){defaultSucc=k;break;}
        }
        if(nd.kind==FK_BRANCH){
            int afterEnd=-1;
            int j=flowIdxByIp(nd.ipEnd);
            if(j>=0){
                for(int k=j+1;k<(int)g_flow.size();k++){
                    if(g_flow[k].kind!=FK_NOP){afterEnd=k;break;}
                }
            }
            nd.succFalse.push_back(afterEnd);
            int trueSucc=-1;
            for(int k=i+1;k<(int)g_flow.size();k++){
                if(g_flow[k].kind!=FK_NOP){trueSucc=k;break;}
            }
            nd.succ.push_back(trueSucc);
        }else if(nd.kind==FK_GOTO){
            int tgt=flowIdxByLabel(pp_to_label(nd.text));
            nd.succ.push_back(tgt);
        }else if(nd.kind==FK_CALL){
            int tgt=flowIdxByLabel(pp_to_label(nd.text));
            nd.succ.push_back(tgt);
            nd.succ.push_back(defaultSucc);
        }else if(nd.kind==FK_RUN){
            nd.succ.push_back(-1);
        }else if(nd.kind==FX_EXIT){
            // 无后继
        }else{
            nd.succ.push_back(defaultSucc);
        }
    }

    g_flowCurrentIdx=flowIdxByIp(g_ip);
    if(g_flowCurrentIdx>=0)g_flow[g_flowCurrentIdx].isCurrent=true;
}

// 渲染: 节点用一行, 节点之间用 ↓ / 条件分支用条件 true|false
//   每节点: [#ip] 形状  文本
//   边: 居于两节点之间, 独占一行
static Element renderFlow(){
    if(g_flowDirty||g_flow.empty()){
        buildFlow();
        g_flowDirty=false;
    }

    // 标题栏
    Element title=vbox({
        text(" [bg:cyan][fg:black] FLOW [/]  "+SCRIPT_FILE+
             "  ("+itoa2(g_flow.size())+" nodes, ip="+itoa2(g_ip)+")  "+
             "F/Esc:close  Up/Down:scroll  PgUp/PgDn:page  Home/End:jump ")|bold
    });
    Element hdr=title|color(Color::Cyan);

    // 内容: 把每节点 + 其下边画成"行"列表
    Elements rows;
    auto nodeShape=[](FlowKind k)->string{
        switch(k){
            case FK_SAY:    return "[SAY]";
            case FK_BRANCH: return "[IF ]";
            case FK_MERGE:  return "[END]";
            case FK_GOTO:   return "[GO ]";
            case FK_CALL:   return "[CAL]";
            case FK_CHOICE: return "[CHC]";
            case FK_INPUT:  return "[IN ]";
            case FK_RUN:    return "[RUN]";
            case FX_EXIT:   return "[END]";
            default:        return "[.. ]";
        }
    };
    for(int i=0;i<(int)g_flow.size();i++){
        const FlowNode& nd=g_flow[i];
        // 节点行
        string ipStr=itoa2(nd.ip);
        // 4 位宽 IP
        while((int)ipStr.size()<4)ipStr=" "+ipStr;
        string line;
        if(!nd.label.empty()){
            line=" L "+ipStr+" :"+nd.label;
        }else{
            line="   "+ipStr+" "+nodeShape(nd.kind)+"  "+nd.text;
        }
        Element ndEl=text(line);
        if(nd.isCurrent)ndEl=ndEl|bold|color(Color::Yellow);
        else if(!nd.label.empty())ndEl=ndEl|color(Color::Magenta);
        else if(nd.kind==FK_BRANCH)ndEl=ndEl|color(Color::Cyan);
        else if(nd.kind==FK_GOTO)ndEl=ndEl|color(Color::Green);
        else if(nd.kind==FK_CALL)ndEl=ndEl|color(Color::Blue);
        else if(nd.kind==FK_RUN)ndEl=ndEl|color(Color::Red);
        else if(nd.kind==FX_EXIT)ndEl=ndEl|dim|color(Color::Red);
        else if(nd.kind==FK_CHOICE)ndEl=ndEl|color(Color::Yellow);
        else if(nd.kind==FK_INPUT)ndEl=ndEl|color(Color::White);
        rows.push_back(ndEl);

        // 边: 每条 succ 画一行箭头; BRANCH 画两条 (true / false)
        if(nd.kind==FK_BRANCH){
            // true 分支
            string tArrow="   |    |--> true";
            if(!nd.succ.empty()&&nd.succ[0]>=0)tArrow+=" -> #"+itoa2(g_flow[nd.succ[0]].ip);
            else tArrow+=" (none)";
            rows.push_back(text(tArrow)|color(Color::Green));
            // false 分支
            string fArrow="   |    |--> false";
            if(!nd.succFalse.empty()&&nd.succFalse[0]>=0)fArrow+=" -> #"+itoa2(g_flow[nd.succFalse[0]].ip);
            else fArrow+=" (none)";
            rows.push_back(text(fArrow)|color(Color::Red));
        }else if(!nd.succ.empty()&&nd.succ[0]>=0){
            string arrow="   |    v";
            if(nd.succ.size()>=2&&nd.succ[1]>=0){
                // CALL 后的双后继
                arrow="   |    |--> call-ret -> #"+itoa2(g_flow[nd.succ[0]].ip);
                rows.push_back(text(arrow)|color(Color::Blue));
                arrow="   |    v";
            }
            arrow+=" -> #"+itoa2(g_flow[nd.succ[0]].ip);
            rows.push_back(text(arrow)|dim);
        }
    }
    if(rows.empty())rows.push_back(text(" (空脚本) ")|dim);

    // 滚动: 屏幕高度未知, 用 ftxui 提供的 screen.dim 限定
    //   简单稳妥做法: 直接把 rows 全部塞进 vbox, 由 ftxui 自然布局
    //   屏幕外的行被裁掉; 实际"滚动"由 ftxui 在我们返回 element 时按
    //   可用高度决定; 但 ftxui vbox 不支持主动指定 top 偏移, 所以
    //   下面用 S.flowScroll 裁掉前面的行
    int total=(int)rows.size();
    int top=S.flowScroll;
    if(top<0)top=0;
    if(top>total-1)top=total>0?total-1:0;
    S.flowScroll=top;   // 回写, 防止 CatchEvent 计算越界
    // 裁切: 跳过 [0, top) 行, 取 [top, end)
    Elements visible;
    visible.reserve(total-top);
    for(int k=top;k<total;k++)visible.push_back(rows[k]);

    int avail=g_termH-5; if(avail<1)avail=1;
    Element body=vbox(visible)|size(HEIGHT, EQUAL, avail);
    // 滚动指示器 (右上角显示当前位置)
    string ind="";
    if(total>0){
        int curLine=0;
        if(g_flowCurrentIdx>=0){
            // 当前 IP 节点所在行下标
            curLine=g_flowCurrentIdx*2;  // 节点 + 边 各占一行, 近似
        }
        ind="  ("+itoa2(top+1)+"/"+itoa2(total)+"  ip-row~"+itoa2(curLine+1)+")";
    }
    Element scrollInd=text(ind)|dim|color(Color::GrayLight);
    return vbox({hdr,separator(),body,scrollInd})|border|flex;
}

// 重置游戏运行态(变量、面板、等待/输入状态、打字机/震屏/闪烁)
// 用于 RUN/CHAIN 切换剧本时清空旧脚本残留
static void resetGameState(){
    VARS.vs.clear();           // 清空所有变量(int + str)
    S.panels.clear();          // 清空所有面板(包括 main)
    S.choices.clear();
    S.choiceIdx=0;
    S.inputBuf.clear();
    S.inputPos=0;
    S.waiting=false;
    S.waitType=false;
    S.lastEvent.clear();
    S.lastKey.clear();
    S.lastMouse.clear();
    S.shaking=false;S.blinking=false;
    S.shakeDur=S.shakeInt=S.shakeOff=S.shakeT0=0;
    S.blinkDur=S.blinkOn=S.blinkT0=0;
    S.typePos=0;S.typeLine=-1;S.typeT0=0;S.typePanel="";
    S.sleepUntil=0;
    S.curLine=0;
    S.showFlow=false;
    S.flowScroll=0;
    S.loadResumeIp=-1;        // 清除可能残留的读档恢复 IP (避免 RUN/CHAIN 后误跳转)
    S.ifTaken.clear();        // 清除可能残留的 IF/ELSE 配对状态 (避免跨剧本误判)
    g_flow.clear();            // 流程图缓存失效
    g_flowDirty=true;
    flushTypewriter();
}
// 用户主动存档: 序列化为 Base64 + XOR 密文写入文件
static bool userSave(const string& fn){
    string d=VARS.serialize();
    // 记录当前 IP+1, 用户 LOAD 时跳回此处继续
    char ib[96];sprintf(ib,"LINE=%d\nIP=%d\nSCRIPT=%s\nPANEL=%s\n",S.curLine,g_ip+1,SCRIPT_FILE.c_str(),S.curPanel.c_str());
    d+=ib;
    string enc=toBase64(xorCrypt(d,ENC_KEY));
    std::ofstream f(fn,std::ios::binary|std::ios::trunc);
    if(!f){doSay("[fg:red][系统] 存档失败: 无法写入 "+fn+"[/]");return false;}
    f<<enc;
    doSay("[fg:green][系统] 存档成功 -> "+fn+"[/]");
    return true;
}
// 用户主动读档: 还原变量 + 面板 + 跳回存档时的下一行
// resumeMode=true (用户主动/F9) 时, 从存档读 IP 并跳回; false (脚本 LOAD) 时不跳
static bool userLoad(const string& fn,bool resumeMode=false){
    std::ifstream f(fn,std::ios::binary);
    if(!f){doSay("[fg:red][系统] 读档失败: 找不到 "+fn+"[/]");return false;}
    string d;{std::ostringstream ss;ss<<f.rdbuf();d=ss.str();}
    if(d.empty()){doSay("[fg:red][系统] 读档失败: 存档为空[/]");return false;}
    string dec=fromBase64(d);
    dec=xorCrypt(dec,ENC_KEY);
    VARS.deserialize(dec);
    int resumeIp=-1;
    for(auto&l:splitBy(dec,'\n')){
        if(l.empty())continue;
        size_t p1=l.find('=');if(p1==string::npos)continue;
        string k=l.substr(0,p1),v=l.substr(p1+1);
        if(k=="IP"&&!v.empty())resumeIp=atoiS(v);
        else if(k=="LINE")S.curLine=atoiS(v);
        else if(k=="SCRIPT")SCRIPT_FILE=v;
        else if(k=="PANEL")S.curPanel=v;
    }
    if(resumeMode){
        doSay("[fg:green][系统] 读档成功 <- "+fn+"  继续执行 IP="+itoa2(resumeIp)+"[/]");
        S.loadResumeIp=resumeIp;
    }else{
        doSay("[fg:green][系统] 读档成功 <- "+fn+"[/]");
    }
    S.choices.clear();S.inputBuf.clear();S.inputPos=0;
    S.waiting=false;S.lastEvent="";
    flushTypewriter();
    return true;
}
// 判断一行是否为"块 IF" (IF 后无内联动作, 需配合 ENDIF 收尾)
// 用于流程跳过时正确计数块作用域, 避免单行 IF 干扰深度统计
static bool isBlockIfLine(const string& rawLine){
    string t=tr(rawLine);
    if(t.empty())return false;
    auto pp=splitTok(t);
    if(pp.empty())return false;
    string c=up(pp[0]);
    if(c!="IF"&&c!="IFE")return false;
    if(pp.size()<4)return false;            // 至少需要 IF lhs op rhs
    string op=pp[2];
    int opPos=-1;
    for(int i=1;i<(int)pp.size();i++)if(pp[i]==op){opPos=i;break;}
    if(opPos<0||opPos+1>=(int)pp.size())return false;
    string action=opPos+2<(int)pp.size()?joinRemain(pp,opPos+2):"";
    return action.empty();                  // 无内联动作 = 块 IF
}

static void execCmd(const string& line,bool& jumped){
    jumped=false;
    string t=tr(line);
    if(t.empty()||t[0]=='#'||t[0]==';')return;
    // 去除行内 ; 注释(支持 #comment 风格, 避免 ; 后续内容被当参数)
    {size_t sc=t.find(';');if(sc!=string::npos)t=tr(t.substr(0,sc));}
    if(t.empty())return;
    // 标签行: name: / :name / :name: / ::name / ::name:  (整行无空格)
    if(t.find(' ')==string::npos && (t.back()==':' || t.front()==':'))return;
    auto parts=splitTok(t);
    if(parts.empty())return;
    string cmd=up(parts[0]);
    auto getArg=[&](int i)->const string&{static string _e;return i<(int)parts.size()?parts[i]:(_e="",_e);};

    // ===== 基础输出 =====
    if(cmd=="SAY"||cmd=="S"||cmd=="PRINT"||cmd=="P"){
        string s=joinRemain(parts,1);
        if(s.empty())return;
        doSay(resolveStr(s),true);
    }else if(cmd=="SAY+"||cmd=="S+"){
        string s=joinRemain(parts,1);
        doSay(resolveStr(s),false);
    }else if(cmd=="NARRATE"||cmd=="N"||cmd=="LN"){
        string s=joinRemain(parts,1);
        doSay("[i][fg:gray]"+resolveStr(s)+"[/][/]");
    }

    // ===== 面板 =====
    else if(cmd=="CLEAR"||cmd=="CLS"){
        S.panels[argPanel(getArg(1))].clear();
    }else if(cmd=="PANEL"||cmd=="USE"||cmd=="SWITCH"){
        string pn=getArg(1);
        if(!pn.empty())S.curPanel=pn;
    }else if(cmd=="PANELNEW"||cmd=="PN"){
        string pn=getArg(1);if(pn.empty())return;
        if(S.panels.find(pn)==S.panels.end()){
            Panel pp;pp.name=pn;S.panels[pn]=pp;
        }
    }else if(cmd=="HIDE"){string pn=getArg(1);if(S.panels.count(pn))S.panels[pn].visible=false;}
    else if(cmd=="SHOW"){string pn=getArg(1);if(S.panels.count(pn))S.panels[pn].visible=true;}
    else if(cmd=="CLEARALL"){for(auto&kv:S.panels)kv.second.clear();}
    else if(cmd=="CURSOR"||cmd=="GOTOXY"){
        Panel&p=S.panels[S.curPanel];
        p.cur=parts.size()>=3?atoiS(getArg(2)):0;
        p.curCol=parts.size()>=2?atoiS(getArg(1)):0;
    }else if(cmd=="BORDER"||cmd=="B"){
        Panel&p=S.panels[S.curPanel];
        p.addTop(string(40,'='));p.add(string(40,'='));
    }else if(cmd=="LOG"){
        string s=joinRemain(parts,1);
        if(S.panels.find("log")==S.panels.end()){Panel pp;pp.name="log";S.panels["log"]=pp;}
        S.panels["log"].add(resolveStr(s));
    }

    // ===== 变量 =====
    else if(cmd=="SET"){
        if(parts.size()>=3){
            string vn=getArg(1);
            int start=2;
            if(parts.size()>start&&parts[start]=="=")start++;
            string rhs=joinRemain(parts,start);
            if(rhs.size()>=2&&rhs.front()=='"'&&rhs.back()=='"'){VARS.setS(vn,stripQuote(rhs));}
            else{
                bool isExpr=false;
                for(char c:rhs)if(c=='+'||c=='-'||c=='*'||c=='/'||c=='%'){isExpr=true;break;}
                if(isExpr)VARS.setI(vn,evalExpr(rhs));
                else{
                    string r=resolveStr(rhs);
                    bool allNum=!r.empty();
                    for(char c:r)if(!isdigit((unsigned char)c)&&c!='-'){allNum=false;break;}
                    if(allNum)VARS.setI(vn,atoll(r.c_str()));else VARS.setS(vn,r);
                }
            }
        }
    }else if(cmd=="ADD"||cmd=="INC"||cmd=="SUB"||cmd=="DEC"){
        if(parts.size()>=3){
            string vn=getArg(1);
            long long delta=evalExpr(getArg(2));
            if(cmd[0]=='S'||cmd[0]=='D')delta=-delta;
            VARS.setI(vn,VARS.getI(vn)+delta);
        }
    }else if(cmd=="RANDOM"||cmd=="RAND"){
        if(parts.size()>=3){
            long long a=evalExpr(getArg(1)),b=evalExpr(getArg(2));
            if(b<a)std::swap(a,b);
            std::uniform_int_distribution<long long>d(a,b);
            static std::mt19937_64 gen((unsigned)time(nullptr));
            long long v=d(gen);
            VARS.setI("random",v);
            if(parts.size()>=4)VARS.setI(getArg(3),v);
        }
    }

    // ===== 流程控制 =====
    else if(cmd=="IF"||cmd=="IFE"){
        if(parts.size()>=4){
            string lhs=getArg(1),op=getArg(2);
            int opPos=-1;
            for(int i=1;i<(int)parts.size();i++){
                if(parts[i]==op){opPos=i;break;}
            }
        if(opPos<0||opPos+1>=(int)parts.size())return;
        string rhsRaw=parts[opPos+1];
        string rhs=resolveStr(rhsRaw);
        string lv=VARS.getS(lhs);
        bool ok=false;
        if(op=="=="||op=="=")ok=(lv==rhs);
        else if(op=="!="||op=="<>")ok=(lv!=rhs);
        else if(op==">")ok=(VARS.getI(lhs)>atoll(rhs.c_str()));
        else if(op=="<")ok=(VARS.getI(lhs)<atoll(rhs.c_str()));
        else if(op==">=")ok=(VARS.getI(lhs)>=atoll(rhs.c_str()));
        else if(op=="<=")ok=(VARS.getI(lhs)<=atoll(rhs.c_str()));
        // 查找内联 ELSE, 支持单行 IF 的 THEN/ELSE 分界 (如 IF x==1 SAY A ELSE SAY B)
        int elsePos=-1;
        for(int i=opPos+2;i<(int)parts.size();i++){
            if(up(parts[i])=="ELSE"){elsePos=i;break;}
        }
        string action=opPos+2<(int)parts.size()?joinRemain(parts,opPos+2):"";
        string thenAct,elseAct;
        if(elsePos>=0){
            for(int i=opPos+2;i<elsePos;i++){if(i>opPos+2)thenAct+=' ';thenAct+=parts[i];}
            thenAct=stripQuote(thenAct);          // 仅拼接 ELSE 之前的部分
            elseAct=joinRemain(parts,elsePos+1);  // ELSE 之后的部分
        }else thenAct=action;
        bool isBlock=action.empty();
        S.ifTaken.push_back((char)(ok?1:0));   // 记录本 IF 是否命中, 供 ELSE/ENDIF 配对
        if(!ok){
            if(isBlock){
                int depth=1;g_ip++;
                while(g_ip<(int)g_script.size()&&depth>0){
                    string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                    if(!pp.empty()){
                        string cc=up(pp[0]);
                        if(isBlockIfLine(g_script[g_ip]))depth++;   // 仅块 IF 计入作用域深度
                        else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                        else if(cc=="ELSE"&&depth==1){break;}   // 落到 ELSE 行, 由 ELSE 处理器决定
                    }
                    g_ip++;
                }
                jumped=true;  // 块内已跳,避免外层 g_ip++
            }else if(elsePos>=0){
                // 单行 IF 不成立且有内联 ELSE -> 执行 ELSE 动作
                bool aj=false; execCmd(elseAct,aj); jumped=aj;
            }
            // 单行 IF 不成立且无内联 ELSE -> 不设 jumped, g_ip 自然 +1
        }else{
            if(!isBlock){
                bool aj=false; execCmd(thenAct,aj); jumped=aj;
            }
        }
    }
    }else if(cmd=="ELSE"){
        bool taken = !S.ifTaken.empty() && S.ifTaken.back()==1;
        if(taken){
            // IF 已命中 THEN 分支 -> 整个 else (内联动作 + else 体) 都应跳过, 跳到 ENDIF
            int depth=1;g_ip++;
            while(g_ip<(int)g_script.size()&&depth>0){
                string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                if(!pp.empty()){
                    string cc=up(pp[0]);
                    if(isBlockIfLine(g_script[g_ip]))depth++;
                    else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                }
                g_ip++;
            }
            jumped=true;
        }else{
            // IF 未命中 -> 执行 else 分支
            if(parts.size()>=2){
                string act=joinRemain(parts,1);
                bool ajumped=false;
                execCmd(act,ajumped);
                if(!ajumped){            // 内联动作未跳转(如 SAY), 仍需跳过到 ENDIF
                    int depth=1;g_ip++;
                    while(g_ip<(int)g_script.size()&&depth>0){
                        string u=tr(g_script[g_ip]);auto pp=splitTok(u);
                        if(!pp.empty()){
                            string cc=up(pp[0]);
                            if(isBlockIfLine(g_script[g_ip]))depth++;
                            else if(cc=="ENDIF"){depth--;if(depth==0)break;}
                        }
                        g_ip++;
                    }
                }
                jumped=true;
            }
            // 块 else (无内联动作): 不跳转, 自然流入 else 体 (由主循环 g_ip++)
        }
    }else if(cmd=="ENDIF"){
        if(!S.ifTaken.empty())S.ifTaken.pop_back();   // 退出一层 IF/ELSE 配对
    }
    else if(cmd=="GOTO"||cmd=="JUMP"){
        if(parts.size()>=2){
            string lbl=getArg(1);
            int idx=findLabelIdx(lbl,g_ip+1);if(idx<0)idx=findLabelIdx(lbl,0);
            if(idx>=0){g_ip=idx;jumped=true;}
        }
    }else if(cmd=="CALL"||cmd=="GOSUB"){
        if(parts.size()>=2){
            string lbl=getArg(1);
            int idx=findLabelIdx(lbl,0);if(idx>=0){g_ip=idx;jumped=true;}
        }
    }

    // ===== 用户交互 =====
    else if(cmd=="CHOICE"||cmd=="MENU"||cmd=="CHOICES"){
        S.choices.clear();S.choiceIdx=0;
        for(size_t i=1;i<parts.size();i++)S.choices.push_back(resolveStr(parts[i]));
        S.waiting=true;S.lastEvent="CHOICE";
    }else if(cmd=="INPUT"||cmd=="ASK"){
        S.inputBuf.clear();S.inputPos=0;
        S.inputMax=parts.size()>=3?atoiS(getArg(2)):64;   // 默认最大 64 字符
        S.inputVar=parts.size()>=2?getArg(1):string("input");
        S.waiting=true;S.lastEvent="INPUT";
    }else if(cmd=="WAIT"||cmd=="SLEEP"||cmd=="PAUSE"){
        int ms=parts.size()>=2?atoiS(getArg(1)):500;
        S.sleepUntil=(int)nowMs()+ms;
    }

    // ===== 效果 =====
    else if(cmd=="TYPESPEED"||cmd=="TYPERATE"){
        S.typeSpeed=parts.size()>=2?atoiS(getArg(1)):20;
    }else if(cmd=="SHAKE"){
        S.shaking=true;
        S.shakeInt=parts.size()>=2?atoiS(getArg(1)):2;
        S.shakeDur=parts.size()>=3?atoiS(getArg(2)):500;
        S.shakeT0=(int)nowMs();
    }else if(cmd=="BLINK"){
        S.blinking=true;
        S.blinkStep=parts.size()>=2?atoiS(getArg(1)):300;
        S.blinkDur=parts.size()>=3?atoiS(getArg(2)):2000;
        S.blinkT0=(int)nowMs();S.blinkOn=true;
    }else if(cmd=="EFFECT"||cmd=="FX"){
        string pn=argPanel(getArg(1));
        string kind=getArg(2);
        int dur=parts.size()>=4?atoiS(getArg(3)):500;
        if(S.panels.count(pn)){Effect e;e.kind=kind;e.dur=dur;S.panels[pn].fx.push_back(e);}
    }else if(cmd=="BEEP"||cmd=="SOUND"){printf("\a");fflush(stdout);}

    // ===== 存档 =====
    else if(cmd=="SAVE"){
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        // 脚本 SAVE: 不记 IP, 由脚本作者用 GOTO 决定 LOAD 后续位置
        S.loadResumeIp=-1;
        userSave(fn);
    }else if(cmd=="LOAD"){
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        // 脚本 LOAD: 还原变量但不自动跳转, 由脚本显式 GOTO
        userLoad(fn,false);
    }else if(cmd=="USERSAVE"||cmd=="F5"){
        // 脚本中调用相当于 F5 主动存档: 记 IP+1, 供 userLoad 跳回
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        S.loadResumeIp=g_ip+1;   // 下一行 IP
        userSave(fn);
    }else if(cmd=="USERLOAD"||cmd=="F9"){
        // 脚本中调用相当于 F9 主动读档: 跳回上次 userSave 的下一行
        string fn=parts.size()>=2?getArg(1):SAVE_FILE;
        if(userLoad(fn,true)){jumped=true;}   // jumped 让主循环下一轮 tick 用 loadResumeIp 跳
    }else if(cmd=="RUN"||cmd=="CHAIN"){
        if(parts.size()>=2){
            string fn=getArg(1);
            std::ifstream f(fn);if(f){
                resetGameState();   // 清空旧剧本的变量/面板/状态
                S.curPanel="main";  // 重置当前面板指针
                g_script.clear();string l;while(getline(f,l))g_script.push_back(l);
                g_ip=0;jumped=true;SCRIPT_FILE=fn;
            }
        }
    }

    // ===== 调试/帮助/退出 =====
    else if(cmd=="DEBUG"){S.debug=!S.debug;}
    else if(cmd=="HELP"||cmd=="?"||cmd=="H"){
        Panel&p=S.panels[S.curPanel];
        p.add("[u][fg:yellow]=== PlotEngine 指令集 ===[/][/]");
        p.add("[b]SAY[/] <text>         显示文本(打字机效果)");
        p.add("[b]NARRATE[/] <text>     灰色旁白");
        p.add("[b]SET[/] v expr         设置变量(int/str/表达式)");
        p.add("[b]ADD/SUB[/] v n        增减");
        p.add("[b]RANDOM[/] a b [v]     随机数存入v");
        p.add("[b]IF[/] v op val        条件");
        p.add("[b]ELSE/ENDIF[/]         分支");
        p.add("[b]GOTO[/] label         跳转");
        p.add("[b]CHOICE[/] a b c...    选项(数字键1-9)");
        p.add("[b]INPUT[/] [var]        玩家输入");
        p.add("[b]WAIT[/] ms            等待");
        p.add("[b]PANEL[/] name         切换面板");
        p.add("[b]PANELNEW[/] name      新建面板");
        p.add("[b]CLEAR[/] [name]       清空面板");
        p.add("[b]SHAKE n ms[/]         抖动效果");
        p.add("[b]BLINK t ms[/]         闪烁效果");
        p.add("[b]SAVE/LOAD[/] [file]   加密存档");
        p.add("[b]DEBUG[/]              切换调试");
    }else if(cmd=="EXIT"||cmd=="QUIT"||cmd=="END"){S.run=false;}
    else if(cmd=="REM"||cmd=="//"){}
    else{
        // 未知: 当作 SAY
        string s=joinRemain(parts,0);
        if(!s.empty())doSay(resolveStr(s),true);
    }
    S.curLine=g_ip;
}

// ============================================================
// 脚本 & 主循环
// ============================================================
static bool loadScript(const string& fn){
    std::ifstream f(fn);if(!f)return false;
    g_script.clear();string l;
    while(getline(f,l))g_script.push_back(l);
    g_ip=0;
    return !g_script.empty();
}
static void tick(long long t){
    // 流程图覆盖层期间冻结游戏: 不推进 IP, 不推进打字机
    if(S.showFlow)return;
    // 读档恢复: 主循环开头先处理 S.loadResumeIp (由 userLoad 设置)
    if(S.loadResumeIp>=0){
        g_ip=S.loadResumeIp;
        S.loadResumeIp=-1;
        S.sleepUntil=0;   // 立即可执行
    }
    if(S.shaking){
        if((int)t-S.shakeT0>=S.shakeDur)S.shaking=false;
        else S.shakeOff=((rand()%(S.shakeInt*2+1))-S.shakeInt);
    }
    if(S.blinking){
        if((int)t-S.blinkT0>=S.blinkDur)S.blinking=false;
        else S.blinkOn=(((int)(t-S.blinkT0)/S.blinkStep)%2)==0;
    }
    if(S.waitType&&S.typeLine>=0){
        // 直接用 doSay 记录的面板,避免遍历查找出错
        Panel* pp=&S.panels[S.typePanel.empty()?S.curPanel:S.typePanel];
        if(!pp||S.typeLine>=(int)pp->lines.size()){
            S.waitType=false;S.typeLine=-1;
        }else{
            string&ln=pp->lines[S.typeLine];
            int total=visibleCharCount(ln);
            if(S.typeSpeed<=0){
                // 即时模式: 一次显示完
                S.typePos=total;
            }else{
                // 基于时间批量推进: 经过的毫秒数 / 单字符毫秒
                long long elapsed=t-S.typeT0;
                int want=(int)(elapsed/S.typeSpeed);
                S.typePos+=want;
                S.typeT0+=want*(long long)S.typeSpeed;
            }
            if(S.typePos>=total){
                S.typePos=total;
                S.waitType=false;S.typeLine=-1;
            }
            S.sleepUntil=(int)t+S.typeSpeed;   // 下次再推进
        }
    }
    if(S.sleepUntil>(int)t){if(S.debug)DLOG("[tick] sleepUntil return, t="+itoa2(t)+" sleepUntil="+itoa2(S.sleepUntil));return;}
    if(S.waiting){if(S.debug)DLOG("[tick] waiting return, choices="+itoa2(S.choices.size()));return;}
    if(g_ip>=(int)g_script.size())return;
    int guard=100;
    while(g_ip<(int)g_script.size()&&S.run&&!S.waiting&&S.sleepUntil<=(int)t&&guard-->0){
        bool jumped=false;
        S.curLine=g_ip;
        if(S.debug)DLOG("[tick] exec ip="+itoa2(g_ip)+" line=["+g_script[g_ip]+"]");
        execCmd(g_script[g_ip],jumped);
        if(!jumped)g_ip++;
        if(S.waitType||S.waiting)break;
    }
}

// ============================================================
// 自检模式: --selftest <script>
//   模拟按 1 走完所有 CHOICE, 模拟填 abc/backspace/x/enter 走 INPUT
//   用于无头环境下回归测试
// ============================================================
static int runSelftest(const char* scriptFile){
    SCRIPT_FILE=scriptFile;
    if(!loadScript(SCRIPT_FILE)){std::cerr<<"无法打开 "<<SCRIPT_FILE<<std::endl;return 1;}
    S.panels["main"]=Panel();
    // selftest 也不默认创建 log 面板
    S.curPanel="main";
    VARS.setI("choice",0);
    // 跑 5000 步,记录日志
    S.typeSpeed=0; // 即时显示
    DLOG("[selftest] start script="+SCRIPT_FILE);
    for(int step=0;step<5000;step++){
        if(g_ip>=(int)g_script.size()){DLOG("[selftest] END ip="+itoa2(g_ip));break;}
        // selftest 防死循环: 检测 IP 序列周期性重复
        static int ipHist[20]={0};static int ipHistIdx=0;static int cycleDet=0;
        int prev=ipHist[ipHistIdx];
        ipHist[ipHistIdx]=g_ip;
        ipHistIdx=(ipHistIdx+1)%20;
        if(prev==g_ip&&g_ip!=0)cycleDet++;
        else cycleDet=0;
        if(cycleDet>=15){DLOG("[selftest] ABORT: 死循环 ip="+itoa2(g_ip)+" 重复 "+itoa2(cycleDet)+" 次");break;}
        DLOG("[selftest] step="+itoa2(step)+" ip="+itoa2(g_ip)+
             " line=["+g_script[g_ip]+"]"+
             " waiting="+itoa2(S.waiting)+
             " waitType="+itoa2(S.waitType));
        // 如果遇到 CHOICE,模拟选第一个
        string t1=tr(g_script[g_ip]);
        if(t1.find("CHOICE")==0||t1.find("MENU")==0){
            bool jumped=false;
            execCmd(g_script[g_ip],jumped);
            DLOG("[selftest] simulated choice 1");
            VARS.setI("choice",1);
            S.choices.clear();
            S.waiting=false;S.lastEvent="";
            flushTypewriter();
            g_ip++;
            continue;
        }
        // 如果遇到 INPUT,模拟填默认值并提交(不超过 inputMax)
        if(t1.find("INPUT")==0||t1.find("ASK")==0){
            bool jumped=false;
            execCmd(g_script[g_ip],jumped);
            // 严格模拟键序列: 'a' 'b' 'c' Backspace 'x' Enter
            // 验证 Event::Backspace 和 Event::Return 路径
            S.inputBuf.clear();S.inputPos=0;
            auto pressChar=[&](char c){
                if(c==127||c==8||c=='\b'){
                    if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                }else if(c=='\r'||c=='\n'){
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                }else if(c>=32&&c<127){
                    if((int)S.inputBuf.size()<S.inputMax){
                        S.inputBuf.insert(S.inputBuf.begin()+S.inputPos,c);
                        S.inputPos++;
                    }
                }
            };
            pressChar('a');pressChar('b');pressChar('c');
            DLOG("[selftest] after abc buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar(127);  // backspace
            DLOG("[selftest] after bsp buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar('x');
            DLOG("[selftest] after x   buf=["+S.inputBuf+"] pos="+itoa2(S.inputPos));
            pressChar('\r');  // enter (commit)
            DLOG("[selftest] simulated input "+S.inputVar+"=[abx]");
            g_ip++;
            continue;
        }
        bool jumped=false;
        // selftest 也需处理 loadResumeIp (真实模式由 tick() 开头处理)
        if(S.loadResumeIp>=0){g_ip=S.loadResumeIp;S.loadResumeIp=-1;}
        execCmd(g_script[g_ip],jumped);
        if(!jumped)g_ip++;
        if(S.waitType){
            // 立即完成
            Panel&p=S.panels[S.curPanel];
            if(S.typeLine>=0&&S.typeLine<(int)p.lines.size()){
                S.typePos=visibleCharCount(p.lines[S.typeLine]);
            }
            S.waitType=false;S.typeLine=-1;
        }
    }
    DLOG("[selftest] done. panels:");
    for(auto&kv:S.panels){
        for(auto&ln:kv.second.lines)DLOG("  ["+kv.first+"] "+ln);
    }
    // 面板滚动自检: 直接调用 handlePanelScroll (真实事件逻辑) + 完整 renderAll() 渲染
    {
        g_termH=40;
        Panel& pp=S.panels[S.curPanel];
        // 灌入 50 行, 置 scroll=10, follow=true, 模拟"在底部附近"
        pp.lines.clear(); pp.fx.clear(); pp.scroll=10; pp.follow=true;
        for(int i=0;i<50;i++)pp.lines.push_back("[fg:cyan]日志行 "+itoa2(i)+"[/] 这是一段用于测试滚动的较长文本内容");
        // 模拟用户按 ↑: 事件逻辑应把 scroll 减 1 并暂停 follow
        bool handled=handlePanelScroll(Event::ArrowUp);
        int afterUp=pp.scroll;
        bool followAfter=pp.follow;
        // 渲染按 ↑ 前后的输出
        auto renderAt=[&](int sc)->std::string{
            pp.scroll=sc; pp.follow=true;
            Element doc=renderAll();
            auto screen=Screen::Create(Dimension::Fixed(100), Dimension::Fixed(40));
            Render(screen, doc);
            return screen.ToString();
        };
        std::string before=renderAt(10);
        std::string after=renderAt(afterUp);
        bool diff=(before!=after);
        std::cerr<<"PANEL_EVENT_TEST handled="<<(handled?"YES":"NO")
                 <<" scrollBefore=10 scrollAfter="<<afterUp
                 <<" followAfter="<<(followAfter?"true":"false")
                 <<" RENDER_DIFFERENT="<<(diff?"YES":"NO")<<std::endl;
        DLOG(std::string("[selftest] PANEL_EVENT_TEST handled=")+(handled?"YES":"NO")+
             " scrollAfter="+itoa2(afterUp)+" RENDER_DIFFERENT="+(diff?"YES":"NO"));
        // 子测试: CHOICE 状态下 ↑ 也应滚动面板(守卫已移除)
        S.choices.clear();
        for(int i=0;i<3;i++)S.choices.push_back("选项"+itoa2(i));
        S.waiting=true; S.lastEvent="CHOICE";
        int scBefore=(int)pp.lines.size(); pp.scroll=scBefore; pp.follow=true;
        bool handled2=handlePanelScroll(Event::ArrowUp);
        int scAfter=pp.scroll;
        std::cerr<<"PANEL_EVENT_TEST_DURING_CHOICE handled="<<(handled2?"YES":"NO")
                 <<" scrollBefore="<<scBefore<<" scrollAfter="<<scAfter<<std::endl;
        S.choices.clear(); S.waiting=false; S.lastEvent="";
    }
    return 0;
}

int main(int argc,char**argv){
#if defined(_WIN32)
    // 统一控制台输入/输出代码页为 UTF-8。否则中文在读写控制台时会被按系统
    // 代码页 (通常为 GBK/CP936) 转码, 产生乱码。FTXUI 自身会在渲染时设置
    // 输出代码页, 这里在程序启动即设置输入+输出, 覆盖所有可能的读写路径。
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
#endif
    srand((unsigned)time(nullptr));
    SCRIPT_FILE=argc>1?argv[1]:"demo.txt";
    SAVE_FILE=argc>2?argv[2]:"save.txt";
    // 自检模式: --selftest <script> - 模拟按键走完整脚本
    if(argc>=2&&string(argv[1])=="--selftest"){
        if(argc<3){std::cerr<<"用法: "<<argv[0]<<" --selftest <script.txt>"<<std::endl;return 1;}
        return runSelftest(argv[2]);
    }
    if(!loadScript(SCRIPT_FILE)){
        std::cerr<<"无法打开脚本: "<<SCRIPT_FILE<<std::endl;
        std::cerr<<"用法: "<<argv[0]<<" <script.txt> [save.txt]"<<std::endl;
        return 1;
    }
    S.panels["main"]=Panel();
    // 不再默认创建 log 面板,LOG 命令按需创建
    S.curPanel="main";
    VARS.setI("random",0);
    VARS.setS("input","");
    VARS.setI("choice",0);

    auto screen=ScreenInteractive::Fullscreen();
    auto comp=Renderer([&]()->Element{
        static int frameNo=0;frameNo++;
        g_termH=screen.dimy();   // 每帧更新终端高度, 供面板滚动估算可见行数
        long long t=nowMs();
        // 仅调试模式或每 60 帧输出,避免每帧刷盘导致 27MB+ 日志
        if(S.debug || frameNo%60==0){
            DLOG("[render] frame="+itoa2(frameNo)+" ip="+itoa2(g_ip)+
                 " waitType="+itoa2(S.waitType)+" waiting="+itoa2(S.waiting)+
                 " choices="+itoa2(S.choices.size())+
                 " sleepUntil="+itoa2(S.sleepUntil));
        }
        tick(t);
        Element doc;
        if(S.showFlow){
            // 流程图覆盖层: 冻结游戏画面, 只渲染流程图
            // (tick() 仍会被调用, 但 showFlow=true 期间不推进 IP, 见 tick() 入口)
            doc=renderFlow();
        }else{
            Element main=renderAll();
            Element dbg=renderDebugBar();
            Element inp=renderInputBox();
            if(S.debug)doc=vbox({main|flex,inp,dbg});
            else doc=vbox({main|flex,inp});
        }
        // 如果还有进行中的动画/打字机/等待,请求 FTXUI 在下一帧再次渲染
        bool needFrame=S.waitType||S.shaking||S.blinking||
                       S.sleepUntil>(int)nowMs()||S.waiting||
                       (g_ip<(int)g_script.size()&&S.run);
        if(needFrame){
            screen.RequestAnimationFrame();
        }
        return doc;
    });
    comp|=CatchEvent([&](Event ev)->bool{
        S.lastKey=ev.input();
        DLOG(string("[ev] is_char=")+((ev.is_character())?"1":"0")+
             " is_mouse="+((ev.is_mouse())?"1":"0")+
             " waiting="+itoa2(S.waiting)+
             " choices="+itoa2(S.choices.size())+
             " input=["+ev.input()+"]");
        if(ev.is_mouse()){
            auto&m=ev.mouse();
            char buf[128];
            sprintf(buf,"(%d,%d) btn=%d %s",m.x,m.y,m.button,m.motion==Mouse::Pressed?"D":"U");
            S.lastMouse=buf;
        }
        // 全局快捷
        if(ev==Event::Escape){
            // 流程图打开时: Esc 仅关闭流程图, 不退出游戏
            if(S.showFlow){S.showFlow=false;return true;}
            S.run=false;screen.Exit();return true;
        }
        if(ev==Event::F2){S.debug=!S.debug;return true;}
        if(ev==Event::F5){userSave(SAVE_FILE);return true;}     // F5 = 立即存档
        if(ev==Event::F9){userLoad(SAVE_FILE,true);return true;} // F9 = 立即读档 (跳回存档点的下一行)
        // 流程图: 用户按 F 触发(不区分大小写). 优先级放在 INPUT 块之前,
        //        这样 INPUT 模式下 f/F 不会作为输入字符
        if(ev==Event::Character('f')||ev==Event::Character('F')){
            S.showFlow=!S.showFlow;
            S.flowScroll=0;
            g_flowDirty=true;   // 重新计算当前 IP 高亮
            if(S.showFlow)flushTypewriter();   // 弹流程图前先刷完打字机
            return true;
        }
        // 流程图打开时: 拦截滚动键
        if(S.showFlow){
            if(ev==Event::ArrowUp){
                if(S.flowScroll>0)S.flowScroll--;
                return true;
            }
            if(ev==Event::ArrowDown){
                S.flowScroll++;
                return true;
            }
            if(ev==Event::PageUp){
                S.flowScroll-=10;
                if(S.flowScroll<0)S.flowScroll=0;
                return true;
            }
            if(ev==Event::PageDown){
                S.flowScroll+=10;
                return true;
            }
            if(ev==Event::Home){
                S.flowScroll=0;
                return true;
            }
            if(ev==Event::End){
                // 跳到当前 IP 节点所在行
                if(g_flowCurrentIdx>=0){
                    S.flowScroll=g_flowCurrentIdx*2-2;  // 留 2 行上下文
                    if(S.flowScroll<0)S.flowScroll=0;
                }
                return true;
            }
        }

        // 面板内容滚动: 始终优先(含 CHOICE/INPUT 期间), UP/DOWN 在任何状态都能翻看面板
        if(handlePanelScroll(ev))return true;

        // 1) 方向键: 切换选项(优先于 is_character)
        if(!S.choices.empty()){
            if(ev==Event::ArrowUp){
                S.choiceIdx=(S.choiceIdx-1+(int)S.choices.size())%(int)S.choices.size();
                DLOG("[ev] ArrowUp -> idx="+itoa2(S.choiceIdx));
                return true;
            }
            if(ev==Event::ArrowDown){
                S.choiceIdx=(S.choiceIdx+1)%(int)S.choices.size();
                DLOG("[ev] ArrowDown -> idx="+itoa2(S.choiceIdx));
                return true;
            }
            // Enter/Return
            if(ev==Event::Return){
                VARS.setI("choice",S.choiceIdx+1);
                S.choices.clear();
                S.waiting=false;S.lastEvent="";
                flushTypewriter();
                DLOG("[ev] Return -> choice="+itoa2(S.choiceIdx+1));
                return true;
            }
        }
        // INPUT 模式: 左右键移动光标 / 回车提交 / 退格删除 (Special 事件, 必须在 is_character 块外)
        if(S.waiting&&S.lastEvent=="INPUT"){
            if(ev==Event::ArrowLeft){
                if(S.inputPos>0)S.inputPos--;
                return true;
            }
            if(ev==Event::ArrowRight){
                if(S.inputPos<(int)S.inputBuf.size())S.inputPos++;
                return true;
            }
            if(ev==Event::Home){S.inputPos=0;return true;}
            if(ev==Event::End){S.inputPos=(int)S.inputBuf.size();return true;}
            if(ev==Event::Return||ev==Event::Backspace){
                if(ev==Event::Return){
                    // 回车提交
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                    DLOG("[ev] INPUT Return commit");
                    return true;
                }
                // Backspace 删除光标前一字符
                if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                return true;
            }
        }

        if(ev.is_character()){
            char c=ev.character().empty()?0:ev.character()[0];
            DLOG("[ev] char=["+string(1,c)+"] choices="+itoa2(S.choices.size())+" waiting="+itoa2(S.waiting));
            // 选项 - 数字键 1-9
            if(!S.choices.empty()){
                if(c>='1'&&c<='9'){
                    int idx=c-'1';
                    if(idx<(int)S.choices.size()){
                        S.choiceIdx=idx;
                        VARS.setI("choice",idx+1);
                        S.choices.clear();
                        S.waiting=false;S.lastEvent="";
                        flushTypewriter();
                        DLOG("[ev] choice num="+itoa2(idx+1));
                        return true;
                    }
                }
                // Enter 字符也接受
                if(c=='\r'||c=='\n'){
                    VARS.setI("choice",S.choiceIdx+1);
                    S.choices.clear();
                    S.waiting=false;S.lastEvent="";
                    flushTypewriter();
                    DLOG("[ev] choice enter -> choice="+itoa2(S.choiceIdx+1));
                    return true;
                }
                // 吸收其它按键,避免误触发
                return true;
            }
            // 打字机进行中: 空格/回车 = 立即显示完
            if(S.waitType&&S.typeLine>=0&&(c==' '||c=='\r'||c=='\n')){
                flushTypewriter();
                return true;
            }
            // 输入
            if(S.waiting&&S.lastEvent=="INPUT"){
                if(c=='\r'||c=='\n'){
                    VARS.setS(S.inputVar,S.inputBuf);
                    S.waiting=false;S.lastEvent="";
                    S.inputBuf.clear();S.inputPos=0;
                    flushTypewriter();
                    return true;
                }else if(c==127||c==8||c=='\b'){
                    if(S.inputPos>0){S.inputBuf.erase(S.inputPos-1,1);S.inputPos--;}
                    return true;
                }else if(c==21){// Ctrl-U 清空
                    S.inputBuf.clear();S.inputPos=0;
                    return true;
                }else if(c>=32&&c<127){
                    if((int)S.inputBuf.size()<S.inputMax){
                        S.inputBuf.insert(S.inputBuf.begin()+S.inputPos,c);
                        S.inputPos++;
                    }
                    return true;
                }
            }
            if(c=='`'){S.debug=!S.debug;return true;}
            if(c=='Q'||c=='q'){S.run=false;screen.Exit();return true;}
        }
        return false;
    });
    screen.Loop(comp);
    return 0;
}
