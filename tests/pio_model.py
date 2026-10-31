"""A cycle-level model of an RP2350 PIO state machine -- just the instructions
the IEC receive program uses -- so it can be fed bus waveforms on the host.

There is no PIO emulator in this project's toolchain, and the receive program
is exactly the kind of code whose failure needs the real bus to show itself:
on 2026-10-01 it decoded garbage only while a ground loop put spikes on the
lines. This model lets a test put the spikes there instead.

What is modelled, and how:

  * Instructions: `wait <0|1> gpio <n>`, `jmp [pin|x--|!x] <label>` and plain
    `jmp`, `in pins, 1`, `set x, <n>`, each with an optional `[delay]`;
    parsed only (tests/primo_turbo_sim.py executes them): `pull block`,
    `out pindirs, 1`, `out y, 1`, `jmp !y`;
    `.wrap_target`/`.wrap`. Anything else is an error, not a silent no-op.
  * Timing: the state machine executes on every `clkdiv`-th system clock.
    An instruction takes one SM cycle plus its delay; a `wait` stalls until
    its condition holds on an SM cycle, then pays its delay.
  * Inputs pass a two-stage synchroniser, so the level an instruction sees is
    the pin's level two system clocks earlier. A spike that falls between two
    SM cycles is never seen -- as on the chip, where the clock divider gates
    when the state machine looks, not when the synchroniser samples.
  * `in pins, 1` with shift-right and autopush at 8 bits, as iec_rx_program_init
    configures it: each push is the ISR, whose top byte is the received byte.

Waveforms are lists of (time_ns, level) transitions per GPIO; `Line` answers
"level at t" and "next time the level is v at or after t" by bisection, so a
millisecond of bus time costs a handful of steps, not 150,000 cycles.
"""
import bisect
import re

SYS_HZ = 150_000_000
SYS_NS = 1e9 / SYS_HZ
SYNC_CYCLES = 2


class Line:
    """A digital line as transitions: level `initial` until the first entry,
    then each (t_ns, level) in order."""

    def __init__(self, initial, transitions=()):
        self.initial = initial
        self.times = []
        self.levels = []
        for t, v in sorted(transitions):
            self.times.append(t)
            self.levels.append(v)

    def at(self, t):
        i = bisect.bisect_right(self.times, t)
        return self.initial if i == 0 else self.levels[i - 1]

    def next_change(self, t):
        """The first transition time strictly after t, or None."""
        i = bisect.bisect_right(self.times, t)
        return self.times[i] if i < len(self.times) else None


def parse_program(text, name):
    """Parse `.program name` out of a .pio source. Returns (instructions,
    wrap_target, wrap) with instructions as tuples and labels resolved."""
    lines = text.splitlines()
    body, inside = [], False
    for raw in lines:
        line = raw.split(";", 1)[0].strip()
        if line.startswith("% c-sdk"):
            inside = False
            continue
        if line.startswith(".program"):
            inside = line.split()[1] == name
            continue
        if inside and line:
            body.append(line)
    if not body:
        raise ValueError(f"program {name!r} not found")

    labels, instrs = {}, []
    wrap_target, wrap = 0, None
    for line in body:
        if line == ".wrap_target":
            wrap_target = len(instrs)
            continue
        if line == ".wrap":
            wrap = len(instrs) - 1
            continue
        if line.startswith("."):
            raise ValueError(f"unsupported directive: {line}")
        line = re.sub(r"^public\s+", "", line)
        m = re.match(r"^(\w+):\s*(.*)$", line)
        if m:
            labels[m.group(1)] = len(instrs)
            line = m.group(2)
            if not line:
                continue
        delay = 0
        m = re.match(r"^(.*?)\s*\[(\d+)\]\s*$", line)
        if m:
            line, delay = m.group(1), int(m.group(2))
        instrs.append((line, delay))
    if wrap is None:
        wrap = len(instrs) - 1

    out = []
    for text_, delay in instrs:
        t = text_.replace(",", " ").split()
        op = t[0]
        if op == "wait" and len(t) == 4 and t[2] == "gpio":
            out.append(("wait", int(t[1]), int(t[3]), delay))
        elif op == "jmp" and len(t) == 2:
            out.append(("jmp", "always", labels[t[1]], delay))
        elif op == "jmp" and len(t) == 3 and t[1] in ("pin", "x--", "!x", "!y"):
            out.append(("jmp", t[1], labels[t[2]], delay))
        elif op == "pull" and t[1:] == ["block"]:
            out.append(("pull", None, None, delay))
        elif op == "out" and len(t) == 3 and t[1] in ("pindirs", "y") and t[2] == "1":
            out.append(("out_" + t[1], None, None, delay))
        elif op == "in" and t[1:] == ["pins", "1"]:
            out.append(("in", None, None, delay))
        elif op == "set" and len(t) == 3 and t[1] == "x":
            out.append(("setx", int(t[2]), None, delay))
        else:
            raise ValueError(f"instruction not modelled: {text_!r}")
    return out, wrap_target, wrap


class StateMachine:
    def __init__(self, program, lines, in_base, jmp_pin=None, clkdiv=1):
        self.prog, self.wrap_target, self.wrap = program
        self.lines = lines          # gpio number -> Line
        self.in_base = in_base
        self.jmp_pin = jmp_pin
        self.tick_ns = SYS_NS * clkdiv

    def _seen(self, gpio, t):
        """The level the state machine sees on its cycle at time t."""
        return self.lines[gpio].at(t - SYNC_CYCLES * SYS_NS)

    def _next_tick(self, t):
        """The first SM cycle at or after time t."""
        k = -(-t // self.tick_ns)          # ceil
        return k * self.tick_ns

    def run(self, until_ns, start_ns=0.0):
        """Run from start_ns until until_ns. Returns the pushed bytes."""
        pc, x = 0, 0
        isr, count = 0, 0
        pushed = []
        t = self._next_tick(start_ns)
        while t < until_ns:
            op, a, b, delay = self.prog[pc]
            nxt = pc + 1 if pc != self.wrap else self.wrap_target
            if op == "wait":
                pol, gpio = a, b
                line = self.lines[gpio]
                # Skip ahead: look at SM cycles only where the seen level can
                # have changed, i.e. just after a transition of the line.
                while self._seen(gpio, t) != pol:
                    ch = line.next_change(t - SYNC_CYCLES * SYS_NS)
                    if ch is None:
                        return pushed
                    t = self._next_tick(ch + SYNC_CYCLES * SYS_NS)
                    if t >= until_ns:
                        return pushed
                pc = nxt
            elif op == "jmp":
                if a == "always":
                    take = True
                elif a == "pin":
                    take = self._seen(self.jmp_pin, t) == 1
                elif a == "x--":
                    take = x != 0
                    x = (x - 1) & 0xFFFFFFFF
                else:  # "!x"
                    take = x == 0
                pc = b if take else nxt
            elif op == "in":
                bit = self._seen(self.in_base, t)
                isr = (isr >> 1) | (bit << 31)
                count += 1
                if count == 8:
                    pushed.append(isr >> 24)
                    isr, count = 0, 0
                pc = nxt
            elif op == "setx":
                x = a
                pc = nxt
            t += self.tick_ns * (1 + delay)
        return pushed
