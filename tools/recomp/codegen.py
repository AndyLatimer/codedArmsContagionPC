"""C code generator: translates every discovered function to a C function over the CPU
context declared in src/psp/recomp.h.

Conventions of the generated code
  R(n)    general register n (uint32_t), R(0) never written
  F(n)/FI(n)  FPU register as float / as raw bits
  RD8..WR32   guest memory access (address masked by the runtime)
  f_XXXXXXXX  one C function per guest function; secondary entry points get e_XXXXXXXX
"""
import os

from allegrex import decode

LOADS = {"lb": ("int8_t", "RD8S"), "lbu": ("uint8_t", "RD8"), "lh": ("int16_t", "RD16S"),
         "lhu": ("uint16_t", "RD16"), "lw": ("uint32_t", "RD32"), "ll": ("uint32_t", "RD32")}
GAME_ID = "ULES00718"


def load_hooks(game=GAME_ID):
    """Port hooks: host functions called right after given (non-branch) guest instructions,
    listed in games/<disc id>/hooks.tsv next to this file (address, function name)."""
    hooks = {}
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "games", game, "hooks.tsv")
    if os.path.exists(path):
        for line in open(path):
            line = line.split("#", 1)[0].strip()
            if line:
                addr, name = line.split()[:2]
                hooks[int(addr, 16)] = name
    return hooks


STORES = {"sb": "WR8", "sh": "WR16", "sw": "WR32"}


