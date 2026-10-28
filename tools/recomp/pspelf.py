"""Loader for the PSP relocatable executable (ELF type 0xFFA0, "PRX") used by the game.

Produces the relocated memory image exactly as the PSP module loader would at a given
base address, plus the metadata the recompiler needs: section ranges, every relocated
word (which tells code addresses apart from data), imports (stubs) and the module entry.

Only what the modules this port recompiles use is implemented (verified by counting):
SHT_PSP_REL (0x700000A0) sections with relocation types R_MIPS_32 (2), R_MIPS_26 (4),
R_MIPS_HI16 (5) and R_MIPS_LO16 (6). BOOT.BIN (CA1 and Contagion) has one PT_LOAD segment;
Contagion's PSMF_JK.PRX has two (code at vaddr 0, data after it) and relocations whose
address is relative to the second one (the "address base" segment index in the info word).
"""
import struct

SHT_PSP_REL = 0x700000A0
R_MIPS_32, R_MIPS_26, R_MIPS_HI16, R_MIPS_LO16 = 2, 4, 5, 6
DEFAULT_BASE = 0x08804000  # standard load address of the first user module
MODULE_START_NID = 0xD632ACDB


class Section:
    def __init__(self, name, type_, addr, offset, size):
        self.name, self.type, self.addr, self.offset, self.size = name, type_, addr, offset, size


class Export:
    """One exported function: library name ("" for the module's own syslib), NID, address."""

    def __init__(self, lib, nid, addr):
        self.lib, self.nid, self.addr = lib, nid, addr


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
        self._parse_exports()
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
        self.segments = []  # (vaddr, file offset, filesz, memsz) of each PT_LOAD
        for i in range(phnum):
            (ptype, off, vaddr, _, filesz, memsz, _, _) = struct.unpack_from("<8I", d, phoff + i * 32)
            if ptype == 1:
                self.segments.append((vaddr, off, filesz, memsz))
        if not self.segments:
            raise ValueError("no PT_LOAD segment")
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
        if self.segments[0][0] != 0:
            raise ValueError("first segment vaddr %#x != 0" % self.segments[0][0])
        img = bytearray(max(v + m for v, _, _, m in self.segments))
        for vaddr, off, filesz, _ in self.segments:
            img[vaddr: vaddr + filesz] = self.raw[off: off + filesz]
        self.image = img  # index = vaddr
        self.seg_memsz = len(img)

    def word(self, vaddr):
        return struct.unpack_from("<I", self.image, vaddr)[0]

    def set_word(self, vaddr, value):
        struct.pack_into("<I", self.image, vaddr, value & 0xFFFFFFFF)

    def _relocate(self):
        """Applies relocations. Records every relocated location and its type in
        self.relocs {vaddr: type} so the recompiler knows which words hold addresses.
        The info word holds the type, the segment the offset is relative to and the segment
        whose (loaded) address is added."""
        entries = []
        for s in self.sections:
            if s.type == SHT_PSP_REL:
                for i in range(0, s.size, 8):
                    off, info = struct.unpack_from("<II", self.raw, s.offset + i)
                    ofs_seg, addr_seg = (info >> 8) & 0xFF, (info >> 16) & 0xFF
                    entries.append((self.segments[ofs_seg][0] + off, info & 0xFF, self.segments[addr_seg][0]))
        self.relocs = {}
        self.hilo_values = set()  # full addresses built by relocated lui/addiu (or lw) pairs
        pending_hi = []
        for off, typ, seg_vaddr in entries:
            base = self.base + seg_vaddr
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

    def _parse_exports(self):
        """.lib.ent: {u32 name, u16 version, u16 attr, u8 len (words), u8 vcount, u16 fcount,
        u32 table}; the table holds fcount + vcount NIDs, then as many addresses."""
        self.exports = []
        try:
            ent = self.section(".lib.ent")
        except KeyError:
            return
        a = ent.addr
        while a < ent.addr + ent.size:
            name_p, _, _, entlen, vcount, fcount, table = struct.unpack_from("<IHHBBHI", self.image, a)
            lib = self._cstr(name_p - self.base) if name_p else ""
            n = fcount + vcount
            for i in range(fcount):
                nid = self.word(table - self.base + i * 4)
                self.exports.append(Export(lib, nid, self.word(table - self.base + (n + i) * 4)))
            a += max(entlen, 4) * 4

    def export(self, nid, lib=None):
        for e in self.exports:
            if e.nid == nid and (lib is None or e.lib == lib):
                return e.addr
        return None

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
        """The module's start function: e_entry, or the syslib's module_start export when the
        header has none (0xFFFFFFFF, as in PSMF_JK.PRX)."""
        if self.e_entry != 0xFFFFFFFF:
            return self.e_entry + self.base
        return self.export(MODULE_START_NID, "")  # None: the module has no start function

    def bytes_at(self, vaddr, n):
        return bytes(self.image[vaddr - self.base: vaddr - self.base + n])
