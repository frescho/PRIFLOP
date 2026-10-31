#!/usr/bin/env python3
"""System simulation of the Primo turbo: the real resident module, a fake
CDOS, and a model of the drive's half, on a modelled open-collector bus.

    tools/external/venv-sim/bin/python tests/primo_turbo_sim.py        # the tests
    tools/external/venv-sim/bin/python tests/primo_turbo_sim.py sweep  # margin and speed

The turbo's protocol is described in docs/user/PRIMO.md.

What runs, and how faithfully:

  * The Primo: tools/primo/turbo/turbo.asm (with xfer.asm), assembled by
    z88dk, relocated as the installer will, executed by the `z80` package
    (kosarev/z80, cycle-exact) at 2.5 MHz. It is entered the way CDOS's
    service call enters it: A = the service index, HL/DE/BC as CDOS passes
    them. Optional video wait states: any memory access may cost 1-3 extra
    T-states at random (the booklet's 2-3 clocks).
  * CDOS: only what the module touches -- RST 28H services 0x18 (send a
    command, read the answer) and 0x1A (read the error channel), done in
    Python; its variables (base at 0x4174, the limit at base+0x0B8F, the
    answer buffer at base+0x0B96); and the replaced routines, as stubs that
    record being called.
  * Or, with kind="B", our own DOS (tools/primo/dos/dos.asm, phase B) in CDOS's place, entered through its service call. Its two
    standard-bus services the turbo uses, 0x18 and 0x1A, are trapped at their
    entries and done by the same Python; tests/primo_dos_sim.py runs them on
    the bit-level bus instead.
  * The drive: its state machine is src/pico/primo_turbo.pio, through
    tests/pio_model.py's parser, executed here at 150 MHz with the input
    synchroniser. Its CPU is `Drive` below -- the protocol engine and the
    "brain" that reads CDOS's records and gives the orders. That class is the
    specification step D's firmware implements in C.
  * The bus: a pull wins at once; a released line reads high `rise_ns` after
    the last pull ends.
  * Faults: bits the Primo misreads, bits the drive sends wrong or samples
    wrong, CLOCK spikes the drive takes for two extra edges. Every one must
    end in a resend -- never in wrong data in memory or in the file.
"""
import collections
import os
import random
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "primo", "turbo"))
import pio_model  # noqa: E402
import build_turbo  # noqa: E402
sys.path.insert(0, os.path.join(ROOT, "tools", "primo", "dos"))
import build_dos  # noqa: E402

try:
    import z80
except ImportError:                     # pragma: no cover
    z80 = None

PIO_SRC = os.path.join(ROOT, "src", "pico", "primo_turbo.pio")

T_NS = 400.0                            # one T-state at 2.5 MHz
OUTPORT, INPORT = 0x80, 0x40
CLK_OUT, DAT_OUT = 0x20, 0x10
CLK_IN, DAT_IN = 0x20, 0x10
GPIO_CLK, GPIO_DATA = 3, 6

MOD = 0xD800                            # where the module sits (under CDOS)
CDOS = 0xDC4B                           # CDOS's base on the user's B64
SCREEN_HI = 0xE8
DRIVER = 0x6E00                         # below everything the tests load or fill
STUBS = {0x0E: 0xF000, 0x10: 0xF010, 0x12: 0xF020}     # the replaced routines
RES = 0x6F00                            # the driver stores DE, AF here
F_C, F_Z = 0x01, 0x40


def fletcher(data, seed=0):
    s1, s2 = seed, 0
    for b in data:
        s1 = (s1 + b) & 0xFF
        s2 = (s2 + s1) & 0xFF
    return s1, s2


def bits_lsb(data):
    for b in data:
        for i in range(8):
            yield (b >> i) & 1


def byte_at(bits):
    return sum(b << i for i, b in enumerate(bits))


def u16(b):
    return b[0] | b[1] << 8


class Line:
    """An open-collector line: each driver pulls or releases; any pull wins at
    once, and after the last release the line reads high `rise_ns` later."""

    def __init__(self, rise_ns):
        self.rise = rise_ns
        self.pulls = {}
        self.times = [0.0]
        self.low = [False]              # was anyone pulling, from that time on

    def set(self, who, t, pulled):
        self.pulls[who] = pulled
        low = any(self.pulls.values())
        if low != self.low[-1]:
            self.times.append(t)
            self.low.append(low)

    def level(self, t):
        i = max(0, _bisect(self.times, t) - 1)
        if self.low[i]:
            return 0
        return 1 if i == 0 or t - self.times[i] >= self.rise else 0

    def next_level(self, t, v, until):
        """Earliest time >= t, below `until`, at which level() == v, or None."""
        if self.level(t) == v:
            return t
        i = _bisect(self.times, t)
        cands = []
        for k in range(max(0, i - 1), len(self.times)):
            cands += [self.times[k], self.times[k] + self.rise]
        for c in sorted(x for x in cands if t <= x < until):
            if self.level(c) == v:
                return c
        return None


def _bisect(a, x):
    lo, hi = 0, len(a)
    while lo < hi:
        mid = (lo + hi) // 2
        if a[mid] <= x:
            lo = mid + 1
        else:
            hi = mid
    return lo


class Pio:
    """primo_turbo.pio, run lazily up to the time the Primo next touches the bus.

    Also the drive CPU's idle watch: when it is not holding and no CLOCK edge
    has come for RESYNC_NS, it flushes the FIFO, releases DATA and restarts
    the program at the wait matching the CLOCK level (step D's firmware does
    the same with pio_sm_restart and a forced jmp)."""

    CYCLE = pio_model.SYS_NS
    SYNC = pio_model.SYNC_CYCLES * pio_model.SYS_NS
    RESYNC_NS = 2_000_000

    def __init__(self, bus, cpu, rng=None, p_spike=0.0):
        with open(PIO_SRC) as f:
            text = f.read()
        prog, self.wrap_target, self.wrap = pio_model.parse_program(text, "primo_turbo")
        self.prog = prog
        self.released_pc = next(i for i, ins in enumerate(prog) if ins[0] == "wait" and ins[1] == 1)
        self.bus, self.cpu = bus, cpu
        self.rng, self.p_spike = rng, p_spike
        self.pc, self.t, self.y, self.osr = 0, 0.0, 0, 0
        self.fifo = collections.deque(cpu.start())
        self.avail = 0.0                # the FIFO's words exist from this time
        self.samples = []
        self.last_edge = 0.0
        self.done = False
        self.resyncs = self.spikes = 0

    def seen(self, line, t):
        return line.level(t - self.SYNC)

    def resync(self, t):
        self.resyncs += 1
        self.fifo = collections.deque(self.cpu.on_resync())
        self.avail = t
        self.bus["data"].set("drive", t, False)
        self.pc = 0 if self.seen(self.bus["clk"], t) == 1 else self.released_pc
        self.t = t
        self.last_edge = t

    def advance(self, until):
        while not self.done and self.t < until:
            op, a, b, _delay = self.prog[self.pc]
            nxt = self.wrap_target if self.pc == self.wrap else self.pc + 1
            if op == "wait":
                assert b == GPIO_CLK, "the program waits on CLOCK only"
                deadline = self.last_edge + self.RESYNC_NS
                tt = self.bus["clk"].next_level(self.t - self.SYNC, a, until - self.SYNC)
                if tt is None or tt + self.SYNC > deadline:
                    if deadline < until:
                        self.resync(deadline)
                        continue
                    return
                self.t = max(self.t, tt + self.SYNC)
                self.last_edge = self.t
                self.pc = nxt
                if self.p_spike and self.rng.random() < self.p_spike:
                    self.spike()
                continue
            elif op == "in":
                bit = self.seen(self.bus["data"], self.t)
                self.samples.append(self.cpu.glitch_sample(bit))
                self.pc = nxt
            elif op == "pull":
                if not self.fifo:
                    got = self.cpu.on_hold(self.t, self.samples)
                    if got is None:
                        self.done = True
                        self.bus["data"].set("drive", self.t, False)
                        return
                    words, ready = got
                    self.fifo.extend(words)
                    self.avail = ready
                if self.avail > self.t:
                    if self.avail >= until:
                        return
                    self.t = self.avail
                    self.last_edge = self.t     # the CPU just acted
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
            else:
                raise ValueError(op)
            self.t += self.CYCLE

    def spike(self):
        """A glitch on CLOCK the drive takes for two edges: two extra words
        consumed, two extra samples -- the worst a spike can do."""
        self.spikes += 1
        for _ in range(2):
            if self.fifo:
                w = self.fifo.popleft()
                self.bus["data"].set("drive", self.t, bool(w & PULL))
                if w & HOLD and self.fifo:
                    self.fifo.popleft()
            self.samples.append(self.seen(self.bus["data"], self.t))


