"""Decoder for the PSP Allegrex CPU: MIPS32 (subset) + Allegrex extensions + FPU + VFPU.

decode(word, pc) -> Insn with a mnemonic and the raw fields. Semantics live in the code
generator; this module only classifies. Unknown encodings decode as mnemonic "unknown"
so coverage can be measured.
"""

GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5",
       "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp",
       "sp", "fp", "ra"]


class Insn:
    __slots__ = ("word", "pc", "op", "rs", "rt", "rd", "sa", "funct", "imm", "simm", "target", "mn")

    def __init__(self, word, pc):
        self.word, self.pc = word, pc
        self.op = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)
        self.mn = "unknown"

    # Control-flow helpers -------------------------------------------------------------
    @property
    def branch_target(self):
        return (self.pc + 4 + (self.simm << 2)) & 0xFFFFFFFF

    @property
    def is_branch(self):
        return self.mn in BRANCHES

    @property
    def is_likely(self):
        return self.mn in LIKELY

    @property
    def has_delay_slot(self):
        return self.mn in BRANCHES or self.mn in ("j", "jal", "jr", "jalr")

    def __repr__(self):
        return "%08x: %08x %s" % (self.pc, self.word, self.mn)


BRANCHES = {"beq", "bne", "blez", "bgtz", "beql", "bnel", "blezl", "bgtzl", "bltz", "bgez",
            "bltzl", "bgezl", "bltzal", "bgezal", "bltzall", "bgezall", "bc1f", "bc1t", "bc1fl",
            "bc1tl", "bvf", "bvt", "bvfl", "bvtl"}
LIKELY = {"beql", "bnel", "blezl", "bgtzl", "bltzl", "bgezl", "bltzall", "bgezall", "bc1fl",
          "bc1tl", "bvfl", "bvtl"}

_SPECIAL = {0: "sll", 2: "srl", 3: "sra", 4: "sllv", 6: "srlv", 7: "srav", 8: "jr", 9: "jalr",
            0xA: "movz", 0xB: "movn", 0xC: "syscall", 0xD: "break", 0xF: "sync", 0x10: "mfhi",
            0x11: "mthi", 0x12: "mflo", 0x13: "mtlo", 0x16: "clz", 0x17: "clo", 0x18: "mult",
            0x19: "multu", 0x1A: "div", 0x1B: "divu", 0x1C: "madd", 0x1D: "maddu", 0x20: "add",
            0x21: "addu", 0x22: "sub", 0x23: "subu", 0x24: "and", 0x25: "or", 0x26: "xor",
            0x27: "nor", 0x2A: "slt", 0x2B: "sltu", 0x2C: "max", 0x2D: "min", 0x2E: "msub",
            0x2F: "msubu"}
_REGIMM = {0: "bltz", 1: "bgez", 2: "bltzl", 3: "bgezl", 0x10: "bltzal", 0x11: "bgezal",
           0x12: "bltzall", 0x13: "bgezall"}
_MAJOR = {2: "j", 3: "jal", 4: "beq", 5: "bne", 6: "blez", 7: "bgtz", 8: "addi", 9: "addiu",
          0xA: "slti", 0xB: "sltiu", 0xC: "andi", 0xD: "ori", 0xE: "xori", 0xF: "lui",
          0x14: "beql", 0x15: "bnel", 0x16: "blezl", 0x17: "bgtzl", 0x20: "lb", 0x21: "lh",
          0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr", 0x28: "sb", 0x29: "sh",
          0x2A: "swl", 0x2B: "sw", 0x2E: "swr", 0x2F: "cache", 0x30: "ll", 0x31: "lwc1",
          0x32: "lv.s", 0x38: "sc", 0x39: "swc1", 0x3A: "sv.s"}
_FPU_S = {0: "add.s", 1: "sub.s", 2: "mul.s", 3: "div.s", 4: "sqrt.s", 5: "abs.s", 6: "mov.s",
          7: "neg.s", 0xC: "round.w.s", 0xD: "trunc.w.s", 0xE: "ceil.w.s", 0xF: "floor.w.s",
          0x24: "cvt.w.s"}
_FPU_COND = ["c.f.s", "c.un.s", "c.eq.s", "c.ueq.s", "c.olt.s", "c.ult.s", "c.ole.s", "c.ule.s",
             "c.sf.s", "c.ngle.s", "c.seq.s", "c.ngl.s", "c.lt.s", "c.nge.s", "c.le.s", "c.ngt.s"]
_V0 = {0: "vadd", 1: "vsub", 2: "vsbn", 7: "vdiv"}
_V1 = {0: "vmul", 1: "vdot", 2: "vscl", 4: "vhdp", 5: "vcrs", 6: "vdet"}
_V3 = {0: "vcmp", 2: "vmin", 3: "vmax", 5: "vscmp", 6: "vsge", 7: "vslt"}
_V4_0 = {0: "vmov", 1: "vabs", 2: "vneg", 3: "vidt", 4: "vsat0", 5: "vsat1", 6: "vzero",
         7: "vone", 0x10: "vrcp", 0x11: "vrsq", 0x12: "vsin", 0x13: "vcos", 0x14: "vexp2",
         0x15: "vlog2", 0x16: "vsqrt", 0x17: "vasin", 0x18: "vnrcp", 0x1A: "vnsin",
         0x1C: "vrexp2"}
_V4_1 = {0: "vrnds", 1: "vrndi", 2: "vrndf1", 3: "vrndf2", 0x12: "vf2h", 0x13: "vh2f",
         0x16: "vsbz", 0x17: "vlgb", 0x18: "vuc2i", 0x19: "vc2i", 0x1A: "vus2i", 0x1B: "vs2i",
         0x1C: "vi2uc", 0x1D: "vi2c", 0x1E: "vi2us", 0x1F: "vi2s"}