class Gen:
    def __init__(self, mod, funcs, starts, trace=(), watch=False, profile=False, stub_base=0, linked=None):
        """stub_base: index of this module's first import in the combined import table.
        linked: {stub address: guest address} for imports another recompiled module exports
        (called directly instead of through the HLE layer)."""
        self.watch = watch
        self.hooks = load_hooks()
        self.mod, self.funcs, self.starts = mod, funcs, starts
        self.trace = set(trace)
        self.profile = profile
        self.base = mod.base
        self.stub_index = {s.addr: stub_base + i for i, s in enumerate(mod.stubs)}
        self.linked = linked or {}
        self.unhandled = {}

    # -- helpers -----------------------------------------------------------------------
    def insn(self, a):
        return decode(self.mod.word(a - self.base), a)

    def call_expr(self, target, pc):
        """C statement(s) calling guest code at `target` (returns to pc + 8)."""
        if target in self.linked:
            return self.call_expr(self.linked[target], pc) + "  /* linked import */"
        if target in self.stub_index:
            return "hle_call(c, %d);" % self.stub_index[target]
        if target in self.funcs:
            return "f_%08x(c);" % target
        f = self.containing(target)
        if f and target in f.entries:
            return "e_%08x(c);" % target
        return "call_indirect(c, 0x%08xu);" % target

    def containing(self, a):
        import bisect
        i = bisect.bisect_right(self.starts, a) - 1
        if i < 0:
            return None
        f = self.funcs[self.starts[i]]
        return f if f.start <= a < f.end else None

    def jump(self, f, target, pc=None):
        """Unconditional transfer to target from inside function f (no link). A backward jump
        (a loop) polls for preemption first (RT_POLL, see psp/recomp.h)."""
        poll = "RT_POLL(); " if pc is not None and target <= pc else ""
        if f.start <= target < f.end and target != f.start:
            return "{ %sgoto L_%08x; }" % (poll, target) if poll else "goto L_%08x;" % target
        if target == f.start:
            return "{ %sgoto L_%08x; }" % (poll, target) if poll else "goto L_%08x;" % target
        return "{ %s return; }" % self.call_expr(target, 0)

    # -- single instruction semantics (no control flow) ------------------------------------
    def op(self, i):
        mn = i.mn
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        S = lambda e: "R(%d) = %s;" % (rd, e) if rd else ";"  # noqa: E731
        T = lambda e: "R(%d) = %s;" % (rt, e) if rt else ";"  # noqa: E731
        addr = "(R(%d) + (uint32_t)%d)" % (rs, i.simm)
        fd, fs, ft = sa, rd, rt
        if mn in ("nop", "sync", "cache", "vnop", "vsync", "vflush"):
            return ";"
        if mn == "sll": return S("R(%d) << %d" % (rt, sa))
        if mn == "srl": return S("R(%d) >> %d" % (rt, sa))
        if mn == "sra": return S("(uint32_t)((int32_t)R(%d) >> %d)" % (rt, sa))
        if mn == "rotr": return S("ROTR(R(%d), %d)" % (rt, sa))
        if mn == "sllv": return S("R(%d) << (R(%d) & 31)" % (rt, rs))
        if mn == "srlv": return S("R(%d) >> (R(%d) & 31)" % (rt, rs))
        if mn == "srav": return S("(uint32_t)((int32_t)R(%d) >> (R(%d) & 31))" % (rt, rs))
        if mn == "rotrv": return S("ROTR(R(%d), R(%d) & 31)" % (rt, rs))
        if mn == "movz": return "if (R(%d) == 0) { %s }" % (rt, S("R(%d)" % rs)) if rd else ";"
        if mn == "movn": return "if (R(%d) != 0) { %s }" % (rt, S("R(%d)" % rs)) if rd else ";"
        if mn == "syscall": return "hle_syscall(c, 0x%x);" % ((i.word >> 6) & 0xFFFFF)
        if mn == "break": return "rt_break(c, 0x%08xu);" % i.pc
        if mn == "mfhi": return S("c->hi")
        if mn == "mflo": return S("c->lo")
        if mn == "mthi": return "c->hi = R(%d);" % rs
        if mn == "mtlo": return "c->lo = R(%d);" % rs
        if mn == "clz": return S("CLZ(R(%d))" % rs)
        if mn == "clo": return S("CLZ(~R(%d))" % rs)
        if mn in ("mult", "multu", "madd", "maddu", "msub", "msubu"):
            signed = not mn.endswith("u")
            prod = ("(int64_t)(int32_t)R(%d) * (int64_t)(int32_t)R(%d)" if signed else
                    "(uint64_t)R(%d) * (uint64_t)R(%d)") % (rs, rt)
            if mn.startswith("mult"):
                return "{ uint64_t p = (uint64_t)(%s); c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }" % prod
            sign = "+" if mn.startswith("madd") else "-"
            return ("{ uint64_t acc = ((uint64_t)c->hi << 32) | c->lo; acc = acc %s (uint64_t)(%s); "
                    "c->lo = (uint32_t)acc; c->hi = (uint32_t)(acc >> 32); }" % (sign, prod))
        if mn == "div": return "rt_div(c, R(%d), R(%d));" % (rs, rt)
        if mn == "divu": return "rt_divu(c, R(%d), R(%d));" % (rs, rt)
        if mn in ("add", "addu"): return S("R(%d) + R(%d)" % (rs, rt))
        if mn in ("sub", "subu"): return S("R(%d) - R(%d)" % (rs, rt))
        if mn == "and": return S("R(%d) & R(%d)" % (rs, rt))
        if mn == "or": return S("R(%d) | R(%d)" % (rs, rt))
        if mn == "xor": return S("R(%d) ^ R(%d)" % (rs, rt))
        if mn == "nor": return S("~(R(%d) | R(%d))" % (rs, rt))
        if mn == "slt": return S("(int32_t)R(%d) < (int32_t)R(%d)" % (rs, rt))
        if mn == "sltu": return S("R(%d) < R(%d)" % (rs, rt))
        if mn == "max": return S("(uint32_t)MAXI((int32_t)R(%d), (int32_t)R(%d))" % (rs, rt))
        if mn == "min": return S("(uint32_t)MINI((int32_t)R(%d), (int32_t)R(%d))" % (rs, rt))
        if mn in ("addi", "addiu"): return T("R(%d) + (uint32_t)%d" % (rs, i.simm))
        if mn == "slti": return T("(int32_t)R(%d) < %d" % (rs, i.simm))
        if mn == "sltiu": return T("R(%d) < (uint32_t)%d" % (rs, i.simm))
        if mn == "andi": return T("R(%d) & 0x%xu" % (rs, i.imm))
        if mn == "ori": return T("R(%d) | 0x%xu" % (rs, i.imm))
        if mn == "xori": return T("R(%d) ^ 0x%xu" % (rs, i.imm))
        if mn == "lui": return T("0x%08xu" % (i.imm << 16))
        if mn in LOADS:
            ty, fn = LOADS[mn]
            return T("(uint32_t)(%s)%s(%s)" % (ty, fn, addr)) if rt else "(void)%s(%s);" % (fn, addr)
        if self.watch and mn in ("sb", "sh", "sw", "swc1", "swl", "swr", "sc"):
            # Debug build (-DCA_RECOMP_WATCH): report stores to CA_WATCH_ADDR.
            return "rt_watch_store(c, 0x%08xu, %s); %s" % (i.pc, addr, self._store(i, addr))
        return self._store(i, addr) if mn in STORES or mn in ("sc", "swl", "swr", "swc1") else self._op2(i)

    def _store(self, i, addr):
        mn, rt = i.mn, i.rt
        T = lambda e: "R(%d) = %s;" % (rt, e) if rt else ";"  # noqa: E731
        if mn in STORES: return "%s(%s, R(%d));" % (STORES[mn], addr, rt)
        if mn == "sc": return "WR32(%s, R(%d)); %s" % (addr, rt, T("1"))
        if mn == "swl": return "rt_swl(c, %s, R(%d));" % (addr, rt)
        if mn == "swr": return "rt_swr(c, %s, R(%d));" % (addr, rt)
        return "WR32(%s, FI(%d));" % (addr, rt)

    def _op2(self, i):
        mn = i.mn
        rs, rt, rd, sa = i.rs, i.rt, i.rd, i.sa
        S = lambda e: "R(%d) = %s;" % (rd, e) if rd else ";"  # noqa: E731
        T = lambda e: "R(%d) = %s;" % (rt, e) if rt else ";"  # noqa: E731
        addr = "(R(%d) + (uint32_t)%d)" % (rs, i.simm)
        fd, fs, ft = sa, rd, rt
        if mn == "sc": return "WR32(%s, R(%d)); %s" % (addr, rt, T("1"))
        if mn == "lwl": return T("rt_lwl(c, %s, R(%d))" % (addr, rt))
        if mn == "lwr": return T("rt_lwr(c, %s, R(%d))" % (addr, rt))
        if mn == "swl": return "rt_swl(c, %s, R(%d));" % (addr, rt)
        if mn == "swr": return "rt_swr(c, %s, R(%d));" % (addr, rt)
        if mn == "lwc1": return "FI(%d) = RD32(%s);" % (rt, addr)
        if mn == "swc1": return "WR32(%s, FI(%d));" % (addr, rt)
        if mn == "ext":
            size = rd + 1
            mask = (1 << size) - 1
            return T("(R(%d) >> %d) & 0x%xu" % (rs, sa, mask))
        if mn == "ins":
            size = rd - sa + 1
            mask = ((1 << size) - 1) << sa
            return T("(R(%d) & 0x%xu) | ((R(%d) << %d) & 0x%xu)" % (rt, ~mask & 0xFFFFFFFF, rs, sa, mask))
        if mn == "wsbh": return S("((R(%d) & 0x00ff00ffu) << 8) | ((R(%d) >> 8) & 0x00ff00ffu)" % (rt, rt))
        if mn == "wsbw": return S("BSWAP32(R(%d))" % rt)
        if mn == "seb": return S("(uint32_t)(int32_t)(int8_t)R(%d)" % rt)
        if mn == "seh": return S("(uint32_t)(int32_t)(int16_t)R(%d)" % rt)
        if mn == "bitrev": return S("BITREV(R(%d))" % rt)
        # FPU
        if mn == "mfc1": return T("FI(%d)" % fs)
        if mn == "mtc1": return "FI(%d) = R(%d);" % (fs, rt)
        if mn == "cfc1": return T("rt_cfc1(c, %d)" % fs)
        if mn == "ctc1": return "rt_ctc1(c, %d, R(%d));" % (fs, rt)
        fpu = {"add.s": "F(%d) + F(%d)", "sub.s": "F(%d) - F(%d)"}
        if mn in fpu: return "F(%d) = %s;" % (fd, fpu[mn] % (fs, ft))
        # Hardware details below follow PPSSPP's verified interpreter (cpu/fpu tests).
        if mn == "mul.s":  # inf * 0 gives a positive NaN
            return ("{ float a = F(%d), b = F(%d); if ((isinf(a) && b == 0.0f) || (isinf(b) && a == 0.0f)) "
                    "FI(%d) = 0x7FC00000u; else F(%d) = a * b; }" % (fs, ft, fd, fd))
        if mn == "div.s": return "F(%d) = rt_fdiv(c, F(%d), F(%d));" % (fd, fs, ft)
        if mn == "sqrt.s":  # a negative input gives a positive NaN
            return "{ float a = F(%d); F(%d) = sqrtf(a); if (a < 0.0f) FI(%d) = 0x7FC00000u; }" % (fs, fd, fd)
        if mn == "abs.s": return "F(%d) = fabsf(F(%d));" % (fd, fs)
        if mn == "mov.s": return "FI(%d) = FI(%d);" % (fd, fs)
        if mn == "neg.s": return "FI(%d) = FI(%d) ^ 0x80000000u;" % (fd, fs)
        cvt = {"round.w.s": 0, "trunc.w.s": 1, "ceil.w.s": 2, "floor.w.s": 3}
        if mn in cvt: return "FI(%d) = rt_f2i(F(%d), %d);" % (fd, fs, cvt[mn])
        if mn == "cvt.w.s": return "FI(%d) = rt_f2i(F(%d), (int)(c->fcr31 & 3));" % (fd, fs)
        if mn == "cvt.s.w": return "F(%d) = (float)(int32_t)FI(%d);" % (fd, fs)
        if mn.startswith("c.") and mn.endswith(".s"):
            cond = i.funct & 15
            return "c->fpcond = rt_fcmp(F(%d), F(%d), %d);" % (fs, ft, cond)
        # Every VFPU instruction (transfers, loads/stores, arithmetic) goes through the
        # interpreter, which handles prefixes and register addressing in one place.
        if mn in ("mfv", "mtv", "mfvc", "mtvc", "lv.s", "sv.s", "lv.q", "sv.q", "lvl.q", "lvr.q",
                  "svl.q", "svr.q") or mn.startswith("v"):
            if self.watch and mn in ("sv.s", "sv.q", "svl.q", "svr.q"):
                # Debug build: VFPU stores are reported word by word too.
                a = "R(%d) + (uint32_t)%d" % (i.rs, ((i.word & 0xFFFC) ^ 0x8000) - 0x8000)
                n = 1 if mn == "sv.s" else 4
                w = " ".join("rt_watch_store(c, 0x%08xu, %s + %d);" % (i.pc, a, 4 * k) for k in range(n))
                return "%s vfpu_exec(c, 0x%08xu);" % (w, i.word)
            return "vfpu_exec(c, 0x%08xu);" % i.word
        if mn in ("halt", "mfic", "mtic", "mfc0", "mtc0"):
            return "rt_privileged(c, 0x%08xu, 0x%08xu);" % (i.word, i.pc)
        self.unhandled[mn] = self.unhandled.get(mn, 0) + 1
        return "rt_unimplemented(c, 0x%08xu, 0x%08xu); /* %s */" % (i.word, i.pc, mn)

    def cond(self, i):
        mn, rs, rt = i.mn, i.rs, i.rt
        base_mn = mn[:-1] if mn in ("beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl",
                                    "bc1fl", "bc1tl", "bvfl", "bvtl") else mn
        base_mn = {"bltzall": "bltzal", "bgezall": "bgezal"}.get(base_mn, base_mn)
        return {
            "beq": "R(%d) == R(%d)" % (rs, rt), "bne": "R(%d) != R(%d)" % (rs, rt),
            "blez": "(int32_t)R(%d) <= 0" % rs, "bgtz": "(int32_t)R(%d) > 0" % rs,
            "bltz": "(int32_t)R(%d) < 0" % rs, "bgez": "(int32_t)R(%d) >= 0" % rs,
            "bltzal": "(int32_t)R(%d) < 0" % rs, "bgezal": "(int32_t)R(%d) >= 0" % rs,
            "bc1f": "!c->fpcond", "bc1t": "c->fpcond",
            "bvf": "!((VFPU_CC >> %d) & 1)" % ((i.word >> 18) & 7),
            "bvt": "((VFPU_CC >> %d) & 1)" % ((i.word >> 18) & 7),
        }[base_mn]

    # -- functions -----------------------------------------------------------------------
    def function(self, f):
        out = []
        name = "f_%08x" % f.start
        traced = f.start in self.trace
        if traced:
            name += "_impl"
        has_entries = bool(f.entries)
        if has_entries:
            out.append("static void i_%08x(Ctx* c, uint32_t entry) {" % f.start)
            out.append("    switch (entry) {")
            for e in sorted(f.entries):
                out.append("    case 0x%08xu: goto L_%08x;" % (e, e))
            out.append("    default: break;\n    }")
        else:
            out.append("%svoid %s(Ctx* c) {" % ("static " if traced else "", name))
        if self.profile:
            out.append("    RT_PROF(0x%08xu);" % f.start)
        labels = set(f.labels) | {f.start} | set(f.entries)
        a = f.start
        skip_label_goto = None
        while a < f.end:
            i = self.insn(a)
            if a in labels:
                out.append("L_%08x:;" % a)
            if i.has_delay_slot:
                d = self.insn(a + 4) if a + 4 < f.end else decode(0, a + 4)
                if d.has_delay_slot:
                    d = decode(0, a + 4)  # branch in delay slot: not supported, treated as nop
                delay = self.op(d)
                out.append("    " + self.control(f, i, delay))
                if a + 4 in labels:
                    # The delay slot is itself a branch target: the fall-through path above
                    # already executed it, so skip over the separate copy emitted here.
                    if a + 8 < f.end:
                        labels.add(a + 8)
                        out.append("    goto L_%08x;" % (a + 8))
                    else:
                        out.append("    " + self.fall_off(f))
                    out.append("L_%08x:;" % (a + 4))
                    out.append("    " + delay)
                a += 8
                continue
            out.append("    " + self.op(i) + "  /* %08x */" % a if i.mn != "nop" else "    ;")
            if a in self.hooks:
                out.append("    %s(c);  /* port hook */" % self.hooks[a])
            a += 4
        out.append("    " + self.fall_off(f))
        out.append("}")
        if has_entries:
            out.append("%svoid %s(Ctx* c) { i_%08x(c, 0x%08xu); }" % ("static " if traced else "", name, f.start, f.start))
            for e in sorted(f.entries):
                out.append("void e_%08x(Ctx* c) { i_%08x(c, 0x%08xu); }" % (e, f.start, e))
        if traced:
            out.append("void f_%08x(Ctx* c) { rt_trace_enter(c, 0x%08xu); f_%08x_impl(c); rt_trace_exit(c, 0x%08xu); }"
                       % (f.start, f.start, f.start, f.start))
        return "\n".join(out)

    def fall_off(self, f):
        """Execution running past the last instruction continues into the next function."""
        if f.end in self.funcs:
            return "{ f_%08x(c); return; }" % f.end
        return "rt_fell_off(c, 0x%08xu);" % f.end

    def control(self, f, i, delay):
        mn, pc = i.mn, i.pc
        if mn == "j":
            return "%s %s" % (delay, self.jump(f, i.target, pc))
        if mn == "jal":
            return "%s R(31) = 0x%08xu; %s" % (delay, pc + 8, self.call_expr(i.target, pc))
        if mn == "jr":
            if i.rs == 31:
                if f.bal_returns:
                    cases = " ".join("case 0x%08xu: goto L_%08x;" % (t, t) for t in sorted(f.bal_returns))
                    return "{ uint32_t t = R(31); %s switch (t) { %s default: return; } }" % (delay, cases)
                return "%s return;" % delay
            cases = " ".join("case 0x%08xu: goto L_%08x;" % (t, t) for t in sorted(f.jump_table_labels))
            return ("{ uint32_t t = R(%d); %s switch (t) { %s default: call_indirect(c, t); return; } }"
                    % (i.rs, delay, cases))
        if mn == "jalr":
            link = "R(%d) = 0x%08xu;" % (i.rd, pc + 8) if i.rd else ""
            return "{ uint32_t t = R(%d); %s %s call_indirect(c, t); }" % (i.rs, delay, link)
        cond = self.cond(i)
        tgt = i.branch_target
        if mn in ("bltzal", "bgezal", "bltzall", "bgezall"):
            # The link register is written whether or not the branch is taken.
            link = "R(31) = 0x%08xu;" % (pc + 8)
            if f.start <= tgt < f.end:
                go = "goto L_%08x;" % tgt
                if i.is_likely:
                    return "{ int cnd = %s; %s if (cnd) { %s %s } }" % (cond, link, delay, go)
                return "{ int cnd = %s; %s %s if (cnd) %s }" % (cond, link, delay, go)
            call = self.call_expr(tgt, pc)
            if i.is_likely:
                return "{ int cnd = %s; %s if (cnd) { %s %s } }" % (cond, link, delay, call)
            return "{ int cnd = %s; %s %s if (cnd) { %s } }" % (cond, link, delay, call)
        go = self.jump(f, tgt, pc)
        if i.is_likely:
            return "if (%s) { %s %s }" % (cond, delay, go)
        return "{ int cnd = %s; %s if (cnd) %s }" % (cond, delay, go)