REL, PULL, HOLD = 0, 1, 2
REQ = [REL] * 16 + [REL | HOLD]


def words_for(data):
    return [PULL if bit == 0 else REL for bit in bits_lsb(data)]


class Drive:
    """The drive's half: answers `XT`, then serves exchanges at the PIO holds.

    Exchange rule: the Primo opens every exchange
    with a frame number and its complement. `cur` is the frame in hand. A
    read frame is confirmed when the Primo asks for the next number; a write
    is complete when its check passes. A repeat of the frame in hand is
    served again; a repeat of the last completed write is taken and dropped.
    Anything else is refused."""

    def __init__(self, files, rng, latency, enabled=True, p_flip=0.0, p_flip_sample=0.0):
        self.files = files
        self.rng, self.latency = rng, latency
        self.enabled = enabled
        self.p_flip, self.p_flip_sample = p_flip, p_flip_sample
        self.error = "00, OK,00,00"
        self.armed = False
        self.enter_ns = 300_000.0       # 0.3 ms from the command to turbo
        self.counters = collections.Counter()

    # -- standard-speed side (what the drive's DOS command parser does)
    def command(self, cmd):
        if not (self.enabled and cmd[:2] == b"XT" and len(cmd) == 10 and cmd[9:] == b"."):
            self.error = "30,SYNTAX ERROR,00,00"     # primo_turbo.c: ERROR_SYNTAX_UNKNOWN
            return self.error
        idx, hl, de, bc = cmd[2], u16(cmd[3:5]), u16(cmd[5:7]), u16(cmd[7:9])
        self.error = "00, OK,00,00"
        self.brain = self.service(idx, hl, de, bc)
        self.cur, self.cur_kind, self.cur_words = 0, None, None
        self.last_write = None
        self.reply = None                       # what an 'R' order brought back
        self.pending = None                     # the order needing a write next
        self.finished = False
        self.armed = True
        self.left = False
        self.reissue = False
        return "00,TURBO,01,00"

    def error_channel(self):
        return self.error

    # -- the PIO's CPU
    def start(self):
        return [REL] + REQ             # the "go" edge releases the ready pull

    def on_resync(self):
        # while a 'K' frame is out and not yet confirmed, the Primo is
        # running the call: a designed wait (primo_turbo.c's s_in_call)
        if self.cur_kind == "read" and self.last_order[0] == "K":
            self.counters["call_waits"] += 1
        else:
            self.counters["resyncs"] += 1
        self.waiting_q = None
        return list(REQ)

    def glitch_sample(self, bit):
        if self.p_flip_sample and self.rng.random() < self.p_flip_sample:
            return bit ^ 1
        return bit

    def on_hold(self, t, samples):
        if self.left:
            return None
        ready = t + self.latency(self.rng)
        if getattr(self, "waiting_q", None):
            return self.at_q(samples), ready
        req = samples[-17:-1]
        n, check = byte_at(req[:8]), byte_at(req[8:])
        if n ^ check != 0xFF:
            self.counters["garbled"] += 1
            return [PULL, PULL] + REQ, ready
        if self.cur_kind == "read" and n == (self.cur + 1) & 0xFF:
            self.cur, self.cur_kind = n, None   # the last read arrived
        elif self.cur_kind == "write" and n == (self.cur + 1) & 0xFF:
            # the Primo took "again" for "stored": drop that write and give
            # the same 'R' order again, in this new frame
            self.counters["reissued"] += 1
            self.cur, self.cur_kind, self.reissue = n, None, True
        if n == self.cur & 0xFF:
            if self.cur_kind is None:
                self.next_exchange()
            elif self.cur_kind == "read":
                self.counters["resent"] += 1
            if self.cur_kind == "read":
                return self.sent_words(), ready
            if self.cur_kind == "quit":
                self.left = True
                return [PULL, REL], ready
            return self.write_words(n), ready
        if self.last_write is not None and n == self.last_write & 0xFF:
            self.counters["dup_writes"] += 1
            return self.write_words(n, dup=True), ready
        self.counters["refused"] += 1
        return [PULL, PULL] + REQ, ready

    def next_exchange(self):
        if self.reissue:
            self.reissue = False
            self.frame(self.last_order)
            return
        if self.pending is not None:
            self.cur_kind = "write"
            return
        if self.finished:
            self.cur_kind = "quit"
            return
        order = self.brain.send(self.reply) if self.started else next(self.brain)
        self.started = True
        self.reply = None
        self.frame(order)

    def frame(self, order):
        self.last_order = order
        op, addr, arg = order
        addr &= 0xFFFF              # primo_turbo.c's order() takes a uint16_t: a load
                                    # that ends at the top of memory ends at 0
        body = bytes([ord(op), addr & 0xFF, addr >> 8])
        if op == "W":
            body += bytes([len(arg) & 0xFF]) + arg
        else:
            body += bytes([arg & 0xFF])
        if op == "R":
            self.pending = arg
        if op in "EJ":
            self.finished = True
        s1, s2 = fletcher(body, seed=self.cur & 0xFF)
        self.cur_kind = "read"
        self.cur_words = [PULL, REL] + words_for(body + bytes([s1, s2])) + [REL] + REQ
        self.counters["orders"] += 1
        self.counters["W bytes"] += len(arg) if op == "W" else 0

    started = False

    def sent_words(self):
        """The frame in hand as it goes out this time: maybe with a wrong bit."""
        w = list(self.cur_words)
        if self.p_flip and self.rng.random() < self.p_flip:
            k = self.rng.randrange(2, len(w) - len(REQ) - 1)
            w[k] ^= PULL
        return w

    def write_words(self, n, dup=False):
        length = self.pending if not dup else self.last_len
        self.waiting_q = (n, length, dup)
        return [PULL, REL] + [REL] * (8 * length + 16) + [REL | HOLD]

    def at_q(self, samples):
        n, length, dup = self.waiting_q
        self.waiting_q = None
        pb = samples[-(8 * length + 16) - 1:-1]
        data = bytes(byte_at(pb[i:i + 8]) for i in range(0, len(pb), 8))
        payload, check = data[:-2], data[-2:]
        ok = bytes(fletcher(payload, seed=n & 0xFF)) == check
        if not ok:
            self.counters["bad_writes"] += 1
        elif not dup:
            self.reply = payload
            self.pending = None
            self.last_write, self.last_len = self.cur, length
            self.cur, self.cur_kind = self.cur + 1, None
            self.counters["R bytes"] += length
        return [PULL, REL if ok else PULL, REL] + REQ

    # -- the brain: what CDOS's routine would have done, as orders
    def service(self, idx, hl, de, bc):
        def read(addr, n):
            out = b""
            while n:
                k = min(n, 255)
                out += (yield ("R", addr, k))
                addr, n = addr + k, n - k
            return out

        name_len = (yield from read(hl, 1))[0]
        name = (yield from read(hl + 1, name_len)) if name_len else b""
        if idx == 0x0E:
            yield from self.save(name, de, read)
            return
        verify = idx == 0x12
        f = self.files.get(name)
        if f is None and isinstance(self.files, dict) and b":" in name:
            f = self.files.get(name.split(b":", 1)[1])     # "//:PFM": no folders here
        if f is None:
            self.error = "62,FILE NOT FOUND,00,00"
            yield ("E", 0, 0)
            return
        base = u16((yield from read(0x4174, 2)))
        limit = u16((yield from read(base + 0x0B8F, 2)))
        scr = (yield from read(0x404A, 1))[0]
        mask = bc & 0xFF
        pos, end, last, mism = 0, de, 0, 0
        # a file that ends before its 0xC9 stores what came and is 99, as
        # the firmware's load() (until then: no error)
        early = False
        while True:
            if pos >= len(f):
                early = True
                break
            t = f[pos]
            if t == 0xC9:
                break
            if t in (0xC3, 0xCD):
                if pos + 3 > len(f):
                    early = True
                    break
                a = u16(f[pos + 1:pos + 3])
                pos += 3
                if not verify and t == 0xC3 and mask & 8:
                    yield ("J", a, last)
                    return
                if not verify and t == 0xCD and mask & 16:
                    yield ("K", a, 0)
                continue
            if pos + 5 > len(f):
                early = True
                break
            off, ln = u16(f[pos + 1:pos + 3]), u16(f[pos + 3:pos + 5])
            data = f[pos + 5:pos + 5 + ln]
            pos += 5 + ln
            bit = {0xD9: 1, 0xD1: 2, 0xD5: 4}.get(t)
            use = bit is not None and mask & bit
            dest = {0xD1: de + off, 0xD5: scr * 256 + off}.get(t, off)
            if use and not verify and dest >> 8 < scr and dest + ln > limit:
                self.error = "52,FILE TOO LARGE,00,00"
                yield ("E", 0, 0)
                return
            if use:
                last = t
            for i in range(0, ln, 255):
                chunk = data[i:i + 255]
                early = len(chunk) < min(255, ln - i)
                if not use:
                    pass                        # CDOS's handler skips it
                elif verify:
                    mem = yield from read(dest + i, len(chunk))
                    mism = min(255, mism + sum(a != b for a, b in zip(mem, chunk)))
                elif chunk:
                    yield ("W", dest + i, chunk)
                if early:
                    break
            if early:
                break
            if use:
                end = dest + ln
        if early:
            self.error = "99,FILE ENDS EARLY,00,00"
        yield ("E", end, mism if verify else last)

    def save(self, name, desc, read):
        key = name.split(b":", 1)[1] if b":" in name else name
        if key in self.files and not name.startswith(b"@"):
            self.error = "63,FILE EXISTS,00,00"     # as the firmware's file_open()
            yield ("E", 0, 0)
            return
        out = bytearray()
        while True:
            t = (yield from read(desc, 1))[0]
            out.append(t)
            if t == 0xC9:
                break
            if t == 0xC3:                       # the file ends here, as CDOS's
                out += yield from read(desc + 1, 2)
                break
            hdr = yield from read(desc + 1, 4)
            src = u16((yield from read(desc + 5, 2)))
            out += hdr
            out += yield from read(src, u16(hdr[2:4]))
            desc += 7
        self.files[key] = bytes(out)
        yield ("E", 0, 0)


