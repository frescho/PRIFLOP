#!/usr/bin/env python3
"""The Primo turbo with the drive's REAL C code: src/pico/primo_turbo.c,
compiled for the host, against the Primo's real Z80 module.

    tools/external/venv-sim/bin/python tests/primo_turbo_csim.py

tests/primo_turbo_sim.py proved the protocol with a Python model of the
drive (`Drive`). This runs the firmware's own C instead -- the port of that
model -- so a mistake in the port shows up here and not on the Primo.

How: primo_turbo.c is built with tests/primo_turbo_host/shim.c, which hands
its PIO, GPIO and clock calls to Python. The C session runs in a thread in
lockstep with the Z80: every time the C polls its receive FIFO it gives the
turn back, and the simulation gives it the turn again whenever the PIO model
produces a sample, needs a word, or time passes. One virtual clock for both.
The PIO model runs the real .pio source (tests/pio_model.py), the bus is the
same open-collector model, and the Primo, fake CDOS and faults are those of
tests/primo_turbo_sim.py: with phase A's module, and (TestCDriveOurs) with our
own DOS in CDOS's place.
"""
import collections
import ctypes
import os
import random
import subprocess
import sys
import tempfile
import threading
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import primo_turbo_sim as ps  # noqa: E402

LIB = None


def build_lib():
    out = os.path.join(tempfile.mkdtemp(), "libpt.so")
    host = os.path.join(HERE, "primo_turbo_host")
    subprocess.run(["gcc", "-O1", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                    "-I", os.path.join(host, "shim"), "-I", os.path.join(ROOT, "src", "pico"),
                    "-o", out, os.path.join(host, "shim.c"),
                    os.path.join(ROOT, "src", "pico", "primo_turbo.c")], check=True)
    return out


class CPio(ps.Pio):
    """The PIO model, fed by the C code's FIFO writes instead of a model."""

    RESYNC_NS = float("inf")            # the C code does its own idle reset

    def __init__(self, bus, bridge, rng=None, p_spike=0.0):
        self.bridge = bridge
        super().__init__(bus, bridge, rng, p_spike)
        self.fifo.clear()
        self.rx = collections.deque()
        self.enabled = False

    def advance(self, until):
        while self.enabled and self.t < until:
            op, a, b, _delay = self.prog[self.pc]
            nxt = self.wrap_target if self.pc == self.wrap else self.pc + 1
            if op == "wait":
                tt = self.bus["clk"].next_level(self.t - self.SYNC, a, until - self.SYNC)
                if tt is None:
                    break
                self.t = max(self.t, tt + self.SYNC)
                self.pc = nxt
                if self.p_spike and self.rng.random() < self.p_spike:
                    self.spike()
                continue
            if op == "in":
                bit = self.seen(self.bus["data"], self.t)
                self.rx.append(self.bridge.glitch_sample(bit) << 31)
                self.pc = nxt
                self.bridge.run_c(self.t)               # the C sees it
            elif op == "pull":
                if not self.fifo:
                    self.bridge.run_c(self.t)
                    if not self.fifo:
                        return                          # stalled until the C puts a word
                self.osr = self.fifo.popleft()
                self.pc = nxt
            elif op == "out_pindirs":
                self.bus["data"].set("drive", self.t + self.CYCLE, bool(self.osr & 1))
                self.osr >>= 1
                self.pc = nxt
            elif op == "out_y":
                self.y = self.osr & 1
                self.osr >>= 1
                self.pc = nxt
            elif op == "jmp":
                self.pc = b if (a == "!y" and self.y == 0) else nxt
            self.t += self.CYCLE
        self.bridge.run_c(until)                         # time passes for the C too

    def spike(self):
        """A glitch on CLOCK the drive takes for two edges."""
        self.spikes += 1
        for _ in range(2):
            self.rx.append(self.seen(self.bus["data"], self.t) << 31)
            if self.fifo:
                w = self.fifo.popleft()
                self.bus["data"].set("drive", self.t, bool(w & ps.PULL))
                if w & ps.HOLD and self.fifo:
                    self.fifo.popleft()


