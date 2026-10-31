#!/usr/bin/env python3
"""Host tests for src/pico/iec_rx.pio, the PIO decoder of IEC data bytes, run
through tests/pio_model.py against generated C64 talker waveforms.

    python3 tests/iec_rx_test.py

Why: on 2026-10-01 PIO receive corrupted data whenever a ground loop put
noise on the bus, while the software path -- which reads CLOCK low only when
two reads 2 us apart agree -- did not. The original program
took any CLOCK edge, however short, as a bit boundary. These tests feed the
program spikes and edge ringing and require every byte to come out right,
and they feed the ORIGINAL program the same, which must fail: a model that
cannot reproduce the bug proves nothing about the fix.

Timing: bit set-up T_S >= 20 us, data valid T_V >= 20 us.
"""
import pathlib
import re
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from pio_model import Line, StateMachine, parse_program  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
PIO_FILE = ROOT / "src" / "pico" / "iec_rx.pio"

CLOCK, DATA = 3, 6          # IEC_PIN_CLOCK, IEC_PIN_DATA (arch-config.h)
US = 1000.0

# The program as it was until 2026-10-02, at the full system clock.
ORIGINAL = """
.program iec_rx
.wrap_target
    wait 1 gpio 3
    in   pins, 1
    wait 0 gpio 3
.wrap
"""


def current_program():
    text = PIO_FILE.read_text()
    m = re.search(r"sm_config_set_clkdiv\(&c,\s*([0-9.]+)f?\)", text)
    clkdiv = float(m.group(1)) if m else 1.0
    return parse_program(text, "iec_rx"), clkdiv, text


def xor_overlay(base, pulses):
    """Line `base` with each (start_ns, width_ns) pulse inverting it."""
    points = sorted(set(base.times + [s for s, _ in pulses] + [s + w for s, w in pulses]))

    def level(t):
        v = base.at(t)
        for s, w in pulses:
            if s <= t < s + w:
                v ^= 1
        return v

    out, prev = [], base.initial
    for t in points:
        v = level(t)
        if v != prev:
            out.append((t, v))
            prev = v
    return Line(base.initial, out)


def talker(byte, t_s=60 * US, t_v=60 * US, data_garbage_outside=False):
    """A C64 sending `byte`, LSB first, starting with CLOCK low at t=0 (where
    iec_pio_rx_arm() starts the state machine). Per bit: DATA set 1 us into the
    low phase, CLOCK released after t_s, pulled low after t_v. Returns
    (clock, data, end_ns, edges) -- edges are the CLOCK transition times.

    data_garbage_outside: DATA holds the bit only while CLOCK is high and the
    inverse otherwise, so a sample outside the valid window shows up."""
    clock, data, edges = [], [], []
    t = 0.0
    for i in range(8):
        bit = (byte >> i) & 1
        data.append((t + 1 * US, (bit ^ 1) if data_garbage_outside else bit))
        rise, fall = t + t_s, t + t_s + t_v
        if data_garbage_outside:
            data.append((rise, bit))
            data.append((fall, bit ^ 1))
        clock += [(rise, 1), (fall, 0)]
        edges += [rise, fall]
        t = fall
    return Line(0, clock), Line(1, data), t + 100 * US, edges


def decode(program, clkdiv, clock, data, end):
    sm = StateMachine(program, {CLOCK: clock, DATA: data}, in_base=DATA,
                      jmp_pin=CLOCK, clkdiv=clkdiv)
    return sm.run(end)


VALUES = [0x00, 0xFF, 0x55, 0xAA, 0x0F, 0xF0, 0x01, 0x80, 0x3C, 0xC3]


class TestCleanBus(unittest.TestCase):
    def setUp(self):
        self.prog, self.div, _ = current_program()

    def test_every_byte_value_at_nominal_timing(self):
        for v in range(256):
            clock, data, end, _ = talker(v)
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v], hex(v))

    def test_every_byte_value_at_the_shortest_legal_timing(self):
        for v in range(256):
            clock, data, end, _ = talker(v, t_s=20 * US, t_v=20 * US)
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v], hex(v))

    def test_data_is_sampled_while_it_is_valid(self):
        """DATA wrong everywhere except while CLOCK is high: only a sample
        inside T_V gives the byte."""
        for v in VALUES:
            clock, data, end, _ = talker(v, t_s=20 * US, t_v=20 * US, data_garbage_outside=True)
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v], hex(v))