class Primo:
    """The Z80 with the module installed and a fake CDOS around it."""

    def __init__(self, drive, seed=1, rise_ns=1000.0, wait_states=False, p_flip_primo=0.0,
                 p_spike=0.0, limit=0xC000, installed=None, entry=MOD, kind="A"):
        self.drive = drive
        self.kind = kind
        self.errbuf = CDOS + 0x0B96
        self.rng = random.Random(seed)
        self.bus = {"clk": Line(rise_ns), "data": Line(rise_ns)}
        self.wait_states, self.p_flip_primo, self.p_spike = wait_states, p_flip_primo, p_spike
        self.extra_ns = 0.0
        self.last_io = 0.0
        self.pio = None
        self.stock = []
        code, syms, relocs = MODULE
        self.syms = syms
        mem = bytearray(65536)
        mem[MOD:MOD + len(code)] = build_turbo.relocate(code, relocs, MOD)
        for idx, stub in STUBS.items():
            mem[MOD + syms["oldtab"] + idx - 0x0E:MOD + syms["oldtab"] + idx - 0x0E + 2] = \
                bytes([stub & 0xFF, stub >> 8])
            mem[stub:stub + 3] = bytes([0xD3, 0xFC, 0xC9])      # OUT (0FCh),A ; RET
        mem[0x0028:0x002B] = bytes([0xD3, 0xFD, 0xC9])          # RST 28H: OUT (0FDh),A ; RET
        mem[0x403B] = 0x80
        mem[0x404A] = SCREEN_HI
        mem[0x4174:0x4176] = bytes([CDOS & 0xFF, CDOS >> 8])
        mem[CDOS + 0x0B8F:CDOS + 0x0B91] = bytes([limit & 0xFF, limit >> 8])
        if installed is not None:               # an installed system (TestInstaller)
            mem = installed
            mem[0x0028:0x002B] = bytes([0xD3, 0xFD, 0xC9])
        if kind == "B":
            entry = self.install_ours(mem, limit)
        # driver: CALL module ; LD (RES),DE ; PUSH AF ; POP HL ; LD (RES+2),HL ; HALT
        drv = bytes([0xCD, entry & 0xFF, entry >> 8, 0xED, 0x53, RES & 0xFF, RES >> 8,
                     0xF5, 0xE1, 0x22, (RES + 2) & 0xFF, (RES + 2) >> 8, 0x76])
        if installed is not None:
            mem[DRIVER:DRIVER + len(drv)] = drv
            self.mem = mem
            return
        mem[DRIVER:DRIVER + len(drv)] = drv
        self.mem = mem

    def install_ours(self, mem, limit):
        """Our DOS at the top of RAM, where the loader (B7) puts it, with
        0x18 and 0x1A trapped: OUT (0FEh) and OUT (0FFh). RST 28H goes to its
        service call. Returns the entry: the service call."""
        code, syms, relocs = OURS
        base = 0xE7FF - len(code)
        img = bytearray(build_dos.relocate(code, relocs, base))
        img[syms["outport"]] = OUTPORT
        img[syms["limit"]:syms["limit"] + 2] = bytes([limit & 0xFF, limit >> 8])
        img[syms["s18"]:syms["s18"] + 3] = bytes([0xD3, 0xFE, 0xC9])
        img[syms["s1a"]:syms["s1a"] + 3] = bytes([0xD3, 0xFF, 0xC9])
        mem[base:base + len(img)] = img
        mem[0x4174:0x4176] = bytes([base & 0xFF, base >> 8])
        svc = base + syms["svc"]
        mem[0x0028:0x002B] = bytes([0xC3, svc & 0xFF, svc >> 8])
        self.dos_base, self.dos_syms = base, syms
        self.errbuf = base + syms["errbuf"]
        return svc

    def now(self):
        return (self.ticks0 - self.m.ticks_to_stop + self.base) * T_NS + self.extra_ns

    def service(self, m, a=None):
        a = m.a if a is None else a
        if a not in (0x18, 0x1A):
            raise AssertionError(f"the module called CDOS service {a:#x}")
        if a == 0x18:
            n = m.memory[m.hl]
            cmd = bytes(m.memory[m.hl + 1:m.hl + 1 + n])
            reply = self.drive.command(cmd)
            if self.drive.armed and self.pio is None:
                # the drive enters turbo after the error channel is read
                # (BUS_CLEANUP), a moment later, and says so by pulling DATA
                t0 = self.now() + self.drive.enter_ns
                self.pio = Pio(self.bus, self.drive, self.rng, self.p_spike)
                self.pio.t = self.pio.last_edge = t0
                self.bus["data"].set("drive", t0, True)
        else:
            reply = self.drive.error_channel()
            self.pio = None if self.drive.left else self.pio
        buf = self.errbuf
        text = reply.encode() + b"\x00"
        for i, c in enumerate(text):
            m.memory[buf + i] = c
        code = int(reply[:2])
        if code < 2:
            m.a, m.f = code, 0x00                # NC NZ
        else:
            m.a, m.f = code, F_Z                 # Z, A = code

    def run(self, idx, hl=0, de=0, bc=0, limit_t=1_500_000_000):
        m = self.m = z80.Z80Machine()
        m.set_memory_block(0, bytes(self.mem))

        def out_cb(port, value):
            p = port & 0xFF
            if p == 0xFD:
                self.service(m)
                return
            if p in (0xFE, 0xFF):                   # our DOS's 0x18 and 0x1A, trapped
                self.service(m, 0x18 if p == 0xFE else 0x1A)
                return
            if p in (0xF6, 0xF8, 0xF9, 0xFA) and getattr(self, "_out_extra", None):
                self._out_extra(m, p, value)        # ROM print stubs (primo_pturbo_sim)
                return
            if p == 0xFC:
                self.stock.append((m.pc, m.hl, m.de, m.bc))
                return
            if p != OUTPORT:
                return
            t = self.last_io = self.now()
            if self.pio:
                self.pio.advance(t)
            self.bus["clk"].set("primo", t, bool(value & CLK_OUT))
            self.bus["data"].set("primo", t, bool(value & DAT_OUT))

        def in_cb(port):
            if port & 0xFF == 0xF7:                 # KYBHND stub: BRK after N rounds
                done = sum(1 for t in getattr(self, "text", []) if t == "R ")
                self.kyb_polls = getattr(self, "kyb_polls", 0) + 1
                if self.kyb_polls == getattr(self, "brk_pulse", 0):
                    return 1                        # BRK tapped once, mid-round
                return 1 if done >= getattr(self, "rounds_wanted", 1) else 0
            if port & 0xFF != INPORT:
                return 0xFF
            t = self.last_io = self.now()
            if self.pio:
                self.pio.advance(t)
            v = 0xFF
            d = self.bus["data"].level(t)
            if self.p_flip_primo and self.rng.random() < self.p_flip_primo:
                d ^= 1
            if not d:
                v &= ~DAT_IN
            if not self.bus["clk"].level(t):
                v &= ~CLK_IN
            return v

        def read_cb(addr):
            if self.rng.random() < 0.3:
                self.extra_ns += self.rng.randint(1, 3) * T_NS
            return m.memory[addr]

        m.set_output_callback(out_cb)
        m.set_input_callback(in_cb)
        if self.wait_states:
            m.set_read_callback(read_cb)
        m.a, m.hl, m.de, m.bc = idx, hl, de, bc
        m.pc, m.sp = DRIVER, 0xBF00
        self.base = 0
        while not m.halted:
            self.ticks0 = 100_000
            m.ticks_to_stop = self.ticks0
            m.run()
            self.base += self.ticks0 - m.ticks_to_stop
            if self.base > limit_t:
                raise RuntimeError("simulation ran away")
        af = m.memory[RES + 2] | m.memory[RES + 3] << 8
        return {"a": af >> 8, "f": af & 0xFF, "de": m.memory[RES] | m.memory[RES + 1] << 8}

    def poke(self, addr, data):
        self.mem[addr:addr + len(data)] = data

    def peek(self, addr, n):
        return bytes(self.m.memory[addr:addr + n])