class Bridge:
    """The C library, its callbacks, and the lockstep with the C thread."""

    made = []                           # every bridge, for files_left()

    def __init__(self, primo, rng, p_flip=0.0, p_flip_sample=0.0, file=None, name=b""):
        self.primo, self.rng = primo, rng
        self.p_flip, self.p_flip_sample = p_flip, p_flip_sample
        # a private copy: dlopen of the same path would share the C's statics
        copy = os.path.join(tempfile.mkdtemp(), "libpt.so")
        with open(LIB, "rb") as src, open(copy, "wb") as dst:
            dst.write(src.read())
        self.lib = ctypes.CDLL(copy)
        Bridge.made.append(self)
        self.now_ns = 0.0
        self.thread = None
        self.c_turn = threading.Semaphore(0)
        self.sim_turn = threading.Semaphore(0)
        self.done = False
        self.pio = None
        self.atn_release_ns = 0.0
        self._keep = []
        self.set_cb("py_time", ctypes.CFUNCTYPE(ctypes.c_uint32), self.clock)
        self.set_cb("py_gpio", ctypes.CFUNCTYPE(ctypes.c_bool, ctypes.c_uint), self.gpio)
        self.set_cb("py_set_data", ctypes.CFUNCTYPE(None, ctypes.c_int),
                    lambda v: self.bus("data").set("drive", self.now_ns, v == 0))
        self.set_cb("py_pindir", ctypes.CFUNCTYPE(None, ctypes.c_int),
                    lambda v: self.bus("data").set("drive", self.now_ns, bool(v)))
        self.set_cb("py_enable", ctypes.CFUNCTYPE(None, ctypes.c_int), self.enable)
        self.set_cb("py_clear", ctypes.CFUNCTYPE(None, ctypes.c_int), self.clear)
        self.set_cb("py_jmp", ctypes.CFUNCTYPE(None, ctypes.c_uint), self.jmp)
        self.set_cb("py_put", ctypes.CFUNCTYPE(None, ctypes.c_uint32), self.put)
        self.set_cb("py_rx_empty", ctypes.CFUNCTYPE(ctypes.c_bool), self.rx_empty)
        self.set_cb("py_get", ctypes.CFUNCTYPE(ctypes.c_uint32), lambda: self.pio.rx.popleft())
        ctypes.c_uint8.in_dll(self.lib, "primo_turbo_enabled").value = 1
        assert self.lib.primo_turbo_init()
        self.set_file(name, file)

    def set_cb(self, name, ftype, fn):
        cb = ftype(fn)
        self._keep.append(cb)
        ctypes.c_void_p.in_dll(self.lib, name).value = ctypes.cast(cb, ctypes.c_void_p).value

    def set_file(self, name, data):
        if data is not None:
            buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
            self.lib.shim_store_put(name, len(name), buf, len(data))

    def file_out(self, name=b"S"):
        buf = (ctypes.c_uint8 * 20000)()
        n = self.lib.shim_store_get(name, len(name), buf)
        return None if n < 0 else bytes(buf[:n])

    def start(self):
        return []

    def clock(self):
        """The C reading the clock costs it time: 100 ns a call. Without this
        a loop that only watches the clock (the wait for ATN's release) would
        spin at one instant for ever, since the C gives the turn back only
        when it polls its FIFO."""
        self.now_ns += 100
        # the drive has been up for a while: an uninitialised timestamp
        # looks like an hour of silence (the second real test, 2026-10-05)
        return int(self.now_ns / 1000 + 3_600_000_000) & 0xFFFFFFFF

    def bus(self, line):
        return self.primo.bus[line]

    def gpio(self, pin):
        if pin == ps.GPIO_CLK:
            return bool(self.bus("clk").level(self.now_ns))
        # ATN: CDOS releases it a little after its UNTALK, when the session
        # has already started (the first real test aborted on exactly this)
        return self.now_ns >= self.atn_release_ns

    def enable(self, on):
        if self.pio:
            self.pio.enabled = bool(on)
            self.pio.t = max(self.pio.t, self.now_ns)

    def clear(self, _):
        if self.pio:
            self.pio.fifo.clear()
            self.pio.rx.clear()

    def jmp(self, addr):
        if self.pio:
            self.pio.pc = addr
            self.pio.t = max(self.pio.t, self.now_ns)

    def put(self, w):
        if self.p_flip and self.rng.random() < self.p_flip:
            w ^= ps.PULL                               # a wrong bit on the line
        self.pio.fifo.append(w)

    def glitch_sample(self, bit):
        if self.p_flip_sample and self.rng.random() < self.p_flip_sample:
            return bit ^ 1
        return bit

    # -- the lockstep
    def rx_empty(self):
        self.sim_turn.release()                      # give the turn back
        self.c_turn.acquire()                        # and wait for it
        return not self.pio.rx

    def run_c(self, t):
        if not self.thread or self.done:
            return
        self.now_ns = max(self.now_ns, t)
        self.c_turn.release()
        self.sim_turn.acquire()

    def start_session(self, t):
        self.done = False                           # a new session, a new thread
        self.now_ns = t
        # CDOS's UNTALK tail (body 0x0714): ATN released, then CLOCK ~50 us
        # later. The session starts before either (the real tests found both).
        self.atn_release_ns = t + 40_000
        self.primo.bus["clk"].set("cdos", t, True)
        self.primo.bus["clk"].set("cdos", t + 90_000, False)
        self.pio = CPio(self.primo.bus, self, self.primo.rng, self.primo.p_spike)
        self.primo.pio = self.pio

        def body():
            self.lib.primo_turbo_poll(0x6F)     # BUS_CLEANUP after the answer's data
            self.lib.primo_turbo_poll(0x5F)     # ... and after the UNTALK: the session
            self.done = True
            self.sim_turn.release()

        self.thread = threading.Thread(target=body, daemon=True)
        self.thread.start()
        self.sim_turn.acquire()                      # until its first poll

    # -- the standard-speed side, as the fake CDOS sees it
    def error_text(self):
        buf = bytes((ctypes.c_uint8 * 46).in_dll(self.lib, "error_buffer"))
        return buf.split(b"\r")[0].decode()

    def command(self, cmd):
        (ctypes.c_uint8 * 120).in_dll(self.lib, "command_buffer")[:len(cmd)] = list(cmd)
        ctypes.c_uint8.in_dll(self.lib, "command_length").value = len(cmd)
        self.lib.primo_turbo_command()
        self.lib.primo_turbo_poll(0x3F)              # BUS_CLEANUP of the command (UNLISTEN)
        return self.error_text()


