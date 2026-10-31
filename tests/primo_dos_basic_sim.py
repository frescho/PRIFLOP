#!/usr/bin/env python3
"""BASIC statements under CDOS and under our own DOS, compared.

    tools/external/venv-sim/bin/python tests/primo_dos_basic_sim.py

Steps B5 and B6 ("statement by statement"): the CMD
statements, the error hook, and the byte channel with EOF() and the two
output hooks. The Primo runs
its real ROM from a cold start: BASIC itself tokenizes and executes what is
typed, and calls the DOS through the CMD vector, the error hook and RST 28H
as it does on the machine. CDOS is booted by its own loader (boot80.prg);
ours is put at CDOS's base with BASIC's top where CDOS's loader puts it, so
the two machines' memory maps are the same below the DOS. The drive is
primo_dos_sim's bit-level IecDrive, with PRIFLOP's turbo for ours.

Two patches to the ROM, both outside what a DOS sees:
  * 0x0361, the line input, halts: the test types a line into BASIC's input
    buffer and returns from it, as the routine does after RETURN.
  * 0x0015, the character output under the DOS's hook, goes through a stub
    in the ROM's unused 0x2F4E that also writes the character to a port, so
    the test reads what was printed.
The keyboard is one key held (port 0x10), so that CMD $ lists on; port 0x3F
is BRK. No interrupts and no NMI: BASIC does not need them to run statements.

What is compared after each statement: the text printed, the stack pointer,
BASIC's pointers, its memory from 0x4000 to the DOS and the screen -- the
free memory excepted, between the arrays and the stack and in the string
space, where both DOSes build names and leave stack -- and
the drive -- its files, its message and, unless the statement loads or
saves (CDOS on the standard bus, ours in turbo), everything it saw on the bus.

primo-library/ (boot80.prg) is fetched and git-ignored; without it, the ROM
or the z80 package, the tests skip.
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import primo_dos_sim as ds                      # noqa: E402
from primo_dos_sim import (CdosPrimo, OurPrimo, CDOS, BOOT80, B64_ROM, HAVE_CDOS,  # noqa: E402
                           build_dos, rec, pattern, dos_message)
from primo_turbo_sim import u16                 # noqa: E402

LINE_IN = 0x0361            # the ROM's line input
CHAR_OUT = 0x0015           # the character output, under the DOS's hook (0x032A)
STUB = 0x2F4E               # 157 unused bytes in the ROM
P_CHAR = 0xFA               # the stub's port: the character printed
KEY = 0x10                  # the key held
TOP = CDOS - 1              # BASIC's top under CDOS, and under ours here
R_ERR = 0x19A2

# BASIC's pointers (TRS-80 Level II's, at the same addresses)
PTRS = {"program": 0x40A4, "variables": 0x40F9, "arrays": 0x40FB, "free": 0x40FD,
        "strings": 0x40A0, "string top": 0x40D6, "top": 0x40B1, "line": 0x40A2,
        "data": 0x40FF, "error line": 0x40EA}
# the DOS vectors' operands, each DOS's own addresses: RST 28H, the error
# hook, EOF, the end of PRINT and the character output
VECTORS = (0x400D, 0x400E, 0x41A7, 0x41A8, 0x4162, 0x4163, 0x41BF, 0x41C0, 0x41C2, 0x41C3)


def basic_memory():
    mem = bytearray(65536)
    mem[0:0x4000] = open(B64_ROM, "rb").read()
    assert mem[STUB:STUB + 5] == b"\xff" * 5
    mem[STUB:STUB + 5] = bytes([0xD3, P_CHAR, 0xC3]) + mem[CHAR_OUT + 1:CHAR_OUT + 3]
    mem[CHAR_OUT:CHAR_OUT + 3] = bytes([0xC3, STUB & 0xFF, STUB >> 8])
    mem[LINE_IN] = 0x76                                 # HALT: a line is wanted
    return mem


class Basic:
    """The B64 running BASIC, with a DOS and a drive."""

    held = KEY

    def cold_start(self):
        m = self.m
        self.printed = bytearray()
        m.set_memory_block(0, bytes(basic_memory()))
        m.pc, m.sp = 0, 0
        self.run_until_halt()
        assert m.pc in (LINE_IN, LINE_IN + 1), f"the cold start stopped at {m.pc:#06x}"
        self.text()

    def text(self):
        t = bytes(self.printed)
        self.printed = bytearray()
        return t

    def out_cb(self, port, value):
        if port & 0xFF == P_CHAR:
            self.printed.append(value)
            return
        super().out_cb(port, value)

    def in_cb(self, port):
        p = port & 0xFF
        if p < 0x40:                                    # the keyboard; port 0 bit 1: Reset
            return 1 if p == self.held else 0
        return super().in_cb(port)

    def resume(self, limit_t=400_000_000):
        self.m.halted = False
        self.run_until_halt(limit_t)
        assert self.m.pc in (LINE_IN, LINE_IN + 1), f"stopped at {self.m.pc:#06x}, not at the line input"
        return self.text()

    def type(self, line):
        """A line typed and RETURN: what BASIC printed until it wants the next."""
        m = self.m
        buf = u16(m.memory[0x40A7:0x40A9])
        m.set_memory_block(buf + 1, line + b"\x00")
        m.hl, m.a, m.f = buf, 0x0D, 0x40
        m.pc = u16(m.memory[m.sp:m.sp + 2])
        m.sp += 2
        return self.resume()

    def raise_error(self, code):
        """The error `code` (E) raised at the prompt, as a statement raises it."""
        self.m.e, self.m.pc = code, R_ERR
        return self.resume()

    def ptrs(self):
        mem = self.m.memory
        return {k: u16(mem[a:a + 2]) for k, a in PTRS.items()}

    def basic_memory(self, frame=0):
        """BASIC's memory and the screen; the free memory -- under the stack, and
        the string space's unused part -- and the vectors excepted."""
        mem = bytearray(self.m.memory)
        free = u16(mem[0x40FD:0x40FF])
        strings, string_top = u16(mem[0x40A0:0x40A2]), u16(mem[0x40D6:0x40D8])
        for a in list(range(free, self.m.sp + frame)) + list(range(strings, string_top)) + list(VECTORS):
            mem[a] = 0
        return bytes(mem[0x4000:CDOS]) + bytes(mem[0xE800:0x10000])


class BasicCdos(Basic, CdosPrimo):
    def install(self):
        self.cold_start()
        m = self.m
        m.set_memory_block(0x4500, open(BOOT80, "rb").read())
        m.pc = 0x4500
        m.sp -= 2
        self.banner = self.resume()
        assert u16(m.memory[0x4174:0x4176]) == CDOS
        self.status_at = CDOS + 0x0B87
        self.errbuf = CDOS + 0x0B96