MODULE = OURS = None
if z80 is not None and os.path.exists(build_turbo.Z80ASM):
    MODULE = build_turbo.module()
    OURS = build_dos.body()


def pattern(n, seed=7):
    r = random.Random(seed)
    return bytes(r.randrange(256) for _ in range(n))


def rec(t, addr, data):
    return bytes([t, addr & 0xFF, addr >> 8, len(data) & 0xFF, len(data) >> 8]) + data


def name_at(primo, addr, name):
    primo.poke(addr, bytes([len(name)]) + name)
    return addr


FAST = lambda r: r.uniform(20_000, 200_000)   # 20-200 us: the drive's CPU at a hold
NAME = 0x7000
BASE = 0x4300                                   # (0x40A4): where BASIC programs go


def do_load(files, name=b"PROG", mask=0x1F, idx=0x10, seed=1, drive_kw=None, latency=FAST, **kw):
    """A load through Primo (kw: its arguments, kind="B" for our own DOS)."""
    rng = random.Random(seed + 1000)
    drive = Drive(files, rng, latency, **(drive_kw or {}))
    primo = Primo(drive, seed=seed, **kw)
    name_at(primo, NAME, name)
    return primo, drive, primo.run(idx, hl=NAME, de=BASE, bc=mask)


def check_flags(test, res, z=False, c=False):
    test.assertEqual(bool(res["f"] & F_C), c, f"carry: {res}")
    test.assertEqual(bool(res["f"] & F_Z), z, f"zero: {res}")