class CPrimo(ps.Primo):
    """primo_turbo_sim.Primo with the C drive behind the fake CDOS."""

    def __init__(self, bridge_kw=None, **kw):
        self.bridge_kw = bridge_kw or {}
        super().__init__(drive=None, **kw)

    def service(self, m, a=None):
        a = m.a if a is None else a
        if a == 0x18:
            # CDOS's 0x18 is two transactions: the command (LISTEN 15 ...
            # UNLISTEN), then the error channel read (TALK 15 ... UNTALK).
            # The firmware's BUS_CLEANUP after the second starts the session.
            n = m.memory[m.hl]
            reply = self.bridge.command(bytes(m.memory[m.hl + 1:m.hl + 1 + n]))
            if reply.startswith("00,TURBO"):
                self.bridge.start_session(self.now())
        elif a == 0x1A:
            reply = self.bridge.error_text()
        else:
            raise AssertionError(f"the module called CDOS service {a:#x}")
        buf = self.errbuf
        for i, c in enumerate(reply.encode() + b"\x00"):
            m.memory[buf + i] = c
        code = int(reply[:2])
        m.a, m.f = (code, 0x00) if code < 2 else (code, ps.F_Z)


def c_load(file, name=b"PROG", mask=0x1F, idx=0x10, seed=1, bridge_kw=None, **kw):
    primo = CPrimo(seed=seed, **kw)
    primo.bridge = Bridge(primo, random.Random(seed + 7), file=file, name=name, **(bridge_kw or {}))
    ps.name_at(primo, ps.NAME, name)
    return primo, primo.run(idx, hl=ps.NAME, de=ps.BASE, bc=mask)


