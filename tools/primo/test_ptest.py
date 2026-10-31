#!/usr/bin/env python3
"""Runs ptest.bas in basic_sim.py against a mock CDOS and breaks the drive in
every way the program claims to detect. Passing means the program's logic --
line targets, error handling, record arithmetic, cleanup -- does what its
comments say; it does not mean the Primo agrees (see the "unverified on
hardware" list).

Run: python3 tools/primo/test_ptest.py
"""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import basic_sim as bs
import primo_basic as pb

SOURCE = os.path.join(HERE, "ptest.bas")
PATTERN = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ" * 5


def source_lines():
    with open(SOURCE, encoding="latin-1") as f:
        return pb.parse_source(f.read())


def record(b):
    """What ptest.bas writes as record b (without the CR)."""
    s = (b * 5) % 36 + 1
    return f"{b:04d}" + PATTERN[s - 1:s - 1 + 123]


class Session:
    """One run of the program with a scripted keyboard."""

    def __init__(self, menu="\r", fail_keys="", hook=None, setup=None, lines=None):
        self.menu, self.fail_keys, self.hook = list(menu), list(fail_keys), hook
        self.prompts, self.prompt_screens, self.final, self.halt = 0, [], None, None
        self.unit_at_final = None
        self.sim = bs.Sim(lines or source_lines(), keys=self._keys)
        if setup:
            setup(self.sim)
        try:
            self.sim.run()
        except bs.Halt as h:
            self.halt = h

    def _keys(self, sim):
        text = sim.screen.text()
        if sim.cur_line == 1340:
            self.prompts += 1
            self.prompt_screens.append(text)
            return self.fail_keys.pop(0) if self.fail_keys else " "
        if sim.cur_line == 730:
            self.final = text
            self.unit_at_final = sim.cdos.unit
            return " "
        if "ENTER=START" in text:
            return self.menu.pop(0) if self.menu else "Q"
        return self.hook(sim) or "" if self.hook else ""

    # -- views
    @property
    def cdos(self):
        return self.sim.cdos

    def var(self, name):
        return self.sim.get_var(name)

    def closes(self):
        return [(u, n, size) for kind, u, n, size in (e for e in self.cdos.events if e[0] == "close")]

    # codes a drive can legitimately cause: FD, no/bad channel, device gone, DOS errors
    DRIVE_ERRORS = {42, 94, 160}

    def assert_only_drive_errors(self, tc):
        """The handler swallows everything, so a bug in the program (RETURN without GOSUB,
        syntax, type mismatch...) would look like a drive fault. Nothing else may occur."""
        odd = [e for e in self.sim.errors_seen if e[0] not in self.DRIVE_ERRORS and e[0] < 190]
        tc.assertEqual(odd, [], "the program itself raised errors")

    def assert_clean_exit(self, tc):
        self.assert_only_drive_errors(tc)
        tc.assertIsNone(self.halt)
        tc.assertEqual(self.cdos.chan, {}, "a channel was left open")
        tc.assertEqual(self.cdos.drives[8], {}, "files left on drive 8")
        tc.assertEqual(self.cdos.drives[9], {}, "files left on drive 9")
        tc.assertEqual(self.cdos.unit, 8, "CMD SET 8 was not restored")


class TestProgram(unittest.TestCase):
    def test_lint_is_clean_and_it_fits(self):
        lines = source_lines()
        self.assertEqual(pb.lint(lines), [])
        prg = pb.build_prg(pb.build_payload(lines))
        # BASIC has about 9 KB (CLEAR's documented maximum is 9450 with nothing else in
        # memory), of which CLEAR 2000 takes 2000 for strings
        self.assertLess(len(prg), 4600)
        self.assertLessEqual(max(len(str(n)) + 1 + len(t) for n, t in lines), pb.MAX_LINE)

    def test_avoids_features_the_manual_does_not_document(self):
        prg = pb.build_prg(pb.build_payload(source_lines()))
        used = {b for _, toks in pb.payload_lines(pb.prg_payload(prg)) for b in toks}
        for word in ("ELSE", "BEEP", "TIME$", "USING", "'"):
            self.assertNotIn(0x80 + pb.KEYWORDS.index(word), used, word)

    def test_runs_the_same_from_the_tokenized_file(self):
        prg = pb.build_prg(pb.build_payload(source_lines()))
        a, b = Session(), Session(lines=bs.load_prg(prg))
        self.assertEqual(a.closes(), b.closes())
        self.assertEqual(a.final, b.final)


