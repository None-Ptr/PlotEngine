#!/usr/bin/env python3
# 重新生成 plotengine_release.cpp —— 从源文件按依赖顺序内联:
#   mingw_std_threads.hpp -> cxx11_compat.hpp -> ftxui_amalgamation.hpp
#   -> image_min.hpp -> PlotEngine.cpp(剔除其 3 个库 #include)
# 产出单文件、零外部依赖、可独立编译分发。
import io

def read(p):
    with io.open(p, 'r', encoding='utf-8') as f:
        return f.read()

HEADER = """// =====================================================================
// plotengine_release.cpp - single-file text-adventure game engine
// (PlotEngine + FTXUI 4.1.1 amalgamation + C++11 compat shim + image_min)
//
// Build (Windows / MinGW):
//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE plotengine_release.cpp \\
//       -o plotengine.exe
// Build (Linux):
//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 plotengine_release.cpp -o plotengine
//
// This file is fully self-contained: all headers have been inlined in
// dependency order (mingw_std_threads.hpp -> cxx11_compat.hpp ->
// ftxui_amalgamation.hpp -> image_min.hpp -> app). No -pthread / -lpthread needed.
// FTXUI is MIT licensed, Copyright (c) 2019 Arthur Sonzogni.
// image_min is zero-dependency (tinf inflate is public domain, tiv from
// ftxui-image-view/TerminalImageViewer, Apache-2.0).
// =====================================================================

"""

mingw = read('mingw_std_threads.hpp')
cxx   = read('cxx11_compat.hpp')
ftxui = read('ftxui_amalgamation.hpp')
img   = read('image_min.hpp')

app = read('PlotEngine.cpp')
# 剔除 PlotEngine.cpp 中的 3 个库 include(它们已被内联)。前缀匹配兼容行尾注释。
drop_prefix = ['#include "mingw_std_threads.hpp"',
               '#include "ftxui_amalgamation.hpp"',
               '#include "image_min.hpp"']
app_lines = []
for ln in app.split('\n'):
    s = ln.strip()
    if any(s.startswith(p) for p in drop_prefix):
        continue
    app_lines.append(ln)
app = '\n'.join(app_lines)

def part(n, name, body):
    return ("\n// ===================== Part %s: %s =====================\n"
            "// =====================================================================\n"
            % (n, name)) + body.rstrip('\n') + "\n"

out = (HEADER
       + part(0, "mingw_std_threads.hpp", mingw)
       + part(1, "cxx11_compat.hpp", cxx)
       + part(2, "ftxui_amalgamation.hpp", ftxui)
       + part("2.5", "image_min.hpp", img)
       + part(3, "PlotEngine.cpp", app))

with io.open('plotengine_release.cpp', 'w', encoding='utf-8') as f:
    f.write(out)
print('plotengine_release.cpp regenerated, total chars=%d' % len(out))
