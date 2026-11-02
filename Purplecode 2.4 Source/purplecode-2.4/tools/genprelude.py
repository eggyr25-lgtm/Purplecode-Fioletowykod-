#!/usr/bin/env python3
"""Regenerate compiler/prelude.c from compiler/prelude.pc (run from the project root)."""
import os
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
import re
rt = open(os.path.join(root, "runtime", "purple_rt.c"), encoding="utf-8").read()
_i = rt.index("purple_font8x8[95][8] = {"); _j = rt.index("};", _i)
_vals = re.findall(r"0x([0-9A-Fa-f]{2})", rt[_i:_j])
assert len(_vals) == 760
src = open(os.path.join(root, "compiler", "prelude.pc"), encoding="utf-8").read().replace("@@FONTHEX@@", "".join(v.lower() for v in _vals))
out = ["/* GENERATED from compiler/prelude.pc by tools/genprelude.py - do not edit by hand. */",
       "const char purple_prelude_src[] ="]
for line in src.split("\n"):
    esc = line.replace("\\", "\\\\").replace('"', '\\"')
    out.append('    "%s\\n"' % esc)
out[-1] += ";"
open(os.path.join(root, "compiler", "prelude.c"), "w", encoding="utf-8").write("\n".join(out) + "\n")
