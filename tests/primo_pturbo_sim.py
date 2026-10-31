#!/usr/bin/env python3
"""PTURBO -- the Primo's own soak test -- run in the simulations before the
user runs it on the machine.

    tools/external/venv-sim/bin/python tests/primo_pturbo_sim.py

PTURBO (tools/primo/turbo/pturbo.asm) calls CDOS's services 0x02, 0x06,
0x0E, 0x10 and 0x12 through RST 28H, and the ROM's print and keyboard
routines. Here RST 28H is a table dispatcher of our own (A = the service
number, as CDOS's interface documents it) over a table whose load, verify and save entries point at the turbo module,
as after TURBO; the other services and the ROM routines are done in Python.
It runs against both drives: the Python model (`Drive`, primo_turbo_sim) and
the firmware's own C (primo_turbo_csim). It must report every file OK, and
under injected faults still never an error that is not a fault's own.

With kind="B" our own DOS stands in for CDOS and the module: RST 28H is its
service call, every service but 0x18 and 0x1A is its own, and PTURBO finds
its signature and sets `vfy` 0: our DOS has no service 0x12, and PTURBO reads the file back on
the standard bus instead (`rbv`). The drives here speak only the turbo, so
`vrf` becomes a stub whose answer -- the differences between the drive's
file and memory -- Python computes; `rbv` itself is tested on the bit-level
drive (TestPturboReadback in tests/primo_dos_sim.py).
"""
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import primo_turbo_sim as ps  # noqa: E402
import primo_turbo_csim as pc  # noqa: E402

# under the turbo module (0xD800), clear of the screen (0xE800 up), which
# PTURBO's screen round saves, clears and loads back
DISP, TAB, PYSTUB = 0xD000, 0xD100, 0xD200
P_TEXT, P_CHAR, P_NUM, P_KEY, P_VRF = 0xFA, 0xF9, 0xF8, 0xF7, 0xF6


def install_rig(primo, rounds):
    """RST 28H through a CDOS-style table (phase A; our DOS has its own);
    ROM print/keyboard stubs; PTURBO."""
    mem = primo.mem
    ours = primo.kind == "B"
    if not ours:
        # our own dispatcher, written for this rig (clean room: not CDOS's
        # code): PUSH HL; PUSH DE; LD E,A; LD D,0; LD HL,TAB; ADD HL,DE;
        # LD E,(HL); INC HL; LD D,(HL); EX DE,HL; POP DE; EX (SP),HL; RET --
        # it jumps to TAB[A] with HL, DE and BC as the caller left them
        mem[0x0028:0x002B] = bytes([0xC3, DISP & 0xFF, DISP >> 8])
        mem[DISP:DISP + 16] = bytes([0xE5, 0xD5, 0x5F, 0x16, 0x00, 0x21, TAB & 0xFF, TAB >> 8,
                                     0x19, 0x5E, 0x23, 0x56, 0xEB, 0xD1, 0xE3, 0xC9])
        mem[PYSTUB:PYSTUB + 3] = bytes([0xD3, 0xFD, 0xC9])      # OUT (0FDh),A ; RET
        for idx in range(0, 0x30, 2):
            target = ps.MOD if idx in (0x0E, 0x10, 0x12) else PYSTUB
            mem[TAB + idx:TAB + idx + 2] = bytes([target & 0xFF, target >> 8])
    for addr, port in ((0x2B75, P_TEXT), (0x032A, P_CHAR), (0x0FAF, P_NUM)):
        mem[addr:addr + 3] = bytes([0xD3, port, 0xC9])          # OUT (port),A ; RET
    mem[0x001D:0x0021] = bytes([0xDB, P_KEY, 0xB7, 0xC9])       # IN A,(P_KEY) ; OR A ; RET
    body = bytearray(ps.build_turbo.pturbo(primo.dev)[5:-3])
    if os.environ.get("PTURBO_FULL") != "1":
        # the real sizes go to 16 KB; a simulated round of them takes many
        # minutes, so by default the sim uses the same edge cases scaled down
        # (1, 255, 256, 257 kept: the frame edges are what matter)
        _, syms = ps.build_turbo.assemble("pturbo.asm")
        o = syms["sizes"] - 0x7000
        for i, v in enumerate((1, 255, 256, 257, 1000, 510, 511, 766)):
            body[o + 2 * i:o + 2 * i + 2] = bytes([v & 0xFF, v >> 8])
        o = syms["slen"] - 0x7000                               # the screen: 768 bytes
        body[o:o + 2] = bytes([0x00, 0x03])
    if ours:                                                    # vrf: OUT (P_VRF),A ; RET
        _, syms = ps.build_turbo.assemble("pturbo.asm")
        o = syms["vrf"] - 0x7000
        body[o:o + 3] = bytes([0xD3, P_VRF, 0xC9])
        primo.pt_syms = syms
    mem[0x7000:0x7000 + len(body)] = body
    scr = ps.SCREEN_HI * 256                                    # something on the screen
    mem[scr:0xF000] = ps.pattern(0xF000 - scr, 11)              # (0xF000 up: the fallback stubs)
    # the driver calls PTURBO, as CDOS's 0xC3 record does
    mem[ps.DRIVER + 1:ps.DRIVER + 3] = bytes([0x00, 0x70])
    primo.text = []
    primo.rounds_wanted = rounds

    orig_service = primo.service

    def service(m, a=None):
        if a is not None:               # our DOS's trapped 0x18 or 0x1A
            orig_service(m, a)
            return
        a = m.a
        base = ps.CDOS
        if a == 0x02:
            m.memory[base + 0x0B8D:base + 0x0B8F] = bytes([m.e, m.d])
            m.memory[base + 0x0B8F:base + 0x0B91] = bytes([m.l, m.h])
            m.f = 0
        elif a in (0x06, 0x08):
            if a == 0x06:
                primo.device = m.c
            m.a, m.f = primo.device, 0
        else:
            orig_service(m)
    primo.service = service