@unittest.skipIf(MODULE is None, "needs the z80 package (see tools/external/README.md) and z88dk")
class TestLoad(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS
    def file(self):
        self.prog = pattern(700, 1)
        self.mc = pattern(300, 2)
        self.scr = pattern(256, 3)
        return (rec(0xD1, 0, self.prog) + rec(0xD9, 0x9000, self.mc) + rec(0xD5, 0x100, self.scr)
                + bytes([0xC9]))

    def test_records_land_where_cdos_puts_them(self):
        primo, drive, res = do_load(kind=self.kind, files={b"PROG": self.file()})
        check_flags(self, res)
        self.assertEqual(primo.peek(BASE, 700), self.prog)
        self.assertEqual(primo.peek(0x9000, 300), self.mc)
        self.assertEqual(primo.peek(SCREEN_HI * 256 + 0x100, 256), self.scr)
        self.assertEqual(res["a"], 0xD5)                        # the last type
        self.assertEqual(res["de"], SCREEN_HI * 256 + 0x100 + 256)
        self.assertFalse(primo.stock)
        self.assertTrue(drive.left)

    def test_wait_states_change_nothing(self):
        primo, drive, res = do_load(kind=self.kind, files={b"PROG": self.file()}, wait_states=True)
        check_flags(self, res)
        self.assertEqual(primo.peek(BASE, 700), self.prog)

    def test_turbo_off(self):
        primo, drive, res = do_load(kind=self.kind, files={b"PROG": self.file()}, drive_kw={"enabled": False})
        if self.kind == "A":            # phase A falls back to CDOS, untouched
            self.assertEqual(primo.stock, [(STUBS[0x10] + 2, NAME, BASE, 0x1F)])
        else:                           # ours has no fallback: the refusal is the error
            check_flags(self, res, z=True)
            self.assertEqual(res["a"], 30)
            self.assertEqual(primo.peek(BASE, 700), bytes(700))

    def test_the_directory_is_never_the_turbo(self):
        primo, drive, res = do_load(kind=self.kind, files={}, name=b"$", mask=0)
        self.assertFalse(drive.armed)
        if self.kind == "A":            # CDOS's own load
            self.assertEqual(len(primo.stock), 1)
        else:                           # our raw load on the standard bus: nobody there here
            check_flags(self, res, c=True)

    def test_missing_file_is_cdos_error_62(self):
        primo, drive, res = do_load(kind=self.kind, files={}, name=b"NONE")
        check_flags(self, res, z=True)
        self.assertEqual(res["a"], 62)

    def test_too_large_is_refused_before_anything_is_stored(self):
        big = rec(0xD9, 0xB000, pattern(0x2000, 4)) + bytes([0xC9])
        primo, drive, res = do_load(kind=self.kind, files={b"BIG": big}, name=b"BIG")
        check_flags(self, res, z=True)
        self.assertEqual(res["a"], 52)
        self.assertEqual(primo.peek(0xB000, 16), bytes(16))

    def test_a_call_record_runs_mid_load(self):
        # a routine at 0x9800 that stores 0x5A at 0x9900 and returns
        f = (rec(0xD9, 0x9800, bytes([0x3E, 0x5A, 0x32, 0x00, 0x99, 0xC9]))
             + bytes([0xCD, 0x00, 0x98]) + rec(0xD9, 0x9000, pattern(40, 5)) + bytes([0xC9]))
        primo, drive, res = do_load(kind=self.kind, files={b"P": f}, name=b"P")
        check_flags(self, res)
        self.assertEqual(primo.peek(0x9900, 1), b"\x5a")
        self.assertEqual(primo.peek(0x9000, 40), pattern(40, 5))

    def test_autostart_jumps_after_finishing(self):
        f = rec(0xD9, 0x9800, bytes([0x3E, 0xA5, 0x32, 0x00, 0x99, 0x76])) + bytes([0xC3, 0x00, 0x98])
        primo, drive, res = do_load(kind=self.kind, files={b"A": f}, name=b"A")
        self.assertEqual(primo.peek(0x9900, 1), b"\xa5")
        self.assertTrue(drive.left)

    def test_autostart_that_returns_returns_like_cdos(self):
        # SCF ; LD A,5A ; LD (9900),A ; RET -- a carry left behind must not
        # come back as a bus error (DEVICE NOT PRESENT, the first real load)
        f = rec(0xD9, 0x9800, bytes([0x37, 0x3E, 0x5A, 0x32, 0x00, 0x99, 0xC9])) + bytes([0xC3, 0x00, 0x98])
        primo, drive, res = do_load(kind=self.kind, files={b"A": f}, name=b"A")
        self.assertEqual(primo.peek(0x9900, 1), b"\x5a")
        check_flags(self, res)
        self.assertEqual(res["a"], 0xD9)
        # CMD LOAD makes BASIC's program end of DE: CDOS calls with DE = the
        # address, so a program that keeps DE returns it (garbage hung BASIC)
        self.assertEqual(res["de"], 0x9800)

    def test_faults_are_resent_never_stored(self):
        for seed in range(6):
            with self.subTest(seed=seed):
                primo, drive, res = do_load(kind=self.kind, files={b"PROG": self.file()}, seed=seed, wait_states=True,
                                            p_flip_primo=1 / 20000, p_spike=1 / 20000,
                                            drive_kw={"p_flip": 0.05})
                check_flags(self, res)
                self.assertEqual(primo.peek(BASE, 700), self.prog)
                self.assertEqual(primo.peek(0x9000, 300), self.mc)


@unittest.skipIf(MODULE is None, "needs the z80 package (see tools/external/README.md) and z88dk")
class TestVerify(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS
    def test_counts_differences(self):
        prog = pattern(500, 6)
        f = rec(0xD1, 0, prog) + bytes([0xC9])
        rng = random.Random(1)
        drive = Drive({b"V": f}, rng, FAST)
        primo = Primo(drive, kind=self.kind)
        name_at(primo, NAME, b"V")
        changed = bytearray(prog)
        for i in (3, 77, 400):
            changed[i] ^= 0xFF
        primo.poke(BASE, bytes(changed))
        res = primo.run(0x12, hl=NAME, de=BASE, bc=0x0E)
        check_flags(self, res)
        self.assertEqual(res["a"], 3)


@unittest.skipIf(MODULE is None, "needs the z80 package (see tools/external/README.md) and z88dk")
class TestSave(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS
    def save(self, data, seed=1, **kw):
        files = {}
        drive = Drive(files, random.Random(seed), FAST, **kw.pop("drive_kw", {}))
        primo = Primo(drive, kind=self.kind, seed=seed, **kw)
        name_at(primo, NAME, b"S")
        primo.poke(BASE, data)
        # CDOS's descriptor for CMD SAVE AUTO: D9 at 0x40A2, 2 bytes from 0x7100; D1; C9
        primo.poke(0x7100, b"\x00\x00")
        desc = bytes([0xD9, 0xA2, 0x40, 2, 0, 0x00, 0x71,
                      0xD1, 0, 0, len(data) & 0xFF, len(data) >> 8, BASE & 0xFF, BASE >> 8, 0xC9])
        primo.poke(0x7200, desc)
        res = primo.run(0x0E, hl=NAME, de=0x7200)
        want = rec(0xD9, 0x40A2, b"\x00\x00") + rec(0xD1, 0, data) + bytes([0xC9])
        return res, files.get(b"S"), want, primo

    def test_the_file_is_what_cdos_would_write(self):
        res, got, want, primo = self.save(pattern(600, 7))
        check_flags(self, res)
        self.assertEqual(got, want)

    def test_faults_are_resent_never_stored(self):
        for seed in range(6):
            with self.subTest(seed=seed):
                res, got, want, primo = self.save(pattern(600, seed), seed=seed, wait_states=True,
                                                  p_flip_primo=1 / 20000, p_spike=1 / 20000,
                                                  drive_kw={"p_flip_sample": 1 / 5000})
                check_flags(self, res)
                self.assertEqual(got, want)


@unittest.skipIf(MODULE is None, "needs the z80 package (see tools/external/README.md) and z88dk")
class TestSoak(unittest.TestCase):
    kind = "A"                          # phase A's module beside CDOS
    """Heavy noise: giving up is allowed (C, a bus error), wrong data is not."""

    def test_heavy_noise_never_lies(self):
        prog = pattern(900, 9)
        f = rec(0xD1, 0, prog) + bytes([0xC9])
        for seed in range(int(os.environ.get("SOAK", "10"))):
            with self.subTest(seed=seed):
                primo, drive, res = do_load(kind=self.kind, files={b"P": f}, name=b"P", seed=seed, p_flip_primo=1 / 1500,
                                            p_spike=1 / 1500, drive_kw={"p_flip": 0.3})
                if not res["f"] & F_C:
                    self.assertEqual(primo.peek(BASE, 900), prog)
                files = {}
                drive = Drive(files, random.Random(seed), FAST, p_flip_sample=1 / 1500)
                primo = Primo(drive, kind=self.kind, seed=seed, p_flip_primo=1 / 1500, p_spike=1 / 1500)
                name_at(primo, NAME, b"S")
                primo.poke(BASE, prog)
                primo.poke(0x7200, bytes([0xD1, 0, 0, 900 & 0xFF, 900 >> 8, BASE & 0xFF, BASE >> 8, 0xC9]))
                res = primo.run(0x0E, hl=NAME, de=0x7200)
                if not res["f"] & (F_C | F_Z):
                    self.assertEqual(files.get(b"S"), f)


OURS_SKIP = "needs the z80 package (see tools/external/README.md) and z88dk"


@unittest.skipIf(OURS is None, OURS_SKIP)
class TestLoadOurs(TestLoad):
    """TestLoad through our own DOS: the turbo inside it (phase B, step B4)."""
    kind = "B"


@unittest.skipIf(OURS is None, OURS_SKIP)
class TestSaveOurs(TestSave):
    kind = "B"


@unittest.skipIf(OURS is None, OURS_SKIP)
class TestSoakOurs(TestSoak):
    kind = "B"


def cdos_memory(port=0x80, load_entry=None):
    """A Primo after a CDOS boot, as far as the installer looks: CDOS's base,
    its service table, its port byte, BASIC's top of memory, and stubs for
    the ROM calls (CLEAR, NEW, print: RET; READY: HALT)."""
    mem = bytearray(65536)
    mem[0x4174:0x4176] = bytes([CDOS & 0xFF, CDOS >> 8])
    tab = CDOS + 0x0B42
    for idx, body in ((0x0E, 0x0A11), (0x10, 0x08CD), (0x12, 0x0A03)):
        a = CDOS + body if not (idx == 0x10 and load_entry) else load_entry
        mem[tab + idx:tab + idx + 2] = bytes([a & 0xFF, a >> 8])
    mem[CDOS + 0x06DB] = port
    top = CDOS - 1
    mem[0x40B1:0x40B3] = bytes([top & 0xFF, top >> 8])
    for a in (0x1B4D, 0x2B75, 0x0028):        # NEW, print, and CDOS's RST 28H
        mem[a] = 0xC9                          # (the installer's `XT+`): RET
    # CLEAR moves the stack below the new top, as the ROM's does:
    # POP BC ; LD HL,(40B1h) ; LD DE,-64 ; ADD HL,DE ; LD SP,HL ; PUSH BC ; RET
    mem[0x1E83:0x1E8E] = bytes([0xC1, 0x2A, 0xB1, 0x40, 0x11, 0xC0, 0xFF, 0x19, 0xF9, 0xC5, 0xC9])
    mem[0x1A19] = 0x76
    mem[0x403B] = 0x80
    mem[0x404A] = SCREEN_HI
    mem[CDOS + 0x0B8F:CDOS + 0x0B91] = bytes([0x00, 0xC0])
    return mem


def run_installer(mem):
    prg = build_turbo.prg()
    body = prg[5:-3]
    mem[build_turbo.INSTALL_AT:build_turbo.INSTALL_AT + len(body)] = body
    m = z80.Z80Machine()
    m.set_memory_block(0, bytes(mem))
    # SP as during a real CMD LOAD: just under CDOS, where the module goes
    m.pc, m.sp, m.ix = build_turbo.INSTALL_AT, CDOS - 0x40, 0x4042
    while not m.halted:
        m.ticks_to_stop = 1_000_000
        m.run()
    # BASIC's display routine needs IX = 0x4042, BASIC's system variables;
    # the first real install lost it and printed garbage (2026-10-05)
    assert m.ix == 0x4042, f"the installer left IX = {m.ix:#06x}"
    return bytearray(m.memory)


@unittest.skipIf(MODULE is None, "needs the z80 package (see tools/external/README.md) and z88dk")
class TestInstaller(unittest.TestCase):
    def test_installs_under_cdos_and_hooks_three_entries(self):
        for port in (0x80, 0x40):
            with self.subTest(port=port):
                mem = run_installer(cdos_memory(port))
                code, syms, relocs = MODULE
                mb = CDOS - len(code)
                want = bytearray(build_turbo.relocate(code, relocs, mb))
                want[syms["outport"]] = port
                tab = CDOS + 0x0B42
                old = bytes(cdos_memory(port)[tab + 0x0E:tab + 0x14])
                want[syms["oldtab"]:syms["oldtab"] + 6] = old
                self.assertEqual(bytes(mem[mb:mb + len(code)]), bytes(want))
                for idx in (0x0E, 0x10, 0x12):
                    self.assertEqual(u16(mem[tab + idx:tab + idx + 2]), mb)
                self.assertEqual(u16(mem[0x40B1:0x40B3]), mb - 1)

    def test_changes_nothing_when_load_is_not_cdos_own(self):
        before = cdos_memory(load_entry=0xD000)
        mem = run_installer(bytearray(before))
        self.assertEqual(mem[0x40B1:0x40B3], before[0x40B1:0x40B3])
        self.assertEqual(mem[CDOS + 0x0B42:CDOS + 0x0B56], before[CDOS + 0x0B42:CDOS + 0x0B56])

    def test_a_load_through_the_installed_module(self):
        mem = run_installer(cdos_memory())
        mb = CDOS - len(MODULE[0])
        prog = pattern(600, 11)
        drive = Drive({b"P": rec(0xD1, 0, prog) + bytes([0xC9])}, random.Random(1), FAST)
        primo = Primo(drive, installed=mem, entry=mb)
        name_at(primo, NAME, b"P")
        res = primo.run(0x10, hl=NAME, de=BASE, bc=0x1F)
        check_flags(self, res)
        self.assertEqual(primo.peek(BASE, 600), prog)
        self.assertFalse(primo.stock)


BOOT80 = os.path.join(ROOT, "primo-library", "bin", "cdos", "boot80.prg")
B64_ROM = os.path.join(ROOT, "tools", "external", "primo-rom", "cdos", "PR_B64.ROM")
ROM_STUBS = """
    org 3F80h
rst28:                  ; CDOS's service call: only 0x18 (XT+) is expected
    cp 18h
    jr nz,odd
    ld a,(3FF1h)        ; count the probes
    inc a
    ld (3FF1h),a
    ld a,(3FF0h)        ; the drive: 0 a PRIFLOP, 1 another drive, 2 none
    cp 1
    jr z,refused
    cp 2
    jr z,nodrive
    ld a,1
    or a                ; NZ NC: OK
    ld a,0
    ret
refused:
    ld a,31
    cp a                ; Z: an error code in A
    ret
nodrive:
    scf                 ; C: a bus error
    ret
odd:
    halt
prints:                 ; the ROM's print: log HL
    push hl
    push de
    ex de,hl
    ld hl,(3FF4h)
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl
    ld (3FF4h),hl
    pop de
    pop hl
    ret
"""


def boot_memory(mem=None, drive=0):
    """A B64 at the moment the boot ROM jumps to 0x4500: its real ROM (the
    loader checks bytes of it; the prologue reads the port from it), with the
    routines the boot calls stubbed (print logs HL, CLEAR moves the stack,
    NEW returns, READY halts, RST 28H answers XT+ as `drive` says), BASIC's
    top at 0xE7FF, and the boot file at 0x4500. `mem`: the RAM of an earlier
    boot, for a Reset+BRK with everything resident."""
    rom = open(B64_ROM, "rb").read()
    if mem is None:
        mem = bytearray(65536)
        mem[0x40B1:0x40B3] = b"\xff\xe7"
        mem[0x4174:0x4176] = b"\x97\x19"
        mem[0x403B] = 0x80
        mem[0x404A] = SCREEN_HI
    mem[0:0x4000] = rom
    stubs, _ = build_turbo.assemble("stubs.asm", extra={"stubs.asm": ROM_STUBS})
    stub_syms = build_turbo.assemble("stubs.asm", extra={"stubs.asm": ROM_STUBS})[1]
    mem[0x3F80:0x3F80 + len(stubs)] = stubs
    mem[0x0028:0x002B] = bytes([0xC3, 0x80, 0x3F])
    p = stub_syms["prints"]
    mem[0x2B75:0x2B78] = bytes([0xC3, p & 0xFF, p >> 8])
    mem[0x1B4D] = 0xC9
    mem[0x1E83:0x1E8E] = bytes([0xC1, 0x2A, 0xB1, 0x40, 0x11, 0xC0, 0xFF, 0x19, 0xF9, 0xC5, 0xC9])
    mem[0x1A19] = 0x76
    mem[0x3FF0], mem[0x3FF1] = drive, 0
    mem[0x3FF4:0x3FF6] = b"\xc0\x3f"
    f, syms = build_turbo.boot_file(open(BOOT80, "rb").read())
    mem[0x4500:0x4500 + len(f)] = f
    return mem


def run_boot(mem):
    m = z80.Z80Machine()
    m.set_memory_block(0, bytes(mem))
    m.pc, m.sp, m.ix = 0x4500, 0x4400, 0x4042
    for _ in range(200):
        if m.halted:
            break
        m.ticks_to_stop = 1_000_000
        m.run()
    mem = bytearray(m.memory)
    assert m.halted and m.pc in (0x1A19, 0x1A1A), f"stopped at {m.pc:#06x}, not READY"
    assert m.ix == 0x4042, f"IX = {m.ix:#06x}"
    texts = []
    for a in range(0x3FC0, u16(mem[0x3FF4:0x3FF6]), 2):
        hl = u16(mem[a:a + 2])
        end = mem.index(0, hl)
        texts.append(bytes(mem[hl:end]))
    return mem, texts


@unittest.skipIf(MODULE is None or not (os.path.exists(BOOT80) and os.path.exists(B64_ROM)),
                 "needs z80, z88dk, primo-library/ and the B64 ROM")
class TestBootFile(unittest.TestCase):
    """The boot-slot file of our own DOS, phase A, booted the way the ROM
    does it: CDOS's own loader, the port prologue, the turbo installer."""

    def cdos_own(self, mem):
        tab = CDOS + 0x0B42
        return [u16(mem[tab + i:tab + i + 2]) for i in (0x0E, 0x10, 0x12)] == \
            [CDOS + 0x0A11, CDOS + 0x08CD, CDOS + 0x0A03]

    def test_on_a_priflop_the_turbo_is_installed(self):
        mem, texts = run_boot(boot_memory(drive=0))
        code, syms, relocs = MODULE
        mb = CDOS - len(code)
        self.assertEqual(u16(mem[0x4174:0x4176]), CDOS)
        self.assertEqual(u16(mem[0x40B1:0x40B3]), mb - 1)
        tab = CDOS + 0x0B42
        for idx in (0x0E, 0x10, 0x12):
            self.assertEqual(u16(mem[tab + idx:tab + idx + 2]), mb)
        self.assertEqual(mem[mb + syms["outport"]], 0x80)
        self.assertEqual(mem[0x3FF1], 1)
        self.assertTrue(texts[0].startswith(b"\x0cCommodore Serial Bus Handler"), texts)
        self.assertEqual(texts[1:], [b"PRIFLOP turbo on\r"])

    def test_on_another_drive_or_none_it_is_plain_cdos(self):
        for drive in (1, 2):
            with self.subTest(drive=drive):
                mem, texts = run_boot(boot_memory(drive=drive))
                self.assertEqual(u16(mem[0x4174:0x4176]), CDOS)
                self.assertEqual(u16(mem[0x40B1:0x40B3]), CDOS - 1)   # not a byte taken
                self.assertTrue(self.cdos_own(mem))
                self.assertEqual(len(texts), 1)                       # only CDOS's banner

    def test_a_reboot_with_everything_resident_does_not_stack(self):
        mem, _ = run_boot(boot_memory(drive=0))
        mem, texts = run_boot(boot_memory(mem, drive=0))
        mb = CDOS - len(MODULE[0])
        self.assertEqual(u16(mem[0x4174:0x4176]), CDOS)
        self.assertEqual(u16(mem[0x40B1:0x40B3]), mb - 1)
        tab = CDOS + 0x0B42
        for idx in (0x0E, 0x10, 0x12):
            self.assertEqual(u16(mem[tab + idx:tab + idx + 2]), mb)
        self.assertEqual(texts[-1], b"PRIFLOP turbo on\r")
        # and when the drive has meanwhile become a plain one: CDOS's own
        # entries, no stale hook into the module
        mem, texts = run_boot(boot_memory(mem, drive=1))
        self.assertTrue(self.cdos_own(mem))

    def test_a_reboot_over_a_stacked_module_does_not_stack_further(self):
        # the real Primo, 2026-10-06: once the top was anywhere but exactly
        # one module below CDOS, every Reset+BRK put another module under
        # the last (55795 -> 53998 -> 53399). Here the top starts one
        # module lower than it should, as left by such a boot.
        mem, _ = run_boot(boot_memory(drive=0))
        n = len(MODULE[0])
        mb = CDOS - n
        code, syms, relocs = MODULE                 # a second copy below,
        mem[mb - n:mb] = build_turbo.relocate(code, relocs, mb - n)  # relocated there
        mem[0x40B1:0x40B3] = bytes([(mb - n - 1) & 0xFF, (mb - n - 1) >> 8])
        for _ in range(2):
            mem, texts = run_boot(boot_memory(mem, drive=0))
            self.assertEqual(u16(mem[0x40B1:0x40B3]), mb - n - 1)
            tab = CDOS + 0x0B42
            for idx in (0x0E, 0x10, 0x12):
                self.assertEqual(u16(mem[tab + idx:tab + idx + 2]), mb - n)
            self.assertEqual(texts[-1], b"PRIFLOP turbo on\r")

    def run_m(self, mem):
        """M (mem.asm) run on `mem` after a boot: its line, and DE/A/F."""
        f, syms = build_turbo.memprg()
        code = f[5:-3]
        mem = bytearray(mem)
        mem[0x7000:0x7000 + len(code)] = code
        mem[0x40F9:0x40FB] = b"\x34\x12"
        mem[0x3FF4:0x3FF6] = b"\xc0\x3f"
        mem[0x3F00] = 0x76                       # where it returns to
        mem[0x43FE:0x4400] = b"\x00\x3f"
        m = z80.Z80Machine()
        m.set_memory_block(0, bytes(mem))
        m.pc, m.sp, m.ix = 0x7000, 0x43FE, 0x4042
        m.ticks_to_stop = 1_000_000
        m.run()
        self.assertTrue(m.halted and m.pc in (0x3F00, 0x3F01), f"pc {m.pc:#06x}")
        out = bytearray(m.memory)
        hl = u16(out[0x3FC0:0x3FC2])
        self.assertEqual(u16(out[0x3FF4:0x3FF6]), 0x3FC2)     # one line
        return bytes(out[hl:out.index(0, hl)]), m.de, m.a, m.f

    def test_m_shows_what_the_boot_left(self):
        n = len(MODULE[0])
        mem, _ = run_boot(boot_memory(drive=0))
        line, de, a, f = self.run_m(mem)
        self.assertEqual(line, b"TOP %d CDOS %d MOD 1 HOOK\r" % (CDOS - n - 1, CDOS))
        self.assertEqual((de, a, f & 0x41), (0x1234, 0xD9, 0))     # NZ NC, end kept
        mem, _ = run_boot(boot_memory(drive=1))
        self.assertEqual(self.run_m(mem)[0], b"TOP %d CDOS %d MOD 0\r" % (CDOS - 1, CDOS))
        mem[0x40B1:0x40B3] = bytes([(CDOS - 3 * n - 1) & 0xFF, (CDOS - 3 * n - 1) >> 8])
        self.assertEqual(self.run_m(mem)[0], b"TOP %d CDOS %d MOD 3\r" % (CDOS - 3 * n - 1, CDOS))
        mem[0x40B1:0x40B3] = b"\x00\x10"         # 4096: not a whole number of modules
        self.assertTrue(self.run_m(mem)[0].startswith(b"TOP 4096 CDOS %d MOD " % CDOS))
        self.assertIn(b"?", self.run_m(mem)[0])

    def test_a_load_through_the_boot_installed_module(self):
        mem, _ = run_boot(boot_memory(drive=0))
        mem[CDOS + 0x0B8F:CDOS + 0x0B91] = bytes([0x00, 0xC0])  # the limit: CMD LOAD sets it
        mb = CDOS - len(MODULE[0])
        prog = pattern(600, 12)
        drive = Drive({b"P": rec(0xD1, 0, prog) + bytes([0xC9])}, random.Random(2), FAST)
        primo = Primo(drive, installed=mem, entry=mb)
        name_at(primo, NAME, b"P")
        res = primo.run(0x10, hl=NAME, de=BASE, bc=0x1F)
        check_flags(self, res)
        self.assertEqual(primo.peek(BASE, 600), prog)
        self.assertFalse(primo.stock)


def sweep():
    prog = pattern(255 * 16, 1)
    f = rec(0xD1, 0, prog) + bytes([0xC9])
    lat = lambda r: 50_000.0
    print("rise_ns   load 4080 B: result  KB/s     save 4080 B: result  KB/s")
    for rise in (500, 1000, 2000, 4000, 6000, 7000, 8000):
        primo, drive, res = do_load({b"P": f}, name=b"P", rise_ns=rise, latency=lat)
        ok = not res["f"] & (F_C | F_Z) and primo.peek(BASE, len(prog)) == prog
        tl = primo.last_io
        files = {}
        d2 = Drive(files, random.Random(1), lat)
        p2 = Primo(d2, rise_ns=rise)
        name_at(p2, NAME, b"S")
        p2.poke(BASE, prog)
        p2.poke(0x7200, bytes([0xD1, 0, 0, len(prog) & 0xFF, len(prog) >> 8, BASE & 0xFF, BASE >> 8, 0xC9]))
        r2 = p2.run(0x0E, hl=NAME, de=0x7200)
        ok2 = not r2["f"] & (F_C | F_Z) and files.get(b"S") == rec(0xD1, 0, prog) + bytes([0xC9])
        print(f"{rise:7.0f}          {'ok  ' if ok else 'FAIL'}  {len(prog) / (tl / 1e9) / 1024:5.2f}"
              f"              {'ok  ' if ok2 else 'FAIL'}  {len(prog) / (p2.last_io / 1e9) / 1024:5.2f}")


if __name__ == "__main__":
    if sys.argv[1:] == ["sweep"]:
        sweep()
    else:
        unittest.main()
