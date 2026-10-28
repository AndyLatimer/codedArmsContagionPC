#!/usr/bin/env python3
"""Tests tools/inspect/inspect_iso.py against a synthetic UMD image built here (no game data):
a tiny ISO 9660 with UMD_DATA.BIN, PARAM.SFO, a hand-made PRX as BOOT.BIN and a "~PSP"
EBOOT.BIN, read both as ISO and as CSO."""
import os
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools", "inspect"))
import inspect_iso  # noqa: E402

S = 2048


def make_sfo(entries):
    keys, vals, idx = b"", b"", b""
    for k, v in entries:
        if isinstance(v, int):
            data, fmt = struct.pack("<I", v), 0x0404
        else:
            data, fmt = v.encode() + b"\0", 0x0204
        data += b"\0" * (-len(data) % 4)
        idx += struct.pack("<HHIII", len(keys), fmt, len(data), len(data), len(vals))
        keys += k.encode() + b"\0"
        vals += data
    keys += b"\0" * (-len(keys) % 4)
    kt = 20 + len(idx)
    return b"\0PSF" + struct.pack("<IIII", 0x101, kt, kt + len(keys), len(entries)) + idx + keys + vals


def make_prx():
    """PRX with .text, .lib.stub (one library, two functions), module info and one REL section."""
    seg = bytearray(0x200)
    # strings / nids at 0x100
    seg[0x100:0x10C] = b"IoFileMgrFor"
    seg[0x100:0x111] = b"IoFileMgrForUser\0"
    struct.pack_into("<II", seg, 0x120, 0x6A638D83, 0x12345678)
    # stub entry at 0x140 (5 words): name, ver/flags, (nfunc<<16)|entlen, nids, funcs
    struct.pack_into("<IIIII", seg, 0x140, 0x100, 0x00010011, (2 << 16) | 5, 0x120, 0x80)
    # module info at 0x160
    struct.pack_into("<HH28s5I", seg, 0x160, 0, 0x0101, b"TestModule", 0, 0, 0, 0x140, 0x154)
    rel = struct.pack("<II", 0x10, 2) + struct.pack("<II", 0x14, 4)
    shstr = b"\0.text\0.lib.stub\0.rodata.sceModuleInfo\0.rel.text\0.shstrtab\0"
    off_seg = 0x100
    off_rel = off_seg + len(seg)
    off_str = off_rel + len(rel)
    off_sh = off_str + len(shstr) + (-(off_str + len(shstr)) % 4)

    def sh(name, typ, addr, off, size):
        return struct.pack("<10I", shstr.index(name.encode() + b"\0") if name else 0, typ, 0, addr, off, size, 0, 0, 4, 0)

    shdrs = (sh("", 0, 0, 0, 0) + sh(".text", 1, 0, off_seg, 0x100) + sh(".lib.stub", 1, 0x140, off_seg + 0x140, 0x14)
             + sh(".rodata.sceModuleInfo", 1, 0x160, off_seg + 0x160, 0x34)
             + sh(".rel.text", 0x700000A0, 0, off_rel, len(rel)) + sh(".shstrtab", 3, 0, off_str, len(shstr)))
    eh = b"\x7fELF\x01\x01\x01" + b"\0" * 9 + struct.pack("<HHIIIIIHHHHHH", 0xFFA0, 8, 1, 0x40, 0x34, off_sh, 0x10a23001,
                                                          0x34, 32, 1, 40, 6, 5)
    ph = struct.pack("<8I", 1, off_seg, 0, 0x160, len(seg), len(seg), 7, 16)
    out = bytearray(eh + ph)
    out += b"\0" * (off_seg - len(out)) + seg + rel + shstr
    out += b"\0" * (off_sh - len(out)) + shdrs
    return bytes(out)


