# One-shot helper: rewrite C++14 binary literals (0b....) with digit separators
# (') into plain C++11 hexadecimal literals inside the FTXUI amalgamation header.
import re
import sys

path = sys.argv[1] if len(sys.argv) > 1 else "ftxui_amalgamation.hpp"

with open(path, "r", encoding="utf-8") as f:
    src = f.read()

# (?<![\w.]) avoids matching the "0b01" substring inside hex like 0x00b01.
pattern = re.compile(r"(?<![\w.])0b([01']+)")

def repl(m):
    bits = m.group(1).replace("'", "")
    return hex(int(bits, 2))

new_src, n = pattern.subn(repl, src)

with open(path, "w", encoding="utf-8") as f:
    f.write(new_src)

print("replaced {} binary literals in {}".format(n, path))
