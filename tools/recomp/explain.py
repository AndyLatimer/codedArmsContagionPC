#!/usr/bin/env python3
"""Explains how the recompiler sees a code address (debugging aid).

Usage: explain.py BOOT.BIN ADDR [ADDR...]
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import functions  # noqa: E402
from allegrex import GPR, decode  # noqa: E402
from pspelf import R_MIPS_32, PspModule  # noqa: E402


def main(path, addrs):
    m = PspModule(open(path, "rb").read())
    funcs, starts = functions.discover(m)
    import bisect
    for a in addrs:
        print("== %08x" % a)
        print("  function start:", a in funcs)
        i = bisect.bisect_right(starts, a) - 1
        if i >= 0:
            f = funcs[starts[i]]
            print("  inside function %08x..%08x (label: %s, jt label: %s, entry: %s)" % (
                f.start, f.end, a in f.labels, a in f.jump_table_labels, a in f.entries))
        print("  built by lui/addiu:", a in m.hilo_values)
        refs = [o + m.base for o, t in m.relocs.items() if t == R_MIPS_32 and m.word(o) == a]
        print("  stored in data at:", " ".join("%08x" % r for r in refs[:8]))
        for k in range(-3, 4):
            x = a + 4 * k
            ins = decode(m.word(x - m.base), x)
            print("    %s%08x: %08x %-8s rs=%s rt=%s imm=%d" % ("->" if k == 0 else "  ", x, ins.word, ins.mn,
                                                               GPR[ins.rs], GPR[ins.rt], ins.simm))


if __name__ == "__main__":
    main(sys.argv[1], [int(x, 16) for x in sys.argv[2:]])
