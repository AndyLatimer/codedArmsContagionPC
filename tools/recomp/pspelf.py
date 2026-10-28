"""Loader for the PSP relocatable executable (ELF type 0xFFA0, "PRX") used by the game.

Produces the relocated memory image exactly as the PSP module loader would at a given
base address, plus the metadata the recompiler needs: section ranges, every relocated
word (which tells code addresses apart from data), imports (stubs) and the module entry.

Only what BOOT.BIN of ULES-00124 actually uses is implemented (verified by counting):
one PT_LOAD segment, SHT_PSP_REL (0x700000A0) sections with relocation types
R_MIPS_32 (2), R_MIPS_26 (4), R_MIPS_HI16 (5) and R_MIPS_LO16 (6), segment index 0.
"""
import struct

SHT_PSP_REL = 0x700000A0
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
DEFAULT_BASE = 0x08804000  # standard load address of the first user module


class Section:
    def __init__(self, name, type_, addr, offset, size):
        self.name, self.type, self.addr, self.offset, self.size = name, type_, addr, offset, size


class Stub:
    """One imported function: library name, NID and the address of its 8-byte stub."""

    def __init__(self, lib, nid, addr):
        self.lib, self.nid, self.addr = lib, nid, addr


class PspModule:
    def __init__(self, data, base=DEFAULT_BASE):
        self.raw = data
        self.base = base
        self._parse_headers()
        self._load_segment()
        self._relocate()
        self._parse_imports()
        self._find_code_ranges()

    # -- headers -----------------------------------------------------------------------
    def _parse_headers(self):
        d = self.raw
        if d[:4] != b"\x7fELF":
            raise ValueError("not an ELF file (is this the encrypted EBOOT.BIN? use BOOT.BIN)")
        (self.e_type, mach, _, self.e_entry, phoff, shoff, _, _, _, phnum, _, shnum,
         shstrndx) = struct.unpack_from("<HHIIIIIHHHHHH", d, 16)
        if self.e_type != 0xFFA0 or mach != 8:
            raise ValueError("unexpected ELF type %#x / machine %d" % (self.e_type, mach))
        if phnum != 1:
            raise ValueError("only single-segment modules are supported (found %d)" % phnum)
        (_, self.seg_offset, self.seg_vaddr, _, self.seg_filesz, self.seg_memsz, _,
         _) = struct.unpack_from("<8I", d, phoff)
        raw_secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)]
        strtab = raw_secs[shstrndx]

        def name(o):
            s = strtab[4] + o
            return d[s:d.index(b"\0", s)].decode()

        self.sections = [Section(name(s[0]), s[1], s[3], s[4], s[5]) for s in raw_secs]

    def section(self, name):
        for s in self.sections:
            if s.name == name:
                return s
        raise KeyError(name)

    # -- image -------------------------------------------------------------------------
    def _load_segment(self):
        img = bytearray(self.seg_memsz)
        img[: self.seg_filesz] = self.raw[self.seg_offset: self.seg_offset + self.seg_filesz]
        self.image = img  # index = vaddr (segment vaddr is 0)
        if self.seg_vaddr != 0:
            raise ValueError("segment vaddr %#x != 0" % self.seg_vaddr)

    def word(self, vaddr):
        return struct.unpack_from("<I", self.image, vaddr)[0]

    def set_word(self, vaddr, value):
        struct.pack_into("<I", self.image, vaddr, value & 0xFFFFFFFF)

    def _relocate(self):
        """Applies relocations. Records every relocated location and its type in
        self.relocs {vaddr: type} so the recompiler knows which words hold addresses."""
        entries = []
        for s in self.sections:
            if s.type == SHT_PSP_REL:
                for i in range(0, s.size, 8):
                    off, info = struct.unpack_from("<II", self.raw, s.offset + i)
                    if (info >> 8) & 0xFF or (info >> 16) & 0xFF:
                        raise ValueError("relocation against segment != 0")
                    entries.append((off, info & 0xFF))
        self.relocs = {}
        self.hilo_values = set()  # full addresses built by relocated lui/addiu (or lw) pairs
        base = self.base
        pending_hi = []
        for off, typ in entries:
            self.relocs[off] = typ
            w = self.word(off)
            if typ == R_MIPS_32:
                self.set_word(off, w + base)
            elif typ == R_MIPS_26:
                target = ((w & 0x03FFFFFF) << 2) + base
                self.set_word(off, (w & 0xFC000000) | ((target >> 2) & 0x03FFFFFF))
            elif typ == R_MIPS_HI16:
                pending_hi.append(off)  # resolved by the following LO16
            elif typ == R_MIPS_LO16:
                lo = self.word(off)
                lo16 = lo & 0xFFFF
                slo = lo16 - 0x10000 if lo16 & 0x8000 else lo16
                for hoff in pending_hi:
                    hw = self.word(hoff)
                    full = ((hw & 0xFFFF) << 16) + slo + base
                    newhi = ((full - (((full & 0xFFFF) ^ 0x8000) - 0x8000)) >> 16) & 0xFFFF
                    self.set_word(hoff, (hw & 0xFFFF0000) | newhi)
                    self.hilo_values.add(full & 0xFFFFFFFF)
                pending_hi = []
                self.set_word(off, (lo & 0xFFFF0000) | ((lo16 + base) & 0xFFFF))
            elif typ != 0:
                raise ValueError("unsupported relocation type %d" % typ)
        if pending_hi:
            raise ValueError("HI16 relocation without LO16")

    # -- imports -----------------------------------------------------------------------
    def _cstr(self, vaddr):
        return bytes(self.image[vaddr: self.image.index(b"\0", vaddr)]).decode()

    def _parse_imports(self):
        stub = self.section(".lib.stub")
        self.stubs = []
        a = stub.addr
        while a < stub.addr + stub.size:
            name_p, _, lens, nids_p, funcs_p = struct.unpack_from("<IIIII", self.image, a)
            entlen, nfunc = lens & 0xFF, lens >> 16
            lib = self._cstr(name_p - self.base)
            for i in range(nfunc):
                nid = self.word(nids_p - self.base + i * 4)
                self.stubs.append(Stub(lib, nid, funcs_p + i * 8))
            a += entlen * 4

    def _find_code_ranges(self):
        """Code lives in .text and, for this compiler, also inside .data: static
        initialisers (C++ constructors) followed by the table of pointers to them.
        Relocations of type R_MIPS_26/HI16/LO16 only ever apply to instructions, so the
        relocated instructions in .data delimit that code; it ends where the following
        pointer table (R_MIPS_32 entries) begins."""
        b = self.base
        t = self.text
        self.code_ranges = [(t.addr + b, t.addr + t.size + b)]
        try:
            d = self.section(".data")
        except KeyError:
            return
        d0, d1 = d.addr, d.addr + d.size
        insn = sorted(o for o, ty in self.relocs.items() if ty in (R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16) and d0 <= o < d1)
        if not insn:
            return
        last = insn[-1]
        tables = sorted(o for o, ty in self.relocs.items() if ty == R_MIPS_32 and last < o < d1)
        end = tables[0] if tables else d1
        self.code_ranges.append((insn[0] + b, end + b))

    # -- helpers -----------------------------------------------------------------------
    @property
    def text(self):
        return self.section(".text")

    @property
    def entry(self):
        return self.e_entry + self.base

    def bytes_at(self, vaddr, n):
        return bytes(self.image[vaddr - self.base: vaddr - self.base + n])
