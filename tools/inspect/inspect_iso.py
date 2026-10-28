#!/usr/bin/env python3
"""Phase 1 inspection of the user's own Coded Arms: Contagion UMD image (ISO or CSO).

Reports, without modifying anything:
  * hashes of the image and of the executables (for TECHNICAL_NOTES / reproducibility)
  * full ISO 9660 file listing and a magic-number survey of the data files
  * UMD_DATA.BIN and PARAM.SFO (disc id, title, versions)
  * state of BOOT.BIN / EBOOT.BIN: plain ELF/PRX, zero-filled, or encrypted (~PSP)
  * for a plain PRX: sections, module info, entry point, relocation types and every
    imported NID, compared with the NIDs the CodedArmsPC HLE runtime already implements

Usage:
  inspect_iso.py GAME.iso [--json report.json] [--known imports_ULES00124.tsv]

Only the Python standard library is used. The image is opened read-only.
"""
import argparse
import collections
import hashlib
import json
import struct
import sys
import zlib

SECTOR = 2048


# ---- image access (ISO or CSO/CISO) ------------------------------------------------------
class Image:
    """Random access to the 2048-byte sectors of a plain ISO or a CSO (CISO v1) file."""

    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(24)
        self.cso = head[:4] == b"CISO"
        if self.cso:
            hsize, total, block, ver, align = struct.unpack_from("<IQIBB", head, 4)
            self.size, self.block, self.align = total, block, align
            n = (total + block - 1) // block
            self.f.seek(24)
            self.index = struct.unpack("<%dI" % (n + 1), self.f.read(4 * (n + 1)))
        else:
            self.f.seek(0, 2)
            self.size = self.f.tell()

    def _cso_block(self, i):
        a, b = self.index[i], self.index[i + 1]
        plain = a & 0x80000000
        start, end = (a & 0x7FFFFFFF) << self.align, (b & 0x7FFFFFFF) << self.align
        self.f.seek(start)
        raw = self.f.read(end - start)
        return raw[:self.block] if plain else zlib.decompress(raw, -15)

    def read(self, offset, size):
        if not self.cso:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            i, o = divmod(offset, self.block)
            chunk = self._cso_block(i)[o:o + size]
            if not chunk:
                break
            out += chunk
            offset += len(chunk)
            size -= len(chunk)
        return bytes(out)

    def hashes(self):
        """Hashes of the uncompressed ISO contents (identical for an ISO and its CSO)."""
        h = {"md5": hashlib.md5(), "sha1": hashlib.sha1(), "crc32": 0}
        off = 0
        step = 1 << 20
        while off < self.size:
            d = self.read(off, min(step, self.size - off))
            if not d:
                break
            h["md5"].update(d)
            h["sha1"].update(d)
            h["crc32"] = zlib.crc32(d, h["crc32"])
            off += len(d)
        return {"size": self.size, "md5": h["md5"].hexdigest(), "sha1": h["sha1"].hexdigest(),
                "crc32": "%08x" % h["crc32"]}


