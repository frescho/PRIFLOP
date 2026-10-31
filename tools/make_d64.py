#!/usr/bin/env python3
"""Build a valid .d64 disk image, optionally with files in it.

Why this exists: sd2iec's D64 support has never been exercised on this
port, and the FatFs removal (2026-09-05) revealed that mounting was in
fact completely broken -- d64_mount() read the image size from a FatFs
handle this backend never opens, so every image came back as size 0 and
failed to identify, and d64_open_write() tested an FA_WRITE flag that was
always clear, so every write to a mounted image returned WRITE PROTECT.
Both are fixed but untested, and testing needs an image to mount.

Building one here rather than downloading it keeps the test self-contained
and means the image's contents are known exactly.

  ./make_d64.py out.d64 "DISK NAME" [file.prg ...]

The checked-in tools/testdisk.d64 was built with:

  ./make_d64.py testdisk.d64 TESTDISK HELLO.PRG IECTEST.PRG

and is the image tools/imgtest.bas expects to find on the host volume:
its INREAD step reads IECTEST back out of the mounted image, which is
why imgtest never formats this one.

The result is a 35-track, 683-sector, 174848-byte image -- the only size
d64ops.c's D64_TYPE_D41 branch accepts.
"""
import sys, os

# Sectors per track: 1-17 -> 21, 18-24 -> 19, 25-30 -> 18, 31-35 -> 17.
def spt(track):
    if track <= 17: return 21
    if track <= 24: return 19
    if track <= 30: return 18
    return 17

TRACKS = 35
TOTAL_SECTORS = sum(spt(t) for t in range(1, TRACKS + 1))   # 683
IMAGE_SIZE = TOTAL_SECTORS * 256                            # 174848

def offset(track, sector):
    """Byte offset of a (track, sector) inside the image."""
    off = sum(spt(t) for t in range(1, track)) * 256
    return off + sector * 256

def petscii(s, length=16, pad=0xA0):
    """CBM names are upper-case, padded with 0xA0 (shifted space)."""
    b = s.upper().encode("ascii", "replace")[:length]
    return b + bytes([pad]) * (length - len(b))


class D64:
    def __init__(self, name="TESTDISK", disk_id="2A"):
        self.img = bytearray(IMAGE_SIZE)
        # free[track] = set of free sector numbers
        self.free = {t: set(range(spt(t))) for t in range(1, TRACKS + 1)}
        # Track 18 sectors 0 and 1 hold the BAM and the first dir sector.
        self.free[18].discard(0)
        self.free[18].discard(1)
        self.dir_entries = []
        self.name = name
        self.disk_id = disk_id
        # Empty first directory sector: no next sector.
        self.img[offset(18, 1) + 0] = 0x00
        self.img[offset(18, 1) + 1] = 0xFF

    def alloc(self):
        """Next free sector, 1541-ish order: outward from track 17, skipping 18."""
        order = list(range(17, 0, -1)) + list(range(19, TRACKS + 1))
        for t in order:
            if self.free[t]:
                s = min(self.free[t])
                self.free[t].discard(s)
                return t, s
        raise RuntimeError("disk full")

    def write_file(self, name, data, ftype=0x82):
        """Store data as a chained sector list and add a directory entry."""
        chunks = [data[i:i + 254] for i in range(0, len(data), 254)] or [b""]
        sectors = [self.alloc() for _ in chunks]

        for i, (t, s) in enumerate(sectors):
            o = offset(t, s)
            if i + 1 < len(sectors):
                nt, ns = sectors[i + 1]
                self.img[o] = nt
                self.img[o + 1] = ns
            else:
                self.img[o] = 0x00
                # Last block: byte 1 is the index of the last used byte.
                self.img[o + 1] = 1 + len(chunks[i])
            self.img[o + 2:o + 2 + len(chunks[i])] = chunks[i]

        self.dir_entries.append((ftype, sectors[0], name, len(sectors)))

    def _write_bam(self):
        o = offset(18, 0)
        self.img[o + 0] = 18          # first directory sector
        self.img[o + 1] = 1
        self.img[o + 2] = 0x41        # DOS version 'A'
        self.img[o + 3] = 0x00

        for t in range(1, TRACKS + 1):
            e = o + 4 + (t - 1) * 4
            freeset = self.free[t]
            self.img[e] = len(freeset)
            bits = 0
            for s in freeset:
                bits |= (1 << s)
            self.img[e + 1] = bits & 0xFF
            self.img[e + 2] = (bits >> 8) & 0xFF
            self.img[e + 3] = (bits >> 16) & 0xFF

        self.img[o + 144:o + 160] = petscii(self.name, 16)
        self.img[o + 160] = 0xA0
        self.img[o + 161] = 0xA0
        self.img[o + 162:o + 164] = petscii(self.disk_id, 2, pad=0x30)
        self.img[o + 164] = 0xA0
        self.img[o + 165:o + 167] = b"2A"
        self.img[o + 167:o + 171] = bytes([0xA0]) * 4

    def _write_dir(self):
        """Directory sectors, 8 entries of 32 bytes each, chained."""
        per_sector = 8
        needed = max(1, (len(self.dir_entries) + per_sector - 1) // per_sector)
        dir_sectors = [(18, 1)]
        while len(dir_sectors) < needed:
            for s in range(2, spt(18)):
                if s in self.free[18]:
                    self.free[18].discard(s)
                    dir_sectors.append((18, s))
                    break
            else:
                raise RuntimeError("directory full")

        for idx, (t, s) in enumerate(dir_sectors):
            o = offset(t, s)
            if idx + 1 < len(dir_sectors):
                self.img[o] = dir_sectors[idx + 1][0]
                self.img[o + 1] = dir_sectors[idx + 1][1]
            else:
                self.img[o] = 0x00
                self.img[o + 1] = 0xFF

            for slot in range(per_sector):
                n = idx * per_sector + slot
                if n >= len(self.dir_entries):
                    break
                ftype, (ft, fs), name, blocks = self.dir_entries[n]
                e = o + 2 + slot * 32
                self.img[e + 0] = ftype
                self.img[e + 1] = ft
                self.img[e + 2] = fs
                self.img[e + 3:e + 19] = petscii(name, 16)
                self.img[e + 28] = blocks & 0xFF
                self.img[e + 29] = (blocks >> 8) & 0xFF

    def build(self):
        self._write_dir()     # may allocate more dir sectors, so before the BAM
        self._write_bam()
        return bytes(self.img)


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out, diskname, files = sys.argv[1], sys.argv[2], sys.argv[3:]
    d = D64(diskname)
    for f in files:
        with open(f, "rb") as fh:
            data = fh.read()
        name = os.path.splitext(os.path.basename(f))[0]
        d.write_file(name, data)
        print("  added %-16s %d bytes" % (name, len(data)))
    img = d.build()
    assert len(img) == IMAGE_SIZE, len(img)
    with open(out, "wb") as fh:
        fh.write(img)
    free = sum(len(d.free[t]) for t in range(1, TRACKS + 1) if t != 18)
    print("wrote %s (%d bytes), %d blocks free" % (out, len(img), free))
    return 0


if __name__ == "__main__":
    sys.exit(main())