_V4_2 = {0: "vsrt1", 1: "vsrt2", 2: "vbfy1", 3: "vbfy2", 4: "vocp", 5: "vsocp", 6: "vfad",
         7: "vavg", 8: "vsrt3", 9: "vsrt4", 0xA: "vsgn", 0x10: "vmfvc", 0x11: "vmtvc",
         0x19: "vt4444", 0x1A: "vt5551", 0x1B: "vt5650"}


def decode(word, pc):
    i = Insn(word, pc)
    op = i.op
    mn = "unknown"
    if op == 0:
        mn = _SPECIAL.get(i.funct, "unknown")
        if mn == "srl" and i.rs == 1:
            mn = "rotr"
        elif mn == "srlv" and i.sa == 1:
            mn = "rotrv"
        elif mn == "sll" and word == 0:
            mn = "nop"
    elif op == 1:
        mn = _REGIMM.get(i.rt, "unknown")
    elif op in _MAJOR:
        mn = _MAJOR[op]
    elif op == 0x10:
        mn = {0: "mfc0", 4: "mtc0"}.get(i.rs, "unknown")
    elif op == 0x11:  # COP1
        if i.rs == 0:
            mn = "mfc1"
        elif i.rs == 2:
            mn = "cfc1"
        elif i.rs == 4:
            mn = "mtc1"
        elif i.rs == 6:
            mn = "ctc1"
        elif i.rs == 8:
            mn = ["bc1f", "bc1t", "bc1fl", "bc1tl"][i.rt & 3]
        elif i.rs == 0x10:
            mn = _FPU_COND[i.funct & 15] if i.funct >= 0x30 else _FPU_S.get(i.funct, "unknown")
        elif i.rs == 0x14 and i.funct == 0x20:
            mn = "cvt.s.w"
    elif op == 0x12:  # COP2 = VFPU transfers / branches
        if i.rs == 3:
            mn = "mfvc" if word & 0x80 else "mfv"
        elif i.rs == 7:
            mn = "mtvc" if word & 0x80 else "mtv"
        elif i.rs == 8:
            mn = ["bvf", "bvt", "bvfl", "bvtl"][i.rt & 3]
    elif op == 0x18:
        mn = _V0.get((word >> 23) & 7, "unknown")
    elif op == 0x19:
        mn = _V1.get((word >> 23) & 7, "unknown")
    elif op == 0x1B:
        mn = _V3.get((word >> 23) & 7, "unknown")
    elif op == 0x1C:
        mn = {0: "halt", 0x24: "mfic", 0x26: "mtic"}.get(i.funct, "unknown")
    elif op == 0x1F:  # SPECIAL3
        if i.funct == 0:
            mn = "ext"
        elif i.funct == 4:
            mn = "ins"
        elif i.funct == 0x20:
            mn = {2: "wsbh", 3: "wsbw", 0x10: "seb", 0x14: "bitrev", 0x18: "seh"}.get(i.sa, "unknown")
    elif op == 0x34:  # VFPU4
        sub = (word >> 21) & 0x1F
        k = (word >> 16) & 0x1F
        if sub == 0:
            mn = _V4_0.get(k, "unknown")
        elif sub == 1:
            mn = _V4_1.get(k, "unknown")
        elif sub == 2:
            mn = _V4_2.get(k, "unknown")
        elif sub == 3:
            mn = "vcst"
        elif sub in (0x10, 0x11, 0x12, 0x13):
            mn = ["vf2in", "vf2iz", "vf2iu", "vf2id"][sub - 0x10]
        elif sub == 0x14:
            mn = "vi2f"
        elif sub == 0x15:
            mn = "vcmov"
        elif sub in (0x18, 0x19, 0x1A, 0x1B):
            mn = "vwbn"
    elif op == 0x35:
        mn = "lvr.q" if word & 2 else "lvl.q"
    elif op == 0x36:
        mn = "lv.q"
    elif op == 0x37:  # VFPU5: prefixes and immediates
        k = (word >> 24) & 3
        mn = ["vpfxs", "vpfxt", "vpfxd", "vfim" if word & 0x00800000 else "viim"][k]
    elif op == 0x3C:  # VFPU6 (bits 22-16 are vt, so the operation is in bits 25-23)
        sub = (word >> 23) & 7
        if sub == 0:
            mn = "vmmul"
        elif sub in (1, 2, 3):
            mn = "vtfm"  # vtfmN/vhtfmN, N = sub + 1; homogeneous when vector size is N - 1
        elif sub == 4:
            mn = "vmscl"
        elif sub == 5:
            # same opcode; vector size picks the operation: triple = vcrsp.t, quad = vqmul.q
            mn = "vcrsp" if (word & 0x8080) == 0x8000 else "vqmul"
        elif sub == 7:
            if (word >> 21) & 3 == 0:
                mn = {0: "vmmov", 3: "vmidt", 6: "vmzero", 7: "vmone"}.get((word >> 16) & 7, "unknown")
            elif (word >> 21) & 3 == 1:
                mn = "vrot"
    elif op == 0x3D:
        mn = "svr.q" if word & 2 else "svl.q"
    elif op == 0x3E:
        mn = "sv.q"
    elif op == 0x3F:
        if word == 0xFFFF0000:
            mn = "vnop"
        elif word == 0xFFFF0320:
            mn = "vsync"
        elif word == 0xFFFF040D:
            mn = "vflush"
    i.mn = mn
    return i