def spiky(v, width_ns, t_s=20 * US, t_v=20 * US):
    """Every bit gets spikes of `width_ns` on CLOCK at 25, 50 and 75 % of both
    its low and its high phase -- downward while released, upward while low."""
    clock, data, end, _ = talker(v, t_s=t_s, t_v=t_v)
    pulses, t = [], 0.0
    for _ in range(8):
        for frac in (0.25, 0.5, 0.75):
            pulses.append((t + frac * t_s, width_ns))
            pulses.append((t + t_s + frac * t_v, width_ns))
        t += t_s + t_v
    return xor_overlay(clock, pulses), data, end


def ringing(v, t_s=20 * US, t_v=20 * US):
    """Each CLOCK edge bounces twice within 150 ns."""
    clock, data, end, edges = talker(v, t_s=t_s, t_v=t_v)
    pulses = [p for e in edges for p in ((e + 20, 30), (e + 90, 40))]
    return xor_overlay(clock, pulses), data, end


class TestTheOriginalProgramFails(unittest.TestCase):
    """The model must reproduce 2026-10-01's failure, or it proves nothing."""

    def setUp(self):
        self.prog = parse_program(ORIGINAL, "iec_rx")

    def test_it_decodes_a_clean_bus(self):
        for v in VALUES:
            clock, data, end, _ = talker(v)
            self.assertEqual(decode(self.prog, 1, clock, data, end), [v])

    def test_a_50ns_spike_breaks_it(self):
        bad = 0
        for v in VALUES:
            clock, data, end = spiky(v, 50)
            bad += decode(self.prog, 1, clock, data, end) != [v]
        self.assertEqual(bad, len(VALUES), "every byte with spikes should come out wrong")

    def test_ringing_breaks_it(self):
        bad = 0
        for v in VALUES:
            clock, data, end = ringing(v)
            bad += decode(self.prog, 1, clock, data, end) != [v]
        self.assertEqual(bad, len(VALUES))


class TestNoisyBus(unittest.TestCase):
    def setUp(self):
        self.prog, self.div, _ = current_program()

    def test_spikes_up_to_a_microsecond_are_ignored(self):
        for width in (10, 30, 50, 100, 200, 500, 1000):
            for v in VALUES:
                clock, data, end = spiky(v, width)
                self.assertEqual(decode(self.prog, self.div, clock, data, end), [v],
                                 f"byte {v:#04x}, {width} ns spikes")

    def test_spikes_on_a_nominal_bus_are_ignored(self):
        for v in VALUES:
            clock, data, end = spiky(v, 500, t_s=60 * US, t_v=60 * US)
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v])

    def test_edge_ringing_is_ignored(self):
        for v in range(256):
            clock, data, end = ringing(v)
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v], hex(v))

    def test_a_spike_at_every_point_of_a_bit(self):
        """One 300 ns spike, slid in 250 ns steps across a whole bit cell of the
        shortest legal timing (bit 3), in both directions by construction."""
        v = 0xA5
        clock0, data, end, edges = talker(v, t_s=20 * US, t_v=20 * US)
        cell_start, cell_end = edges[5], edges[7]          # bit 3: low phase + high phase
        t = cell_start + 100
        while t < cell_end - 400:
            clock = xor_overlay(clock0, [(t, 300)])
            self.assertEqual(decode(self.prog, self.div, clock, data, end), [v],
                             f"spike at {t - cell_start:.0f} ns into the bit")
            t += 250

    def test_the_filter_has_a_limit(self):
        """A 4 us pulse is a real edge to any decoder that must also accept
        20 us bits -- documented, and proof these tests can fail."""
        v = 0x55                                                   # a duplicated bit shows
        clock0, data, end, edges = talker(v, t_s=20 * US, t_v=20 * US)
        clock = xor_overlay(clock0, [(edges[0] + 8 * US, 4 * US)])  # inside bit 0's high phase
        self.assertNotEqual(decode(self.prog, self.div, clock, data, end), [v])


class TestTheSource(unittest.TestCase):
    def test_it_fits_beside_the_transmit_program(self):
        """A PIO block holds 32 instructions; iec_tx takes 21."""
        (instrs, _, _), _, _ = current_program()
        tx = parse_tx_length()
        self.assertLessEqual(len(instrs) + tx, 32, f"rx {len(instrs)} + tx {tx}")

    def test_jmp_pin_is_clock(self):
        _, _, text = current_program()
        self.assertRegex(text, r"sm_config_set_jmp_pin\(&c,\s*clock_pin\)")


def parse_tx_length():
    text = (ROOT / "src" / "pico" / "iec_tx.pio").read_text()
    body = text.split(".program iec_tx", 1)[1].split("% c-sdk", 1)[0]
    n = 0
    for line in body.splitlines():
        line = line.split(";", 1)[0].strip()
        if not line or line.startswith("."):
            continue
        if re.match(r"^\w+:\s*$", line):
            continue
        n += 1
    return n


if __name__ == "__main__":
    unittest.main(verbosity=1)