class TestDefaultRun(unittest.TestCase):
    def setUp(self):
        self.s = Session()

    def test_passes_and_cleans_up(self):
        self.assertEqual(self.s.sim.errors_seen, [])
        self.s.assert_clean_exit(self)
        self.assertEqual(self.s.var("FC"), 0)
        self.assertEqual(self.s.prompts, 0)

    def test_three_files_of_16k_and_the_binary_file(self):
        self.assertEqual(self.s.closes(), [(9, "PT1", 16384), (9, "PT2", 16384), (9, "PT3", 16384), (9, "PTB", 512)])

    def test_log(self):
        lines = self.s.final.splitlines()
        for n in (1, 2, 3):
            self.assertIn(f"PT{n} W........ R........ OK", lines)
        for expected in ("ROUND 1: 3/ 3 OK, FAIL 0", "SCREEN OK", "PTB BIN OK", "TEST COMPLETE  FAILURES 0", "ANY KEY FOR MENU"):
            self.assertIn(expected, lines)
        self.assertEqual(lines[0], "PTEST DRIVE 9  SIZE 16K FILES 3")

    def test_drive_8_is_selected_again_before_the_menu_returns(self):
        self.assertEqual(self.s.unit_at_final, 8)

    def test_the_screen_test_never_scrolls_the_pattern(self):
        self.assertEqual(self.s.sim.screen.scrolls, 0)

    def test_string_space_stays_inside_what_clear_reserves(self):
        # live strings only; temporaries come on top but the program CLEARs 2000
        self.assertLess(self.s.sim.max_strings, 1200)

    def test_records_are_what_the_comments_say(self):
        self.assertEqual(len(record(1)), 127)
        self.assertNotEqual(record(1), record(2))
        captured = {}

        def on_close(unit, name, data):
            captured[name] = data
            return data
        Session(menu="56\r", setup=lambda sim: setattr(sim.cdos, "on_close", on_close))
        self.assertEqual(captured["PT1"], "".join(record(b) + "\r" for b in range(1, 129)).encode())


