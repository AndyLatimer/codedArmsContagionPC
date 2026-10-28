"""Function and label discovery for the relocated module.

Uses only facts the binary provides:
  * jal targets (R_MIPS_26 relocations) and the module entry point are function starts;
  * R_MIPS_32 relocated words that point into .text are code pointers. A pointer whose
    preceding instruction pair ends a function ("jr ra" + delay slot) or that is already
    a known start is treated as a function; any other is a label inside the function
    that contains it (switch jump tables);
  * the Metrowerks compiler emits functions contiguously, so a function ends where the
    next one starts.
"""
import bisect
import struct

from allegrex import decode
from pspelf import R_MIPS_32, R_MIPS_26

JR_RA = 0x03E00008


class Function:
    def __init__(self, start):
        self.start = start
        self.end = None          # exclusive
        self.labels = set()      # branch targets and jump-table labels inside the function
        self.jump_table_labels = set()
        self.indirect = False    # address taken (called through a pointer)
        self.entries = set()     # secondary entry points (targets of jumps from other functions)
        self.bal_returns = set() # return addresses of branch-and-link calls to this same function


def discover(mod):
    base = mod.base
    ranges = mod.code_ranges
    t0, t1 = ranges[0]

    def in_text(a):
        return any(r0 <= a < r1 for r0, r1 in ranges)

    def range_end(a):
        for r0, r1 in ranges:
            if r0 <= a < r1:
                return r1
        return a

    starts = {mod.entry}
    for off, typ in mod.relocs.items():
        if typ == R_MIPS_26 and in_text(off + base):
            w = mod.word(off)
            if w >> 26 == 3:  # jal
                starts.add(((off + base + 4) & 0xF0000000) | ((w & 0x03FFFFFF) << 2))
    starts |= {r0 for r0, _ in ranges}
    # Code pointers stored in data, or built in code with relocated lui/addiu pairs.
    code_ptrs = set()
    for off, typ in mod.relocs.items():
        if typ == R_MIPS_32:
            v = mod.word(off)
            if in_text(v):
                code_ptrs.add(v)
    # Addresses built in code with lui/addiu are function pointers (switch tables only
    # ever live in data), so they are always function starts.
    hilo_ptrs = {v for v in mod.hilo_values if in_text(v) and v % 4 == 0}
    code_ptrs |= hilo_ptrs
    indirect = set()
    labels_from_ptrs = set()
    for p in code_ptrs:
        outside_text = not (t0 <= p < t1)  # pointer tables into the .data code (initialisers)
        if p in starts or p in hilo_ptrs or p == t0 or outside_text or mod.word(p - base - 8) == JR_RA:
            indirect.add(p)
        else:
            labels_from_ptrs.add(p)
    starts |= indirect
    starts = sorted(a for a in starts if in_text(a))

    funcs = {}
    for i, s in enumerate(starts):
        f = Function(s)
        f.end = min(starts[i + 1] if i + 1 < len(starts) else range_end(s), range_end(s))
        f.indirect = s in indirect
        funcs[s] = f
    for p in labels_from_ptrs:
        f = funcs[starts[bisect.bisect_right(starts, p) - 1]]
        f.labels.add(p)
        f.jump_table_labels.add(p)
        # Also an entry point: a switch in a neighbouring function may jump here (the
        # function boundary between them comes from a jal into the middle of the code).
        f.entries.add(p)
    def containing(addr):
        return funcs[starts[bisect.bisect_right(starts, addr) - 1]]

    # Branch targets. A branch or "j" that leaves its function is a tail jump: the
    # Metrowerks compiler shares function epilogues. The target becomes a secondary entry
    # point of the function that contains it (unless it is that function's start).
    for f in funcs.values():
        for a in range(f.start, f.end, 4):
            ins = decode(mod.word(a - base), a)
            tgt = ins.branch_target if ins.is_branch else (ins.target if ins.mn == "j" else None)
            if tgt is None or not in_text(tgt):
                continue
            if f.start <= tgt < f.end:
                f.labels.add(tgt)
                # Branch-and-link to the same function: a local subroutine of hand-written
                # assembly (Contagion's decompressor). It is emitted as a goto, and "jr ra"
                # in this function goes back to the matching return address. A C call would
                # be wrong: such a subroutine may leave without returning (to the loop top).
                if ins.mn in ("bltzal", "bgezal", "bltzall", "bgezall"):
                    f.bal_returns.add(a + 8)
                    f.labels.add(a + 8)
            else:
                g = containing(tgt)
                if tgt != g.start:
                    g.entries.add(tgt)
                    g.labels.add(tgt)
    return funcs, starts


def report(mod):
    funcs, starts = discover(mod)
    base = mod.base
    stats = dict(functions=len(funcs), indirect=sum(f.indirect for f in funcs.values()),
                 jt_labels=sum(len(f.jump_table_labels) for f in funcs.values()))
    out_of_range = 0
    jr_other, jr_unresolved, j_tail, j_outside = 0, 0, 0, 0
    for f in funcs.values():
        for lab in f.labels:
            if not (f.start <= lab < f.end):
                out_of_range += 1  # must stay 0: labels are always inside their function
        for a in range(f.start, f.end, 4):
            ins = decode(mod.word(a - base), a)
            if ins.mn == "jr" and ins.rs != 31:
                jr_other += 1
                if not f.jump_table_labels:
                    jr_unresolved += 1
            elif ins.mn == "j":
                if ins.target in funcs:
                    j_tail += 1
                elif not (f.start <= ins.target < f.end):
                    j_outside += 1
    stats.update(secondary_entries=sum(len(f.entries) for f in funcs.values()),
                 branch_targets_outside_function=out_of_range, jr_non_ra=jr_other,
                 jr_without_jump_table=jr_unresolved, j_tail_calls=j_tail, j_outside=j_outside)
    return stats