class BasicOurs(Basic, OurPrimo):
    """Ours at CDOS's base, hooked in as CDOS's loader hooks CDOS (until step
    B7 has a loader of its own): the vectors found before are kept for
    chaining, ours set (CMD, RST 28H, the error hook, EOF, the end of PRINT,
    the character output), BASIC's top goes under the DOS, CLEAR 50, NEW,
    READY."""

    def install(self):
        self.cold_start()
        code, syms, relocs = self.dos or ds.DOS
        m, mem = self.m, self.m.memory
        base = CDOS
        assert len(code) <= 0xE7FF - base
        self.dos_base, self.syms = base, syms
        img = bytearray(build_dos.relocate(code, relocs, base))
        img[syms["outport"]] = ds.OUTPORT
        img[syms["oldcmd"]:syms["oldcmd"] + 2] = mem[0x4174:0x4176]
        img[syms["oldhook"]:syms["oldhook"] + 3] = mem[0x41A6:0x41A9]
        m.set_memory_block(base, bytes(img))
        w = lambda a, v: m.set_memory_block(a, bytes([v & 0xFF, v >> 8]))   # noqa: E731
        w(0x4174, base)
        w(0x400D, base + syms["svc"])
        for vec, sym in ((0x41A6, "ehook"), (0x4161, "efn"), (0x41BE, "pend"), (0x41C1, "cout")):
            m.set_memory_block(vec, bytes([0xC3]))
            w(vec + 1, base + syms[sym])
        w(0x40B1, TOP)
        # CLEAR 50, NEW, READY: LD DE,50 ; CALL 1E83h ; CALL 1B4Dh ; JP 1A19h
        m.set_memory_block(0x4500, bytes([0x11, 50, 0, 0xCD, 0x83, 0x1E, 0xCD, 0x4D, 0x1B,
                                          0xC3, 0x19, 0x1A]))
        m.pc = 0x4500
        m.sp -= 2
        self.banner = self.resume()
        self.status_at = base + syms["status"]
        self.errbuf = base + syms["errbuf"]


TRANSFERS = (b"LOAD", b"SAVE")


class StmtTwin:
    """CDOS and ours, the same files, every line typed into both."""

    def __init__(self, files=None, dos=None, **kw):
        files = files or {}
        self.c = BasicCdos(dict(files), **kw)
        ours = type("Ours", (BasicOurs,), {"dos": dos}) if dos else BasicOurs
        self.o = ours(dict(files), **kw)
        for p in (self.c, self.o):
            p.type(b"CLS")                              # the banners differ
            p.poke(0x4500, bytes(0x1000))               # and what booted them
        self.check("after the boot", b"", b"", bus=False)

    def __call__(self, *lines, bus=None, frame=0):
        """Type the lines into both; each must give the same. Returns what
        CDOS printed for the last one. `frame`: bytes over the stack pointer
        not compared -- a frame the DOS left there (BRK in CMD $), holding
        its own return addresses."""
        for line in lines:
            nc, no = len(self.c.drive.log), len(self.o.drive.log)
            tc, to = self.c.type(line), self.o.type(line)
            same_bus = bus if bus is not None else not any(w in line.upper() for w in TRANSFERS)
            self.check(line, tc, to, same_bus, nc, no, frame)
        return tc

    def error(self, code):
        nc, no = len(self.c.drive.log), len(self.o.drive.log)
        tc, to = self.c.raise_error(code), self.o.raise_error(code)
        self.check(f"error {code}", tc, to, True, nc, no)
        return tc

    def check(self, what, tc, to, bus=True, nc=0, no=0, frame=0):
        c, o = self.c, self.o
        assert to == tc, f"{what!r}: printed {to!r}, CDOS {tc!r}"
        assert o.m.sp == c.m.sp, f"{what!r}: the stack at {o.m.sp:#06x}, CDOS's at {c.m.sp:#06x}"
        pc, po = c.ptrs(), o.ptrs()
        assert po == pc, f"{what!r}: pointers " + ", ".join(
            f"{k} {po[k]:#06x} (CDOS {pc[k]:#06x})" for k in pc if po[k] != pc[k])
        mc, mo = c.basic_memory(frame), o.basic_memory(frame)
        if mo != mc:
            diff = [i for i in range(len(mc)) if mc[i] != mo[i]]
            addr = lambda i: i + 0x4000 if i < CDOS - 0x4000 else i - (CDOS - 0x4000) + 0xE800  # noqa: E731
            raise AssertionError(f"{what!r}: memory differs at " + ", ".join(
                f"{addr(i):#06x} {mo[i]:02x} (CDOS {mc[i]:02x})" for i in diff[:8]) + f" ({len(diff)} bytes)")
        dc, do = c.drive, o.drive
        assert do.files == dc.files, f"{what!r}: the drive's files differ: {sorted(do.files)} {sorted(dc.files)}"
        assert do.error == dc.error, f"{what!r}: the drive's message {do.error!r}, CDOS's {dc.error!r}"
        if bus:
            lo, lc = do.log[no:], dc.log[nc:]
            if lo != lc:
                i = next((i for i, (x, y) in enumerate(zip(lo, lc)) if x != y), min(len(lo), len(lc)))
                raise AssertionError(f"{what!r}: the drive saw, from its event {i}: {lo[i:i + 5]}, with CDOS {lc[i:i + 5]}")

    def svc(self, idx, **kw):
        """A service call between statements, in both: the CPU state kept."""
        out = []
        for p in (self.c, self.o):
            m = p.m
            st = (m.pc, m.sp, m.af, m.bc, m.de, m.hl, m.ix, m.iy, m.halted)
            out.append(p.call(idx, **kw))
            m.pc, m.sp, m.af, m.bc, m.de, m.hl, m.ix, m.iy, m.halted = st
        return out

    def drive_error(self, code, text=b"SOME ERROR", track=7, sector=9):
        for p in (self.c, self.o):
            p.drive.error = dos_message(code, text, track, sector)

    def answer(self, code, text=b"ANSWER"):
        """Both drives answer their next command with `code`."""
        for p in (self.c, self.o):
            d, real = p.drive, p.drive.command

            def once(cmd, d=d, real=real):
                d.command = real
                d.error = dos_message(code, text)
            d.command = once

    def poke(self, addr, data):
        for p in (self.c, self.o):
            p.poke(addr, data)


HAVE = HAVE_CDOS and ds.DOS is not None

D1 = rec(0xD1, 0, b"\x00\x00")
D9 = rec(0xD9, 0x8000, b"x")
D5 = rec(0xD5, 0x100, b"y")
ENDINGS = {b"D9": D9, b"D5": D5, b"D1": D1, b"D9D5": D9 + D5, b"D5D9": D5 + D9, b"D1D9": D1 + D9,
           b"D9D1": D9 + D1, b"D1D5": D1 + D5, b"D5D1": D5 + D1}