class TestMenu(unittest.TestCase):
    def test_initial_menu(self):
        seen = []

        def keys(sim):
            seen.append(sim.screen.text())
            return "Q"
        bs.Sim(source_lines(), keys=keys).run()
        self.assertEqual(seen[0].splitlines(), [
            "PTEST - PRIMO DRIVE TEST", "",
            "1 DRIVE ..... 9", "2 FILE SIZE . 16K", "3 FILES ..... 3", "4 REPEAT .... 1",
            "5 SCREEN .... ON", "6 BINARY .... ON", "", "ENTER=START  Q=QUIT", "FREE 5000"])

    def test_q_quits_from_the_menu_and_restores_drive_8(self):
        s = Session(menu="Q")
        self.assertIsNone(s.halt)
        self.assertEqual(s.cdos.unit, 8)
        self.assertEqual(s.closes(), [])

    def test_keys_change_the_settings(self):
        s = Session(menu="1234Q")
        self.assertEqual([s.var(v) for v in ("DV", "SZ", "NF", "RI", "RP")], [8, 2, 4, 1, 3])

    def test_cycles(self):
        def after(keys, var):
            s = Session(menu=keys + "Q")
            return s.var(var)
        self.assertEqual([after("2" * n, "SZ") for n in range(5)], [1, 2, 3, 0, 1])
        self.assertEqual([after("3" * n, "NF") for n in range(6)], [3, 4, 5, 1, 2, 3])
        self.assertEqual([after("4" * n, "RP") for n in range(5)], [1, 3, 5, 0, 1])
        self.assertEqual([after("1" * n, "DV") for n in range(3)], [9, 8, 9])
        self.assertEqual([after("5" * n, "SC") for n in range(3)], [1, 0, 1])
        self.assertEqual([after("6" * n, "BN") for n in range(3)], [1, 0, 1])

    def test_drive_8_is_used(self):
        s = Session(menu="1\r")
        self.assertEqual({u for u, _, _ in s.closes()}, {8})
        s.assert_clean_exit(self)

    def test_sizes(self):
        self.assertEqual({sz for _, n, sz in Session(menu="2\r").closes() if n.startswith("PT") and n != "PTB"}, {24576})
        self.assertEqual({sz for _, n, sz in Session(menu="222\r").closes() if n != "PTB"}, {8192})
        # MIX depends on round and file number: round 1 -> 24K, 8K, 16K
        mix = [sz for _, n, sz in Session(menu="22\r").closes() if n != "PTB"]
        self.assertEqual(mix, [24576, 8192, 16384])

    def test_file_count_and_rounds(self):
        s = Session(menu="33356\r")             # 1 file (three presses wrap 3 -> 1), no screen/binary test
        self.assertEqual([n for _, n, _ in s.closes()], ["PT1"])
        s = Session(menu="4456\r")              # 5 rounds of 3 files
        self.assertEqual(len(s.closes()), 15)
        self.assertIn("ROUND 5: 3/ 3 OK, FAIL 0", s.final)

    def test_screen_and_binary_tests_can_be_switched_off(self):
        s = Session(menu="56\r")
        self.assertNotIn("SCREEN OK", s.final)
        self.assertNotIn("PTB BIN OK", s.final)
        self.assertEqual(s.cdos.tests, 0)

    def test_files_are_removed_at_the_end_of_each_round(self):
        seen = []

        def hook(sim):
            c = sim.cdos.chan.get(3)
            if sim.get_var("RN") == 2 and c and c["mode"] == "w" and c["name"] == "PT1" and not seen:
                seen.append(set(sim.cdos.drives[9]))
            return ""
        Session(menu="4\r", hook=hook)
        self.assertEqual(seen, [set()])

    def test_infinite_repeat_stops_on_q(self):
        def hook(sim):
            return "Q" if sim.get_var("RN") >= 3 and 3 in sim.cdos.chan else ""
        s = Session(menu="444\r", hook=hook)
        self.assertEqual(s.var("RP"), 0)
        self.assertEqual(s.var("RN"), 3)
        self.assertIn("STOPPED  FAILURES 0", s.final)
        s.assert_clean_exit(self)


