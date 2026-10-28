#!/usr/bin/env python3
"""Coverage report for the recompiler: decodes every word of .text and lists mnemonics,
unknown encodings, imports and module metadata.

Usage: analyze.py BOOT.BIN
"""
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from allegrex import decode  # noqa: E402
from pspelf import PspModule  # noqa: E402


def main(path):
    mod = PspModule(open(path, "rb").read())
    text = mod.text
    counts = collections.Counter()
    unknown = []
    for a in range(text.addr, text.addr + text.size, 4):
        ins = decode(mod.word(a), a + mod.base)
        counts[ins.mn] += 1
        if ins.mn == "unknown":
            unknown.append(ins)
    total = sum(counts.values())
    print("base %#x entry %#x text %#x..%#x (%d words)" % (mod.base, mod.entry, text.addr + mod.base,
                                                        text.addr + text.size + mod.base, total))
    print("relocated words: %d, imports: %d" % (len(mod.relocs), len(mod.stubs)))
    print("unknown encodings: %d (%.3f%%)" % (len(unknown), 100.0 * len(unknown) / total))
    by_op = collections.Counter("op%02x" % u.op for u in unknown)
    print("unknown by major opcode:", dict(by_op.most_common(12)))
    for u in unknown[:15]:
        print("   ", u)
    print("mnemonics:")
    for mn, c in sorted(counts.items(), key=lambda x: -x[1]):
        print("  %-10s %d" % (mn, c))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