ON_ERROR = (b"10 ON ERROR GOTO 100", b"90 END", b'100 PRINT "E";ERR;ERL;:RESUME NEXT')


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestTheHarness(unittest.TestCase):
    def test_basic_runs_and_cdos_boots(self):
        p = BasicCdos({})
        self.assertTrue(p.banner.startswith(b"\x0cCommodore Serial Bus Handler"), p.banner)
        self.assertEqual(p.type(b"PRINT 1+1"), b" 2 \rOk\r\x00")
        self.assertEqual(p.ptrs()["top"], TOP)

    def test_ours_boots_to_the_same_basic(self):
        t = StmtTwin()
        self.assertEqual(t.o.ptrs(), t.c.ptrs())


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestStatements(unittest.TestCase):
    """Step B5: the CMD statements of part F and the error hook, typed into
    BASIC under CDOS and under ours. Each line must give the same text,
    stack, pointers, memory and drive (StmtTwin.check). What CDOS does here
    was found by running it so, not from its code."""

    def test_the_error_message(self):
        t = StmtTwin({b"P": b"1"})
        t(b"CMD ERROR", b'CMD KILL"NONE"', b"CMD ERROR")
        t.drive_error(74)
        t(b"CMD ERROR", b"CMD ERROR A$", b"CMD ERROR,", b"CMD ERROR 5", b"CMD ERROR:PRINT 8")
        t(b'10 CMD ERROR A$,B$,C$,D$', b'20 PRINT A$;"|";B$;"|";C$;"|";D$',
          b"30 CMD ERROR A,B$,C,D:PRINT A;B$;C;D", b'40 PRINT "X";:CMD ERROR:PRINT "N"',
          b"50 CMD ERROR A(3),Z$(2):PRINT Z$(2)", *ON_ERROR[:1], *ON_ERROR[1:])
        t.drive_error(74)
        t(b"RUN", b"PRINT FRE(0),FRE(\"\")")
        t(b"60 CMD ERROR A$,B$,C$,D$,E$", b'70 CMD ERROR "X"', b"RUN 60")

    def test_a_message_held_back(self):
        # a drive command's 01, and a drive error not shown (ON ERROR), are
        # held back for CMD ERROR, which then does not read the drive; a
        # shown error, CMD ERROR and CMD RESET let go of it
        t = StmtTwin({b"P": b"1"})
        t(b'CMD KILL"X"', b"CMD ERROR", b"CMD ERROR")
        t(b'CMD KILL"X"', b'CMD LOAD"NONE"', b"CMD ERROR")
        t(b'CMD KILL"X"', b"CMD RESET", b"CMD ERROR")
        t(b'CMD KILL"X"', b"CMD XYZ", b"CMD SET 9", b'CMD LOAD"P"', b"CMD SET 8", b"CMD ERROR")
        t(b'CMD KILL"X"', b"CMD $", b"CMD ERROR")
        t.answer(1)
        t(b'CMD NEW"A,B"', b"CMD ERROR")
        t.answer(0)
        t(b'CMD KILL"X"', b"CMD ERROR")
        t(*ON_ERROR, b'20 CMD LOAD"NONE"', b"30 ERROR 2", b"40 CMD ERROR")
        t(b"RUN", bus=False)
        t(b'50 CMD KILL"X":CMD ERROR A$:PRINT A$', b"RUN 50", b"CMD ERROR")

    def test_load(self):
        files = {b"SCR": rec(0xD5, 0x100, b"\xaa" * 16) + b"\xc9",
                 b"MC": rec(0xD9, 0x8000, b"\x3e\x07\xc9") + b"\xc3\x00\x80\xc9"}
        t = StmtTwin(files)
        t(b'10 PRINT "HI"', b"20 A=5", b'CMD SAVE"S1"', b'CMD SAVE AUTO"S2"', b'CMD SAVE SCREEN"S3"', b"NEW")
        t(b'CMD LOAD"S1"', b"LIST", b'CMD LOAD"S2"', b"NEW")
        t(b'10 CMD LOAD"S1"', b"RUN", bus=False)
        t(b'CMD LOAD SCREEN"S3"', b'CMD LOAD"S3"', b"CMD LOAD SCREEN", b"CMD LOAD 5", b'CMD LOAD"S"+"1"', b"LIST",
          b'CMD LOAD"S1",X', b"NEW", b'CMD LOAD"MC"', b"NEW")
        t(b'10 CMD LOAD"SCR":PRINT "CONT"', b"RUN", b'CMD LOAD"NONE"', bus=False)
        self.assertLimit(t, b'CMD LOAD"SCR"', b"CLEAR 1000", b'CMD LOAD"SCR"', b"CLEAR 50")

    def assertLimit(self, t, *lines):
        # the load limit each statement leaves, read back with service 0x04:
        # the drive's turbo stops a load there (out of memory)
        for line in lines:
            t(line, bus=False)
            c, o = t.svc(0x04)
            self.assertEqual(o["hl"], c["hl"], line)

    def test_how_a_load_ends(self):
        # a screen record last: on with the statement ("Ok"); anything else is
        # taken as the program, ending where the last record ended
        # (inside a program, only where the program is replaced or kept: a
        # load of machine code RUNs the program loading it again, forever)
        t = StmtTwin({k: v + b"\xc9" for k, v in ENDINGS.items()})
        for k in ENDINGS:
            t(b'10 PRINT "OLD"', b'CMD LOAD"' + k + b'"', b"NEW")
            if k.endswith((b"D1", b"D5")):
                t(b'10 CMD LOAD"' + k + b'":PRINT "C"', b"RUN", b"NEW", bus=False)

    def test_out_of_memory(self):
        # the directory too long for the free memory (a load's out of memory
        # differs: test_where_ours_differs)
        t = StmtTwin({b"F%d" % i: b"x" for i in range(100)})
        t(b"CLEAR 30000", b"DIM A(1600)", b"PRINT FRE(0)", b"CMD $", b"CLEAR 50", b"CMD $")

    def test_save(self):
        t = StmtTwin({b"OLD": b"1"})
        t(b'10 PRINT "HI"', b'CMD SAVE"S1"', b'CMD SAVE AUTO"S2"', b'CMD SAVE SCREEN"S3"', b'CMD SAVE"OLD"',
          b"CMD SAVE", b"CMD SAVE 5", b'CMD SAVE"S"+"4"', b'CMD SAVE"Q"5', b'CMD SAVE"R":PRINT 9')
        self.assertEqual(t.o.drive.files[b"S1"][:1], b"\xd1")

    def test_the_directory(self):
        t = StmtTwin({b"ONE": b"1" * 300, b"PQ": b"2", b"PZ": b"3", b"\xc1\xd2X": b"4"})
        t(b"CMD $", b'CMD $"P*"', b'CMD $"PQ"', b'CMD $"ZZ"', b'CMD $""', b"CMD $,", b"CMD $ 5",
          b'CMD $:PRINT "D"', b'CMD $"P"5', b'10 CMD $:PRINT "SAME"', b'20 PRINT "AFTER"', b"RUN")
        self.assertLimit(t, b"CMD $", b"CLEAR 1000", b"CMD $", b"CLEAR 50")
        t.c.held = t.o.held = 0x3F                     # BRK
        t(b"CMD $", frame=6)

    def test_kill_new_reset(self):
        t = StmtTwin({b"S1": b"1", b"S2": b"2", b"T": b"3"})
        t(b'CMD KILL"S*"', b'CMD NEW"DISK,ID"', b"CMD KILL", b"CMD NEW", b'CMD KILL"X"5', b'CMD NEW"A"5',
          b'CMD KILL"P":PRINT 3', b'CMD NEW"A":PRINT 4', b"CMD RESET", b"CMD RESET 5", b"CMD RESET:PRINT 7")
        t.answer(31)
        t(b'CMD KILL"T"')

    def test_set(self):
        t = StmtTwin({b"P": b"1"})
        t(b"CMD SET 9", b"CMD ERROR", b'CMD LOAD"P"', b'CMD KILL"P"', b"CMD $", b"CMD RESET", b"CMD SET 8",
          b"CMD SET 9,6", b"CMD ERROR", b"CMD SET 8", b"CMD SET 9X", b"CMD ERROR", b"CMD SET 8", b"CMD SET 7",
          b"CMD SET 12", b"CMD SET", b"CMD SET 8,", b"CMD SET ,", b"CMD SET 300", b"CMD SET -1",
          b'CMD SET "A"', b"CMD SET 8,5", b"CMD SET ,4", b"CMD SET 11,5", b"CMD SET 8:PRINT 2")

    def test_words_that_are_not_ours(self):
        t = StmtTwin()
        t(b"CMD XYZ", b"CMD", b"CMD 5", b"CMD :PRINT 1")

    def test_every_cmd(self):
        # RST 28H points back at the DOS; the device CMD SET chose, whatever
        # service 0x06 set; the status cleared
        t = StmtTwin()
        t.poke(0x400D, b"\x34\x12")
        t(b"CMD ERROR")
        self.assertEqual(u16(t.c.peek(0x400D, 2)), CDOS + 0x0B32)
        self.assertEqual(u16(t.o.peek(0x400D, 2)), t.o.dos_base + t.o.syms["svc"])
        t.svc(0x06, bc=9)
        t(b"CMD ERROR")
        t.svc(0x0A, bc=0x83)
        t(b"CMD RESET")
        c, o = t.svc(0x0C)
        self.assertEqual((o["a"], c["a"]), (0, 0))

    def test_the_error_hook(self):
        # every code: below 90 the hook before ours (the ROM's), 90-99 our
        # two letters, 100 DEVICE NOT PRESENT, from 101 the drive's message
        # (once there is one: CDOS's buffer starts with a stray "1")
        t = StmtTwin()
        for code in range(0, 101):
            t.error(code)
        t(b'CMD LOAD"NONE"')
        for code in range(101, 256):
            t.error(code)
        t(*ON_ERROR, b'20 CMD LOAD"NONE"', b"30 ERROR 5", b"RUN", bus=False)
        t(b'40 PRINT "A":CMD LOAD"NONE"', b"10", b"RUN", bus=False)

    def test_no_drive(self):
        t = StmtTwin({b"P": b"1"}, drive_dev=9)
        t(b"CMD ERROR", b'CMD LOAD"P"', b'CMD SAVE"Q"', b"CMD $", b'CMD KILL"P"', b'CMD NEW"A"', b"CMD RESET",
          b"CMD ERROR", *ON_ERROR, b'20 CMD LOAD"P"', b"RUN")

    def test_where_ours_differs(self):
        """Pinned, so that a change shows up here."""
        p = BasicOurs({b"P": rec(0xD1, 0, b"\x00\x00") + b"\xc9"})
        c = BasicCdos({b"P": rec(0xD1, 0, b"\x00\x00") + b"\xc9"})
        # verify is gone: CMD TEST goes to the CMD vector before ours
        self.assertEqual(p.type(b'CMD TEST"P"'), b"\x07SN Error\rOk\r\x00")
        self.assertEqual(c.type(b'CMD TEST"P"'), b"Ok\r\x00")
        # the printer group is an extension: without it, the same
        self.assertEqual(p.type(b"CMD LLIST"), b"\x07SN Error\rOk\r\x00")
        self.assertEqual(p.type(b'CMD LPRINT"A"'), b"\x07SN Error\rOk\r\x00")
        # an empty name: the drive's answer through the turbo (CDOS, on the
        # standard bus, ends in a bus error against the model)
        self.assertEqual(p.type(b'CMD LOAD""'), b"\x0762,FILE NOT FOUND,00,00\rOk\r\x00")
        # out of memory: the same error, but the turbo stores nothing (CDOS
        # stores up to the limit first, over the program's start)
        for q in (p, c):
            q.drive.files[b"BIG"] = rec(0xD1, 0, pattern(20000)) + b"\xc9"
            q.type(b"CLEAR 30000")
            self.assertEqual(q.type(b'CMD LOAD"BIG"'), b"\x07OM Error\rOk\r\x00")
        self.assertEqual(p.peek(0x43EA, 2), b"\x00\x00")
        self.assertEqual(c.peek(0x43EA, 2), pattern(20000)[:2])
        # a drive with its turbo off: the load is an error, never a transfer
        p.drive.turbo = False
        self.assertEqual(p.type(b'CMD LOAD"P"'), b"\x0730,SYNTAX ERROR,00,00\rOk\r\x00")
        p.drive.turbo = True
        # no name at all: CDOS says MO, ours loads PFM, the file selector
        # SCREEN without a name stays MO in both
        self.assertEqual(c.type(b"CMD LOAD"), b"\x07MO Error\rOk\r\x00")
        self.assertEqual(p.type(b"CMD LOAD"), b"\x0762,FILE NOT FOUND,00,00\rOk\r\x00")
        for q in (p, c):
            self.assertEqual(q.type(b"CMD LOAD SCREEN"), b"\x07MO Error\rOk\r\x00")
        p.drive.files[b"PFM"] = rec(0xD1, 0, b"\x08\x44\x0a\x00\xb2\x22PFM\x22\x00\x00\x00") + b"\xc9"
        self.assertEqual(p.type(b"CMD LOAD:PRINT 1"), b"\x00")
        self.assertEqual(p.type(b"LIST"), b'10 PRINT"PFM"\rOk\r\x00')

    MUTATIONS = [                       # each must be caught
        ("RST 28H not taken back", ("    ld (RSTVEC),hl", "    nop\n    nop\n    nop")),
        ("the status not cleared", ("    xor a\n    ld (status),a       ; and the status",
                                    "    xor a\n    nop\n    nop\n    nop                 ; and the status")),
        ("a drive command's 01 not held", ("    ld (held),a         ; 01:", "    nop\n    nop\n    nop                 ; 01:")),
        ("the drive's code off by one", ("    add a,E_NP\n    ld e,a\n    ld a,1", "    add a,E_NP+1\n    ld e,a\n    ld a,1")),
        ("a screen last taken as a program", ("    cp 0D5h\n    ret z", "    cp 0D1h\n    ret z")),
        ("the listing without LIST's wait", ("    call R_LWAIT", "    nop\n    nop\n    nop")),
        ("CMD SET takes 12", ("    sub 8\n    cp 4", "    sub 8\n    cp 5")),
        ("the hook's letters off by one", ("    ld hl,etab-90", "    ld hl,etab-89")),
        ("CMD LOAD alone gives MO, as CDOS", ("    jr nz,ld0\n", "    jr ld0\n")),
        ("CMD ERROR's parts read as INPUT", ("    ld (INPDEV),a", "    nop\n    nop\n    nop")),
        ("the load's limit two bytes high", ("    ld bc,-1Ah", "    ld bc,-18h")),
        ("the name without its prefix", ("    ld (hl),b\n    inc hl\n    ld (hl),c", "    ld (hl),c\n    inc hl\n    ld (hl),b")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                dos = build_dos.body(patch=(patch,))
                with self.assertRaises((AssertionError, RuntimeError)):
                    self.short_pass(dos)

    def short_pass(self, dos=None):
        t = StmtTwin({b"P": rec(0xD1, 0, b"\x00\x00") + b"\xc9", b"S": rec(0xD5, 0, b"\x01") + b"\xc9"}, dos=dos)
        t.poke(0x400D, b"\x34\x12")
        t(b"CMD ERROR")
        assert u16(t.o.peek(0x400D, 2)) == t.o.dos_base + t.o.syms["svc"], "RST 28H not taken back"
        t.svc(0x0A, bc=0x83)
        t(b"CMD RESET", b'CMD KILL"P"', b"CMD ERROR", b"CMD $", b"CMD SET 12", b'CMD LOAD"S"')
        c, o = t.svc(0x04)
        assert o["hl"] == c["hl"], "the load's limit"
        t.error(90)
        t.drive_error(74)
        t(*ON_ERROR, b'20 CMD LOAD"NONE"', b"30 CMD ERROR A$:PRINT A$", b"RUN", bus=False)
        got = t.o.type(b"CMD LOAD")
        assert got.startswith(b"E 162 "), f"CMD LOAD alone: not PFM ({got!r})"   # caught by ON ERROR: 100 + 62

    def test_the_short_pass_passes_unmutated(self):
        self.short_pass()


FILES = {b"SEQ": b"HELLO\r12,34\rLAST", b"ONE": b"Z", b"E": b"", b"G": b"\r\x00\xff", b"N": b"5,7X",
         b"L": b"12,34\r56\r\"Q,R\",X\r A B \r7\x00Z\r" + b"W" * 300 + b"\rEND", b"NUM": b"1E3\rABC\r-5\r"}
CATCH = (b"1 ON ERROR GOTO 1000", b"1000 PRINT \"E\";ERR;ERL:RESUME NEXT", b"999 END")


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestByteChannel(unittest.TestCase):
    """Step B6: the byte channel -- CMD OPEN, CREATE, CONT, DEF, CLOSE,
    CLEAR, PRINT, PRINT#, INPUT#, GET#, and EOF() -- typed into BASIC under
    CDOS and under ours, compared as in TestStatements."""

    def test_open_and_close(self):
        t = StmtTwin(FILES)
        t(b'CMD CONT 4,"SEQ"', b"CMD CLOSE 4", b'CMD DEF 6,"SEQ,P,R"', b'CMD OPEN 7,"SEQ,P"', b'CMD CREATE 8,"N2"',
          b"CMD CLOSE", b"CMD CLEAR", b'CMD OPEN 1,"SEQ"', b'CMD OPEN 15,"SEQ"', b'CMD OPEN 14,"SEQ"', b'CMD OPEN 14,"SEQ"',
          b'CMD OPEN 0,"SEQ"', b'CMD OPEN 16,"SEQ"', b'CMD PRINT#3,"X"', b"CMD GET#3,A$", b"PRINT EOF(3)", b"CMD CLOSE 3",
          b"CMD OPEN 5", b"CMD OPEN", b'CMD OPEN 5,"NONE"', b"CMD CLOSE 5", b"CMD ERROR", b"CMD CLOSE 5,6",
          b'CMD OPEN 3,"SEQ":PRINT 3', b'CMD OPEN 3,"SEQ"X', b"CMD OPEN 3,5", b"CMD CLOSE 3", b"CMD OPEN 3,5",
          b'CMD OPEN 3,""', b"CMD CLOSE 3", b'CMD OPEN 4,"SEQ,S"', b"CMD CLOSE 4", b'CMD OPEN 4,"0:SEQ"', b"CMD CLOSE 4",
          b'CMD OPEN 4,"SE"+"Q"', b"CMD CLOSE 4", b'CMD CREATE 4,"' + b"N" * 40 + b'"', b"CMD CLOSE 4", b'CMD DEF 4,"#"',
          b"CMD CLOSE 4", b"CMD OPEN 2,A$", b"CMD CLEAR 5", b"CMD CLEAR:PRINT 1")

    def test_ten_files_at_most(self):
        t = StmtTwin(FILES)
        t(*[b'CMD OPEN %d,"SEQ"' % i for i in range(2, 14)], b"CMD CLOSE 2", b'CMD OPEN 13,"SEQ"', b"CMD CLEAR",
          b'CMD OPEN 13,"SEQ"')

    def test_print(self):
        t = StmtTwin(FILES)
        t(b'CMD CREATE 4,"C"', b'CMD PRINT#4,1;2,3;-4.5;"Z";', b'CMD PRINT#4,"Y"', b"CMD PRINT#4,", b"CMD PRINT#4",
          b'CMD PRINT#4,TAB(5)"T"', b'CMD PRINT#4,"A","B"', b"CMD PRINT#4,CHR$(13);CHR$(0);CHR$(255);",
          b'CMD PRINT#4,USING "##.#";3.14159', b'CMD PRINT#4,@5,"P"', b'PRINT "SCREEN"', b"CMD PRINT#4,1/0",
          b'CMD PRINT#4,"A";1/0', b'CMD PRINT#4,"A":PRINT "B"', b'CMD PRINT#4,"C";:PRINT "D"', b"CMD PRINT#4,X;Y$;",
          b"CMD CLOSE 4", b'CMD OPEN 4,"C"', b"CMD GET#4,A$,B$,C$,D$,E$", b"PRINT A$;B$;C$;D$;E$", b"CMD CLOSE 4")
        t(b'CMD OPEN 5,"SEQ"', b'CMD PRINT "A","B";', b'CMD PRINT#5,"X"', b'CMD PRINT #5 "Y"', b'CMD PRINT#(2+3);"Z"',
          b'CMD PRINT#5;"Z"', b"CMD PRINT#", b"CMD PRINT#,1", b'CMD PRINT "S:C"', b"CMD PRINT", b'CMD PRINT "I";',
          b"CMD ERROR")

    def test_input(self):
        t = StmtTwin(FILES)
        t(b'CMD OPEN 5,"SEQ"', b"CMD INPUT 5,A$", b"CMD INPUT#5,A$", b"CMD INPUT#", b"CMD INPUT#9,A$", b"CMD INPUT")
        t(*CATCH, b'10 CMD OPEN 5,"L"', b"20 CMD INPUT#5,A,B:PRINT A;B", b'30 CMD INPUT#5,A$,B$:PRINT A$;"|";B$;"|"',
          b'40 CMD INPUT#5,A$:PRINT "[";A$;"]";EOF(5)', b'50 CMD INPUT#5,A$:PRINT "[";A$;"]";EOF(5)',
          b"60 CMD INPUT#5,A$:PRINT LEN(A$);EOF(5)", b'70 CMD INPUT#5,A$:PRINT "[";A$;"]";EOF(5)', b"80 CMD CLOSE 5",
          b'110 CMD OPEN 7,"NUM":CMD INPUT#7,N:PRINT N:CMD INPUT#7,N:PRINT N', b"120 CMD INPUT#7,N:PRINT N", b"RUN")

    def test_long_lines(self):
        # 250 bytes at most per INPUT#, the rest for the next; a CR ends a
        # line inside quotes too
        t = StmtTwin(FILES)
        t(b"0 CLEAR 1000", *CATCH, b'10 CMD OPEN 5,"L":FOR I=1 TO 5:CMD INPUT#5,A$:NEXT',
          b'30 FOR I=1 TO 6:CMD INPUT#5,A$:PRINT "[";A$;"]";LEN(A$);EOF(5):NEXT', b"RUN")

    def test_get(self):
        t = StmtTwin(FILES)
        t(*CATCH, b'40 CMD OPEN 6,"G":FOR I=1 TO 3:CMD GET#6,A$:PRINT ASC(A$);LEN(A$);EOF(6):NEXT',
          b"60 CMD GET#6:PRINT 6", b"70 CMD GET#9,A$", b"95 CMD GET#6,A$;B$",
          b'100 CMD OPEN 7,"ONE":CMD GET#7,G$,H$:PRINT G$;LEN(H$);EOF(7):CMD GET#7,G$:PRINT LEN(G$);EOF(7)',
          b'110 CMD OPEN 8,"E":PRINT EOF(8):CMD GET#8,G$:PRINT LEN(G$);EOF(8):CMD CLOSE 8',
          b'120 PRINT FRE(""):CMD OPEN 9,"SEQ":CMD GET#9,A$,B$:PRINT FRE(""):B$="":PRINT FRE("")', b"RUN")
        t(b'CMD OPEN 5,"N"', b"CMD GET#5,A")
        # without "#": channel 15, the drive's message, until its end (FD)
        t = StmtTwin(FILES)
        t(b"CMD GET A$,B$,C$,D$,E$,F$,G$,H$,I$,J$,K$,L$,M$,N$,O$",
          b'PRINT A$;B$;C$;D$;E$;F$;G$;H$;I$;J$;K$;L$;M$;"|";N$;"|";O$', b"CMD GET A$", b"CMD GET A$,B$",
          b"CMD INPUT 6", b'CMD OPEN 5,"NONE"', b"CMD GET A$,B$,C$", b"PRINT A$;B$;C$", b"CMD GET 6,A$")

    def test_eof(self):
        t = StmtTwin(FILES)
        t(b'CMD OPEN 5,"SEQ"', *[b"PRINT EOF(%s)" % x for x in (
            b"1", b"15", b"5", b"0", b"256", b"2.7", b"-1", b"40000", b'"A"', b"32767", b"32768", b"-32768", b"-32769",
            b"65536", b"1E10", b"-1E10", b"5.9", b"-0.5", b"1.9", b"14.99", b"5#", b"5.5#", b"-251", b"261", b"1D20",
            b"5%")], b"PRINT EOF(5)+1")

    def test_the_file_keeps_its_drive_and_mode(self):
        t = StmtTwin(FILES)
        t(b'CMD OPEN 5,"SEQ"', b"CMD SET 9", b"CMD GET#5,A$", b"PRINT EOF(5)", b"CMD CLOSE 5", b"CMD SET 8",
          b'CMD DEF 4,"SEQ"', b"CMD GET#4,A$", b'CMD PRINT#4,"Q"', b"CMD CLOSE 4", b'CMD DEF 4,"NEW,S,W"',
          b'CMD PRINT#4,"Q"', b"CMD GET#4,A$", b"CMD CLOSE 4", b'CMD CONT 3,"NEW"', b"CMD INPUT#3,A$",
          b'CMD PRINT#3,"R"', b"CMD CLOSE 3")
        t(*CATCH, b'10 CMD CREATE 4,"W1":CMD INPUT#4,A$:CMD GET#4,A$:PRINT EOF(4):CMD CLOSE 4',
          b'20 CMD OPEN 4,"E":CMD INPUT#4,A$:CMD CLOSE 4',
          b'30 CMD OPEN 4,"ONE":CMD GET#4,A$:CMD GET#4,A$:CMD INPUT#4,A$:PRINT EOF(4):CMD CLOSE 4',
          b'40 CMD OPEN 4,"SEQ":CMD RESET:CMD GET#4,A$:PRINT A$:CMD CLOSE 4', b'50 CMD OPEN 4,"SEQ":CMD CLEAR:CMD GET#4,A$',
          b'60 CMD CREATE 4,"W2":FOR I=1 TO 40:CMD PRINT#4,I;"ABCDEFGH";:NEXT:CMD PRINT#4:CMD CLOSE 4',
          b'70 CMD OPEN 4,"W2":CMD INPUT#4,A$:PRINT LEN(A$):CMD CLOSE 4',
          b'80 CMD CREATE 4,"W3":CMD PRINT#4,1/0:PRINT "SCR":CMD PRINT#4,"OK":CMD CLOSE 4',
          b'90 CMD OPEN 4,"W3":CMD INPUT#4,A$:PRINT A$:CMD CLOSE 4', b'100 CMD OPEN 4,"SEQ":CMD OPEN 4,"SEQ":CMD CLOSE 4',
          b"RUN")

    def test_held_messages(self):
        t = StmtTwin(FILES)
        t(b'CMD OPEN 5,"NONE"', b"CMD ERROR", b'CMD OPEN 5,"NONE"', b"CMD CLEAR", b"CMD ERROR", b'CMD KILL"X"',
          b"CMD CLEAR", b"CMD ERROR", b'CMD KILL"X"', b'CMD OPEN 6,"SEQ"', b"CMD ERROR", b'CMD KILL"X"', b"CMD CLOSE 6",
          b"CMD ERROR", b'CMD KILL"X"', b'CMD PRINT "I"', b"CMD ERROR")

    def test_no_drive(self):
        t = StmtTwin(FILES, drive_dev=9)
        t(b'CMD OPEN 5,"SEQ"', b'CMD CREATE 4,"X"', b'CMD PRINT "I"', b"CMD CLEAR", b"CMD CLOSE 5", b"PRINT EOF(5)",
          b"CMD ERROR")

    def test_the_hooks_are_each_dos_own(self):
        t = StmtTwin()
        for vec in (0x4161, 0x41BE, 0x41C1):
            self.assertEqual(t.c.peek(vec, 1), b"\xc3")
            self.assertEqual(t.o.peek(vec, 1), b"\xc3")
            self.assertTrue(CDOS <= u16(t.c.peek(vec + 1, 2)) < 0xE800)
        self.assertEqual(u16(t.o.peek(0x4162, 2)), t.o.dos_base + t.o.syms["efn"])
        self.assertEqual(u16(t.o.peek(0x41BF, 2)), t.o.dos_base + t.o.syms["pend"])
        self.assertEqual(u16(t.o.peek(0x41C2, 2)), t.o.dos_base + t.o.syms["cout"])

    def test_where_ours_differs(self):
        """Pinned, so that a change shows up here."""
        p, c = BasicOurs(dict(FILES)), BasicCdos(dict(FILES))
        # GET# into a number variable is TM with the drive left talking; the
        # next statement on the bus: CDOS gives DEVICE NOT PRESENT after
        # about 240 ms, ours goes on
        for q in (p, c):
            q.type(b'CMD OPEN 5,"SEQ"')
            self.assertEqual(q.type(b"CMD GET#5,A"), b"\x07TM Error\rOk\r\x00")
        self.assertEqual(p.type(b"CMD GET#5,A$"), b"Ok\r\x00")
        self.assertEqual(c.type(b"CMD GET#5,A$"), b"\x07DEVICE NOT PRESENT\rOk\r\x00")
        # so after a CMD GET from channel 15 that ends in SN
        for q in (p, c):
            self.assertEqual(q.type(b"CMD GET 6,A$"), b"\x07SN Error\rOk\r\x00")
        self.assertEqual(p.type(b'CMD OPEN 7,"ONE"'), b"Ok\r\x00")
        self.assertEqual(c.type(b'CMD OPEN 7,"ONE"'), b"\x07DEVICE NOT PRESENT\rOk\r\x00")
        # a name of 50 with its ",S,W" overruns CDOS's buffer: its next
        # statement goes wrong; ours takes it
        for q in (p, c):
            self.assertEqual(q.type(b'CMD CREATE 4,"' + b"N" * 50 + b'"'), b"Ok\r\x00")
        self.assertEqual(p.type(b"CMD CLOSE 4"), b"Ok\r\x00")
        self.assertNotEqual(c.type(b"CMD CLOSE 4"), b"Ok\r\x00")

    MUTATIONS = [                       # each must be caught
        ("OPEN without its suffix", ("    ld (hl),b\n    pop hl\nop2:", "    nop\n    pop hl\nop2:")),
        ("eleven files", ("fent:\n    ld hl,files\n    ld b,NFILES", "fent:\n    ld hl,files\n    ld b,NFILES+1")),
        ("the file's own drive forgotten", ("    ld a,(hl)\n    ld (dev),a\n    inc hl\n    ld (fcur),hl",
                                            "    ld a,(hl)\n    nop\n    nop\n    nop\n    inc hl\n    ld (fcur),hl")),
        ("PRINT# to a file open for reading", ("    jp c,R_ERR          ; open for reading",
                                               "    nop\n    nop\n    nop")),
        ("INPUT# 251 bytes", ("    ld b,250", "    ld b,251")),
        ("the EOF flag never set", ("    set 7,(hl)", "    nop\n    nop")),
        ("no UNLISTEN at the end of PRINT", ("    push bc\n    call s28\n    pop bc", "    push bc\n    nop\n    nop\n    nop\n    pop bc")),
        ("GET#'s byte not in the input buffer", ("    ld (hl),a           ; as CDOS, the byte passes through the input buffer",
                                                 "    nop")),
        ("CLEAR does not close 15", ("    call forget\n    ld c,0Fh\n    call s16\n    call dsk\n    pop hl",
                                     "    call forget\n    ld c,0Eh\n    call s16\n    call dsk\n    pop hl")),
        ("RESET keeps the files", ("    call forget         ; the drive closes them all, CDOS forgets them",
                                   "    nop\n    nop\n    nop")),
        ("EOF() of a file for writing", ("    bit 1,a             ; FNOR: open for writing", "    bit 2,a")),
        ("EOF(1) allowed", ("    sub 2\n    cp 13", "    sub 1\n    cp 14")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                dos = build_dos.body(patch=(patch,))
                with self.assertRaises((AssertionError, RuntimeError)):
                    self.short_pass(dos)

    def short_pass(self, dos=None):
        t = StmtTwin(FILES, dos=dos)
        t(b'CMD OPEN 5,"SEQ"', b'CMD CREATE 4,"C"', b'CMD PRINT#4,"A";5', b'CMD PRINT#5,"X"', b"CMD SET 9",
          b"CMD GET#5,A$,B$", b"CMD SET 8", b"PRINT EOF(5);EOF(1)")
        t(*CATCH, b'10 CMD OPEN 6,"L":CMD INPUT#6,A$:FOR I=1 TO 6:CMD INPUT#6,A$:NEXT:PRINT A$;EOF(6)',
          b'20 CMD OPEN 7,"ONE":CMD GET#7,A$,B$:PRINT EOF(7)', b'30 FOR I=8 TO 14:CMD OPEN I,"SEQ":NEXT',
          b'40 PRINT EOF(4):CMD RESET:CMD GET#5,A$', b"RUN", bus=False)
        t(b"CMD CLEAR", b"CMD CLOSE 4", b'CMD OPEN 4,"C"', b"CMD INPUT#4,A$", b"CMD GET#4,A$,B$,C$")

    def test_the_short_pass_passes_unmutated(self):
        self.short_pass()


class BasicBare(Basic, OurPrimo):
    """The B64 after a cold start, no DOS yet: boot files are run on it as
    the boot ROM runs them (stored at 0x4500, jumped to), one after another
    as after Reset+BRK with everything resident."""

    def install(self):
        self.cold_start()

    def boot(self, f):
        m = self.m
        m.set_memory_block(0x4500, f)
        m.pc = 0x4500
        m.sp -= 2
        return self.resume()


def phase_a_boot_file():
    sys.path.insert(0, os.path.join(ds.ROOT, "tools", "primo", "turbo"))
    import build_turbo
    return build_turbo.boot_file(open(BOOT80, "rb").read())[0]


BANNER = b"\x0cPRIFLOP DOS, turbo\rOk\r\x00"


@unittest.skipUnless(HAVE, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestBootFile(unittest.TestCase):
    """Step B7: the boot file of our own DOS (loader.asm), booted on the real
    ROM's BASIC with the bit-level drive: fresh, over itself, over CDOS, over
    phase A, refused, on port 0x40 and 0x80."""

    BODY = ds.DOS

    def setUp(self):
        self.file, self.lsyms = build_dos.boot_file()
        self.size = len(self.BODY[0])

    def state(self, p):
        mem = p.m.memory
        return {"base": u16(mem[0x4174:0x4176]), "top": u16(mem[0x40B1:0x40B3]),
                "vectors": bytes(mem[a] for v in (0x400C, 0x4161, 0x41A6, 0x41BE, 0x41C1) for a in range(v, v + 3))}

    def assertOurs(self, p, base, top, oldcmd=0x1997, oldhook=b"\xc9\x00\x00", port=0x80):
        mem, syms = p.m.memory, self.BODY[1]
        self.assertEqual(u16(mem[0x4174:0x4176]), base)
        self.assertEqual(u16(mem[0x40B1:0x40B3]), top)
        img = bytes(build_dos.relocate(self.BODY[0], self.BODY[2], base))
        live = bytearray(mem[base:base + self.size])
        for k in ("oldcmd", "oldhook"):                      # written by the loader
            live[syms[k]:syms[k] + 3] = img[syms[k]:syms[k] + 3]
        d, t = syms["p_data"], syms["p_anchor"]              # the variables: the XT+ exchange's
        self.assertEqual(bytes(live[:d]) + bytes(live[t:]), img[:d] + img[t:],
                         "the body is not the image relocated to its base")
        self.assertFalse(any(live[syms["p_pad"]:t]), "the padding is not zeros")
        self.assertEqual((mem[base + syms["dev"]], mem[base + syms["cdev"]], mem[base + syms["status"]],
                          mem[base + syms["shadow"]], mem[base + syms["held"]], mem[base + syms["redir"]]),
                         (8, 8, 0, 0, 0, 0))
        self.assertFalse(any(live[syms["files"]:syms["files"] + 30]))
        self.assertEqual(mem[base + syms["outport"]], port)
        self.assertEqual(u16(mem[base + syms["oldcmd"]:base + syms["oldcmd"] + 2]), oldcmd)
        self.assertEqual(bytes(mem[base + syms["oldhook"]:base + syms["oldhook"] + 3]), oldhook)
        for vec, sym in ((0x400C, "svc"), (0x4173, None), (0x41A6, "ehook"), (0x4161, "efn"), (0x41BE, "pend"),
                         (0x41C1, "cout")):
            self.assertEqual(mem[vec], 0xC3)
            self.assertEqual(u16(mem[vec + 1:vec + 3]), base + (syms[sym] if sym else 0))
        self.assertTrue(p.drive.turbo)

    def assertWorks(self, p):
        # a few statements on what was booted: the directory, save and load
        # through the turbo, the byte channel, an error message
        self.assertTrue(p.type(b"CMD $").endswith(b"BLOCKS FREE.\rOk\r\x00"))
        p.type(b'10 PRINT "HI"')
        self.assertEqual(p.type(b'CMD SAVE"BOOTED"'), b"Ok\r\x00")
        p.type(b"NEW")
        p.type(b'CMD LOAD"BOOTED"')
        self.assertEqual(p.type(b"LIST"), b'10 PRINT "HI"\rOk\r\x00')
        p.type(b'CMD CREATE 4,"SQ"')
        p.type(b'CMD PRINT#4,"AB"')
        p.type(b"CMD CLOSE 4")
        p.type(b'CMD OPEN 5,"SQ"')
        p.type(b"CMD GET#5,A$,B$")
        self.assertEqual(p.type(b"PRINT A$;B$;EOF(5)"), b"AB 0 \rOk\r\x00")
        self.assertEqual(p.type(b'CMD LOAD"NONE"'), b"\x0762,FILE NOT FOUND,00,00\rOk\r\x00")
        self.assertEqual(p.type(b"CMD XYZ"), b"\x07SN Error\rOk\r\x00")

    def test_fresh(self):
        p = BasicBare({})
        self.assertEqual(p.boot(self.file), BANNER)
        base = 0xE7FE - self.size + 1                       # 0xE7FF left free, as CDOS leaves it
        self.assertEqual(base, 0xDC6E)
        self.assertOurs(p, base, base - 1)
        self.assertWorks(p)

    def test_m_reads_the_layout(self):
        sys.path.insert(0, os.path.join(ds.ROOT, "tools", "primo", "turbo"))
        import build_turbo
        p = BasicBare({b"M": build_turbo.memprg()[0]})
        p.boot(self.file)
        self.assertEqual(p.type(b'CMD LOAD"M"'), b"TOP 56429 DOS 56430 PRIFLOP\r\x00")
        q = BasicBare({b"M": build_turbo.memprg()[0]})
        q.boot(phase_a_boot_file())
        self.assertRegex(q.type(b'CMD LOAD"M"'), rb"^TOP \d+ CDOS 56395 MOD 1 HOOK\r")

    def test_the_boot_file_is_no_larger_than_cdoss(self):
        self.assertLessEqual(len(self.file), os.path.getsize(BOOT80))
        self.assertLessEqual(self.size, 0xBB5)

    def test_over_itself_it_does_not_stack(self):
        p = BasicBare({})
        p.boot(self.file)
        for _ in range(2):
            self.assertEqual(p.boot(self.file), BANNER)
            self.assertOurs(p, 0xDC6E, 0xDC6D)              # its own saved vectors, not itself
        # a top lowered by the user since: kept, and nothing below touched
        p.poke(0xD000, b"KEEP")
        p.m.set_memory_block(0x40B1, b"\x00\xd8")
        p.boot(self.file)
        self.assertOurs(p, 0xDC6E, 0xD800)
        self.assertEqual(p.peek(0xD000, 4), b"KEEP")
        self.assertWorks(p)

    def test_over_cdos(self):
        p = BasicBare({})
        p.boot(open(BOOT80, "rb").read())
        self.assertEqual(u16(p.peek(0x4174, 2)), CDOS)
        self.assertEqual(p.boot(self.file), BANNER)
        self.assertOurs(p, CDOS + 0xBB4 - self.size, CDOS + 0xBB4 - self.size - 1)
        self.assertWorks(p)

    def test_over_cdos_with_memory_kept_below_it(self):
        p = BasicBare({})
        p.boot(open(BOOT80, "rb").read())
        p.m.set_memory_block(0x40B1, b"\x00\xd0")
        p.boot(self.file)
        self.assertOurs(p, CDOS + 0xBB4 - self.size, 0xD000)

    def test_over_phase_a(self):
        p = BasicBare({})
        p.boot(phase_a_boot_file())
        self.assertLess(u16(p.peek(0x40B1, 2)), CDOS - 1)   # the module is under CDOS
        self.assertEqual(p.boot(self.file), BANNER)
        self.assertOurs(p, CDOS + 0xBB4 - self.size, CDOS + 0xBB4 - self.size - 1)
        self.assertWorks(p)

    def test_refused_nothing_changes(self):
        for how in ("another drive", "no drive", "another ROM"):
            with self.subTest(how):
                p = BasicBare({}, drive_dev=9 if how == "no drive" else 8)
                p.boot(open(BOOT80, "rb").read())            # CDOS stays as it is
                if how == "another drive":
                    p.drive.turbo = None
                if how == "another ROM":
                    p.poke(0x2B1D, b"\x00")
                before = (self.state(p), p.peek(CDOS, 0xE800 - CDOS))
                text = p.boot(self.file)
                want = {"another drive": b"No PRIFLOP turbo: 31,SYNTAX ERROR,00,00\r",
                        "no drive": b"No drive\r", "another ROM": b"Not this BASIC ROM\r"}[how]
                self.assertEqual(text, want + b"Ok\r\x00")
                self.assertEqual((self.state(p), p.peek(CDOS, 0xE800 - CDOS)), before)

    def test_port_0x40(self):
        p = type("Port40", (BasicBare,), {"outport": 0x40})({})
        p.poke(0x029F, b"\x40")
        p.poke(0x0687, b"\x40")
        self.assertEqual(p.boot(self.file), BANNER)
        self.assertOurs(p, 0xDC6E, 0xDC6D, port=0x40)
        self.assertWorks(p)
        q = BasicBare({})                                   # the two copies disagree: 0x80
        q.poke(0x0687, b"\x40")
        q.boot(self.file)
        self.assertOurs(q, 0xDC6E, 0xDC6D)

    def test_never_a_base_ending_in_0x97(self):
        # ASM takes a CMD vector whose low byte is 0x97 for "no DOS"
        p = BasicBare({})
        p.m.set_memory_block(0x40B1, bytes([(0xDB97 + self.size) & 0xFF, (0xDB97 + self.size) >> 8]))
        p.boot(self.file)
        self.assertOurs(p, 0xDB96, 0xDB95)

    def test_mutations_are_caught(self):
        for what, patch in [
                ("the top moved though memory was kept", ("    jr nz,pl2", "    nop\n    nop")),
                ("chained to itself", ("    ld de,chcmd\n    ldi\n    ldi\n    pop hl", "    ld de,chhook\n    ldi\n    ldi\n    pop hl")),
                ("phase A's module not joined", ("    cp A_OP\n    jr nz,cdos1", "    cp A_OP+1\n    jr nz,cdos1")),
                ("the padding not zeroed", ("    ld (de),a\n    dec de\n    djnz pad", "    nop\n    dec de\n    djnz pad"))]:
            with self.subTest(what):
                f, _ = build_dos.boot_file(patch=(patch,))
                with self.assertRaises(AssertionError):
                    self.short_pass(f)

    def short_pass(self, f):
        p = BasicBare({})
        p.boot(phase_a_boot_file())
        p.boot(f)
        self.assertOurs(p, 0xDC6E, 0xDC6D)
        p.boot(f)
        self.assertOurs(p, 0xDC6E, 0xDC6D)
        p.m.set_memory_block(0x40B1, b"\x00\xd8")
        p.boot(f)
        self.assertOurs(p, 0xDC6E, 0xD800)

    def test_the_short_pass_passes_unmutated(self):
        self.short_pass(self.file)


if __name__ == "__main__":
    unittest.main()
