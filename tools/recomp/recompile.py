#!/usr/bin/env python3
"""Recompiles BOOT.BIN of the user's own Coded Arms image into C sources.

Usage:
  recompile.py --iso CodedArms.iso --out build/generated
  recompile.py --elf BOOT.BIN --out build/generated

The output is derived from the game's code: it is written to the build directory only
and must never be committed or distributed.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import codegen  # noqa: E402
import functions  # noqa: E402
from iso9660 import read_iso_file  # noqa: E402
from pspelf import PspModule  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso")
    ap.add_argument("--elf")
    ap.add_argument("--out", required=True)
    ap.add_argument("--trace", default="", help="comma separated function addresses to log (debugging)")
    ap.add_argument("--watch", action="store_true", help="debug: report stores to CA_WATCH_ADDR at run time")
    ap.add_argument("--profile", action="store_true", help="debug: count the calls of every function (profdump)")
    args = ap.parse_args()
    if args.iso:
        data = read_iso_file(args.iso, "PSP_GAME/SYSDIR/BOOT.BIN")
    elif args.elf:
        data = open(args.elf, "rb").read()
    else:
        ap.error("--iso or --elf is required")
    mod = PspModule(data)
    funcs, starts = functions.discover(mod)
    trace = list(funcs) if args.trace == "all" else [int(x, 16) for x in args.trace.split(",") if x]
    gen, files = codegen.generate(mod, funcs, starts, args.out, trace=trace, watch=args.watch, profile=args.profile)
    print("functions: %d, files: %d, imports: %d" % (len(funcs), len(files), len(mod.stubs)))
    if gen.unhandled:
        print("UNHANDLED mnemonics:", gen.unhandled)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