class TestFileFaults(unittest.TestCase):
    """Each fault is injected on drive 9, file PT2 unless said otherwise."""

    def flip(self, offset, name="PT2"):
        def on_close(unit, n, data):
            if n != name:
                return data
            return data[:offset] + bytes([data[offset] ^ 1]) + data[offset + 1:]
        return lambda sim: setattr(sim.cdos, "on_close", on_close)

    def test_a_flipped_byte_is_located_to_the_character(self):
        s = Session(setup=self.flip(5000))
        rec, pos = 5000 // 128 + 1, 5000 % 128 + 1
        want = ord(record(rec)[pos - 1])
        shown = s.prompt_screens[0].splitlines()
        self.assertIn(f"*** PT2 DATA REC {rec} POS {pos}", shown)
        self.assertIn(f"    EXP {want} GOT {want ^ 1} LEN 127", shown)
        self.assertEqual(s.prompts, 1)
        self.assertEqual(s.var("FC"), 1)
        self.assertIn("ROUND 1: 2/ 3 OK, FAIL 1", s.final)
        self.assertIn("TEST COMPLETE  FAILURES 1", s.final)
        s.assert_clean_exit(self)                                 # the bad file was removed

    def test_the_run_goes_on_after_a_key_and_stops_on_q(self):
        s = Session(setup=self.flip(5000))
        self.assertIn("PT3", [n for _, n, _ in s.closes()])
        s = Session(setup=self.flip(5000), fail_keys="Q")
        self.assertNotIn("PT3", [n for _, n, _ in s.closes()])
        self.assertIn("STOPPED  FAILURES 1", s.final)
        s.assert_clean_exit(self)

    def test_the_first_and_last_bytes_of_a_file(self):
        for offset in (0, 16383):
            s = Session(setup=self.flip(offset))
            self.assertEqual(s.var("FC"), 1, offset)
            self.assertTrue(any(l.startswith("*** PT2 DATA REC") for l in s.prompt_screens[0].splitlines()), offset)

    def test_short_file_at_a_record_boundary(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", lambda u, n, d: d[:1024] if n == "PT2" else d))
        self.assertIn("*** PT2 SHORT: EOF AT REC 9", s.prompt_screens[0].splitlines())

    def test_short_file_inside_a_record(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", lambda u, n, d: d[:1000] if n == "PT2" else d))
        self.assertIn("*** PT2 DATA REC 8 POS 105", s.prompt_screens[0].splitlines())

    def test_extra_data_after_the_last_record(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", lambda u, n, d: d + b"XYZ\r" if n == "PT2" else d))
        self.assertIn("*** PT2 LONG: DATA AFTER REC 128", s.prompt_screens[0].splitlines())

    def test_a_shifted_record_is_not_mistaken_for_a_good_one(self):
        def swap(unit, n, data):
            if n != "PT2":
                return data
            a, b = data[:128], data[128:256]
            return b + a + data[256:]
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", swap))
        self.assertEqual(s.var("FC"), 1)

    def test_the_drive_stops_answering_during_a_write(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "fail_write_after", 3000))
        self.assertEqual(s.var("FC"), 3)                          # all three files
        shown = s.prompt_screens[0].splitlines()
        self.assertIn("*** PT1 ERR 160 LINE 1020", shown)
        self.assertIn("    DEVICE NOT PRESENT", shown)
        self.assertEqual(s.cdos.failed_writes, 3)                 # one attempt per file, then the file is given up
        self.assertIn("ROUND 1: 0/ 3 OK, FAIL 3", s.final)
        self.assertIn("SCREEN OK", s.final)                       # the other tests do not depend on PRINT#
        s.assert_clean_exit(self)

    def test_disk_full_reported_only_through_the_error_channel(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "close_silent_status", (72, "DISK FULL")))
        shown = s.prompt_screens[0].splitlines()
        self.assertIn("*** PT1 WRITE: DRIVE ERROR 72", shown)
        self.assertIn("    DISK FULL", shown)
        self.assertEqual(s.var("FC"), 4)                          # three files and the binary test
        self.assertTrue(any("*** PTB WRITE: DRIVE ERROR 72" in p for p in s.prompt_screens))
        s.assert_clean_exit(self)

    def test_a_dos_error_raised_by_the_system_on_create(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "create_error", 74))
        shown = s.prompt_screens[0].splitlines()
        self.assertIn("*** PT1 ERR 264 LINE 1010", shown)
        self.assertIn("    DRIVE ERROR 74", shown)
        s.assert_clean_exit(self)

    def test_status_73_is_not_a_failure(self):
        # 73 is the drive's "DOS version" greeting, not an error (errormsg.c treats it the same way)
        s = Session(setup=lambda sim: setattr(sim.cdos, "close_silent_status", (73, "CBM DOS V2.6 1541")))
        self.assertEqual(s.var("FC"), 0)
        s.assert_clean_exit(self)

    def test_a_drive_that_is_not_there(self):
        s = Session(setup=lambda sim: sim.cdos.absent.add(9))
        self.assertIsNone(s.halt)
        self.assertEqual(s.var("FC"), 5)                          # three files, screen, binary
        self.assertTrue(all("ERR 160" in p for p in s.prompt_screens[:1]))
        self.assertEqual(s.cdos.unit, 8)

    def test_a_stale_file_from_an_earlier_run_is_replaced(self):
        def setup(sim):
            sim.cdos.drives[9]["PT1"] = ("seq", b"junk\r")
            sim.cdos.drives[9]["PTB"] = ("seq", b"junk")
            sim.cdos.drives[9]["PTS1"] = ("scr", " " * 672)
        s = Session(setup=setup)
        self.assertEqual(s.var("FC"), 0)
        s.assert_clean_exit(self)


