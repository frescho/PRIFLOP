#!/usr/bin/env python3
"""A standard IEC drive at bit level, and the real CDOS run against it.

    tools/external/venv-sim/bin/python tests/primo_dos_sim.py          # the tests
    tools/external/venv-sim/bin/python tests/primo_dos_sim.py margins  # CDOS's timing margins

Step B1: the reference that our own DOS (phase B) is compared
against. Before the model judges anything, real CDOS has to work against it.

What runs, and how faithfully:

  * The Primo: the user's B64 ROM, and CDOS as boot80.prg brings it -- its own
    loader relocates it under BASIC's top -- executed by the `z80` package
    (cycle-exact) at 2.5 MHz. The ROM routines the boot calls are stubbed (print
    logs, CLEAR moves the stack, NEW returns, READY halts). Services are called
    with RST 28H through the ROM's own vector, as programs call them.
  * The bus: ATN, CLOCK and DATA as open-collector lines (primo_turbo_sim.Line):
    a pull wins at once, a release reads high `rise_ns` later. The Primo drives
    them through port 0x80 (bit 1 ATN, bit 5 CLOCK, bit 4 DATA; set = pulled)
    and reads CLOCK and DATA on port 0x40 (bits 5 and 4; set = high).
  * The drive: `IecDrive`, a coroutine that follows sd2iec's own state machine
    (src/iec.c: iec_mainloop, _iec_getc, _iec_putc, the listen and talk handlers)
    with its delays as parameters, and a small DOS behind it: files, the
    directory, channel 15 (commands and the error message), channels 0-14.
    The timing is PRIFLOP's (iec.c's delays). A 1541 is not modelled as a fixed
    point: its ROM's delays are not known here, and the published table leaves the drive's internal pauses open. Instead
    `margins` moves one delay at a time away from PRIFLOP's until CDOS fails.
    Those limits are the bar our DOS has to clear.

primo-library/ (boot80.prg) is fetched and git-ignored; without it, or without
the z80 package, the tests skip.
"""
import math
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from primo_turbo_sim import Line, u16, T_NS     # noqa: E402
import primo_turbo_sim as ps                    # noqa: E402

try:
    import z80
except ImportError:                     # pragma: no cover
    z80 = None

BOOT80 = os.path.join(ROOT, "primo-library", "bin", "cdos", "boot80.prg")
B64_ROM = os.path.join(ROOT, "tools", "external", "primo-rom", "cdos", "PR_B64.ROM")
HAVE_CDOS = z80 is not None and os.path.exists(BOOT80) and os.path.exists(B64_ROM)

OUTPORT, INPORT = 0x80, 0x40
O_ATN, O_CLK, O_DAT = 0x02, 0x20, 0x10
I_CLK, I_DAT = 0x20, 0x10
CDOS = 0xDC4B                           # CDOS's base after a boot on the B64
F_C, F_Z = 0x01, 0x40
US = 1000.0                             # ns


# ---------------------------------------------------------------- the drive

class AtnFall(Exception):
    """ATN was asserted: the drive's ATN interrupt."""


class AtnRise(Exception):
    """ATN was released while the drive took command bytes."""


PRIFLOP = dict(                         # src/iec.c, in microseconds
    name="PRIFLOP",
    atn_isr=(1, 3),                     # the ATN interrupt pulls DATA
    react=(2, 4),                       # a debounced read: two reads 2 us apart
    put_pre=60,                         # _iec_putc: "fudged delay" before releasing CLOCK
    put_clk=40,                         # CLOCK pulled, before the first bit
    put_pre_bits=21,
    bit_gap=45, t_s=22, t_v=75, bit_hold=22, bit_settle=14,
    eoi_hold=80,                        # T_EI as listener
    ack_delay=5, ack_after=50,          # after bit 8: pull DATA, then pause
    turn_rel=50, turn_clk=70,           # BUS_ATNPROCESS, talker: release DATA, pull CLOCK
    eoi_timeout=256,                    # listener: CLOCK not pulled this long = EOI
    after_byte=250,                     # _iec_putc: wait for DATA released, at most
)


def dos_message(code, text, a=0, b=0):
    return b"%02d,%s,%02d,%02d\r" % (code, text, a, b)


OK_MSG = dos_message(0, b" OK")


class Channel:
    def __init__(self, name, data=b"", write=False):
        self.name, self.data, self.write, self.pos = name, bytearray(data), write, 0


def cbm_match(pat, name):
    for i, c in enumerate(pat):
        if c == ord("*"):
            return True
        if i >= len(name) or (c != ord("?") and c != name[i]):
            return False
    return len(name) == len(pat)