def c_save(data, seed=1, bridge_kw=None, **kw):
    primo = CPrimo(seed=seed, **kw)
    primo.bridge = Bridge(primo, random.Random(seed + 7), file=None, name=b"S", **(bridge_kw or {}))
    ps.name_at(primo, ps.NAME, b"S")
    primo.poke(ps.BASE, data)
    primo.poke(0x7200, bytes([0xD1, 0, 0, len(data) & 0xFF, len(data) >> 8,
                              ps.BASE & 0xFF, ps.BASE >> 8, 0xC9]))
    res = primo.run(0x0E, hl=ps.NAME, de=0x7200)
    return primo, res, primo.bridge.file_out(), ps.rec(0xD1, 0, data) + bytes([0xC9])


def files_left(bridge):
    """(files left open, files opened into a buffer still holding one): both
    must be 0, or littlefs's open-file list breaks (shim.c)."""
    return (ctypes.c_int.in_dll(bridge.lib, "shim_open_files").value,
            ctypes.c_int.in_dll(bridge.lib, "shim_reopened").value)


def stats(primo):
    lib = primo.bridge.lib
    names = ["sessions", "completed", "aborted", "exchanges", "resent", "garbled", "refused",
             "bad_writes", "dup_writes", "reissued", "resyncs", "bytes_to", "bytes_from", "us",
             "call_waits"]
    arr = (ctypes.c_uint32 * len(names)).in_dll(lib, "primo_turbo_stats")
    return dict(zip(names, arr))


