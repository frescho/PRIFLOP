#!/usr/bin/env python3
"""PFM, the PRIFLOP Manager, on the simulated B64.

    tools/external/venv-sim/bin/python tests/primo_pfm_sim.py

The Primo runs its real ROM with our DOS, as in primo_dos_basic_sim.py, and
loads PFM with a bare CMD LOAD from a bit-level drive. Two more patches to
the ROM, outside what PFM sees:
  * 0x0025 (KKBHND, wait for a key) takes the keys from a queue the test
    fills, and halts when it is empty;
  * 0x0080 (DIRCUR) also writes the row and column to two ports.
PFM draws its text into the video RAM itself, so the
test reads the screen back from the video RAM: every cell is matched against
the letters the ROM draws (`Video`), normal or inverse. A cell that is not a
ROM letter shows as '\x7f'.

The drive is primo_dos_sim's IecDrive with what PFM needs of the firmware
(LfsDrive): CD, X and its switches, U0>, XW, the drive's reset, file types in
the listing, and -- from reading lfsops.c and parser.c -- how littlefs names
come about: a name written over the bus goes through pet2asc (ASCII capitals
become small letters, 0xC1-0xDA capitals), and a name is matched ignoring
case. *Unverified on the drive* until step 6f.

What is checked:
  * the listing, page by page, with 0, 1, 13, 14 and 200 files, and its parser
    (header, types, splat files, long names, a name that is not printable);
  * the clean handover: after a file is chosen, the machine is in the state a
    typed CMD LOAD"name" leaves (text, stack, pointers, memory, screen), for a
    BASIC program, machine code with an autostart, and a screen; Q is NEW;
    the negative controls are mutations that must fail it;
  * the settings: PRIFLOP.CFG rewritten, keys kept, unknown lines kept, read
    by the firmware's own parser (netconfig.c, built for the host); the
    switches and the device; the refusals.
  * the memory PFM takes, against the stack it runs on, the cache included;
  * the directory cache: a page back reads nothing, a page forward only its
    own entries, a small cache slides and reads again;
  * the speed: a move and a page, in T-states;
  * its own text drawing: every printable character, as the ROM draws it.
"""
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "primo", "pfm"))
import primo_dos_sim as ds                                  # noqa: E402
import primo_dos_basic_sim as bs                            # noqa: E402
from primo_dos_sim import IecDrive, Channel, dos_message, rec, OK_MSG   # noqa: E402
from primo_turbo_sim import u16                             # noqa: E402
import build_pfm                                            # noqa: E402

HAVE = bs.HAVE and os.path.exists(build_pfm.Z80ASM)
KEYSTUB = bs.STUB + 5           # after the character output's stub
CURSTUB = KEYSTUB + 7
P_KEY, P_ROW, P_COL = 0xFB, 0xFC, 0xFD
KKBHND, DIRCUR = 0x0025, 0x0080
DOWN, UP, LEFT, RIGHT, RET, BRK = 0x0A, 0x1F, 0x08, 0x19, 0x0D, 0x01     # KEYS.PRG on the B64
T_NS = 400.0                    # one T-state at 2.5 MHz
JUNK_FROM, JUNK_TO = 0x4400, 0xD800     # the free RAM, from PFM's origin to under the stack


def tolower_pet(c):             # parser.c
    if 0x61 <= c <= 0x7A:
        return c - 0x20
    if 0xC1 <= c <= 0xDA:
        return c - 0x80
    return c


def pet2asc(name):              # lfsops.c
    out = bytearray()
    for c in name:
        if 192 < c < 128 + 91:
            c -= 128
        elif 64 < c < 91:
            c += 32
        elif 64 < c < 91:
            c += 128
        elif c == 255:
            c = ord("~")
        out.append(c)
    return bytes(out)


def fold(name):
    return bytes(tolower_pet(c) for c in name)


def display(real):              # lfsops.c: small letters shown as capitals
    return bytes(c - 32 if 0x61 <= c <= 0x7A else c for c in real)


def asc2pet(name):              # src/utils.c
    return bytes(c + 128 if 65 <= c <= 90 else c - 32 if 97 <= c <= 122 else
                 c - 128 if 193 <= c <= 218 else c for c in name)


class FolderView:
    """The drive's files as the turbo's model sees them: a name is looked up
    as the firmware's file_open() does (primo_turbo.c), path and folder
    included."""

    def __init__(self, drive):
        self.d = drive

    def _find(self, name):
        files, base = self.d.resolve(name)
        return files, self.d.find(base, files), base

    def get(self, name, default=None):
        files, real, _ = self._find(name)
        return default if real is None else files[real]

    def __contains__(self, name):
        return self._find(name)[1] is not None

    def __getitem__(self, name):
        files, real, _ = self._find(name)
        if real is None:
            raise KeyError(name)
        return files[real]

    def __setitem__(self, name, data):
        files, real, base = self._find(name)
        files[base if real is None else real] = data


def pfm_autostart(cfg):
    """PRIFLOP.CFG's PFM_AUTOSTART as the firmware reads it (netconfig.c):
    the first 1023 bytes, a line ends at \\n or a NUL, the key and the value
    trimmed, any case, a key of 32 bytes or more ignored, the last line
    counts. -1: no such line; 1: ON; 0: anything else. Checked against the
    C code in TestAutostart.test_the_model_is_the_firmware."""
    out = -1
    for line in cfg[:1023].split(b"\n"):
        line = line.split(b"\x00")[0].strip(b" \t\r\n\v\f")
        if b"=" not in line:
            continue
        key, value = line.split(b"=", 1)
        if not key or len(key) >= 32 or key.strip(b" \t\r\n\v\f").upper() != b"PFM_AUTOSTART":
            continue
        out = 1 if value.strip(b" \t\r\n\v\f").upper() == b"ON" else 0
    return out


