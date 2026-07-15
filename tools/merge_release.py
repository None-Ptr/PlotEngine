#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Merge PlotEngine.cpp and all its header dependencies into a single
self-contained plotengine_release.cpp, ordered by dependency.

Dependency graph (inlined in this order):
  mingw_std_threads.hpp   (Win32 std::thread/mutex/condition_variable shim)
  cxx11_compat.hpp        (C++11 shim used by the amalgamation)
    ^ included by
  ftxui_amalgamation.hpp  (the whole FTXUI 4.1.1 library, single-header)
    ^ included by
  PlotEngine.cpp          (the application / game engine)

The internal cross-includes ("mingw_std_threads.hpp", "cxx11_compat.hpp" and
"ftxui_amalgamation.hpp") are stripped from the inlined bodies so the result
needs no external files. Include guards are kept intact, so there is no risk of
duplicate definitions.
"""
import io
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

FILES = {
    "threads": os.path.join(ROOT, "mingw_std_threads.hpp"),
    "compat": os.path.join(ROOT, "cxx11_compat.hpp"),
    "amalgam": os.path.join(ROOT, "ftxui_amalgamation.hpp"),
    "app": os.path.join(ROOT, "PlotEngine.cpp"),
}
OUT = os.path.join(ROOT, "plotengine_release.cpp")


def read(path):
    with io.open(path, "r", encoding="utf-8", newline="") as f:
        return f.read()


def strip_includes(text, *names):
    """Remove any #include "<name>" line (allowing surrounding comments)."""
    lines = text.split("\n")
    out = []
    for ln in lines:
        s = ln.strip()
        drop = False
        for n in names:
            if s.startswith("#include") and ('"%s"' % n) in s:
                drop = True
                break
        if not drop:
            out.append(ln)
    return "\n".join(out)


def main():
    threads = read(FILES["threads"])
    compat = read(FILES["compat"])
    amalgam = read(FILES["amalgam"])
    app = read(FILES["app"])

    # ftxui_amalgamation.hpp references cxx11_compat.hpp; we inline it instead.
    amalgam = strip_includes(amalgam, "cxx11_compat.hpp")
    # PlotEngine.cpp references mingw_std_threads.hpp and ftxui_amalgamation.hpp;
    # we inline both instead.
    app = strip_includes(app, "mingw_std_threads.hpp", "ftxui_amalgamation.hpp")

    banner = (
        "// =====================================================================\n"
        "// plotengine_release.cpp - single-file text-adventure game engine\n"
        "// (PlotEngine + FTXUI 4.1.1 amalgamation + C++11 compat shim)\n"
        "//\n"
        "// Build (Windows / MinGW):\n"
        "//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 -DUNICODE -D_UNICODE plotengine_release.cpp \\\n"
        "//       -o plotengine.exe\n"
        "// Build (Linux):\n"
        "//   g++ -std=gnu++11 -O2 -finput-charset=UTF-8 -fexec-charset=UTF-8 plotengine_release.cpp -o plotengine\n"
        "//\n"
        "// This file is fully self-contained: all headers have been inlined in\n"
        "// dependency order (mingw_std_threads.hpp -> cxx11_compat.hpp ->\n"
        "// ftxui_amalgamation.hpp -> app). No -pthread / -lpthread needed.\n"
        "// FTXUI is MIT licensed, Copyright (c) 2019 Arthur Sonzogni.\n"
        "// =====================================================================\n"
        "\n"
    )

    parts = [
        banner,
        "// ===================== Part 0: mingw_std_threads.hpp ===============\n",
        threads,
        "\n\n// ===================== Part 1: cxx11_compat.hpp =====================\n",
        compat,
        "\n\n// ============== Part 2: ftxui_amalgamation.hpp (FTXUI) ==============\n",
        amalgam,
        "\n\n// ===================== Part 3: PlotEngine.cpp =======================\n",
        app,
        "\n",
    ]

    with io.open(OUT, "w", encoding="utf-8", newline="") as f:
        f.write("".join(parts))

    size = os.path.getsize(OUT)
    print("wrote %s (%d bytes, %d lines)"
          % (OUT, size, len("".join(parts).split("\n"))))


if __name__ == "__main__":
    main()