class TestScreenFaults(unittest.TestCase):
    @staticmethod
    def corrupt(snap):
        i = 100
        return snap[:i] + ("#" if snap[i] != "#" else "@") + snap[i + 1:]

    def test_screen_saved_wrongly_halts_with_the_systems_count(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_save_screen", self.corrupt))
        self.assertIsNotNone(s.halt)
        self.assertEqual((s.halt.code, s.halt.line, s.halt.count), (42, 2820, 1))

    def test_screen_loaded_wrongly_halts_with_the_systems_count(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_load_screen", self.corrupt))
        self.assertEqual((s.halt.code, s.halt.line, s.halt.count), (42, 2820, 1))

    def test_a_difference_that_does_not_repeat_is_reported_and_the_run_goes_on(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_test", lambda diff, call: 1 if call == 1 else diff))
        self.assertIsNone(s.halt)
        shown = s.prompt_screens[0].splitlines()
        self.assertIn("*** PTS1 DIFF AFTER LOAD", shown)
        self.assertIn("    RETRY PASSED", shown)
        self.assertIn("PTB BIN OK", s.final)                      # the binary test still ran
        s.assert_clean_exit(self)

    def test_save_screen_error_is_reported(self):
        def setup(sim):
            sim.cdos.absent.add(9)
        s = Session(setup=setup, menu="\r")
        self.assertTrue(any("*** PTS1 ERR 160 LINE 2010" in p for p in s.prompt_screens))


class TestBinaryFaults(unittest.TestCase):
    def test_a_translated_byte_is_named(self):
        def on_close(unit, n, data):
            return data.replace(b"\r", b"\n", 1) if n == "PTB" else data
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", on_close))
        shown = [l for l in s.prompt_screens[0].splitlines() if l.startswith("*** PTB")]
        self.assertEqual(shown, ["*** PTB BYTE 13 EXP 13 GOT 10"])

    def test_missing_and_extra_bytes(self):
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", lambda u, n, d: d[:300] if n == "PTB" else d))
        self.assertIn("*** PTB SHORT AT BYTE 300", s.prompt_screens[0].splitlines())
        s = Session(setup=lambda sim: setattr(sim.cdos, "on_close", lambda u, n, d: d + b"\x00" if n == "PTB" else d))
        self.assertIn("*** PTB LONG: DATA AFTER BYTE 511", s.prompt_screens[0].splitlines())

    def test_a_get_that_returns_nothing_for_a_zero_byte_is_accepted(self):
        # C64 BASIC's GET# yields "" for CHR$(0); if CDOS does the same the test must not cry wolf
        original = bs.Cdos.get_ch

        def get_ch(self, n):
            c = original(self, n)
            return "" if c == "\x00" else c
        bs.Cdos.get_ch = get_ch
        try:
            s = Session()
        finally:
            bs.Cdos.get_ch = original
        self.assertEqual(s.var("FC"), 0)


class TestAbort(unittest.TestCase):
    def test_q_while_writing(self):
        def hook(sim):
            c = sim.cdos.chan.get(3)
            return "Q" if c and c["mode"] == "w" and c["sent"] > 1000 else ""
        s = Session(hook=hook)
        self.assertEqual(s.prompts, 0)
        self.assertEqual(s.var("FC"), 0)
        self.assertIn("STOPPED  FAILURES 0", s.final)
        s.assert_clean_exit(self)

    def test_q_while_reading_leaves_earlier_files_to_be_cleaned_up(self):
        def hook(sim):
            c = sim.cdos.chan.get(3)
            return "Q" if c and c["mode"] == "r" and c["name"] == "PT2" and c["pos"] > 1000 else ""
        s = Session(hook=hook)
        self.assertIn("STOPPED  FAILURES 0", s.final)
        self.assertNotIn("PT3", [n for _, n, _ in s.closes()])
        s.assert_clean_exit(self)                                 # PT1 was scratched on the way out


if __name__ == "__main__":
    unittest.main()