# ---- ISO 9660 -----------------------------------------------------------------------------
def walk_iso(img):
    """Returns {path: (lba, size)} for every file of the image."""
    pvd = img.read(16 * SECTOR, SECTOR)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        raise ValueError("not an ISO 9660 image")
    files = {}

    def walk(lba, size, prefix, depth):
        d = img.read(lba * SECTOR, size)
        p = 0
        while p < len(d):
            ln = d[p]
            if ln == 0:
                p = (p // SECTOR + 1) * SECTOR
                continue
            flba, fsize = struct.unpack_from("<I", d, p + 2)[0], struct.unpack_from("<I", d, p + 10)[0]
            flags = d[p + 25]
            raw = d[p + 33:p + 33 + d[p + 32]]
            p += ln
            if raw in (b"\0", b"\1"):
                continue
            name = raw.decode("latin-1").split(";")[0]
            path = prefix + name
            if flags & 2:
                if depth < 16:
                    walk(flba, fsize, path + "/", depth + 1)
            else:
                files[path] = (flba, fsize)

    root = pvd[156:156 + 34]
    walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", 0)
    return files


def read_file(img, files, path):
    for k, (lba, size) in files.items():
        if k.upper() == path.upper():
            return img.read(lba * SECTOR, size)
    return None


# ---- PARAM.SFO ----------------------------------------------------------------------------
def parse_sfo(d):
    if not d or d[:4] != b"\0PSF":
        return None
    _, keys, vals, n = struct.unpack_from("<IIII", d, 4)
    out = {}
    for i in range(n):
        ko, fmt, ln, _, vo = struct.unpack_from("<HHIII", d, 20 + i * 16)
        key = d[keys + ko:d.index(b"\0", keys + ko)].decode()
        v = d[vals + vo:vals + vo + ln]
        out[key] = struct.unpack("<I", v[:4])[0] if fmt == 0x0404 else v.split(b"\0")[0].decode("utf-8", "replace")
    return out


# ---- executables --------------------------------------------------------------------------
def classify_exe(d):
    if d is None:
        return "absent"
    if not d.strip(b"\0"):
        return "zero-filled (use EBOOT.BIN; it must be decrypted first)"
    if d[:4] == b"\x7fELF":
        return "plain ELF"
    if d[:4] == b"~PSP":
        return "encrypted ~PSP container (module name %r)" % d[10:38].split(b"\0")[0].decode("latin-1")
    return "unknown (magic %s)" % d[:4].hex()


def parse_prx(d, base=0x08804000):
    """Header-level analysis of a PSP PRX: sections, module info, relocations, imports."""
    e_type, mach, _, entry, phoff, shoff, flags, _, _, phnum, _, shnum, shstrndx = struct.unpack_from(
        "<HHIIIIIHHHHHH", d, 16)
    info = {"e_type": "%#06x" % e_type, "machine": mach, "e_flags": "%#010x" % flags,
            "entry": "%#010x" % (entry + base if e_type == 0xFFA0 else entry), "segments": []}
    for i in range(phnum):
        p = struct.unpack_from("<8I", d, phoff + i * 32)
        info["segments"].append({"type": "%#x" % p[0], "offset": p[1], "vaddr": "%#x" % p[2],
                                 "paddr": "%#x" % p[3], "filesz": p[4], "memsz": p[5], "flags": p[6]})
    secs = [struct.unpack_from("<10I", d, shoff + i * 40) for i in range(shnum)] if shoff else []
    names = {}
    if secs and shstrndx < len(secs):
        st = secs[shstrndx][4]
        for i, s in enumerate(secs):
            names[i] = d[st + s[0]:d.index(b"\0", st + s[0])].decode("latin-1")
    info["sections"] = [{"name": names.get(i, ""), "type": "%#x" % s[1], "addr": "%#x" % s[3],
                         "offset": s[4], "size": s[5]} for i, s in enumerate(secs)]
    seg_off = struct.unpack_from("<I", d, phoff + 4)[0] if phnum else 0

    def vread(vaddr, n):  # segment vaddr is 0 for a PRX; file offset = segment offset + vaddr
        return d[seg_off + vaddr:seg_off + vaddr + n]

    def cstr(vaddr):
        o = seg_off + vaddr
        return d[o:d.index(b"\0", o)].decode("latin-1")

    reloc_types = collections.Counter()
    for i, s in enumerate(secs):
        if s[1] == 0x700000A0:
            for o in range(0, s[5], 8):
                reloc_types[struct.unpack_from("<II", d, s[4] + o)[1] & 0xFF] += 1
        elif s[1] == 0x700000A1:
            reloc_types["type-A1 (compressed, unsupported by pspelf.py)"] += 1
    info["relocations"] = {str(k): v for k, v in sorted(reloc_types.items(), key=str)}
    by_name = {names.get(i): s for i, s in enumerate(secs)}
    mi = by_name.get(".rodata.sceModuleInfo")
    if mi is None and phnum:  # module info pointer lives in the segment paddr for PRX
        mi_addr = struct.unpack_from("<I", d, phoff + 12)[0] & 0x7FFFFFFF
        mi = (0, 0, 0, mi_addr) if mi_addr else None
    if mi:
        attr, ver, mname = struct.unpack_from("<HH28s", vread(mi[3], 32))
        gp, ent, ent_end, stub, stub_end = struct.unpack_from("<5I", vread(mi[3], 52), 32)
        info["module"] = {"name": mname.split(b"\0")[0].decode("latin-1"), "attr": "%#x" % attr,
                          "version": "%d.%d" % (ver >> 8, ver & 0xFF), "gp": "%#x" % gp}
        imports = []
        a = stub
        while a < stub_end:
            name_p, _, lens, nids_p, funcs_p = struct.unpack_from("<IIIII", vread(a, 20))
            entlen, nfunc = lens & 0xFF, lens >> 16
            lib = cstr(name_p) if name_p else "?"
            for k in range(nfunc):
                imports.append((lib, "%08x" % struct.unpack_from("<I", vread(nids_p + 4 * k, 4))[0]))
            a += max(entlen, 5) * 4
        info["imports"] = imports
    return info


def load_known(path):
    known = {}
    if path:
        for line in open(path, encoding="utf-8"):
            if line.startswith("#") or not line.strip():
                continue
            c = line.rstrip("\n").split("\t")
            known[c[1].lower()] = c[2]
    return known


def survey(img, files, limit=64):
    """Groups data files by their first bytes (magic) to spot the container formats."""
    groups = collections.defaultdict(list)
    for path, (lba, size) in sorted(files.items()):
        if "/UPDATE/" in path.upper():
            continue
        head = img.read(lba * SECTOR, min(16, size))
        printable = bytes(c if 32 <= c < 127 else 46 for c in head[:16])
        groups[printable.decode()].append((path, size))
    return {k: v[:limit] for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("image")
    ap.add_argument("--json")
    ap.add_argument("--known", help="imports TSV of an already supported game (CodedArmsPC)")
    ap.add_argument("--no-hash", action="store_true")
    a = ap.parse_args()

    img = Image(a.image)
    rep = {"image": a.image, "format": "CSO" if img.cso else "ISO"}
    if not a.no_hash:
        rep["hash"] = img.hashes()
    files = walk_iso(img)
    rep["files"] = {k: v[1] for k, v in sorted(files.items())}
    umd = read_file(img, files, "UMD_DATA.BIN")
    rep["umd_data"] = umd.split(b"\0")[0].decode("latin-1") if umd else None
    rep["param_sfo"] = parse_sfo(read_file(img, files, "PSP_GAME/PARAM.SFO"))
    rep["executables"] = {}
    for exe in ("PSP_GAME/SYSDIR/BOOT.BIN", "PSP_GAME/SYSDIR/EBOOT.BIN"):
        d = read_file(img, files, exe)
        e = {"state": classify_exe(d)}
        if d is not None:
            e.update(size=len(d), md5=hashlib.md5(d).hexdigest(), sha1=hashlib.sha1(d).hexdigest())
            if d[:4] == b"\x7fELF":
                e["prx"] = parse_prx(d)
        rep["executables"][exe] = e
    rep["magic_survey"] = survey(img, files)

    known = load_known(a.known)
    for exe, e in rep["executables"].items():
        imps = e.get("prx", {}).get("imports")
        if imps and known:
            missing = [(lib, nid) for lib, nid in imps if nid not in known]
            e["imports_missing_in_hle"] = missing

    # ---- human readable summary ----
    print("Image: %s (%s)" % (a.image, rep["format"]))
    if "hash" in rep:
        h = rep["hash"]
        print("  size %d  MD5 %s  SHA-1 %s  CRC32 %s" % (h["size"], h["md5"], h["sha1"], h["crc32"]))
    print("UMD_DATA.BIN: %s" % rep["umd_data"])
    sfo = rep["param_sfo"] or {}
    for k in ("TITLE", "DISC_ID", "DISC_VERSION", "PSP_SYSTEM_VER", "REGION", "PARENTAL_LEVEL"):
        if k in sfo:
            print("  %-15s %s" % (k, sfo[k]))
    print("Files: %d" % len(files))
    for exe, e in rep["executables"].items():
        print("%s: %s%s" % (exe, e["state"], ("  %d B sha1 %s" % (e["size"], e["sha1"])) if "size" in e else ""))
        p = e.get("prx")
        if p:
            print("  type %s entry %s module %s, %d sections, relocations %s"
                  % (p["e_type"], p["entry"], p.get("module", {}).get("name"), len(p["sections"]), p["relocations"]))
            if "imports" in p:
                libs = collections.Counter(lib for lib, _ in p["imports"])
                print("  imports: %d functions from %d libraries" % (len(p["imports"]), len(libs)))
                for lib, n in sorted(libs.items()):
                    print("    %-28s %d" % (lib, n))
            if "imports_missing_in_hle" in e:
                m = e["imports_missing_in_hle"]
                print("  NIDs not implemented by the CodedArmsPC runtime: %d" % len(m))
                for lib, nid in m:
                    print("    %s %s" % (lib, nid))
    print("Magic survey (first 16 bytes -> files):")
    for k, v in list(rep["magic_survey"].items())[:40]:
        print("  %-18s %4d  e.g. %s" % (k, len(v), v[0][0]))
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump(rep, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