class IecDrive:
    """One IEC device: sd2iec's bus state machine as a coroutine, and a DOS.

    The coroutine yields operations; `advance(until)` runs them up to the time
    the Primo next touches the bus, so every line change the drive reacts to is
    already in the bus history:

      ("set", line, pulled)       drive a line now
      ("read", line)              -> its level now
      ("delay", ns)
      ("wait", line, level, ns)   -> True when it reads `level`, False after `ns`
                                     (ns None: no timeout; line None: only ATN)
      ("wait_atn", line, level, ns)  the same, but ATN ends it (False) as a
                                     timeout would, instead of raising

    Every wait and delay also watches ATN, as the drive's interrupt and
    iec_check_atn() do: asserted, it raises AtnFall in the coroutine (after the
    interrupt pulled DATA); released while command bytes are being taken, it
    raises AtnRise."""

    def __init__(self, bus, files=None, dev=8, timing=PRIFLOP, rng=None, disk=b"PRIFLOP",
                 turbo=True):
        self.bus, self.dev, self.tm = bus, dev, timing
        # the Primo turbo (primo_turbo.c): True PRIFLOP with it on, False with
        # it off (`XT-`), None a drive without it (a 1541: no X commands)
        self.turbo = turbo
        self.armed, self.pio, self.sessions = 0, None, []
        self.files = {} if files is None else files
        self.disk = disk
        self.rng = rng or random.Random(1)
        self.t = 0.0
        self.under_atn = False
        self.error = OK_MSG
        self.channels = {}
        self.cmdbuf = bytearray()
        self.cmd_ready = False
        self.state, self.sa = "idle", 0
        self.log = []                   # (kind, bytes): what the drive saw, in order
        self.atns = 0
        self.gen = self.main()
        self.op = next(self.gen)

    # -- the scheduler

    def us(self, key):
        v = self.tm[key]
        if isinstance(v, tuple):
            return self.rng.uniform(*v) * US
        return v * US

    def advance(self, until):
        while True:
            op = self.op
            kind = op[0]
            if kind == "set":
                self.bus[op[1]].set("drive", self.t, op[2])
                self.op = self.gen.send(None)
                continue
            if kind == "read":
                self.op = self.gen.send(self.bus[op[1]].level(self.t))
                continue
            if kind == "turbo":
                if not self.session_step(until):
                    return
                self.op = self.gen.send(None)
                continue
            if kind == "delay":
                end, cond = self.t + op[1], None
            else:
                _, line, level, timeout = op
                end = math.inf if timeout is None else self.t + timeout
                cond = None
                if line is not None:
                    cond = self.bus[line].next_level(self.t, level, min(end, until))
            limit = min(until, end if cond is None else cond + 1e-6)
            atn_t = self.bus["atn"].next_level(self.t, 1 if self.under_atn else 0, limit)
            if atn_t is not None and (cond is None or atn_t <= cond):
                if kind == "wait_atn":                  # a wait that ATN only ends
                    self.t = atn_t + self.us("react")
                    self.op = self.gen.send(False)
                elif self.under_atn:
                    self.t = atn_t + self.us("react")
                    self.op = self.gen.throw(AtnRise())
                else:
                    self.atns += 1
                    t_isr = atn_t + self.us("atn_isr")
                    self.bus["data"].set("drive", t_isr, True)
                    self.t = t_isr + self.us("react")
                    self.op = self.gen.throw(AtnFall())
                continue
            if cond is not None:
                self.t = cond + self.us("react")
                self.op = self.gen.send(True)
            elif end <= until:
                self.t = end
                self.op = self.gen.send(None if kind == "delay" else False)
            else:
                return

    def session_step(self, until):
        """The turbo session (primo_turbo.c's session()): the PIO model and the
        drive's brain (primo_turbo_sim) run until the brain has finished, or
        ATN comes. True when it is over; self.t is then its end."""
        if self.pio is None:
            self.pio = ps.Pio(self.bus, self.tdrive, self.rng)
            self.pio.t = self.pio.last_edge = self.t
        atn_t = self.bus["atn"].next_level(self.t, 0, until)
        self.pio.advance(until if atn_t is None else atn_t)
        if self.pio.done:
            end = self.pio.t
        elif atn_t is not None:
            end = atn_t                                 # ATN ends it: DATA let go
            self.bus["data"].set("drive", end, False)
        else:
            return False
        self.sessions.append(("left" if self.tdrive.left else "aborted", self.tdrive.counters))
        self.error = self.tdrive.error.encode() + b"\r"
        self.pio = None
        self.t = max(self.t, end)
        return True

    def poll(self):
        """primo_turbo_poll() at each BUS_CLEANUP: armed by `XT`, the session
        starts after the UNTALK that ends the read of its answer."""
        last = next((e[1] for e in reversed(self.log) if e[0] == "atn"), None)
        if self.armed == 1:
            self.armed = 2                              # the command's own transaction
        elif self.armed == 2 and last == 0x5F:
            self.armed = 0
            return True
        elif self.armed == 2 and last != 0x6F:
            self.armed = 0                              # not the read of the answer
        return False

    def session(self):
        """Wait for ATN and CLOCK released (20 ms at most), pull DATA: "in
        turbo". Then the PIO and the brain have the bus until the end."""
        if not (yield ("wait", "clk", 1, 20_000 * US)):
            self.sessions.append(("no start", None))
            return
        yield ("set", "data", True)
        self.log.append(("turbo", self.tdrive.idx))
        yield ("turbo",)

    # -- bus primitives (sd2iec's iec.c)

    def release_all(self):
        yield ("set", "clk", False)
        yield ("set", "data", False)

    def getc(self):
        """_iec_getc: one byte as listener -> (byte, eoi)."""
        yield ("wait", "clk", 1, None)                 # talker ready to send
        yield ("set", "data", False)                   # ready for data
        yield ("wait", "data", 1, None)
        eoi = False
        if not (yield ("wait", "clk", 0, self.tm["eoi_timeout"] * US)):
            eoi = True                                 # EOI: acknowledge it
            yield ("set", "data", True)
            yield ("delay", self.us("eoi_hold"))
            yield ("set", "data", False)
            yield ("wait", "clk", 0, None)
        val = 0
        for i in range(8):
            yield ("wait", "clk", 1, None)
            if (yield ("read", "data")):
                val |= 1 << i
            yield ("wait", "clk", 0, None)
        yield ("delay", self.us("ack_delay"))
        yield ("set", "data", True)                    # frame acknowledge
        yield ("delay", self.us("ack_after"))
        return val, eoi

    def putc(self, byte, eoi):
        """_iec_putc: one byte as talker. False: the listener went away."""
        first = yield ("read", "data")
        yield ("delay", self.us("put_pre"))
        yield ("set", "clk", False)                    # ready to send
        yield ("wait", "data", 1, None)                # listener ready for data
        if eoi or first:
            yield ("wait", "data", 1, None)
            yield ("wait", "data", 0, None)            # the EOI acknowledge
        elif self.tm.get("t_ne"):
            yield ("delay", self.us("t_ne"))           # a slow talker (T_NE)
        yield ("set", "clk", True)
        yield ("delay", self.us("put_clk"))
        yield ("wait", "data", 1, None)
        yield ("delay", self.us("put_pre_bits"))
        for i in range(8):
            if not (yield ("read", "data")):
                return False
            yield ("delay", self.us("bit_gap"))
            yield ("set", "data", not (byte >> i) & 1)
            yield ("delay", self.us("t_s"))
            yield ("set", "clk", False)
            yield ("delay", self.us("t_v"))
            yield ("set", "clk", True)
            yield ("delay", self.us("bit_hold"))
            yield ("set", "data", False)
            yield ("delay", self.us("bit_settle"))
        yield ("wait", "data", 0, None)                # frame acknowledge
        # "Wait for 250us or until DATA is high or ATN is low": ATN here does
        # not undo the byte, it was sent (iec.c, after tx_done)
        yield ("wait_atn", "data", 1, self.tm["after_byte"] * US)
        return True

    # -- the state machine (iec_mainloop)

    def main(self):
        while True:
            try:
                if self.state == "listen":
                    yield from self.listen()
                elif self.state == "talk":
                    yield from self.talk()
                self.state = "idle"
                yield from self.release_all()
                self.cleanup()
                if self.poll():
                    yield from self.session()
                yield ("wait", None, 0, None)          # until ATN
            except AtnFall:
                pass
            try:
                yield from self.attention()
            except AtnFall:                            # pragma: no cover
                raise AssertionError("ATN asserted twice")

    def attention(self):
        """BUS_FOUNDATN .. BUS_ATNFINISH: take command bytes until ATN is released.
        Raises nothing: AtnRise ends it."""
        self.under_atn = True
        self.state = "idle"
        try:
            yield ("set", "clk", False)
            yield ("set", "data", True)
            yield ("wait", "clk", 0, 250 * US)
            mine = True
            while True:
                if not mine:
                    yield ("wait", None, 1, None)      # until ATN is released
                b, _ = yield from self.getc()
                self.log.append(("atn", b))
                if b == 0x3F:
                    if self.state == "listen":
                        self.state = "idle"
                elif b == 0x5F:
                    if self.state == "talk":
                        self.state = "idle"
                elif b == 0x40 + self.dev:
                    self.state = "talk"
                elif b == 0x20 + self.dev:
                    self.state = "listen"
                elif b & 0x60 == 0x60:
                    self.sa = b & 0x0F
                    if b & 0xF0 == 0xE0:
                        self.close(self.sa)
                    elif b & 0xF0 == 0xF0:
                        self.cmdbuf = bytearray()
                        self.opening = True
                    else:
                        self.opening = False
                else:                                  # BUS_NOTFORME
                    self.state = "idle"
                    mine = False
                    yield from self.release_all()
        except AtnRise:
            pass
        self.under_atn = False

    def listen(self):
        """iec_listen_handler: bytes until the next ATN."""
        cmdlike = self.sa == 15 or getattr(self, "opening", False)
        ch = self.channels.get(self.sa)
        if not cmdlike and (ch is None or not ch.write):
            return
        if cmdlike and self.sa == 15 and not getattr(self, "opening", False):
            self.cmdbuf = bytearray()
        while True:
            b, eoi = yield from self.getc()
            if cmdlike:
                self.cmdbuf.append(b)
                if eoi:
                    self.cmd_ready = True
            else:
                ch.data.append(b)

    def talk(self):
        """BUS_ATNPROCESS as talker: the turnaround, then iec_talk_handler."""
        yield ("set", "data", False)
        yield ("delay", self.us("turn_rel"))
        yield ("set", "clk", True)
        yield ("delay", self.us("turn_clk"))
        if self.sa == 15:
            data, self.error = self.error, OK_MSG
            ch = Channel(b"", data)
        else:
            ch = self.channels.get(self.sa)
            if ch is None or ch.write:
                return
        sent = bytearray()
        while ch.pos < len(ch.data):
            b = ch.data[ch.pos]
            last = ch.pos == len(ch.data) - 1
            if not (yield from self.putc(b, last)):
                break
            ch.pos += 1
            sent.append(b)
        self.log.append(("talk", self.sa, bytes(sent)))

    # -- the DOS behind it

    def cleanup(self):
        """BUS_CLEANUP: a name or command that came with EOI is acted on."""
        if not self.cmd_ready:
            return
        self.cmd_ready = False
        text = bytes(self.cmdbuf)
        self.cmdbuf = bytearray()
        if getattr(self, "opening", False) and self.sa != 15:
            self.opening = False
            self.log.append(("open", self.sa, text))
            self.open(self.sa, text)
        else:
            self.opening = False
            self.log.append(("cmd", text))
            self.command(text.rstrip(b"\r"))

    def open(self, sa, name):
        self.error = OK_MSG
        mode = b"W" if sa == 1 else b"R"
        parts = name.split(b",")
        name = parts[0]
        for p in parts[1:]:
            if p[:1] in (b"W", b"R", b"A"):
                mode = p[:1]
        replace = name.startswith(b"@")
        if sa != 1 and name.startswith(b"$") and mode == b"R":       # "$" or "$:pattern"
            pat = name.split(b":", 1)[1] if b":" in name else b"*"
            self.channels[sa] = Channel(name, self.directory(pat))
            return
        if b":" in name:
            name = name.split(b":", 1)[1]
        if mode == b"W":
            if name in self.files and not replace:
                self.error = dos_message(63, b"FILE EXISTS")
                return
            self.channels[sa] = Channel(name, b"", write=True)
            return
        if mode == b"A":
            if name not in self.files:
                self.error = dos_message(62, b"FILE NOT FOUND")
                return
            self.channels[sa] = Channel(name, self.files[name], write=True)
            return
        if name not in self.files:
            self.error = dos_message(62, b"FILE NOT FOUND")
            return
        self.channels[sa] = Channel(name, self.files[name])

    def close(self, sa):
        # iec.c: closing channel 15 closes every file (free_multiple_buffers)
        for s in (list(self.channels) if sa == 15 else [sa]):
            ch = self.channels.pop(s, None)
            if ch is not None and ch.write:
                self.files[ch.name] = bytes(ch.data)

    def command(self, text):
        if text[:1] == b"S" and b":" in text:
            names = text.split(b":", 1)[1].split(b",")
            n = sum(1 for x in names if self.files.pop(x, None) is not None)
            self.error = dos_message(1, b" FILES SCRATCHED", n)
        elif text[:1] == b"N":
            self.files.clear()
            self.error = OK_MSG
        elif text[:1] == b"I":
            self.error = OK_MSG
        elif text[:2] == b"XT" and self.turbo is not None:
            if text in (b"XT+", b"XT-"):
                self.turbo = text == b"XT+"
                self.error = OK_MSG
            elif self.turbo and len(text) == 10 and text[9:] == b".":
                self.tdrive = ps.Drive(self.files, self.rng, ps.FAST)
                self.tdrive.idx = text[2]
                self.error = self.tdrive.command(text).encode() + b"\r"
                self.armed = 1
            else:                                      # primo_turbo.c: ERROR_SYNTAX_UNKNOWN
                self.error = dos_message(30, b"SYNTAX ERROR")
        elif text[:2] in (b"UJ", b"UI", b"U:", b"U9"):
            self.error = dos_message(73, b"PRIFLOP MODEL")
        else:
            self.error = dos_message(31, b"SYNTAX ERROR")

    def directory(self, pat=b"*"):
        """A 1541-style listing: a BASIC program loaded at 0x0401. `pat`: the
        names listed, the CBM way ('?' any one character, '*' the rest)."""
        lines = [(0, b"\x12\"" + self.disk.ljust(16) + b"\" 01 2A")]
        for name, data in self.files.items():
            if not cbm_match(pat, name):
                continue
            blocks = max(1, (len(data) + 253) // 254)
            lines.append((blocks, b"   "[len(str(blocks)) - 1:] + b"\"" + name + b"\"" +
                          b" " * (17 - len(name)) + b"PRG"))
        used = sum(max(1, (len(d) + 253) // 254) for d in self.files.values())
        lines.append((664 - used, b"BLOCKS FREE."))
        out = bytearray(b"\x01\x04")
        addr = 0x0401
        for num, text in lines:
            addr += 4 + len(text) + 1
            out += bytes([addr & 0xFF, addr >> 8, num & 0xFF, num >> 8]) + text + b"\x00"
        return bytes(out + b"\x00\x00")


# ---------------------------------------------------------------- the Primo

class CdosPrimo:
    """The B64 with CDOS booted by its own loader, and a drive on the bus.
    One Z80 for the whole test, so the drive's clock runs on across calls."""

    DRIVER = 0x6E00
    RES = 0x6F00
    SP = 0xBF00
    outport = OUTPORT                                   # the port the lines are written to

    def __init__(self, files=None, timing=PRIFLOP, rise_ns=1000.0, seed=1, dev=8, drive_dev=8):
        self.bus = {k: Line(rise_ns) for k in ("atn", "clk", "data")}
        self.rng = random.Random(seed)
        self.drive = IecDrive(self.bus, files, drive_dev, timing, self.rng)
        self.texts = []
        self.outs = []                                  # (ns, value): every write to the port
        self.base = 0
        self.ticks0 = 0
        self.m = m = z80.Z80Machine()
        m.set_memory_block(0, bytes(cdos_boot_memory()))
        m.set_output_callback(self.out_cb)
        m.set_input_callback(self.in_cb)
        self.install()
        if dev != 8:
            self.call(0x06, bc=dev)

    def install(self):
        """CDOS, by its own loader."""
        m = self.m
        m.pc, m.sp, m.ix = 0x4500, 0x4400, 0x4042
        self.run_until_halt()
        assert m.pc in (0x1A19, 0x1A1A), f"the boot stopped at {m.pc:#06x}, not READY"
        assert u16(m.memory[0x4174:0x4176]) == CDOS, "CDOS is not where the B64 puts it"
        self.status_at = CDOS + 0x0B87
        self.errbuf = CDOS + 0x0B96

    def now(self):
        return (self.base + self.ticks0 - self.m.ticks_to_stop) * T_NS

    def out_cb(self, port, value):
        p = port & 0xFF
        if p == 0xF8:                                   # the ROM's print (stub)
            hl = self.m.hl
            mem = self.m.memory
            end = hl
            while mem[end]:
                end += 1
            self.texts.append(bytes(mem[hl:end]))
            return
        if p != self.outport:
            return
        t = self.now()
        self.drive.advance(t)
        self.outs.append((t, value))
        self.bus["atn"].set("primo", t, bool(value & O_ATN))
        self.bus["clk"].set("primo", t, bool(value & O_CLK))
        self.bus["data"].set("primo", t, bool(value & O_DAT))

    def in_cb(self, port):
        p = port & 0xFF
        if p == 0x00:
            return 0x00                                 # the Reset button is not pressed
        if p != INPORT:
            return 0xFF
        t = self.now()
        self.drive.advance(t)
        v = 0xFF
        if not self.bus["data"].level(t):
            v &= ~I_DAT
        if not self.bus["clk"].level(t):
            v &= ~I_CLK
        return v

    def run_until_halt(self, limit_t=50_000_000):     # 20 s at 2.5 MHz
        m = self.m
        start = self.base
        while not m.halted:
            self.ticks0 = 100_000
            m.ticks_to_stop = self.ticks0
            m.run()
            self.base += self.ticks0 - m.ticks_to_stop
            self.ticks0 = 0
            m.ticks_to_stop = 0
            if self.base - start > limit_t:
                raise RuntimeError(f"ran away at {m.pc:#06x}")

    def idle(self, us):
        """Time passes with the Primo at READY: the drive goes on."""
        self.base += int(us * US / T_NS)
        self.drive.advance(self.now())

    def call(self, idx, hl=0, de=0, bc=0, f=0):
        """RST 28H with A = idx -> the registers and flags it returns."""
        m = self.m
        r = self.RES
        drv = bytes([0xEF,                                     # RST 28H
                     0x22, r & 0xFF, r >> 8,                   # LD (RES),HL
                     0xED, 0x53, (r + 2) & 0xFF, r >> 8,       # LD (RES+2),DE
                     0xED, 0x43, (r + 4) & 0xFF, r >> 8,       # LD (RES+4),BC
                     0xF5, 0xE1,                               # PUSH AF ; POP HL
                     0x22, (r + 6) & 0xFF, r >> 8,             # LD (RES+6),HL
                     0x76])
        m.set_memory_block(self.DRIVER, drv)
        m.halted = False
        m.iff1 = m.iff2 = 0
        m.a, m.hl, m.de, m.bc = idx, hl, de, bc
        m.f = f
        m.pc, m.sp, m.ix = self.DRIVER, self.SP, 0x4042
        self.run_until_halt()
        mem = m.memory
        af = u16(mem[r + 6:r + 8])
        return {"hl": u16(mem[r:r + 2]), "de": u16(mem[r + 2:r + 4]),
                "bc": u16(mem[r + 4:r + 6]), "a": af >> 8, "f": af & 0xFF,
                "status": mem[self.status_at], "iff": m.iff1}

    def poke(self, addr, data):
        self.m.set_memory_block(addr, bytes(data))

    def cstr(self, addr):
        mem = self.m.memory
        end = addr
        while mem[end] and end - addr < 80:
            end += 1
        return bytes(mem[addr:end])

    def peek(self, addr, n):
        return bytes(self.m.memory[addr:addr + n])


def cdos_boot_memory():
    """A B64 at the moment the boot ROM jumps to 0x4500 with boot80.prg loaded:
    the real ROM, with the four routines the loader calls stubbed."""
    mem = bytearray(65536)
    mem[0:0x4000] = open(B64_ROM, "rb").read()
    mem[0x2B75:0x2B78] = bytes([0xD3, 0xF8, 0xC9])          # print: OUT (0F8h),A ; RET
    mem[0x1B4D] = 0xC9                                       # NEW: RET
    # CLEAR moves the stack below the new top, as the ROM's does:
    # POP BC ; LD HL,(40B1h) ; LD DE,-64 ; ADD HL,DE ; LD SP,HL ; PUSH BC ; RET
    mem[0x1E83:0x1E8E] = bytes([0xC1, 0x2A, 0xB1, 0x40, 0x11, 0xC0, 0xFF, 0x19, 0xF9, 0xC5, 0xC9])
    mem[0x1A19] = 0x76                                       # READY: HALT
    mem[0x40B1:0x40B3] = b"\xff\xe7"
    # BASIC's start-up writes the JP opcodes of its vectors; CDOS writes only
    # the operands (RST 28H, CMD, the error hook, end of PRINT, output)
    for a in (0x400C, 0x4173, 0x41A6, 0x41BE, 0x41C1):
        mem[a] = 0xC3
    mem[0x4174:0x4176] = b"\x97\x19"
    mem[0x403B] = 0x80
    mem[0x404A] = 0xE8
    f = open(BOOT80, "rb").read()
    mem[0x4500:0x4500 + len(f)] = f
    return mem


class OurPrimo(CdosPrimo):
    """The same B64 and drive with our DOS (tools/primo/dos/dos.asm) in place
    of CDOS: relocated under BASIC's top and hooked into RST 28H, as the
    loader (step B7) will do."""

    dos = None                          # (code, syms, relocs) if not the built one

    def install(self):
        code, syms, relocs = self.dos or DOS
        m = self.m
        base = 0xE7FF - len(code)
        self.dos_base, self.syms = base, syms
        img = bytearray(build_dos.relocate(code, relocs, base))
        img[syms["outport"]] = OUTPORT
        m.set_memory_block(base, bytes(img))
        m.set_memory_block(0x400D, bytes([(base + syms["svc"]) & 0xFF, (base + syms["svc"]) >> 8]))
        m.set_memory_block(0x4174, bytes([base & 0xFF, base >> 8]))     # CMD: the drive's anchor
        m.set_memory_block(0x40B1, bytes([(base - 1) & 0xFF, (base - 1) >> 8]))
        self.status_at = base + syms["status"]
        self.errbuf = base + syms["errbuf"]


sys.path.insert(0, os.path.join(ROOT, "tools", "primo", "dos"))
import build_dos  # noqa: E402
DOS = None
if z80 is not None and os.path.exists(build_dos.Z80ASM):
    DOS = build_dos.body()


# ---------------------------------------------------------------- helpers

def pattern(n, seed=7):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(n))


def rec(t, addr, data):
    return bytes([t, addr & 0xFF, addr >> 8, len(data) & 0xFF, len(data) >> 8]) + data


NAME = 0x7000
BASE = 0x4300
DESC = 0x7100


def name_at(p, addr, name):
    p.poke(addr, bytes([len(name)]) + name)
    return addr


def ok(res):
    return not res["f"] & (F_C | F_Z)


# ---------------------------------------------------------------- the tests

@unittest.skipUnless(HAVE_CDOS, "needs the z80 package, primo-library/ and the B64 ROM")
class TestCdosAgainstTheModel(unittest.TestCase):
    """Real CDOS against the model: if CDOS fails here, the model is wrong."""

    timing = PRIFLOP

    def primo(self, files=None, **kw):
        kw.setdefault("timing", self.timing)
        return CdosPrimo(files, **kw)

    def load(self, p, name, de=BASE, mask=0x1F, limit=0xC000):
        p.call(0x02, hl=limit, de=0)
        name_at(p, NAME, name)
        return p.call(0x10, hl=NAME, de=de, bc=mask)

    def test_the_boot_leaves_cdos_and_its_banner(self):
        p = self.primo()
        self.assertTrue(p.texts[0].startswith(b"\x0cCommodore Serial Bus Handler"), p.texts)
        self.assertEqual(p.call(0x00)["a"], 0x10)
        self.assertEqual(p.call(0x08)["a"], 8)

    def test_error_channel(self):
        p = self.primo()
        res = p.call(0x1A)
        self.assertTrue(ok(res), res)
        # stored with the CR turned into the end mark
        self.assertEqual(p.peek(CDOS + 0x0B96, len(OK_MSG)), OK_MSG[:-1] + b"\x00")

    def test_command_and_its_answer(self):
        p = self.primo({b"A": b"x", b"B": b"y"})
        name_at(p, NAME, b"S:A")
        res = p.call(0x18, hl=NAME)
        self.assertTrue(ok(res), res)                 # 01 counts as OK
        self.assertEqual(res["a"], 1)
        self.assertEqual(set(p.drive.files), {b"B"})
        name_at(p, NAME, b"BOGUS")
        res = p.call(0x18, hl=NAME)
        self.assertTrue(res["f"] & F_Z and res["a"] == 31, res)

    def test_load_basic_and_code_records(self):
        prog, code = pattern(700, 1), pattern(300, 2)
        f = rec(0xD1, 0, prog) + rec(0xD9, 0x8000, code) + b"\xc9"
        p = self.primo({b"PROG": f})
        res = self.load(p, b"PROG")
        self.assertTrue(ok(res), res)
        self.assertEqual(p.peek(BASE, 700), prog)
        self.assertEqual(p.peek(0x8000, 300), code)
        self.assertEqual(res["a"], 0xD9)                # the last record type
        self.assertEqual(res["de"], 0x8000 + 300)       # the end of the last record

    def test_load_of_a_missing_file_is_error_62(self):
        p = self.primo({})
        res = self.load(p, b"NONE")
        self.assertTrue(res["f"] & F_Z and res["a"] == 62, res)

    def test_verify_counts_differences(self):
        prog = pattern(500, 3)
        p = self.primo({b"P": rec(0xD1, 0, prog) + b"\xc9"})
        p.poke(BASE, prog)
        p.call(0x02, hl=0xC000, de=0)
        name_at(p, NAME, b"P")
        res = p.call(0x12, hl=NAME, de=BASE, bc=0x0E)
        self.assertTrue(ok(res), res)
        self.assertEqual(res["a"], 0)
        p.poke(BASE + 10, b"\x00\x01\x02")
        p.poke(BASE + 10, bytes(x ^ 0xFF for x in prog[10:13]))
        res = p.call(0x12, hl=NAME, de=BASE, bc=0x0E)
        self.assertEqual(res["a"], 3, res)

    def test_save_writes_records(self):
        prog = pattern(600, 4)
        p = self.primo({})
        p.poke(BASE, prog)
        # the descriptor: type, 4 header bytes, source pointer, then the end
        p.poke(DESC, bytes([0xD1, 0, 0, len(prog) & 0xFF, len(prog) >> 8,
                            BASE & 0xFF, BASE >> 8, 0xC9]))
        name_at(p, NAME, b"S")
        res = p.call(0x0E, hl=NAME, de=DESC)
        self.assertTrue(ok(res), res)
        self.assertEqual(p.drive.files.get(b"S"), rec(0xD1, 0, prog) + b"\xc9")
        res = p.call(0x0E, hl=NAME, de=DESC)            # again: the drive refuses
        self.assertTrue(res["f"] & F_Z and res["a"] == 63, res)

    def test_directory_through_the_handler(self):
        p = self.primo({b"ONE": b"1" * 300, b"TWO": b"2"})
        want = p.drive.directory()
        # CDOS's own raw handler takes the first byte as a 0xDD record (§9);
        # set a handler that is CDOS's: (0x4174)+0x04E9
        p.call(0x02, hl=0xC000, de=CDOS + 0x04E9)
        name_at(p, NAME, b"$")
        res = p.call(0x10, hl=NAME, de=0x8000, bc=0x00)
        # the handler drops the load address; the file ends inside its "record",
        # which the load reports as 0x63 with Z -- CMD $ takes that as the end
        self.assertTrue(res["f"] & F_Z and res["a"] == 0x63, res)
        self.assertEqual(res["de"], 0x8000 + len(want) - 2)
        self.assertEqual(p.peek(0x8000, len(want) - 2), want[2:])

    def test_byte_channel_write_then_read(self):
        p = self.primo({})
        name_at(p, NAME, b"SEQ,S,W")
        self.assertTrue(ok(p.call(0x14, hl=NAME, bc=2)))
        text = b"HELLO PRIMO\r12345\r"
        p.call(0x20, bc=8)                              # LISTEN 8
        p.call(0x24, bc=2)                              # secondary 2
        for c in text:
            p.call(0x1E, bc=c)
        p.call(0x28)                                    # UNLISTEN: the last goes with EOI
        self.assertTrue(ok(p.call(0x16, bc=2)))         # CLOSE 2
        self.assertEqual(p.drive.files.get(b"SEQ"), text)
        name_at(p, NAME, b"SEQ,S,R")
        self.assertTrue(ok(p.call(0x14, hl=NAME, bc=3)))
        p.call(0x22, bc=8)                              # TALK 8
        p.call(0x26, bc=3)                              # secondary 3, turnaround
        got = bytearray()
        for _ in range(len(text)):
            r = p.call(0x1C)
            got.append(r["a"])
        p.call(0x2A)                                    # UNTALK
        self.assertEqual(bytes(got), text)
        self.assertTrue(p.peek(CDOS + 0x0B87, 1)[0] & 0x40)    # EOI on the last
        self.assertTrue(ok(p.call(0x16, bc=3)))

    def test_no_drive_is_device_not_present(self):
        p = self.primo({}, drive_dev=9)                 # nobody answers as 8
        name_at(p, NAME, b"X")
        res = p.call(0x14, hl=NAME, bc=2)
        self.assertTrue(res["f"] & F_C, res)
        self.assertTrue(res["status"] & 0x80, res)


# ---------------------------------------------------------------- CDOS against ours

# What each service returns besides the registers it keeps;
# anything else CDOS leaves in a register is its own business. "*HL": HL
# points at the same text (the error message, in each DOS's own buffer).
_MSG = ("A", "Z", "*HL")
OUTPUTS = {0x00: ("A", "Z", "*HL"), 0x02: ("Z",), 0x04: ("HL", "DE", "Z"), 0x06: ("Z",),
           0x08: ("A", "Z"), 0x0A: ("Z",), 0x0C: ("A", "Z"),
           0x0E: _MSG, 0x10: ("A", "Z", "DE"),
           0x14: _MSG, 0x16: _MSG, 0x18: _MSG, 0x1A: _MSG,
           0x1C: ("A",), 0x1E: ("A",)}
# CDOS's load and save return BC = 0. With mask 0 (CMD $) that looks as if C
# were kept; ours leaves BC undefined there ("What may differ").
UNDEFINED = {0x0E: ("B", "C"), 0x10: ("B", "C")}
REGS = (("A", lambda x: x["a"]), ("B", lambda x: x["bc"] >> 8), ("C", lambda x: x["bc"] & 0xFF),
        ("DE", lambda x: x["de"]), ("HL", lambda x: x["hl"]))

JUNK = ({"hl": 0x1357, "de": 0x2468, "bc": 0x9ABC, "f": 0x00},
        {"hl": 0xFDB9, "de": 0xECA8, "bc": 0x7531, "f": 0xFF})


class Twin:
    """CDOS twice (with different values in the registers a call does not
    use) and ours once, on identical drives: every call goes to all three,
    and ours must agree with CDOS wherever CDOS's own result does not depend
    on those values.

    Registers: one CDOS keeps, ours keeps (B, C and DE excepted after a bus
    error, carry set, and those in UNDEFINED); of the rest, only what the
    service returns (OUTPUTS) must agree. Flags: C always, Z where it means something. Also compared after each call: the status byte, the interrupt
    flip-flop, memory outside the DOS and the stack, and the drive -- what it
    saw on the bus, its files and its error message."""

    def __init__(self, files=None, dos=None, same_bus=True, **kw):
        files = files or {}
        self.same_bus = same_bus        # False: ours goes through the turbo, CDOS
                                        # on the standard bus; the drive's log
                                        # then differs, its files and message not
        self.c = [CdosPrimo(dict(files), **kw), CdosPrimo(dict(files), **kw)]
        ours = type("Ours", (OurPrimo,), {"dos": dos}) if dos else OurPrimo
        self.o = ours(dict(files), **kw)
        for m in self.c + [self.o]:
            m.snap = bytes(m.m.memory)
        self.calls = 0

    def call(self, idx, **inputs):
        """inputs: hl, de, bc (16-bit) or c (just C, B is junk)."""
        res = []
        for m, junk in ((self.c[0], JUNK[0]), (self.c[1], JUNK[1]), (self.o, JUNK[0])):
            regs = dict(junk)
            for k, v in inputs.items():
                if k == "c":
                    regs["bc"] = regs["bc"] & 0xFF00 | v
                else:
                    regs[k] = v
            res.append((regs, m.call(idx, **regs)))
        self.calls += 1
        (i0, c0), (i1, c1), (io, o) = res
        where = f"call {self.calls}, service {idx:#04x}"
        outs = OUTPUTS.get(idx, ())
        for name, get in REGS:
            ins = (get(dict(i0, a=idx)), get(dict(i1, a=idx)))
            if (get(c0), get(c1)) == ins:
                if c0["f"] & F_C and name in ("B", "C", "DE"):
                    continue            # a bus error: BC, DE are not part of what it says
                if name in UNDEFINED.get(idx, ()):
                    continue
                want = get(dict(io, a=idx))
                assert get(o) == want, f"{where}: CDOS keeps {name}, ours made it {get(o):#x} (was {want:#x})"
            elif name in outs and not (c0["f"] & F_C and name == "DE"):
                assert get(o) == get(c0), f"{where}: {name} is {get(o):#x}, CDOS gives {get(c0):#x}"
        if "*HL" in outs:
            assert self.o.cstr(o["hl"]) == self.c[0].cstr(c0["hl"]), f"{where}: HL points elsewhere"
        for flag, bit in (("C", F_C), ("Z", F_Z)):
            if (c0["f"] & bit, c1["f"] & bit) == (i0["f"] & bit, i1["f"] & bit):
                assert o["f"] & bit == io["f"] & bit, f"{where}: CDOS keeps flag {flag}, ours does not"
                continue
            if flag == "Z" and "Z" not in outs:
                continue
            if c0["f"] & bit == c1["f"] & bit:
                assert o["f"] & bit == c0["f"] & bit, \
                    f"{where}: flag {flag} is {bool(o['f'] & bit)}, CDOS's {bool(c0['f'] & bit)}"
        assert o["status"] == c0["status"], f"{where}: status {o['status']:#x}, CDOS's {c0['status']:#x}"
        assert o["iff"] == c0["iff"], f"{where}: interrupts {'on' if o['iff'] else 'off'}, not as CDOS"
        wc, wo = self.writes(self.c[0]), self.writes(self.o)
        assert wo == wc, f"{where}: ours wrote {sorted(wo.items())[:6]}, CDOS {sorted(wc.items())[:6]}"
        dc, do = self.c[0].drive, self.o.drive
        if self.same_bus:
            assert do.log == dc.log, f"{where}: the drive saw {do.log[-4:]}, with CDOS {dc.log[-4:]}"
        assert do.files == dc.files and do.error == dc.error, f"{where}: the drive's state differs"
        return c0, o

    def poke(self, addr, data):
        for m in self.c + [self.o]:
            m.poke(addr, data)
            m.snap = bytes(m.m.memory)

    def writes(self, m):
        """{address: new value} for what the last call changed, outside the
        DOS itself, the stack and the driver."""
        now = bytes(m.m.memory)
        old = getattr(m, "snap", None) or now
        m.snap = now
        lo_dos = min(CDOS, getattr(m, "dos_base", CDOS))
        return {a: now[a] for a in range(0x4000, lo_dos)
                if now[a] != old[a] and not (0x6E00 <= a < 0x7000 or 0xBE00 <= a < 0xBF00)}

    def name(self, text, addr=NAME):
        self.poke(addr, bytes([len(text)]) + text)
        return addr


def talker_phases(outs):
    """From the port writes of a send: the shortest bit set-up (DATA set,
    CLOCK held), bit valid (CLOCK released) and hold (CLOCK pulled again,
    DATA still set), in microseconds."""
    ts, tv, th = [], [], []
    for k in range(1, len(outs) - 2):
        (t0, v0), (t1, v1), (t2, v2), (t3, v3) = outs[k - 1:k + 3]
        bit = v1 & O_DAT
        if (v0 & O_CLK and v1 & O_CLK == 0 and v2 & O_CLK and v2 & O_DAT == bit
                and v0 & O_DAT == bit and t2 - t1 < 200 * US):
            ts.append(t1 - t0)
            tv.append(t2 - t1)
            if v3 & O_DAT == 0 and bit:
                th.append(t3 - t2)
    return min(ts) / US, min(tv) / US, (min(th) / US if th else None)


def works(cls, timing, scenario="channel", seed=1):
    """Does the DOS (`cls`: CdosPrimo or OurPrimo) get `scenario` right?
      channel  the error channel, a command, a file written and read back
               through the byte channel (every service of step B2)
      load     a 300-byte load and save (ours: through the turbo, with `XT`
               and the error channel on the standard bus)
      dir      the directory as CMD $ loads it, raw (mask 0), on the
               standard bus (CDOS: through its own raw handler)
      absent   OPEN with no device 8 on the bus: device not present"""
    try:
        if scenario == "absent":
            p = cls({}, timing=timing, seed=seed, drive_dev=9)
            name_at(p, NAME, b"F")
            r = p.call(0x14, hl=NAME, bc=2)
            return bool(r["f"] & F_C and r["status"] & 0x80)
        if scenario == "dir":
            p = cls({b"ONE": b"1" * 300, b"TWO": b"2"}, timing=timing, seed=seed)
            want = p.drive.directory()
            p.call(0x02, hl=0xC000, de=CDOS + 0x04E9)
            r = p.call(0x10, hl=name_at(p, NAME, b"$"), de=0x8000, bc=0)
            return bool(r["f"] & F_Z) and r["a"] == 0x63 and p.peek(0x8000, len(want) - 2) == want[2:]
        if scenario == "load":
            prog = pattern(300, seed)
            f = rec(0xD1, 0, prog) + b"\xc9"
            p = cls({b"P": f}, timing=timing, seed=seed)
            p.call(0x02, hl=0xC000, de=0)
            name_at(p, NAME, b"P")
            if not ok(p.call(0x10, hl=NAME, de=BASE, bc=0x1F)) or p.peek(BASE, 300) != prog:
                return False
            p.poke(DESC, bytes([0xD1, 0, 0, 300 & 0xFF, 300 >> 8, BASE & 0xFF, BASE >> 8, 0xC9]))
            name_at(p, NAME, b"S")
            return ok(p.call(0x0E, hl=NAME, de=DESC)) and p.drive.files.get(b"S") == f
        text = bytes(range(32, 96))
        p = cls({b"A": b"1"}, timing=timing, seed=seed)
        if not ok(p.call(0x1A)):
            return False
        name_at(p, NAME, b"S:A")
        if p.call(0x18, hl=NAME)["a"] != 1 or b"A" in p.drive.files:
            return False
        name_at(p, NAME, b"T,S,W")
        if not ok(p.call(0x14, hl=NAME, bc=2)):
            return False
        p.call(0x20, bc=8)
        p.call(0x24, bc=2)
        for c in text:
            if p.call(0x1E, bc=c)["f"] & F_C:
                return False
        p.call(0x28)
        p.call(0x16, bc=2)
        if p.drive.files.get(b"T") != text:
            return False
        name_at(p, NAME, b"T,S,R")
        p.call(0x14, hl=NAME, bc=3)
        p.call(0x22, bc=8)
        p.call(0x26, bc=3)
        got = bytes(p.call(0x1C)["a"] for _ in text)
        eoi = p.peek(p.status_at, 1)[0] == 0x40
        p.call(0x2A)
        return got == text and eoi and ok(p.call(0x16, bc=3))
    except RuntimeError:
        return False


# (delay, PRIFLOP's value, the far end, the scenario): one at a time, from PRIFLOP's
SWEEP = [
    ("t_v", 75, 0, "channel"),          # drive talks: CLOCK released, the bit valid
    ("t_s", 22, 0, "channel"),          # DATA set before CLOCK is released
    ("bit_hold", 22, 0, "channel"),     # CLOCK pulled, before DATA is released
    ("bit_gap", 45, 0, "channel"),      # between bits
    ("put_pre", 60, 0, "channel"),      # turnaround and between bytes: CLOCK held
    ("react", 4, 2000, "channel"),      # how late the drive sees a line change
    ("ack_delay", 5, 3000, "channel"),  # drive listens: bit 8 to its frame acknowledge
    ("atn_isr", 3, 3000, "channel"),    # ATN to DATA pulled
    ("ack_after", 50, 3000, "absent"),  # not addressed: DATA held after the acknowledge
    ("t_ne", 0, 3000, "channel"),       # drive talks: listener ready to the first CLOCK (EOI)
]
RANGED = ("react", "atn_isr")


def limit(cls, key, base, far, scenario):
    """The last value of `key`, going from `base` towards `far`, that works."""
    tm = dict(PRIFLOP)
    val = lambda v: (v, v) if key in RANGED else v      # noqa: E731
    tm[key] = val(base)
    if not works(cls, tm, scenario):
        return None
    tm[key] = val(far)
    if works(cls, tm, scenario):
        return far
    good, bad = base, far
    while abs(bad - good) > 1:
        mid = (good + bad) // 2
        tm[key] = val(mid)
        if works(cls, tm, scenario):
            good = mid
        else:
            bad = mid
    return good


def margins():
    """Each delay moved alone until the DOS fails: the last value that works,
    for CDOS and for ours. Ours must reach at least as far as CDOS."""
    print(f"{'delay':10} {'PRIFLOP':>8} {'CDOS':>8} {'ours':>8}")
    for key, base, far, scenario in SWEEP:
        c = limit(CdosPrimo, key, base, far, scenario)
        o = limit(OurPrimo, key, base, far, scenario)
        flag = "" if o is not None and c is not None and (o - c) * (far - base) >= 0 else "  <- worse"
        print(f"{key:10} {base:8} {str(c):>8} {str(o):>8}{flag}", flush=True)


@unittest.skipUnless(HAVE_CDOS and DOS is not None, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestOursAgainstCdos(unittest.TestCase):
    """Step B2: every service call made to CDOS and to ours, compared."""

    def test_the_simple_services(self):
        t = Twin()
        t.call(0x00)
        t.call(0x02, hl=0xA000, de=0xB000)
        t.call(0x04)
        for idx, c in ((0x06, 9), (0x08, 0), (0x06, 8), (0x08, 0), (0x0A, 0x42), (0x0C, 0),
                       (0x0A, 0), (0x0C, 0)):
            t.call(idx, c=c)

    def test_0x00_points_at_a_zero_byte(self):
        t = Twin()
        c, o = t.call(0x00)
        self.assertEqual(t.o.peek(o["hl"], 1), t.c[0].peek(c["hl"], 1))

    def test_0x2c_is_not_provided(self):
        # the printer's ASCII/PETSCII service was dropped: C, as 0x12
        t = Twin()
        r = t.o.call(0x2C, bc=0x41)
        self.assertTrue(r["f"] & 1)

    def test_error_channel_and_commands(self):
        t = Twin({b"A": b"x", b"B": b"y"})
        t.call(0x1A)
        t.call(0x18, hl=t.name(b"S:A"))
        t.call(0x18, hl=t.name(b"BOGUS"))
        t.call(0x1A)
        t.call(0x18, hl=t.name(b"UJ"))
        t.call(0x18, hl=t.name(b""))

    def test_open_close(self):
        t = Twin({b"F": b"data"})
        for name, sa in ((b"F", 2), (b"NONE", 3), (b"N,S,W", 4), (b"", 5)):
            t.call(0x14, hl=t.name(name), c=sa)
            t.call(0x16, c=sa)
        t.call(0x16, c=0x13)            # bit 4 set: CDOS clears it

    def test_byte_channel(self):
        t = Twin()
        t.call(0x14, hl=t.name(b"SEQ,S,W"), c=2)
        t.call(0x20, c=8)
        t.call(0x24, c=2)
        for ch in b"HELLO\r1,2,3\r":
            t.call(0x1E, c=ch)
        t.call(0x28)
        t.call(0x16, c=2)
        t.call(0x14, hl=t.name(b"SEQ,S,R"), c=3)
        t.call(0x22, c=8)
        t.call(0x26, c=3)
        for _ in range(14):             # two past the end
            t.call(0x1C)
        t.call(0x2A)
        t.call(0x16, c=3)

    def test_listen_talk_drop_a_held_byte(self):
        t = Twin()
        t.call(0x14, hl=t.name(b"Q,S,W"), c=2)
        t.call(0x20, c=8)
        t.call(0x24, c=2)
        t.call(0x1E, c=0x41)
        t.call(0x1E, c=0x42)
        t.call(0x20, c=8)               # LISTEN again: 0x42 is dropped
        t.call(0x24, c=2)
        t.call(0x1E, c=0x43)
        t.call(0x28)
        t.call(0x16, c=2)

    MUTATIONS = [                       # each must be caught
        ("the received bit inverted", ("add a,0F0h", "add a,0E0h")),
        ("DE lost in the byte receive", ("s1c:\n    push bc\n    push de", "s1c:\n    push bc\n    push bc")),
        ("a status bit wrong", ("    or 40h\n", "    or 41h\n")),
        ("EOI never acknowledged", ("    or LDAT\n    call setl\n    call dly\n    ld a,(status)",
                                    "    nop\n    call setl\n    call dly\n    ld a,(status)")),
        ("one bit too few sent", ("    ld b,8\nsb2:", "    ld b,7\nsb2:")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                dos = build_dos.body(patch=(patch,))
                with self.assertRaises((AssertionError, RuntimeError)):
                    self.run_all(Twin({b"A": b"x"}, dos=dos))

    def run_all(self, t):
        """A short pass through every B2 service."""
        t.call(0x1A)
        t.call(0x18, hl=t.name(b"S:A"))
        t.call(0x14, hl=t.name(b"Q,S,W"), c=2)
        t.call(0x20, c=8)
        t.call(0x24, c=2)
        for ch in b"AB\x00\xff":
            t.call(0x1E, c=ch)
        t.call(0x28)
        t.call(0x16, c=2)
        t.call(0x14, hl=t.name(b"Q,S,R"), c=3)
        t.call(0x22, c=8)
        t.call(0x26, c=3)
        for _ in range(5):
            t.call(0x1C)
        t.call(0x2A)
        t.call(0x16, c=3)

    def test_the_short_pass_passes_unmutated(self):
        self.run_all(Twin({b"A": b"x"}))

    def test_our_bits_last_at_least_as_long_as_cdos(self):
        # when the Primo talks it sets the pace; a slow listener (a real 1541)
        # must get at least what CDOS gives it
        got = []
        for cls in (CdosPrimo, OurPrimo):
            p = cls({})
            name_at(p, NAME, b"S:" + bytes(range(0x41, 0x5B)))
            p.outs.clear()
            p.call(0x18, hl=NAME)
            got.append(talker_phases(p.outs))
        (cs, cv, ch), (os_, ov, oh) = got
        self.assertGreaterEqual(os_, cs)
        self.assertGreaterEqual(ov, cv)
        self.assertGreaterEqual(oh, ch)

    def test_nobody_there(self):
        t = Twin({b"F": b"x"}, drive_dev=9)
        t.call(0x1A)
        t.call(0x14, hl=t.name(b"F"), c=2)
        t.call(0x0C)
        t.call(0x0A, c=0)
        t.call(0x20, c=8)
        t.call(0x24, c=2)
        t.call(0x1E, c=1)
        t.call(0x28)
        t.call(0x22, c=8)
        t.call(0x26, c=2)
        t.call(0x2A)
        t.call(0x16, c=2)


HANDLER = 0x6C00
TARGET = 0xA000
H_FAIL = bytes([0x3E, 0x77, 0x37, 0xC9])       # LD A,77h ; SCF ; RET


def entry(t, a, n, src):
    """A save descriptor entry: type, address, length, the data's address."""
    return bytes([t, a & 0xFF, a >> 8, n & 0xFF, n >> 8, src & 0xFF, src >> 8])


@unittest.skipUnless(HAVE_CDOS and DOS is not None, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestPturboReadback(unittest.TestCase):
    """PTURBO's own verify (`rbv` in tools/primo/turbo/pturbo.asm), for our
    DOS, which has no service 0x12: the file read back on the standard bus
    through the services 0x14, 0x22, 0x26, 0x1C, 0x2A, 0x16, compared with
    memory. Under CDOS and ours alike, on the bit-level drive."""

    MEM = 0x9000

    def rig(self, cls, file, n, t=0xD1, name=b"PT0"):
        code, syms = ps.build_turbo.assemble("pturbo.asm")
        p = cls({name: file} if file is not None else {})
        p.poke(0x7000, code)
        p.poke(syms["vmem"], bytes([self.MEM & 0xFF, self.MEM >> 8]))
        p.poke(syms["vlen"], bytes([n & 0xFF, n >> 8]))
        p.poke(syms["vtype"], bytes([t]))
        return p, syms

    def rbv(self, p, syms):
        """CALL rbv with HL = PTURBO's "PT0" -> the registers, as call()."""
        r, a = p.RES, syms["rbv"]
        p.poke(p.DRIVER, bytes([0xCD, a & 0xFF, a >> 8,              # CALL rbv
                                0xF5, 0xE1, 0x22, (r + 6) & 0xFF, r >> 8, 0x76]))
        m = p.m
        m.halted, m.iff1, m.iff2 = False, 0, 0
        m.hl, m.pc, m.sp, m.ix = syms["n_load"], p.DRIVER, p.SP, 0x4042
        p.run_until_halt()
        af = u16(m.memory[r + 6:r + 8])
        return {"a": af >> 8, "f": af & 0xFF}

    def test_readback(self):
        d = pattern(700, 3)
        f = rec(0xD1, 0, d) + b"\xc9"
        for cls in (CdosPrimo, OurPrimo):
            with self.subTest(dos=cls.__name__):
                cases = [(f, d, 0), (f, d[:350] + bytes([d[350] ^ 1]) + d[351:], 1),
                         (f[:-1], d, 255),                               # no 0xC9
                         (f + b"\x00", d, 255),                          # a byte too many
                         (rec(0xD5, 0, d) + b"\xc9", d, 1)]               # the type
                for file, mem, want in cases:
                    p, syms = self.rig(cls, file, len(d))
                    p.poke(self.MEM, mem)
                    r = self.rbv(p, syms)
                    self.assertFalse(r["f"] & (F_C | F_Z), r)
                    self.assertEqual(r["a"], want)
                p, syms = self.rig(cls, None, 1)                          # no file: 62
                r = self.rbv(p, syms)
                self.assertTrue(r["f"] & F_Z and not r["f"] & F_C, r)
                self.assertEqual(r["a"], 62)


@unittest.skipUnless(HAVE_CDOS and DOS is not None, "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestTransfers(unittest.TestCase):
    """Step B4: load (0x10) and save (0x0E) through PRIFLOP's turbo, against
    CDOS's own on the standard bus. The drive's files and message, memory,
    the registers, flags and status must agree; the bus traffic cannot. A
    plain file (mask 0: CMD $) still goes over the standard bus, and agrees in
    that too. Where the turbo cannot agree -- the drive parses the records --
    test_where_the_turbo_differs pins what it does instead.
    Step B3's tests of the standard-bus load and save went with that code
    (git history)."""

    D = pattern(700, 1)
    C = pattern(300, 2)

    def twin(self, files, target=None, limit=0xC000, **kw):
        t = Twin(files, same_bus=False, **kw)
        if target:
            t.poke(TARGET, target)
        t.call(0x02, hl=limit, de=HANDLER)
        return t

    def load(self, t, name=b"F", base=BASE, mask=0x1F):
        return t.call(0x10, hl=t.name(name), de=base, c=mask)

    def test_records(self):
        d, c = self.D, self.C
        t = self.twin({b"F": rec(0xD1, 0, d) + rec(0xD9, 0x8000, c) + rec(0xD5, 0x40, c[:64]) + b"\xc9",
                       b"G": rec(0xD9, 0x8000, c) + rec(0xD1, 3, d[:9]) + b"\xc9"})
        self.load(t)
        self.assertEqual(t.o.peek(BASE, 700), d)
        self.load(t, b"G")
        self.load(t, b"G", base=0x9000)
        self.load(t, b"NONE")
        self.assertEqual([k for k, _ in t.o.drive.sessions], ["left"] * 4)

    def test_the_limit_and_the_screen(self):
        d = self.D
        t = self.twin({b"EDGE": rec(0xD9, 0x8000, d[:0x10]) + b"\xc9",
                       b"SCREEN": rec(0xD9, 0xE800, d[:0x10]) + b"\xc9"}, limit=0x8010)
        self.load(t, b"EDGE")
        self.load(t, b"SCREEN")

    def test_autostart_and_calls(self):
        d = self.D
        files = {b"AUTO": rec(0xD9, 0x8000, d[:9]) + b"\xc3\x00\xa0\xc9",
                 b"SCR": rec(0xD5, 0, d[:9]) + b"\xc3\x00\xa0" + rec(0xD9, 0x8000, d[:9]),
                 b"CALL": rec(0xD9, 0x8000, d[:9]) + b"\xcd\x00\xa0" + rec(0xD1, 0, d[:9]) + b"\xc9"}
        t = self.twin(files, target=b"\xc9")
        for name in files:
            self.load(t, name)
        t = self.twin(files, target=bytes([0x11, 0x34, 0x12, 0xC9]))
        self.load(t, b"AUTO")                   # DE comes back from the program

    def test_save(self):
        d = self.D
        t = self.twin({b"OLD": b"x"})
        t.poke(0x8000, d)
        for desc, name in ((entry(0xD1, 0, 600, 0x8000) + b"\xc9", b"A"),
                           (entry(0xD9, 0x1234, 3, 0x8000) + entry(0xD5, 0x10, 2, 0x8010) + b"\xc9", b"B"),
                           (entry(0xD9, 0x1234, 0, 0x8000) + b"\xc9", b"D"),       # length 0: nothing
                           (entry(0x55, 0x1234, 3, 0x8000) + b"\xc9", b"E"),
                           (entry(0xD1, 0, 5, 0x8000) + b"\xc9", b"OLD")):        # 63, FILE EXISTS
            t.poke(DESC, desc)
            t.call(0x0E, hl=t.name(name), de=DESC)
        self.assertEqual(t.o.drive.files[b"A"], rec(0xD1, 0, d[:600]) + b"\xc9")

    def test_save_then_load(self):
        d = self.D
        t = self.twin({})
        t.poke(0x8000, d)
        t.poke(DESC, entry(0xD9, 0x9000, 700, 0x8000) + b"\xc9")
        t.call(0x0E, hl=t.name(b"S"), de=DESC)
        self.load(t, b"S")
        self.assertEqual(t.o.peek(0x9000, 700), d)

    def test_nobody_there(self):
        t = Twin({}, drive_dev=9)
        t.call(0x10, hl=t.name(b"F"), de=BASE, c=0x1F)
        t.poke(DESC, b"\xc9")
        t.call(0x0E, hl=t.name(b"F"), de=DESC)

    def test_a_plain_file_on_the_standard_bus(self):
        # CMD $: mask 0, through CDOS's own raw handler (body 0x04E9) and our
        # own raw load; the same bus traffic and the same result
        files = {b"ONE": b"1" * 300, b"TWO": b"2", b"P": rec(0xD1, 0, self.D[:9]) + b"\xc9"}
        t = Twin(files)
        want = t.o.drive.directory()
        t.call(0x02, hl=0xC000, de=CDOS + 0x04E9)
        for name in (b"$", b"P", b"NONE"):
            t.call(0x10, hl=t.name(name), de=0x8000, c=0)
        t.call(0x10, hl=t.name(b"$"), de=0x8000, c=0)
        self.assertEqual(t.o.peek(0x8000, len(want) - 2), want[2:])
        t.call(0x02, hl=0x8010, de=CDOS + 0x04E9)
        t.call(0x10, hl=t.name(b"$"), de=0x8000, c=0)        # out of memory
        self.assertEqual(t.o.drive.sessions, [])

    def test_a_drive_without_the_turbo_refuses(self):
        # a 1541 (31, an unknown command), and PRIFLOP after `XT-` (30, its
        # ERROR_SYNTAX_UNKNOWN): an error, never a transfer
        for turbo, code in ((None, 31), (False, 30)):
            with self.subTest(turbo=turbo):
                p = OurPrimo({b"F": rec(0xD1, 0, self.D[:9]) + b"\xc9"})
                p.drive.turbo = turbo
                p.call(0x02, hl=0xC000, de=0)
                r = p.call(0x10, hl=name_at(p, NAME, b"F"), de=BASE, bc=0x1F)
                self.assertTrue(r["f"] & F_Z and not r["f"] & F_C, r)
                self.assertEqual((r["a"], r["de"]), (code, BASE))       # DE kept, as on any error
                self.assertEqual(p.cstr(r["hl"]), b"%d,SYNTAX ERROR,00,00" % code)
                self.assertEqual(p.peek(BASE, 9), bytes(9))
                p.poke(DESC, entry(0xD1, 0, 5, 0x8000) + b"\xc9")
                r = p.call(0x0E, hl=name_at(p, NAME, b"S"), de=DESC)
                self.assertTrue(r["f"] & F_Z and r["a"] == code, r)
                self.assertNotIn(b"S", p.drive.files)
                self.assertEqual([e for e in p.drive.log if e[0] in ("open", "turbo")], [])
                self.assertEqual(p.drive.sessions, [])

    def test_an_answer_that_is_not_the_turbos_refuses(self):
        # 00 or 01 without `,T`: Z with that code; no session
        for msg in (OK_MSG, dos_message(1, b" FILES SCRATCHED")):
            with self.subTest(msg=msg):
                p = OurPrimo({b"F": rec(0xD1, 0, self.D[:9]) + b"\xc9"})
                p.drive.command = lambda text, d=p.drive, msg=msg: setattr(d, "error", msg)
                r = p.call(0x10, hl=name_at(p, NAME, b"F"), de=BASE, bc=0x1F)
                self.assertTrue(r["f"] & F_Z and not r["f"] & F_C, r)
                self.assertEqual(r["a"], int(msg[:2]))
                self.assertEqual(p.drive.sessions, [])

    def test_where_the_turbo_differs(self):
        """: where the drive, parsing the records, does what
        CDOS's standard load does not. Pinned, so a change shows up here."""
        d = self.D
        p = OurPrimo({b"MID": rec(0xD1, 0, d)[:40],                 # ends inside a record
                      b"OM": rec(0xD9, 0x8000, d[:0x20]) + b"\xc9",
                      b"ODD": b"\x55" + rec(0xD9, 0x8000, d[:5])[1:] + rec(0xD9, 0x8100, d[:3]) + b"\xc9",
                      b"C9": b"\xc9",
                      b"D1C3": rec(0xD1, 0, d[:9]) + b"\xc3\x00\xa0",   # CDOS does not call it
                      b"REB": (rec(0xD9, TARGET, bytes([0x11, 0x00, 0x88, 0xC9]))   # LD DE,8800h
                               + b"\xcd\x00\xa0" + rec(0xD1, 0, d[:9]) + b"\xc9")})
        p.poke(HANDLER, H_FAIL)                         # never called: it would end in C

        def load(name, limit=0xC000):
            p.call(0x02, hl=limit, de=HANDLER)
            return p.call(0x10, hl=name_at(p, NAME, name), de=BASE, bc=0x1F)
        # a file that ends early: the drive's 99 with Z, the bytes that came
        # stored (CDOS: 0x63 = 99 inside a record, a bus error inside a
        # header). Until: no error, the last whole chunk the end.
        r = load(b"MID")
        self.assertTrue(r["f"] & F_Z and not r["f"] & F_C and r["a"] == 99, r)
        self.assertEqual(p.peek(BASE, 35), d[:35])
        # out of memory: the drive's 52 before any byte is stored (CDOS: 0xA8,
        # after storing up to the limit)
        r = load(b"OM", limit=0x8010)
        self.assertTrue(r["f"] & F_Z and r["a"] == 52, r)
        self.assertEqual(p.peek(0x8000, 0x10), bytes(0x10))
        # a type the drive does not know: skipped as a record with a 4-byte
        # header, and the handler is not called (CDOS calls it)
        r = load(b"ODD")
        self.assertTrue(ok(r), r)
        self.assertEqual(p.peek(0x8100, 3), d[:3])
        # "the last type" is this load's: 0 with no record (CDOS keeps the last
        # load's)
        self.assertEqual(load(b"C9")["a"], 0)
        # a 0xC3 right after a 0xD1 record is called, as after any other (B3's
        # corner: CDOS ends the load there)
        p.poke(TARGET, bytes([0x3E, 0x5A, 0x32, 0x00, 0xA1, 0xC9]))   # LD A,5Ah ; LD (0A100h),A
        self.assertTrue(ok(load(b"D1C3")))
        self.assertEqual(p.peek(0xA100, 1), b"\x5a")
        # the DE a 0xCD call returns is not the new base: the drive placed the
        # records already (CDOS takes it)
        r = load(b"REB")
        self.assertTrue(ok(r), r)
        self.assertEqual(p.peek(BASE, 9), d[:9])
        # save: the file ends at a 0xC3 entry, as CDOS's (until 
        # the drive read on, here into the 0xC9)
        p.poke(0x8000, d)
        p.poke(DESC, entry(0xD9, 0x1234, 3, 0x8000) + b"\xc3\x00\xa0\xc9")
        self.assertTrue(ok(p.call(0x0E, hl=name_at(p, NAME, b"S"), de=DESC)))
        self.assertEqual(p.drive.files[b"S"], rec(0xD9, 0x1234, d[:3]) + b"\xc3\x00\xa0")

    def test_verify_is_not_provided(self):
        p = OurPrimo({b"P": rec(0xD1, 0, b"x") + b"\xc9"})
        r = p.call(0x12, hl=name_at(p, NAME, b"P"), de=BASE, bc=0x0E)
        self.assertTrue(r["f"] & F_C, r)
        self.assertEqual(p.drive.log, [])               # nothing on the bus

    MUTATIONS = [                       # each must be caught
        ("the index not sent", ("    ld (xtbuf+3),a      ; the command",
                                "    nop\n    nop\n    nop                 ; the command")),
        ("the turbo's answer misread", ("    cp 'T'              ; \"00,T\": the turbo's\n    jr z,xgo",
                                        "    cp 'T'              ; \"00,T\": the turbo's\n    jr nz,xgo")),
        ("DE lost on an error", ("xerr:\n    pop de\n    pop hl\n", "xerr:\n    pop hl\n    pop hl\n")),
        ("no EOI in the status after a load", ("    ld a,40h\n    ld (status),a\n",
                                               "    ld a,00h\n    ld (status),a\n")),
        ("a plain file sent to the turbo", ("    jp z,raw ", "    nop\n    nop\n    nop ")),
        ("the raw load keeps the load address", ("    call rx             ; the load address, dropped\n    call rx\n",
                                                 "    call rx             ; the load address, dropped\n")),
        ("the raw limit one byte too high", ("    scf\n    sbc hl,de           ; C: the address",
                                             "    or a\n    sbc hl,de           ; C: the address")),
        ("the raw load misses EOI", ("    bit 6,a\n    jr z,raw1\n", "    bit 7,a\n    jr z,raw1\n")),
    ]

    def test_mutations_are_caught(self):
        for what, patch in self.MUTATIONS:
            with self.subTest(what):
                dos = build_dos.body(patch=(patch,))
                with self.assertRaises((AssertionError, RuntimeError)):
                    self.short_pass(dos)

    def short_pass(self, dos=None):
        """Loads and a save through the turbo, and plain files raw, on one Twin."""
        d = self.D
        t = Twin({b"F": rec(0xD1, 0, d[:40]) + rec(0xD9, 0x8000, d[:30]) + b"\xc3\x00\xa0\xc9",
                  b"G": rec(0xD9, 0x8000, d[:30]) + b"\xc9", b"ONE": b"1" * 30},
                 dos=dos, same_bus=False)
        t.poke(TARGET, b"\xc9")
        t.call(0x02, hl=0xC000, de=CDOS + 0x04E9)
        for name in (b"F", b"G", b"NONE"):
            self.load(t, name)
        self.load(t, b"$", base=0x8000, mask=0)
        t.call(0x02, hl=0x8010, de=CDOS + 0x04E9)
        self.load(t, b"$", base=0x8000, mask=0)
        t.poke(0x8000, d)
        t.poke(DESC, entry(0xD9, 0x1234, 30, 0x8000) + b"\xc9")
        t.call(0x0E, hl=t.name(b"S"), de=DESC)

    def test_the_short_pass_passes_unmutated(self):
        self.short_pass()


if __name__ == "__main__":
    if sys.argv[1:] == ["margins"]:
        margins()
    else:
        unittest.main()