def make_iso(tree):
    """tree: {dir path: {name: bytes}}; only nested dirs used by the test are supported."""
    dirs = sorted(set([""] + [d for d in tree]), key=lambda p: (p.count("/"), p))
    # every intermediate dir must be listed
    for d in list(dirs):
        parts = d.split("/") if d else []
        for i in range(1, len(parts)):
            if "/".join(parts[:i]) not in dirs:
                dirs.append("/".join(parts[:i]))
    dirs = sorted(set(dirs), key=lambda p: (p.count("/") if p else -1, p))
    lba = {d: 20 + i for i, d in enumerate(dirs)}
    nxt = 20 + len(dirs)
    data = {}
    for d in dirs:
        for name, content in tree.get(d, {}).items():
            data[(d, name)] = (nxt, content)
            nxt += max(1, (len(content) + S - 1) // S)
    img = bytearray(nxt * S)

    def rec(name, l, size, isdir):
        n = name if isinstance(name, bytes) else (name + ";1").encode() if not isdir else name.encode()
        r = struct.pack("<BBIIIIBBBBBBBBBBHHB", 0, 0, l, 0, size, 0, 0, 0, 0, 0, 0, 0, 0, 2 if isdir else 0, 0, 0, 1, 1, len(n)) + n
        r += b"\0" * (len(r) % 2)
        return bytes([len(r)]) + r[1:]

    for d in dirs:
        entries = rec(b"\0", lba[d], S, True) + rec(b"\1", lba[d], S, True)
        for sub in dirs:
            if sub and (sub.rsplit("/", 1)[0] if "/" in sub else "") == d:
                entries += rec(sub.split("/")[-1], lba[sub], S, True)
        for name, content in tree.get(d, {}).items():
            l, c = data[(d, name)]
            entries += rec(name, l, len(c), False)
        img[lba[d] * S:lba[d] * S + len(entries)] = entries
    for (d, name), (l, c) in data.items():
        img[l * S:l * S + len(c)] = c
    pvd = bytearray(S)
    pvd[0:6] = b"\x01CD001"
    pvd[156:156 + 34] = rec(b"\0", lba[""], S, True)
    img[16 * S:17 * S] = pvd
    return bytes(img)


def make_cso(iso, block=S):
    n = (len(iso) + block - 1) // block
    body, index = b"", []
    base = 24 + 4 * (n + 1)
    for i in range(n):
        index.append(base + len(body))
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        body += c.compress(iso[i * block:(i + 1) * block]) + c.flush()
    index.append(base + len(body))
    return b"CISO" + struct.pack("<IQIBB2x", 24, len(iso), block, 1, 0) + struct.pack("<%dI" % (n + 1), *index) + body


def main():
    prx = make_prx()
    tree = {
        "": {"UMD_DATA.BIN": b"ULUS-10184|0123456789ABCDEF|0001|G\0"},
        "PSP_GAME": {"PARAM.SFO": make_sfo([("DISC_ID", "ULUS10184"), ("TITLE", "TEST TITLE"), ("PARENTAL_LEVEL", 5)])},
        "PSP_GAME/SYSDIR": {"BOOT.BIN": prx, "EBOOT.BIN": b"~PSP" + b"\0" * 6 + b"Contagion\0" + b"\0" * 100},
        "PSP_GAME/USRDIR": {"DATA.ARC": b"KCEO ARCDT 1.0\0\0" + b"\0" * 32},
    }
    iso = make_iso(tree)
    failures = 0
    with tempfile.TemporaryDirectory() as t:
        known = os.path.join(t, "known.tsv")
        open(known, "w").write("# lib nid name\nIoFileMgrForUser\t6a638d83\tsceIoRead\ti\tixi\n")
        for name, blob in (("t.iso", iso), ("t.cso", make_cso(iso))):
            p = os.path.join(t, name)
            open(p, "wb").write(blob)
            img = inspect_iso.Image(p)
            files = inspect_iso.walk_iso(img)
            checks = {
                "listing": set(files) == {"UMD_DATA.BIN", "PSP_GAME/PARAM.SFO", "PSP_GAME/SYSDIR/BOOT.BIN",
                                          "PSP_GAME/SYSDIR/EBOOT.BIN", "PSP_GAME/USRDIR/DATA.ARC"},
                "sfo": inspect_iso.parse_sfo(inspect_iso.read_file(img, files, "PSP_GAME/PARAM.SFO"))
                == {"DISC_ID": "ULUS10184", "TITLE": "TEST TITLE", "PARENTAL_LEVEL": 5},
                "boot": inspect_iso.read_file(img, files, "PSP_GAME/SYSDIR/BOOT.BIN") == prx,
                "hash": img.hashes()["sha1"] == __import__("hashlib").sha1(iso).hexdigest(),
            }
            info = inspect_iso.parse_prx(prx)
            checks["imports"] = info["imports"] == [("IoFileMgrForUser", "6a638d83"), ("IoFileMgrForUser", "12345678")]
            checks["module"] = info["module"]["name"] == "TestModule"
            checks["relocs"] = info["relocations"] == {"2": 1, "4": 1}
            checks["encrypted"] = inspect_iso.classify_exe(b"~PSP" + b"\0" * 6 + b"X\0" + b"\0" * 40).startswith("encrypted")
            checks["zeroed"] = inspect_iso.classify_exe(b"\0" * 64).startswith("zero-filled")
            # end-to-end CLI run
            r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "..", "tools", "inspect", "inspect_iso.py"), p,
                                "--known", known, "--json", os.path.join(t, "r.json")], capture_output=True, text=True)
            checks["cli"] = r.returncode == 0 and "not implemented by the CodedArmsPC runtime: 1" in r.stdout \
                and "12345678" in r.stdout and "KCEO ARCDT 1.0.." in r.stdout
            for k, ok in checks.items():
                print("%-4s %-10s %s" % (name[-3:], k, "ok" if ok else "FAIL"))
                failures += not ok
            if not checks["cli"]:
                print(r.stdout, r.stderr)
    print("FAILED %d" % failures if failures else "all passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
