"""Minimal read-only ISO 9660 reader (enough to fetch files from a UMD image)."""
import struct

SECTOR = 2048


def read_iso_file(iso_path, path):
    with open(iso_path, "rb") as f:
        def sectors(lba, size):
            f.seek(lba * SECTOR)
            return f.read(size)

        pvd = sectors(16, SECTOR)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("not an ISO 9660 image")
        lba, size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        for part in path.split("/"):
            d = sectors(lba, size)
            p = 0
            found = False
            while p < len(d):
                ln = d[p]
                if ln == 0:
                    p = (p // SECTOR + 1) * SECTOR
                    continue
                name = d[p + 33: p + 33 + d[p + 32]].decode("latin-1").split(";")[0]
                if name.upper() == part.upper():
                    lba, size = struct.unpack_from("<I", d, p + 2)[0], struct.unpack_from("<I", d, p + 10)[0]
                    found = True
                    break
                p += ln
            if not found:
                raise FileNotFoundError(path)
        return sectors(lba, size)