def generate(mod, funcs, starts, outdir, per_file=150, trace=(), watch=False, profile=False):
    """Single module (kept for tools that recompile one module)."""
    return generate_modules([(mod, funcs, starts, "")], outdir, per_file, trace, watch, profile)


def generate_modules(modules, outdir, per_file=150, trace=(), watch=False, profile=False):
    """modules: [(PspModule, funcs, starts, path on disc)], the main module (BOOT.BIN) first.
    All share one function table and one import table, so code and pointers can cross
    between them; an import that another module exports is linked directly to it."""
    os.makedirs(outdir, exist_ok=True)
    exports = {}
    for mod, _, _, _ in modules:
        for e in mod.exports:
            exports.setdefault((e.lib, e.nid), e.addr)
    gens, stub_base = [], 0
    for mod, funcs, starts, _ in modules:
        linked = {s.addr: exports[(s.lib, s.nid)] for s in mod.stubs if (s.lib, s.nid) in exports}
        gens.append(Gen(mod, funcs, starts, trace, watch, profile, stub_base, linked))
        stub_base += len(mod.stubs)
    with open(os.path.join(outdir, "funcs.h"), "w") as h:
        h.write("/* Generated by tools/recomp - do not edit, do not distribute. */\n#pragma once\n#include \"psp/recomp.h\"\n")
        for _, funcs, starts, _ in modules:
            for s in starts:
                f = funcs[s]
                h.write("void f_%08x(Ctx* c);\n" % f.start)
                for e in sorted(f.entries):
                    h.write("void e_%08x(Ctx* c);\n" % e)
    files = []
    for k, (g, (mod, funcs, starts, _)) in enumerate(zip(gens, modules)):
        # Calls into another module resolve through its functions too.
        for g2, (_, funcs2, starts2, _) in zip(gens, modules):
            if g2 is not g:
                for a, f in funcs2.items():
                    g.funcs.setdefault(a, f)
        ordered = [funcs[s] for s in starts]
        prefix = "code_" if k == 0 else "code_m%d_" % k
        for n in range(0, len(ordered), per_file):
            path = os.path.join(outdir, "%s%03d.c" % (prefix, n // per_file))
            files.append(path)
            with open(path, "w") as out:
                out.write("/* Generated by tools/recomp - do not edit, do not distribute. */\n#include \"funcs.h\"\n\n")
                for f in ordered[n:n + per_file]:
                    out.write(g.function(f) + "\n\n")
    # Address -> function table for indirect calls (sorted for binary search).
    with open(os.path.join(outdir, "table.c"), "w") as t:
        t.write("/* Generated by tools/recomp - do not edit, do not distribute. */\n#include \"funcs.h\"\n\n")
        entries = []
        for _, funcs, starts, _ in modules:
            for s in starts:
                f = funcs[s]
                entries.append((f.start, "f_%08x" % f.start))
                for e in f.entries:
                    entries.append((e, "e_%08x" % e))
        entries.sort()
        t.write("const GuestFunc g_guest_funcs[] = {\n")
        for a, n in entries:
            t.write("    {0x%08xu, %s},\n" % (a, n))
        t.write("};\nconst unsigned g_guest_func_count = %d;\n\n" % len(entries))
        t.write("const HleImport g_hle_imports[] = {\n")
        count = 0
        for mod, _, _, _ in modules:
            for s in mod.stubs:
                t.write("    {\"%s\", 0x%08xu, 0x%08xu},\n" % (s.lib, s.nid, s.addr))
                count += 1
        t.write("};\nconst unsigned g_hle_import_count = %d;\n" % count)
        main = modules[0][0]
        t.write("const uint32_t g_module_base = 0x%08xu, g_module_entry = 0x%08xu;\n" % (main.base, main.entry))
        # Further modules, loaded by sceKernelLoadModule at their fixed base.
        t.write("const ExtraModule g_extra_modules[] = {\n")
        for mod, _, _, path in modules[1:]:
            t.write("    {\"%s\", 0x%08xu, 0x%08xu, 0x%08xu},\n" % (path, mod.base, mod.seg_memsz, mod.entry or 0))
        t.write("    {0, 0, 0, 0}\n};\nconst unsigned g_extra_module_count = %d;\n" % (len(modules) - 1))
    return gens[0], files