@unittest.skipIf(ps.MODULE is None, "needs the z80 package and z88dk")
class TestCDrive(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS

    @classmethod
    def setUpClass(cls):
        global LIB
        LIB = build_lib()

    def setUp(self):
        Bridge.made.clear()

    def tearDown(self):
        for b in Bridge.made:
            if b.thread and not b.done:
                continue                # still in a session: its file is open by right
            self.assertEqual(files_left(b), (0, 0), "a file left open (free_buffer, not cleanup)")

    def file(self):
        self.prog = ps.pattern(700, 1)
        self.mc = ps.pattern(300, 2)
        return ps.rec(0xD1, 0, self.prog) + ps.rec(0xD9, 0x9000, self.mc) + bytes([0xC9])

    def test_load(self):
        primo, res = c_load(kind=self.kind, file=self.file())
        ps.check_flags(self, res)
        self.assertEqual(primo.peek(ps.BASE, 700), self.prog)
        self.assertEqual(primo.peek(0x9000, 300), self.mc)
        self.assertEqual(res["a"], 0xD9)
        self.assertEqual(stats(primo)["completed"], 1)

    def test_missing_file(self):
        primo, res = c_load(kind=self.kind, file=None, name=b"NONE")
        ps.check_flags(self, res, z=True)
        self.assertEqual(res["a"], 62)

    def test_save(self):
        primo, res, got, want = c_save(kind=self.kind, data=ps.pattern(600, 3))
        ps.check_flags(self, res)
        self.assertEqual(got, want)

    def test_a_file_that_ends_early_is_an_error(self):
        # 7a: 99 with Z, as CDOS's 0x63 for an EOI inside a
        # record; the bytes that came are stored. Inside a header, or no 0xC9
        # (CDOS: a bus error), it is the same 99.
        whole = self.file()
        for cut in (40, 300, 3, 700 + 5 + 2, len(whole) - 1):
            with self.subTest(cut=cut):
                primo, res = c_load(kind=self.kind, file=whole[:cut])
                ps.check_flags(self, res, z=True)
                self.assertEqual(res["a"], 99)
                if 5 < cut <= 705:
                    self.assertEqual(primo.peek(ps.BASE, cut - 5), self.prog[:cut - 5])

    def test_save_ends_at_a_c3_entry(self):
        # 7a: as CDOS, the file ends with the 0xC3 and its
        # address; what follows in the descriptor is not read
        data = ps.pattern(10, 4)
        primo = CPrimo(seed=1, kind=self.kind)
        primo.bridge = Bridge(primo, random.Random(8), file=None, name=b"S")
        ps.name_at(primo, ps.NAME, b"S")
        primo.poke(ps.BASE, data)
        primo.poke(0x7200, bytes([0xD9, 0x34, 0x12, 10, 0, ps.BASE & 0xFF, ps.BASE >> 8,
                                  0xC3, 0x00, 0xA0, 0xC9]))
        res = primo.run(0x0E, hl=ps.NAME, de=0x7200)
        ps.check_flags(self, res)
        self.assertEqual(primo.bridge.file_out(), ps.rec(0xD9, 0x1234, data) + b"\xc3\x00\xa0")

    def test_load_with_faults(self):
        for seed in range(4):
            with self.subTest(seed=seed):
                primo, res = c_load(kind=self.kind, file=self.file(), seed=seed, wait_states=True, p_flip_primo=1 / 20000,
                                    p_spike=1 / 20000, bridge_kw={"p_flip": 1 / 20000})
                ps.check_flags(self, res)
                self.assertEqual(primo.peek(ps.BASE, 700), self.prog)
                self.assertEqual(primo.peek(0x9000, 300), self.mc)

    def test_save_with_faults(self):
        for seed in range(4):
            with self.subTest(seed=seed):
                primo, res, got, want = c_save(kind=self.kind, data=ps.pattern(600, seed), seed=seed, wait_states=True,
                                               p_flip_primo=1 / 20000, p_spike=1 / 20000,
                                               bridge_kw={"p_flip_sample": 1 / 5000})
                ps.check_flags(self, res)
                self.assertEqual(got, want)

    def test_screen_round_trip(self):
        # CMD SAVE SCREEN, CMD LOAD, CMD TEST at the real size and place: one
        # 0xD5 record of 0x1800 bytes from (0x404A) x 256 = 0xE800 to the top
        # of memory. The screen is above the limit and must not count against
        # it; the end wraps to 0x0000 in 16 bits, as in CDOS's HL.
        top = ps.SCREEN_HI * 256
        data = ps.pattern(0x1800, 5)
        for seed, kw in ((1, {}), (2, dict(wait_states=True, p_flip_primo=1 / 20000,
                                           p_spike=1 / 20000,
                                           bridge_kw={"p_flip": 1 / 20000,
                                                      "p_flip_sample": 1 / 5000}))):
            with self.subTest(seed=seed):
                bkw = kw.pop("bridge_kw", {})
                primo = CPrimo(seed=seed, kind=self.kind, **kw)
                primo.bridge = Bridge(primo, random.Random(seed + 7), file=None, name=b"SCR", **bkw)
                ps.name_at(primo, ps.NAME, b"SCR")
                primo.poke(top, data)
                primo.poke(0x7200, bytes([0xD5, 0, 0, 0x00, 0x18, top & 0xFF, top >> 8, 0xC9]))
                res = primo.run(0x0E, hl=ps.NAME, de=0x7200)
                ps.check_flags(self, res)
                got = primo.bridge.file_out(b"SCR")
                self.assertEqual(got, ps.rec(0xD5, 0, data) + bytes([0xC9]))

                primo, res = c_load(kind=self.kind, file=got, name=b"SCR", seed=seed, bridge_kw=bkw, **kw)
                ps.check_flags(self, res)
                self.assertEqual(primo.peek(top, 0x1800), data)
                self.assertEqual(res["a"], 0xD5)
                self.assertEqual(res["de"], 0x0000)

                if self.kind == "B":
                    continue                    # our own DOS has no verify
                for flips, want in (((), 0), ((0, 0x17FF), 2)):
                    changed = bytearray(data)
                    for i in flips:
                        changed[i] ^= 0xFF
                    primo = CPrimo(seed=seed, kind=self.kind, **kw)
                    primo.bridge = Bridge(primo, random.Random(seed + 7), file=got, name=b"SCR",
                                          **bkw)
                    ps.name_at(primo, ps.NAME, b"SCR")
                    primo.poke(top, bytes(changed))
                    res = primo.run(0x12, hl=ps.NAME, de=ps.BASE, bc=0x0E)
                    ps.check_flags(self, res)
                    self.assertEqual(res["a"], want)

    def test_pcall(self):
        # PCALL.PRG, the machine's 0xCD test: two calls between data records,
        # then an autostart that checks both ran in order and the data is whole
        f, syms = ps.build_turbo.pcall()
        cd2 = bytes([0xCD, syms["call2"] & 0xFF, syms["call2"] >> 8])
        for file, want in ((f, 1), (f.replace(cd2, b""), 2)):    # without call 2: BAD
            primo = CPrimo(seed=1, kind=self.kind)
            primo.bridge = Bridge(primo, random.Random(8), file=file, name=b"PCALL")
            ps.name_at(primo, ps.NAME, b"PCALL")
            primo.poke(0x2B75, b"\xc9")                          # ROM PRINTS: RET
            primo.poke(0x40F9, b"\x34\x52")                      # BASIC's program end
            res = primo.run(0x10, hl=ps.NAME, de=ps.BASE, bc=0x1F)
            self.assertEqual(primo.peek(syms["result"], 1)[0], want)
            # what CMD LOAD reads on the return (body 0x00AA): no error, not a
            # screen, and DE -- the program end it stores -- left as it was
            ps.check_flags(self, res)
            self.assertNotEqual(res["a"], 0xD5)
            self.assertEqual(res["de"], 0x5234)

    def test_a_long_call_is_a_wait_not_a_resync(self):
        # a 0xCD call that keeps the Primo away ~20 ms (PCALL's PRINT, when it
        # scrolls the screen, is like this): the drive's idle resets meanwhile
        # are call_waits; `resyncs` stays a fault counter
        delay = bytes([0x01, 0xD0, 0x07,           # LD BC,2000
                       0x0B, 0x78, 0xB1, 0x20, 0xFB,  # DEC BC; LD A,B; OR C; JR NZ
                       0xC9])
        prog = ps.pattern(300, 4)
        f = (ps.rec(0xD9, 0x9800, delay) + bytes([0xCD, 0x00, 0x98])
             + ps.rec(0xD1, 0, prog) + bytes([0xC9]))
        primo, res = c_load(kind=self.kind, file=f, name=b"W")
        ps.check_flags(self, res)
        self.assertEqual(primo.peek(ps.BASE, 300), prog)
        st = stats(primo)
        self.assertGreater(st["call_waits"], 0)     # the sim batches the idle time: 1 here
        self.assertEqual(st["resyncs"], 0)

    def test_turbo_off_falls_back(self):
        primo = CPrimo(seed=1, kind=self.kind)
        primo.bridge = Bridge(primo, random.Random(1), file=self.file(), name=b"PROG")
        ctypes.c_uint8.in_dll(primo.bridge.lib, "primo_turbo_enabled").value = 0
        ps.name_at(primo, ps.NAME, b"PROG")
        res = primo.run(0x10, hl=ps.NAME, de=ps.BASE, bc=0x1F)
        if self.kind == "A":            # phase A falls back to CDOS, untouched
            self.assertEqual(primo.stock, [(ps.STUBS[0x10] + 2, ps.NAME, ps.BASE, 0x1F)])
        else:                           # ours has no fallback: the refusal is the error
            ps.check_flags(self, res, z=True)
            self.assertEqual(res["a"], 30)     # ERROR_SYNTAX_UNKNOWN

    def test_xt_plus_says_pfm_at_boot(self):
        """XT+'s answer carries PRIFLOP.CFG's PFM_AUTOSTART (primo_turbo.c): 00, OK,01,00 when it is ON."""
        primo = CPrimo(seed=1, kind=self.kind)
        b = Bridge(primo, random.Random(1), file=self.file(), name=b"PROG")
        pfm = ctypes.c_byte.in_dll(b.lib, "shim_pfm_autostart")
        for value, cmd, want in ((-1, b"XT+", "00, OK,00,00"), (0, b"XT+", "00, OK,00,00"),
                                 (1, b"XT+", "00, OK,01,00"), (1, b"XT-", "00, OK,00,00")):
            with self.subTest(value=value, cmd=cmd):
                pfm.value = value
                self.assertEqual(b.command(cmd), want)
        pfm.value = -1

    def test_verify_counts(self):
        if self.kind == "B":
            self.skipTest("our own DOS has no verify")
        prog = ps.pattern(500, 6)
        primo = CPrimo(seed=1, kind=self.kind)
        primo.bridge = Bridge(primo, random.Random(1), file=ps.rec(0xD1, 0, prog) + bytes([0xC9]),
                              name=b"V")
        ps.name_at(primo, ps.NAME, b"V")
        changed = bytearray(prog)
        for i in (3, 77, 400):
            changed[i] ^= 0xFF
        primo.poke(ps.BASE, bytes(changed))
        res = primo.run(0x12, hl=ps.NAME, de=ps.BASE, bc=0x0E)
        ps.check_flags(self, res)
        self.assertEqual(res["a"], 3)

    def test_too_large(self):
        big = ps.rec(0xD9, 0xB000, ps.pattern(0x2000, 4)) + bytes([0xC9])
        primo, res = c_load(kind=self.kind, file=big, name=b"BIG")
        ps.check_flags(self, res, z=True)
        self.assertEqual(res["a"], 52)
        self.assertEqual(primo.peek(0xB000, 16), bytes(16))

    def test_call_and_autostart(self):
        f = (ps.rec(0xD9, 0x9800, bytes([0x3E, 0x5A, 0x32, 0x00, 0x99, 0xC9]))
             + bytes([0xCD, 0x00, 0x98])
             + ps.rec(0xD9, 0x9A00, bytes([0x3E, 0xA5, 0x32, 0x01, 0x99, 0x76]))
             + bytes([0xC3, 0x00, 0x9A]))
        primo, res = c_load(kind=self.kind, file=f, name=b"A")
        self.assertEqual(primo.peek(0x9900, 2), b"\x5a\xa5")

    def test_slow_primo_start(self):
        # the Primo reaches its "ready?" poll 5 ms late: the drive's idle
        # resets must not drop the ready pull meanwhile
        primo = CPrimo(seed=1, kind=self.kind)
        primo.bridge = Bridge(primo, random.Random(1), file=self.file(), name=b"PROG")
        ps.name_at(primo, ps.NAME, b"PROG")
        orig = primo.service

        def slow(m, a=None, orig=orig):
            orig(m, a)
            if m.a == 0 and primo.bridge.thread:           # just answered 00,TURBO
                primo.extra_ns += 5_000_000
        primo.service = slow
        res = primo.run(0x10, hl=ps.NAME, de=ps.BASE, bc=0x1F)
        ps.check_flags(self, res)
        self.assertEqual(primo.peek(ps.BASE, 700), self.prog)

    def test_heavy_noise_never_lies(self):
        prog = ps.pattern(900, 9)
        f = ps.rec(0xD1, 0, prog) + bytes([0xC9])
        for seed in range(int(os.environ.get("SOAK", "4"))):
            with self.subTest(seed=seed):
                primo, res = c_load(kind=self.kind, file=f, name=b"P", seed=seed, p_flip_primo=1 / 1500, p_spike=1 / 1500,
                                    bridge_kw={"p_flip": 1 / 1500})
                if not res["f"] & ps.F_C:
                    self.assertEqual(primo.peek(ps.BASE, 900), prog)
                primo, res, got, want = c_save(kind=self.kind, data=prog, seed=seed, p_flip_primo=1 / 1500,
                                               p_spike=1 / 1500, bridge_kw={"p_flip_sample": 1 / 1500})
                if not res["f"] & (ps.F_C | ps.F_Z):
                    self.assertEqual(got, want)


@unittest.skipIf(ps.OURS is None, "needs the z80 package and z88dk")
class TestCDriveOurs(TestCDrive):
    """TestCDrive through our own DOS: the turbo inside it (phase B, step B4)."""
    kind = "B"


if __name__ == "__main__":
    unittest.main()