def run_pturbo(primo, rounds):
    install_rig(primo, rounds)

    def out_extra(m, port, value):
        if port == P_TEXT:
            hl, out = m.hl, []
            while m.memory[hl]:
                out.append(chr(m.memory[hl]))
                hl += 1
            primo.text.append("".join(out).replace("\r", "\n"))
        elif port == P_CHAR:
            primo.text.append("\n" if m.a == 13 else chr(m.a))
        elif port == P_VRF:
            # what rbv would count: the drive's file against the type, the
            # length, the bytes in memory and the 0xC9 (A, NC NZ)
            s, mem = primo.pt_syms, m.memory
            name = bytes(mem[m.hl + 1:m.hl + 1 + mem[m.hl]])
            vmem, n = ps.u16(mem[s["vmem"]:s["vmem"] + 2]), ps.u16(mem[s["vlen"]:s["vlen"] + 2])
            want = ps.rec(mem[s["vtype"]], 0, bytes(mem[vmem:vmem + n])) + b"\xc9"
            got = primo.stored(name)
            if got is None:
                m.a, m.f = 62, ps.F_Z
                return
            bad = 255 if len(got) != len(want) else min(255, sum(a != b for a, b in zip(got, want)))
            m.a, m.f = bad, 0
        else:
            primo.text.append(str(m.hl))
    primo._out_extra = out_extra
    res = primo.run(0, hl=0, de=0, bc=0)
    return "".join(primo.text), res


@unittest.skipIf(ps.MODULE is None, "needs the z80 package and z88dk")
class TestPturboPython(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS

    def make(self, dev=8, **kw):
        files = {}
        drive = ps.Drive(files, random.Random(1), ps.FAST, **kw.pop("drive_kw", {}))
        primo = ps.Primo(drive, kind=self.kind, **kw)
        primo.dev, primo.device = dev, dev
        primo.stored = files.get
        return primo, drive, files

    def test_two_rounds_all_ok(self):
        primo, drive, files = self.make()
        text, res = run_pturbo(primo, 2)
        self.assertIn("R 1 OK 9 ERR 0", text)
        self.assertIn("R 2 OK 18 ERR 0", text)
        self.assertEqual(sorted(files), [b"PT%d" % i for i in range(8)] + [b"PTS"])

    def test_brk_tapped_mid_round_stops_after_it(self):
        # BRK seen once, during round 1's transfers: PTURBO finishes the
        # round and stops (the first version looked only at the round's end
        # and ran on; rounds_wanted=3 bounds that)
        primo, drive, files = self.make()
        primo.brk_pulse = 5
        text, res = run_pturbo(primo, 3)
        self.assertIn("R 1 OK 9 ERR 0", text)
        self.assertNotIn("R 2", text)
        self.assertIn("stopped", text)

    def test_faults_change_nothing_visible(self):
        primo, drive, files = self.make(seed=3, wait_states=True, p_flip_primo=1 / 20000,
                                        p_spike=1 / 20000, drive_kw={"p_flip": 0.05,
                                                                     "p_flip_sample": 1 / 5000})
        text, res = run_pturbo(primo, 1)
        self.assertIn("R 1 OK 9 ERR 0", text)


@unittest.skipIf(ps.MODULE is None, "needs the z80 package and z88dk")
class TestPturboC(unittest.TestCase):
    kind = "A"

    @classmethod
    def setUpClass(cls):
        pc.LIB = pc.build_lib()

    def test_one_round_all_ok(self):
        primo = pc.CPrimo(seed=1, kind=self.kind)
        primo.bridge = pc.Bridge(primo, random.Random(1))
        primo.dev, primo.device = 8, 8
        primo.stored = primo.bridge.file_out
        text, res = run_pturbo(primo, 1)
        self.assertIn("R 1 OK 9 ERR 0", text, text)
        self.assertEqual(pc.files_left(primo.bridge), (0, 0),
                         "a file left open: the drive's lfs_remove() would loop")


@unittest.skipIf(ps.OURS is None, "needs the z80 package and z88dk")
class TestPturboPythonOurs(TestPturboPython):
    """PTURBO on our own DOS (phase B, step B4), verifying by reading back."""
    kind = "B"


@unittest.skipIf(ps.OURS is None, "needs the z80 package and z88dk")
class TestPturboCOurs(TestPturboC):
    kind = "B"


if __name__ == "__main__":
    unittest.main()