class LfsDrive(IecDrive):
    """The drive model with the firmware's names, CD, X and U0> (see above)."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.types = {}             # real name -> b"DIR", b"SEQ", b"*PRG" ...
        self.cmds = []              # every command on channel 15
        self.xmode, self.hide = 1, False
        self.corrupt = None         # a name whose writes are stored wrong
        self.opens = 0              # how often the listing was opened
        self.disk = b"PRIFLOP"      # the root's header: lfsop_getvolumename
        self.folders = {}           # a folder's real name -> its files (one level)
        self.cwd = None             # the folder the drive is in; None: the root
        self.pet_header = False     # True: the firmware before 6n (asc2pet on a folder's name)

    def here(self):
        return self.files if self.cwd is None else self.folders[self.cwd]

    def resolve(self, name):
        """(the files a name refers to, the name without its path): parser.c's
        parse_path -- "//" before the colon is the root, anything else the
        folder the drive is in."""
        if name[:1] == b"@":
            name = name[1:]
        if b":" in name:
            path, name = name.split(b":", 1)
            if path.lstrip(b"0123456789").startswith(b"//"):
                return self.files, name
        return self.here(), name

    def find(self, name, files=None):
        for real in self.files if files is None else files:
            if fold(display(real)) == fold(name):
                return real
        return None

    def open(self, sa, name):
        self.error = OK_MSG
        mode = b"W" if sa == 1 else b"R"
        parts = name.split(b",")
        base = parts[0]
        for p in parts[1:]:
            if p[:1] in (b"W", b"R", b"A"):
                mode = p[:1]
        if sa != 1 and base.startswith(b"$") and mode == b"R":
            self.channels[sa] = Channel(base, self.directory())
            self.opens += 1
            return
        replace = base.startswith(b"@")
        files, base = self.resolve(base)
        real = self.find(base, files)
        if mode == b"W":
            if real is not None and not replace:
                self.error = dos_message(63, b"FILE EXISTS")
                return
            if real is not None:            # '@': the old one goes; the new
                del files[real]             # name is the old one's, as shown
                base = display(real)
            self.channels[sa] = Channel(pet2asc(base), b"", write=True)
            self.channels[sa].dest = files
            return
        if real is None:
            self.error = dos_message(62, b"FILE NOT FOUND")
            return
        self.channels[sa] = Channel(real, files[real])

    def close(self, sa):
        for s in (list(self.channels) if sa == 15 else [sa]):
            ch = self.channels.pop(s, None)
            if ch is not None and ch.write:
                data = bytes(ch.data)
                if self.corrupt == ch.name and data:
                    data = data[:-1] + bytes([data[-1] ^ 1])
                getattr(ch, "dest", self.files)[ch.name] = data

    def command(self, text):
        self.cmds.append(text)
        if text[:1] == b"S" and b":" in text:
            n = 0
            for name in text[1:].split(b","):
                files, name = self.resolve(name)
                real = self.find(name, files)
                if real is not None:
                    del files[real]
                    n += 1
            self.error = dos_message(1, b" FILES SCRATCHED", n)
        elif text[:3] == b"CD:":
            self.error = OK_MSG
            if text == b"CD:_":
                self.cwd = None
            elif self.cwd is None and self.find(text[3:]) in self.folders:
                self.cwd = self.find(text[3:])
        elif text == b"X":
            self.error = dos_message(3, b"E%d%s*-I0:R" % (self.xmode, b"+" if self.hide else b"-"),
                                     self.dev, 0)
        elif text in (b"XE+", b"XE-"):
            self.hide = text == b"XE+"
            self.error = dos_message(3, b"E%d%s*-I0:R" % (self.xmode, text[2:]), self.dev, 0)
        elif text == b"XW":
            self.error = dos_message(3, b"E%d%s*-I0:R" % (self.xmode, b"+" if self.hide else b"-"),
                                     self.dev, 0)
        elif text[:3] == b"U0>" and len(text) == 4:
            self.dev = text[3]
            self.error = OK_MSG
        elif text == b"U\xca":
            self.error = OK_MSG
        elif text == b"XT+" and self.turbo is not None:     # primo_turbo.c: the answer says
            super().command(text)                           # whether PFM starts at boot
            cfg = self.find(b"PRIFLOP.CFG", self.files)
            if cfg is not None and pfm_autostart(self.files[cfg]) == 1:
                self.error = dos_message(0, b" OK", 1, 0)
        else:
            super().command(text)
            if getattr(self, "tdrive", None) is not None:
                self.tdrive.files = FolderView(self)

    def directory(self, pat=b"*"):
        # lfsop_getdirlabel: since 6n a folder's name as stored; before, through
        # asc2pet, capitals to 0xC1-0xDA (src/lfsops.c; seen on the Primo 2026-10-11)
        label = self.disk if self.cwd is None else asc2pet(self.cwd) if self.pet_header else self.cwd
        lines = [(0, b"\x12\"" + label.ljust(16) + b"\" 01 2A")]
        for real, data in self.here().items():
            name = display(real)
            blocks = max(1, (len(data) + 253) // 254)
            t = self.types.get(real, b"PRG")
            pad = b" " * (17 - len(name) - (1 if t.startswith(b"*") else 0))
            lines.append((blocks, b"   "[len(str(blocks)) - 1:] + b"\"" + name + b"\"" + pad + t))
        lines.append((664, b"BLOCKS FREE."))
        out = bytearray(b"\x01\x04")
        addr = 0x0401
        for num, text in lines:
            addr += 4 + len(text) + 1
            out += bytes([addr & 0xFF, addr >> 8, num & 0xFF, num >> 8]) + text + b"\x00"
        return bytes(out + b"\x00\x00")


class Screen:
    """The 16 x 42 text screen, from what DSPHND and DIRCUR were given."""

    def __init__(self):
        self.clear()
        self.scrolls = 0

    def clear(self):
        self.cells = [[" "] * 42 for _ in range(16)]
        self.inv = [[False] * 42 for _ in range(16)]
        self.row = self.col = 0
        self.inverse = False

    def char(self, c):
        if c == 0x0C:
            self.clear()
        elif c in (0x01, 0x14):
            self.inverse = False
        elif c == 0x04:
            self.inverse = True
        elif c == 0x08:
            self.col -= 1
            if self.col < 0:
                self.row, self.col = self.row - 1, 41
            self.cells[self.row][self.col] = " "
        elif c == 0x0D:
            self.newline()
        elif c >= 0x20:
            self.cells[self.row][self.col] = chr(c)
            self.inv[self.row][self.col] = self.inverse
            self.col += 1
            if self.col == 42:
                self.newline()

    def newline(self):
        self.row, self.col = self.row + 1, 0
        if self.row == 16:
            self.scrolls += 1
            self.cells = self.cells[1:] + [[" "] * 42]
            self.inv = self.inv[1:] + [[False] * 42]
            self.row = 15

    def text(self, row):
        return "".join(self.cells[row]).rstrip()

    def inverse_rows(self):
        return [r for r in range(16) if any(self.inv[r])]


def rom_letters():
    """{12 lines of 6 pixels: character}, as the ROM draws 0x20-0x7E."""
    if not ROM_LETTERS:
        p = Primo({})
        p.type(b"CLS")
        p.type(b"FOR I=32 TO 126:PRINT CHR$(I);:NEXT")
        for k in range(95):
            r, c = 1 + k // 42, k % 42
            ROM_LETTERS[Video.cell(p.m.memory, p.m.memory[0x404A] << 8, r, c)] = chr(32 + k)
        assert len(ROM_LETTERS) == 95
    return ROM_LETTERS


ROM_LETTERS = {}


class Video:
    """The 16 x 42 text screen, read back from the video RAM."""

    def __init__(self, primo):
        self.p = primo

    @staticmethod
    def cell(mem, base, r, c):
        x = 4 + 6 * c
        out = []
        for y in range(12):
            a = base + (r * 12 + y) * 32 + x // 8
            v = (mem[a] << 8 | (mem[a + 1] if a + 1 < 0x10000 else 0)) >> (10 - x % 8)
            out.append(v & 0x3F)
        return tuple(out)

    def cells(self, r):
        mem, letters = self.p.m.memory, rom_letters()
        base = self.p.hw_page << 8          # what the hardware shows, not 0x404A
        out = []
        for c in range(42):
            g = self.cell(mem, base, r, c)
            if g in letters:
                out.append((letters[g], False))
            else:
                inv = tuple(v ^ 0x3F for v in g)
                out.append((letters[inv], True) if inv in letters else ("\x7f", False))
        return out

    def text(self, row):
        return "".join(ch for ch, _ in self.cells(row)).rstrip()

    def inverse_rows(self):
        return [r for r in range(16) if any(inv for _, inv in self.cells(r))]

    @property
    def scrolls(self):
        return self.p.rom_screen.scrolls


class Primo(bs.BasicOurs):
    """The B64 with our DOS, the keyboard from a queue, the screen read back."""

    def __init__(self, files=None, **kw):
        self.keys = []
        self.rom_screen = Screen()
        self.screen = Video(self)
        self.min_sp = 0xFFFF
        super().__init__(files, **kw)
        bus, files = self.drive.bus, self.drive.files
        self.drive = LfsDrive(bus, files, 8, self.drive.tm, self.rng)

    def cold_start(self):
        m = self.m
        self.printed = bytearray()
        mem = bs.basic_memory()
        jp = lambda a: bytes([0xC3, a & 0xFF, a >> 8])     # noqa: E731
        # IN A,(P_KEY) ; OR A ; RET NZ ; HALT ; JR the IN
        mem[KEYSTUB:KEYSTUB + 7] = bytes([0xDB, P_KEY, 0xB7, 0xC0, 0x76, 0x18, 0xF9])
        mem[KKBHND:KKBHND + 3] = jp(KEYSTUB)
        orig = u16(mem[DIRCUR + 1:DIRCUR + 3])
        # PUSH AF ; LD A,D ; OUT (P_ROW),A ; LD A,E ; OUT (P_COL),A ; POP AF ; JP orig
        mem[CURSTUB:CURSTUB + 11] = bytes([0xF5, 0x7A, 0xD3, P_ROW, 0x7B, 0xD3, P_COL, 0xF1]) + jp(orig)
        mem[DIRCUR:DIRCUR + 3] = jp(CURSTUB)
        m.set_memory_block(0, bytes(mem))
        m.pc, m.sp = 0, 0
        self.run_until_halt()
        assert m.pc in (bs.LINE_IN, bs.LINE_IN + 1)
        self.hw_page = m.memory[0x404A]     # the screen the video circuit shows
        # the free RAM holds what the last program left there, never zeros:
        # PFM's variables (not in its file) must not count on zeros
        # (2026-10-11 on the B64: the glyph buffer's blank lines were not)
        m.set_memory_block(JUNK_FROM, bytes((i * 167 + 0x5A) & 0xFF
                                            for i in range(JUNK_TO - JUNK_FROM)))
        self.text()

    def out_cb(self, port, value):
        p = port & 0xFF
        if p == self.outport:
            self.min_sp = min(self.min_sp, self.m.sp)
        if p == bs.P_CHAR:
            self.rom_screen.char(value)
            self.min_sp = min(self.min_sp, self.m.sp)
        elif p == P_ROW:
            self.rom_screen.row = value
        elif p == P_COL:
            self.rom_screen.col = value
            return
        super().out_cb(port, value)

    def in_cb(self, port):
        if port & 0xFF == P_KEY:
            self.min_sp = min(self.min_sp, self.m.sp)
            return self.keys.pop(0) if self.keys else 0
        return super().in_cb(port)

    def at_keys(self):
        return self.m.halted and self.m.pc in (KEYSTUB + 4, KEYSTUB + 5)

    def run(self, limit_t=800_000_000):
        self.run_until_halt(limit_t)
        pc = self.m.pc
        assert self.at_keys() or pc in (bs.LINE_IN, bs.LINE_IN + 1), f"stopped at {pc:#06x}"

    def type(self, line):
        """A line typed: what it printed, until BASIC or PFM wants input."""
        m = self.m
        buf = u16(m.memory[0x40A7:0x40A9])
        m.set_memory_block(buf + 1, line + b"\x00")
        m.hl, m.a, m.f = buf, 0x0D, 0x40
        m.pc = u16(m.memory[m.sp:m.sp + 2])
        m.sp += 2
        m.halted = False
        self.run()
        return self.text()

    def press(self, *keys):
        """Keys for PFM, then on until it waits for more, or BASIC does."""
        assert self.at_keys(), "PFM is not waiting for a key"
        for k in keys:
            self.keys.append(ord(k) if isinstance(k, str) else k)
        self.m.halted = False
        self.run()
        return self.text()

    def row(self, r):
        return self.screen.text(r)


def pfm_files(n=0, patch=(), extra=None):
    data, syms = build_pfm.pfm_file(patch)
    files = {b"PFM": data}
    for i in range(n):
        files[b"F%03d" % i] = rec(0xD1, 0, b"\x00\x00") + b"\xc9"
    files.update(extra or {})
    return files, syms


def started(files, **kw):
    p = Primo(files, **kw)
    p.type(b"CLS")
    p.type(b"CMD LOAD")
    assert p.at_keys(), "PFM did not start"
    return p


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestListing(unittest.TestCase):
    def page(self, p, syms):
        mem = p.m.memory
        n = mem[syms["count"]]
        out = []
        for i in range(n):
            e = u16(mem[syms["pgptr"]:syms["pgptr"] + 2]) + i * 22
            out.append((bytes(mem[e:e + mem[e + 21]]), bytes(mem[e + 16:e + 19]), u16(mem[e + 19:e + 21])))
        return out, mem[syms["more"]]

    def test_pages(self):
        for n in (0, 1, 14, 15, 200):
            with self.subTest(files=n):
                files, syms = pfm_files(n - 1 if n else 0)
                pfm = files[b"PFM"]
                p = started(files)
                names = sorted(display(k) for k in p.drive.files)
                if n == 0:
                    del p.drive.files[b"PFM"]           # an empty disk: read again
                    p.press("c", BRK)                   # the settings and back: read again
                    p.drive.files[b"PFM"] = pfm
                    names = []
                    p.press(" ")                        # nothing to move to
                    self.assertEqual(self.page(p, syms), ([], 0))
                    continue
                listed = list(p.drive.files)
                seen, pages = [], 0
                while True:
                    entries, more = self.page(p, syms)
                    pages += 1
                    seen += [e[0] for e in entries]
                    self.assertEqual(p.row(14), ("Page %d" % pages) + ("  (more)" if more else ""))
                    for i, (name, t, blocks) in enumerate(entries):
                        self.assertEqual(t, b"PRG")
                        self.assertIn(name.decode(), p.row(i))
                    self.assertEqual(p.screen.scrolls, 0)
                    if not more:
                        break
                    p.press("N" if pages % 2 else RIGHT)
                self.assertEqual(seen, [display(k) for k in listed])
                self.assertEqual(pages, (n + 13) // 14)
                self.assertEqual(sorted(seen), names)
                # back, page by page, to the first
                for i in range(pages - 1):
                    p.press("P" if i % 2 else LEFT)
                self.assertEqual(p.row(14).split()[:2], ["Page", "1"])
                self.assertEqual(p.drive.opens, 1)          # forward and back: read once

    def test_cursor_moves_across_pages(self):
        files, syms = pfm_files(19)                     # 20 with PFM: pages of 14 and 6
        p = started(files)
        self.assertEqual(p.screen.inverse_rows(), [0])  # no title row: the bar only
        p.press(*[DOWN] * 13)
        self.assertEqual(p.screen.inverse_rows(), [13])
        p.press(DOWN)                                   # past the last line: page 2
        self.assertEqual(p.row(14), "Page 2")
        self.assertEqual(p.screen.inverse_rows(), [0])
        p.press(UP)                                     # back: page 1, its last line
        self.assertEqual(p.row(14), "Page 1  (more)")
        self.assertEqual(p.screen.inverse_rows(), [13])
        p.press(UP, UP, LEFT)                           # LEFT: the page before; none
        self.assertEqual(p.row(14), "Page 1  (more)")
        self.assertEqual(p.screen.inverse_rows(), [11])
        p.press(RIGHT)                                  # RIGHT: the page after
        self.assertEqual(p.row(14), "Page 2")

    def test_the_parser(self):
        files, syms = pfm_files(0, extra={b"SUB": b"", b"seqfile": b"x" * 600, b"OPEN": b"y",
                                          b"ABCDEFGHIJKLMNOP": b"z", b"\x85": b"boot"})
        p = Primo(files)
        p.drive.types.update({b"SUB": b"DIR", b"seqfile": b"SEQ", b"OPEN": b"*PRG"})
        p.type(b"CMD LOAD")
        entries, more = self.page(p, syms)
        self.assertEqual(entries, [(b"PFM", b"PRG", (len(files[b"PFM"]) + 253) // 254), (b"SUB", b"DIR", 1), (b"SEQFILE", b"SEQ", 3),
                                   (b"OPEN", b"PRG", 1), (b"ABCDEFGHIJKLMNOP", b"PRG", 1), (b"\x85", b"PRG", 1)])
        self.assertEqual(p.row(0).split(), ["PFM", "PRG", str((len(p.drive.files[b"PFM"]) + 253) // 254)])  # no title row; the root: no ".."
        self.assertEqual(p.row(5).split(), ["?", "PRG", "1"])     # not printable: '?'
        self.assertEqual(p.row(15), "RET load up/dn move <> page C conf Q quit")

    def test_folders_and_images(self):
        files, syms = pfm_files(0, extra={b"SUB": b"", b"GAMES.D64": b"d" * 300})
        p = Primo(files)
        p.drive.types[b"SUB"] = b"DIR"
        p.type(b"CMD LOAD")
        p.press(DOWN, RET)
        self.assertEqual(p.drive.cmds[-1], b"CD:SUB")
        p.press(DOWN, DOWN, RET)
        self.assertEqual(p.drive.cmds[-1], b"CD:GAMES.D64")
        p.press(BRK, "b")                               # no keys of PFM's (the user)
        self.assertEqual(p.drive.cmds[-1], b"CD:GAMES.D64")
        self.assertEqual(p.row(14), "Page 1")


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestHandover(unittest.TestCase):
    """After a file is chosen, the state a typed CMD LOAD"name" leaves."""

    TARGETS = {b"BAS": rec(0xD1, 0, b"\x08\x44\x0a\x00\xb2\x22OK\x22\x00\x00\x00") + b"\xc9",
               b"MC": rec(0xD9, 0x8000, b"\x3e\x07\xc9") + b"\xc3\x00\x80\xc9",
               b"SCR": rec(0xD5, 0x100, b"\xaa" * 16) + b"\xc9"}

    def twins(self, patch=(), many=0):
        files, syms = pfm_files(0, patch, self.TARGETS)
        files.update((b"F%03d" % i, rec(0xD1, 0, b"\x00\x00") + b"\xc9") for i in range(many))
        return started(dict(files)), Primo(dict(files)), syms

    def choose(self, p, name):
        names = [display(k) for k in p.drive.files]
        p.press(*[DOWN] * names.index(name))
        return p.press(RET)

    @staticmethod
    def memory(p):
        """BASIC's memory as the twin harness compares it, less what a typed
        line and the screen leave: the input buffer past the line's end, the
        screen and the screen handler's state (0x4047-0x4049) -- the screen is
        checked on its own."""
        mem = bytearray(p.basic_memory())
        buf = u16(p.m.memory[0x40A7:0x40A9]) - 0x4000
        end = mem.index(0, buf + 1)
        mem[end:buf + 256] = bytes(buf + 256 - end)
        mem[0x47:0x4A] = bytes(3)
        return bytes(mem[:ds.CDOS - 0x4000])

    def compare(self, a, b, what, ta, tb):
        # what came after the screen was cleared (PFM's last, CLS's and NEW's
        # first: both send normal, then clear)
        self.assertIn(b"\x01\x0c", ta)
        ta, tb = ta.split(b"\x01\x0c")[-1], tb.split(b"\x01\x0c")[-1]
        self.assertEqual(ta, tb, f"{what}: printed")
        self.assertEqual(a.m.sp, b.m.sp, f"{what}: the stack")
        self.assertEqual(a.ptrs(), b.ptrs(), f"{what}: BASIC's pointers")
        ma, mb = self.memory(a), self.memory(b)
        if ma != mb:
            diff = [i for i in range(len(ma)) if ma[i] != mb[i]]
            self.fail(f"{what}: memory differs at " + ", ".join(
                f"{i + 0x4000:#06x}" for i in diff[:6]) + f" ({len(diff)} bytes)")
        # the screen: cleared by PFM, then only what the load printed
        shown = [a.rom_screen.text(r) for r in range(16) if a.rom_screen.text(r)]   # as the ROM printed it
        printed = [x for x in tb.replace(b"\x00", b"").decode("latin-1").split("\r") if x]
        self.assertEqual(shown, printed, f"{what}: the screen")

    def handover(self, name, patch=(), many=0):
        a, b, syms = self.twins(patch, many)
        # with `many` files the listing is not read to its end: its channel
        # is still open when RETURN comes
        self.assertEqual(a.m.memory[syms["copen"]], 1 if many else 0)
        wlen = u16(a.m.memory[syms["wlen"]:syms["wlen"] + 2])
        ta = self.choose(a, name)
        b.type(b"CLS")
        b.poke(0x4400, bytes(wlen + 1))     # what PFM's wiper zeroes (the rest: junk in both)
        tb = b.type(b'CMD LOAD"' + name + b'"')
        self.compare(a, b, name, ta, tb)
        if name == b"BAS":
            self.assertEqual(a.type(b"LIST"), b'10 PRINT"OK"\rOk\r\x00')

    def test_load_is_the_typed_load(self):
        for name in self.TARGETS:
            with self.subTest(name):
                self.handover(name)

    # 6l (the B64, 2026-10-12, step 8): RETURN while the listing's channel was
    # still open -- the root's many files -- loaded "00, OK,00,00": CLOSE
    # leaves HL at the drive's message, and go_load took the name from there
    OPEN_BUG = ("go_load:\n    push hl             ; the entry: CLOSE leaves HL at the drive's message\n"
                "    call dclose         ; (6l: the status was loaded as the name)\n",
                "go_load:\n    call dclose\n    push hl\n")

    def test_load_with_the_listing_open(self):
        for name in (b"MC", b"BAS"):
            with self.subTest(name):
                self.handover(name, many=40)

    def test_load_with_the_listing_open_control(self):
        for name in (b"MC", b"BAS"):
            with self.subTest(name):
                self.handover(name, (self.OPEN_BUG,))     # the channel closed: no harm
                with self.assertRaises(AssertionError):
                    self.handover(name, (self.OPEN_BUG,), many=40)

    def test_quit_is_new(self):
        a, b, syms = self.twins()
        mem = a.m.memory
        cend = syms["cache"] + u16(mem[syms["cmax"]:syms["cmax"] + 2]) * 22
        self.assertGreater(cend, syms["bssend"])
        ta = a.press("q")
        b.type(b"CLS")
        b.poke(0x4400, bytes(0x1800))
        tb = b.type(b"NEW")
        self.compare(a, b, "Q", ta, tb)
        # the directory's cache, past PFM's variables, is cleared as well
        self.assertEqual(bytes(mem[syms["bssend"]:cend]), bytes(cend - syms["bssend"]))

    NEGATIVE = [
        ("PFM's memory left in place", ("    ld (hl),0\n    ldir\n    ld hl,(INPBUF)", "    nop\n    nop\n    nop\n    ld hl,(INPBUF)")),
        ("the screen not cleared", ("    ld a,C_CLS\n    call DSPHND\n    ld hl,(STRSPC)", "    nop\n    nop\n    nop\n    nop\n    nop\n    ld hl,(STRSPC)")),
        ("the line one byte early", ("    ld de,(INPBUF)\n    inc de\n    call setpre\n    ld hl,s_load", "    ld de,(INPBUF)\n    nop\n    call setpre\n    ld hl,s_load")),
        ("inverse left on", ("    ld a,C_NORM         ; the screen as BASIC leaves it", "    ld a,C_INV          ; the screen as BASIC leaves it")),
    ]

    def test_negative_controls(self):
        for what, patch in self.NEGATIVE:
            with self.subTest(what):
                with self.assertRaises((AssertionError, RuntimeError)):
                    for name in (b"MC", b"BAS"):
                        self.handover(name, (patch,))


CFG = (b"# PRIFLOP\r\nMODE=STA\r\nSSID=home\r\nPASSWORD=secret1\r\nAP_PASSWORD=appass\r\n"
       b"ADMIN=sha256:0011:2233\r\nXYZ = kept\nMDNS=priflop")


def netconfig(data):
    """The firmware's own parser on `data`: {field: value}."""
    src = r"""
#include <stdio.h>
#include <string.h>
#include "netconfig.h"
int main(int c, char **v) {
  static char buf[4096]; net_config_t cfg;
  FILE *f = fopen(v[1], "rb"); unsigned n = fread(buf, 1, 1023, f);  /* as netconfig_load() reads it */ fclose(f);
  netconfig_set_defaults(&cfg); netconfig_parse_buffer(&cfg, buf, n);
  printf("MODE=%s\nSSID=%s\nPASSWORD=%s\nAP_SSID=%s\nAP_PASSWORD=%s\nMDNS=%s\nADMIN=%s\nCOUNTRY=%s\nPFM_AUTOSTART=%d\n",
         cfg.ap_mode ? "AP" : "STA", cfg.ssid, cfg.password, cfg.ap_ssid, cfg.ap_password,
         cfg.mdns_name, cfg.admin_password, cfg.country, cfg.pfm_autostart);
  return 0;
}
"""
    with tempfile.TemporaryDirectory() as tmp:
        c, exe, cfg = (os.path.join(tmp, x) for x in ("dump.c", "dump", "cfg"))
        open(c, "w").write(src)
        open(cfg, "wb").write(data)
        subprocess.run(["gcc", "-I", os.path.join(ROOT, "src", "pico"), "-I", os.path.join(ROOT, "src"),
                        "-DNETCONFIG_HOST_TEST", "-o", exe, c, os.path.join(ROOT, "src", "pico", "netconfig.c"),
                        os.path.join(ROOT, "src", "sha256.c")], check=True, capture_output=True)
        out = subprocess.run([exe, cfg], check=True, capture_output=True).stdout.decode()
    return dict(line.split("=", 1) for line in out.splitlines())


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestSettings(unittest.TestCase):
    def settings(self, cfg=CFG, patch=(), **drive):
        files, syms = pfm_files(0, patch)
        if cfg is not None:
            files[b"PRIFLOP.CFG"] = cfg
        p = started(files)
        for k, v in drive.items():
            setattr(p.drive, k, v)
        p.press("c")
        return p

    def field(self, p, i):
        return p.row(2 + i)

    def test_the_fields_shown(self):
        p = self.settings()
        self.assertEqual(p.row(0), "PFM  Settings")
        rows = [p.row(2 + i) for i in range(9)]
        self.assertEqual([r[1:21].rstrip() for r in rows],
                         ["Wi-Fi mode", "Network name", "Wi-Fi password", "Country",
                          "Web admin password", "PFM at boot", "Turbo", "Hide extensions", "Device number"])
        self.assertEqual([r[21:].strip() for r in rows],
                         ["STA", "home", "****", "(world)", "****", "OFF", "ON", "OFF", "8"])
        p2 = self.settings(cfg=None)                    # no file: the firmware's defaults
        self.assertEqual([p2.row(2 + i)[21:].strip() for i in range(5)],
                         ["AP", "PRIFLOP", "(none)", "(world)", "****"])

    def edit(self, p, field, text):
        cur = p.m.memory[self.syms_scur(p)]
        keys = [DOWN] * (field - cur) if field >= cur else [UP] * (cur - field)
        p.press(*keys, RET)
        self.assertIn("RETURN keeps it", p.row(11))
        p.press(*text, RET)

    def syms_scur(self, p):
        return build_pfm.assemble()[1]["scur"]

    def test_the_config_rewritten(self):
        p = self.settings()
        self.edit(p, 1, "Cafe Wifi")
        self.edit(p, 3, "hu")
        self.edit(p, 4, "n3w")
        p.press(UP, UP, UP, UP, RET)                    # the mode: AP
        self.assertEqual(self.field(p, 1)[0], "*")
        p.press("w")
        self.assertEqual(p.row(14), "Written. R restarts the drive for Wi-Fi.")
        files = p.drive.files
        self.assertIn(b"PRIFLOP.CFG", files)            # capitals: what the firmware opens
        self.assertNotIn(b"priflop.cfg", files)
        self.assertNotIn(b"priflop.tmp", files)
        new = files[b"PRIFLOP.CFG"]
        self.assertEqual(new, b"# PRIFLOP\r\nMODE=AP\r\nSSID=Cafe Wifi\r\nPASSWORD=secret1\r\n"
                              b"AP_PASSWORD=appass\r\nADMIN=n3w\r\nXYZ = kept\nMDNS=priflop\r\nCOUNTRY=hu\r\n")
        cfg = netconfig(new)
        self.assertEqual(cfg, {"MODE": "AP", "SSID": "Cafe Wifi", "PASSWORD": "secret1", "AP_SSID": "PRIFLOP",
                               "AP_PASSWORD": "appass", "MDNS": "priflop", "ADMIN": "n3w", "COUNTRY": "HU",
                               "PFM_AUTOSTART": "-1"})
        self.assertEqual([c for c in p.drive.cmds if c.startswith(b"S")],
                         [b"S//:PRIFLOP.TMP", b"S//:PRIFLOP.CFG", b"S//:PRIFLOP.TMP"])
        # unchanged keys keep their lines, byte for byte; nothing else is sent
        self.assertFalse([c for c in p.drive.cmds if c[:1] in (b"U", b"XT", b"XW")])

    def test_a_secret_is_written_only_if_typed(self):
        p = self.settings()
        self.edit(p, 1, "x")
        p.press("w")
        self.assertIn(b"ADMIN=sha256:0011:2233\r\n", p.drive.files[b"PRIFLOP.CFG"])
        self.assertIn(b"PASSWORD=secret1\r\n", p.drive.files[b"PRIFLOP.CFG"])
        p.press(DOWN, DOWN, DOWN, RET, BRK)             # BRK: the old value stays
        self.assertEqual(self.field(p, 4)[0], " ")

    def test_no_file_yet(self):
        p = self.settings(cfg=None)
        self.edit(p, 2, "pw")
        p.press("w")
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], b"PASSWORD=pw\r\n")

    def test_refusals(self):
        p = self.settings(xmode=4)                      # read again by C below
        p.press(BRK, "c")
        self.edit(p, 1, "x")
        p.press("w")
        self.assertEqual(p.row(14), "Extension mode 2 or 4: not written")
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], CFG)
        p = self.settings(cfg=b"SSID=a\r\n" + b"#" * 1100)
        self.edit(p, 1, "x")
        p.press("w")
        self.assertEqual(p.row(14), "PRIFLOP.CFG could not be read")
        # a temporary copy that reads back wrong: the config is not touched
        p = self.settings(corrupt=b"priflop.tmp")
        self.edit(p, 1, "x")
        p.press("w")
        self.assertEqual(p.row(14), "READ BACK DIFFERS")
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], CFG)

    def test_the_switches_and_the_device(self):
        p = self.settings()
        p.press(*[DOWN] * 7, RET, DOWN, RET, "w")      # hiding on, device 9
        self.assertEqual(p.drive.cmds[-3:], [b"XE+", b"U0>\x09", b"XW"])
        self.assertEqual(p.drive.dev, 9)
        self.assertEqual(p.row(14), "Written. R restarts the drive for Wi-Fi.")
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], CFG)    # not rewritten
        p.press("r")
        self.assertEqual(p.drive.cmds[-1], b"U\xca")
        p.press(BRK)                                    # the listing, from drive 9
        self.assertEqual(p.row(14), "Page 1")
        p.press("q")                                    # BASIC follows: CMD SET 9
        self.assertEqual(p.type(b"CMD $")[:3], b"0 \x12")         # the header, from drive 9
        self.assertEqual(p.m.memory[p.dos_base + p.syms["cdev"]], 9)

    def test_pfm_at_boot(self):
        self.pfm_at_boot()

    def test_the_reread_is_needed(self):
        # without reading the file again after a W, the next W starts from
        # the file as it was before the first (found 2026-10-12)
        with self.assertRaises(AssertionError):
            self.pfm_at_boot((("    call cfgread        ; the file as it is now: a second W in the same", "    or a"),))

    def pfm_at_boot(self, patch=()):
        p = self.settings(patch=patch)
        p.press(*[DOWN] * 5, RET)                       # PFM at boot: ON
        self.assertEqual(self.field(p, 5)[0], "*")
        self.assertEqual(self.field(p, 5)[21:].strip(), "ON")
        p.press("w")
        new = p.drive.files[b"PRIFLOP.CFG"]
        self.assertEqual(new, CFG + b"\r\nPFM_AUTOSTART=ON\r\n")     # added at the end, the rest kept
        self.assertEqual(netconfig(new), dict(netconfig(CFG), PFM_AUTOSTART="1"))   # and the firmware's

        self.assertFalse([c for c in p.drive.cmds if c[:1] in (b"U", b"XT", b"XW")])
        self.edit(p, 1, "two")                          # a second W in the same visit keeps the first's
        p.press("w")                                    # change (it started from the file read before)
        two = CFG.replace(b"SSID=home", b"SSID=two")
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], two + b"\r\nPFM_AUTOSTART=ON\r\n")
        p.press(DOWN, DOWN, DOWN, DOWN, RET, "w")       # OFF again: the line replaced where it is
        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], two + b"\r\nPFM_AUTOSTART=OFF\r\n")
        p.press(BRK, "c")                               # read back by PFM
        self.assertEqual(self.field(p, 5)[21:].strip(), "OFF")

    MUTATIONS = [
        ("the config's name in ASCII capitals: stored as priflop.cfg",
         ("            defb 0D0h, 0D2h, 0C9h, 0C6h, 0CCh, 0CFh, 0D0h\n            defm \".\"\n            defb 0C3h, 0C6h, 0C7h",
          "            defm \"PRIFLOP.CFG\"")),
        ("no read back", ("    ld hl,n_tmp\n    call vfile", "    ld hl,n_tmp\n    nop\n    nop\n    nop")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                with self.assertRaises(AssertionError):
                    if "read back" in what:
                        p = self.settings(patch=(patch,), corrupt=b"priflop.tmp")
                        self.edit(p, 1, "x")
                        p.press("w")
                        self.assertEqual(p.drive.files[b"PRIFLOP.CFG"], CFG)
                    else:
                        p = self.settings(patch=(patch,))
                        self.edit(p, 1, "x")
                        p.press("w")
                        self.assertIn(b"PRIFLOP.CFG", p.drive.files)


class BootPrimo(Primo):
    """The B64 after a cold start, no DOS yet: our boot file is run on it as
    the boot ROM runs it (stored at 0x4500, jumped to), with the LfsDrive."""

    def install(self):
        self.cold_start()

    def boot(self, f=None):
        m = self.m
        m.set_memory_block(0x4500, f or boot_file())
        m.pc = 0x4500
        m.sp -= 2
        m.halted = False
        self.run()
        base, syms = u16(m.memory[0x4174:0x4176]), ds.DOS[1]
        self.dos_base, self.syms = base, syms
        self.status_at, self.errbuf = base + syms["status"], base + syms["errbuf"]
        return self.text()


_BOOT = []


def boot_file():
    if not _BOOT:
        _BOOT.append(bs.build_dos.boot_file()[0])
    return _BOOT[0]


def booted(cfg, f=None, extra=None):
    files, _ = pfm_files(3, extra=extra)
    if cfg is not None:
        files[b"PRIFLOP.CFG"] = cfg
    p = BootPrimo(files)
    p.boot_text = p.boot(f)
    return p


AUTO_ON = [b"PFM_AUTOSTART=ON\r\n",
           b"MODE=AP\r\n  pfm_AutoStart \t= oN \r\nSSID=x\r\n",         # any case, spaces around
           b"PFM_AUTOSTART=OFF\r\nPFM_AUTOSTART=ON",                     # the last line counts; no \n
           b"PFM_AUTOSTART=ON\x00junk\r\n",                              # a NUL ends the line
           CFG + b"\r\nPFM_AUTOSTART=ON\r\n"]
AUTO_OFF = [None, CFG, b"PFM_AUTOSTART=OFF\r\n", b"PFM_AUTOSTART=ON\r\nPFM_AUTOSTART=OFF\r\n",
            b"PFM_AUTOSTART=ONE\r\n", b"PFM_AUTOSTART=O N\r\n", b"XPFM_AUTOSTART=ON\r\n",
            b"PFM_AUTOSTARTX=ON\r\n", b"PFM_AUTOSTART ON\r\n", b"PFM_AUTOSTART=\r\n",
            b"#" * 1030 + b"\nPFM_AUTOSTART=ON\r\n"]                   # past the bytes read
CASES = [(c, True) for c in AUTO_ON] + [(c, False) for c in AUTO_OFF]


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestAutostart(unittest.TestCase):
    """6m: with PFM_AUTOSTART=ON in PRIFLOP.CFG our DOS's boot starts PFM,
    as a bare CMD LOAD typed after the banner would. The drive says so in
    its answer to the loader's XT+ (00, OK,01,00; primo_turbo.c), so the
    loader reads nothing more (loader.asm, step 7)."""

    def test_the_model_is_the_firmware(self):
        # LfsDrive's reading of the key against netconfig.c's own
        for cfg, want in CASES:
            with self.subTest(cfg=cfg if cfg is None else cfg[-40:]):
                fw = int(netconfig(cfg or b"")["PFM_AUTOSTART"])
                self.assertEqual(pfm_autostart(cfg or b""), fw)
                self.assertEqual(fw == 1, want)

    def test_on_is_the_typed_cmd_load(self):
        a = booted(AUTO_ON[0])
        self.assertTrue(a.at_keys(), "PFM did not start")
        b = booted(b"PFM_AUTOSTART=OF\r\n")             # as long, off
        self.assertFalse(b.at_keys())
        b.type(b"CMD LOAD")
        self.assertTrue(b.at_keys())
        self.assertEqual([a.row(r) for r in range(16)], [b.row(r) for r in range(16)])
        self.assertEqual(a.m.sp, b.m.sp, "the stack")
        self.assertEqual(a.ptrs(), b.ptrs(), "BASIC's pointers")
        ma, mb = TestHandover.memory(a), TestHandover.memory(b)
        diff = [hex(i + 0x4000) for i in range(len(ma)) if ma[i] != mb[i]]
        self.assertFalse(diff, "memory differs")
        self.assertEqual(a.drive.cmds, b.drive.cmds)
        for p in (a, b):                                    # Q: BASIC, our DOS working
            p.press("q")
            self.assertTrue(p.type(b"CMD $").endswith(b"BLOCKS FREE.\rOk\r\x00"))

    def test_the_cases(self):
        for cfg, want in CASES:
            with self.subTest(cfg=cfg if cfg is None else cfg[-40:]):
                p = booted(cfg)
                self.assertEqual(p.at_keys(), want)
                if not want:
                    self.assertEqual(p.boot_text, bs.BANNER)
                    self.assertTrue(p.type(b"CMD $").endswith(b"BLOCKS FREE.\rOk\r\x00"))
                    p.type(b"CMD LOAD")
                # PFM's settings read the file as the drive did
                p.press("c")
                self.assertEqual(p.row(2 + 5)[21:].strip(), "ON" if want else "OFF")

    def test_written_by_pfm_then_booted(self):
        p = booted(CFG)
        p.type(b"CMD LOAD")
        p.press("c", *[DOWN] * 5, RET, "w")
        q = booted(p.drive.files[b"PRIFLOP.CFG"])
        self.assertTrue(q.at_keys())

    def test_no_pfm(self):
        files = {b"PRIFLOP.CFG": AUTO_ON[0]}
        p = BootPrimo(files)
        self.assertEqual(p.boot(), b"\x0cPRIFLOP DOS, turbo\r\x0762,FILE NOT FOUND,00,00\rOk\r\x00")
        self.assertTrue(p.type(b"CMD $").endswith(b"BLOCKS FREE.\rOk\r\x00"))

    def test_the_boot_time(self):
        """T-states from the jump to 0x4500 to the prompt, the boot ROM's
        loading of the file left out: step 7 costs nothing on the bus."""
        times = {}
        skip = bs.build_dos.boot_file(patch=(("    call same\n    jr nz,done\n", "    call same\n    jr done\n"),))[0]
        for what, f in (("step 7 skipped", skip), ("with step 7", None)):
            files = pfm_files(0)[0]
            files[b"PRIFLOP.CFG"] = CFG
            p = BootPrimo(files)
            t0 = p.base
            p.boot(f)
            times[what] = (p.base - t0) * T_NS / 1e9
        print("\nthe boot after the load: " + ", ".join(f"{k} {v:.3f} s" for k, v in times.items()),
              file=sys.stderr)
        self.assertLess(times["with step 7"] - times["step 7 skipped"], 0.001)
        self.assertLessEqual(len(boot_file()), os.path.getsize(bs.BOOT80))

    MUTATIONS = [
        ("the answer not read", ("    call same\n    jr nz,done\n", "    call same\n    jr done\n")),
        ("the answer read one byte off", ("    ld hl,IMG+B_ERRBUF+6", "    ld hl,IMG+B_ERRBUF+5")),
        ("always on", ("    call same\n    jr nz,done\n", "    call same\n    nop\n    nop\n")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                f = bs.build_dos.boot_file(patch=(patch,))[0]
                with self.assertRaises(AssertionError):
                    for cfg, want in CASES:
                        self.assertEqual(booted(cfg, f).at_keys(), want)
        # and PFM's own reading of the value, against the drive's
        files, _ = pfm_files(0, (("    or a\n    jr nz,cp7\n    inc b", "    nop\n    nop\n    nop\n    inc b"),),
                             {b"PRIFLOP.CFG": b"PFM_AUTOSTART=ONE\r\n"})
        p = started(files)
        p.press("c")
        self.assertEqual(p.row(2 + 5)[21:].strip(), "ON")


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestFolders(unittest.TestCase):
    """Inside a folder (the B64, 2026-10-11): the drive stays there after a
    load, but PFM and PRIFLOP.CFG are in the root; the folder's name comes
    in the header as stored (since 6n), or as asc2pet made it (before)."""

    def inside(self, pet_header=False):
        files, _ = pfm_files(0, extra={b"PRIFLOP.CFG": CFG, b"SUB": b""})
        p = Primo(files)
        p.drive.pet_header = pet_header
        p.drive.types[b"SUB"] = b"DIR"
        p.drive.folders[b"SUB"] = {b"HELLO": rec(0xD1, 0, b"\x00\x00") + b"\xc9"}
        p.type(b"CLS")
        p.type(b"CMD LOAD")
        names = [display(k) for k in p.drive.files]
        p.press(*[DOWN] * names.index(b"SUB"), RET)
        self.assertEqual(p.drive.cwd, b"SUB")
        return p

    def test_dotdot_goes_up(self):
        for pet in (False, True):
            with self.subTest(pet_header=pet):
                self.dotdot_goes_up(pet)

    def dotdot_goes_up(self, pet):
        p = self.inside(pet)
        self.assertEqual(p.row(0).split(), ["..", "DIR", "0"])     # first, under the bar
        self.assertEqual(p.screen.inverse_rows(), [0])
        self.assertEqual(p.row(1).split(), ["HELLO", "PRG", "1"])
        p.press(BRK, "b")                               # not up: only ".." is (the user)
        self.assertEqual(p.drive.cmds[-1], b"CD:SUB")
        p.press(RET)
        self.assertEqual(p.drive.cmds[-1], b"CD:_")
        self.assertIsNone(p.drive.cwd)
        self.assertEqual(p.row(0).split(), ["PFM", "PRG", str((len(p.drive.files[b"PFM"]) + 253) // 254)])   # the root: no ".."

    def test_cmd_load_from_a_folder(self):
        p = self.inside()
        p.press(DOWN, RET)                              # HELLO, from the folder
        self.assertFalse(p.at_keys())
        self.assertEqual(p.drive.cwd, b"SUB")           # the drive stays there
        p.type(b"CMD LOAD")                             # PFM, from the root
        self.assertTrue(p.at_keys(), "PFM did not start from a folder")
        self.assertEqual(p.row(0).split(), ["..", "DIR", "0"])     # in the folder

    def test_settings_from_a_folder(self):
        p = self.inside()
        p.press("c")
        self.assertEqual(p.row(3)[21:].strip(), "home")    # the root's PRIFLOP.CFG
        TestSettings.edit(self, p, 1, "x")
        p.press("w")
        self.assertEqual(p.row(14), "Written. R restarts the drive for Wi-Fi.")
        self.assertIn(b"SSID=x\r\n", p.drive.files[b"PRIFLOP.CFG"])
        self.assertEqual(list(p.drive.folders[b"SUB"]), [b"HELLO"])

    syms_scur = TestSettings.syms_scur


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestCache(unittest.TestCase):
    def pages(self, p, syms, keys):
        """The names on each page, pressing each of `keys` in turn."""
        out = [[e[0] for e in TestListing.page(None, p, syms)[0]]]
        for k in keys:
            p.press(k)
            out.append([e[0] for e in TestListing.page(None, p, syms)[0]])
        return out

    def test_a_small_cache_slides(self):
        # no room past the config buffer: 46 entries, 3 pages and a bit
        files, syms = pfm_files(199, (("    ld hl,-256\n    add hl,sp\n", "    ld hl,0\n    nop\n"),))
        p = started(files)
        self.assertEqual(u16(p.m.memory[syms["cmax"]:syms["cmax"] + 2]), 1024 // 22)
        names = [display(k) for k in p.drive.files]
        want = [names[i:i + 14] for i in range(0, 200, 14)]
        self.assertEqual(self.pages(p, syms, [RIGHT] * 14), want)
        self.assertEqual(p.drive.opens, 1)              # forward: read once, sliding
        self.assertEqual(self.pages(p, syms, [LEFT] * 14)[1:], want[::-1][1:])
        self.assertGreater(p.drive.opens, 1)            # back past the cache: again

    def test_a_folder_empties_it(self):
        files, syms = pfm_files(20, extra={b"SUB": b""})
        p = Primo(files)
        p.drive.types[b"SUB"] = b"DIR"
        p.type(b"CMD LOAD")
        p.press(RIGHT)
        sub = list(p.drive.files).index(b"SUB") - 14
        p.press(*[DOWN] * sub, RET)                     # CD:SUB, and the listing again
        self.assertEqual(p.drive.cmds[-1], b"CD:SUB")
        self.assertEqual(p.drive.opens, 2)
        self.assertEqual(p.row(14), "Page 1  (more)")


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestSpeed(unittest.TestCase):
    """In seconds of a 2.5 MHz Z80, the bus included (a move took 5.6 s, a page 8.6 s and more, before PFM drew its own text)."""

    def timed(self, p, *keys):
        b = p.base
        p.press(*keys)
        return (p.base - b) * T_NS / 1e9

    def measure(self, patch=()):
        files, syms = pfm_files(40, patch)
        p = started(files)
        return self.timed(p, DOWN), self.timed(p, RIGHT), self.timed(p, LEFT)

    def test_a_move_and_a_page(self):
        move, forward, back = self.measure()
        print(f"\nmove {move:.2f} s, a page forward {forward:.2f} s, back {back:.2f} s", file=sys.stderr)
        self.assertLess(move, 0.1)
        self.assertLess(forward, 1.5)
        self.assertLess(back, 0.6)

    def test_a_slow_move_is_caught(self):
        move, _, _ = self.measure((("    jr nc,k_next0\n    jr k_move\n", "    jr nc,k_next0\n    jp dir2\n"),))
        self.assertGreater(move, 0.1)                   # the whole page drawn again


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestOwnScreen(unittest.TestCase):
    """PFM draws with the ROM's letters at the ROM's places: what it drew
    reads back, cell by cell, as letters the ROM draws."""

    NAMES = [bytes(c for c in range(0x21, 0x7F) if c != 0x22 and not 0x61 <= c <= 0x7A)[i:i + 16]
             for i in range(0, 67, 16)]
    SMALL = "`abcdefghijklmnopqrstuvwxyz{|}~"

    def check(self, patch=()):
        files, syms = pfm_files(0, patch, {n: b"x" for n in self.NAMES})
        p = started(files)
        for i, name in enumerate(self.NAMES):
            self.assertEqual(p.row(1 + i)[1:17].rstrip(), name.decode())
        self.assertNotIn("\x7f", "".join(p.row(r) for r in range(16)))
        p.press("c", DOWN, RET, *self.SMALL)           # typed into the network name
        self.assertEqual(p.row(13), self.SMALL)
        p.press(LEFT, LEFT, "!")                        # back two places, cleared
        self.assertEqual(p.row(13), self.SMALL[:-2] + "!")
        p.press(RET)
        self.assertEqual(p.row(3)[21:], (self.SMALL[:-2] + "!")[:19])

    def test_every_letter(self):
        self.check()

    def test_the_letters_are_not_seen(self):
        """The start: the ROM draws the letters PFM copies into a screen in
        the cache's memory, not on the one shown (the user, 2026-10-11).
        Stopped right after the copy: nothing on the screen, 0x404A back,
        PFM's code untouched; drawn on the screen, the control fails."""
        stop = ("    ld (vbase),a\n    jp cls", "    ld (vbase),a\n    halt")
        shown = ("    jr c,si0            ; it does not", "    jr si0              ; it does not")
        for patch, seen in (((stop,), False), ((stop, shown), True)):
            with self.subTest(seen=seen):
                files, syms = pfm_files(0, patch)
                code = build_pfm.assemble(patch)[0]
                p = Primo(files)
                p.type(b"CLS")
                with self.assertRaisesRegex(AssertionError, "stopped at"):
                    p.type(b"CMD LOAD")
                self.assertEqual(p.m.memory[0x404A], p.hw_page)
                self.assertEqual(bytes(p.m.memory[0x4400:0x4400 + len(code)]), code)
                text = "".join(p.row(r) for r in (13, 14, 15))
                self.assertEqual("ABCDEFGHIJ" in text, seen, text)

    MUTATIONS = [
        ("a pixel to the right", ("    add a,4\n    ld c,a\n    and 7", "    add a,5\n    ld c,a\n    and 7")),
        ("the letters copied a line high", ("    ld de,32\n    add hl,de           ; line 1", "    ld de,0\n    add hl,de           ; line 1")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                with self.assertRaises((AssertionError, KeyError, RuntimeError)):
                    self.check((patch,))


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestMemory(unittest.TestCase):
    def test_pfm_stays_under_its_stack(self):
        """PFM's code and variables against the lowest stack it ran on, all
        screens and the config write included; on the B64 here. The B32 has
        16K RAM less, and its own ROM."""
        files, syms = pfm_files(30)
        files[b"PRIFLOP.CFG"] = CFG
        p = started(files)
        p.min_sp = 0xFFFF                               # from here on: PFM's
        p.press("N", "P", DOWN, "c")
        TestSettings.edit(TestSettings(), p, 1, "abc")
        p.press("w", BRK)
        top = syms["bssend"]
        cend = syms["cache"] + u16(p.m.memory[syms["cmax"]:syms["cmax"] + 2]) * 22
        strspc = u16(p.m.memory[0x40A0:0x40A2])
        print(f"\nPFM {syms['start']:#06x}-{top - 1:#06x}, its cache to {cend - 1:#06x}; the lowest stack "
              f"{p.min_sp:#06x}, {strspc - p.min_sp} bytes under the strings ({strspc:#06x})", file=sys.stderr)
        self.assertLess(top, p.min_sp - 64)
        self.assertLess(cend, p.min_sp - 64)


if __name__ == "__main__":
    unittest.main()
