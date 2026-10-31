#!/usr/bin/env python3
"""Tests for primo_basic.py -- the host tokenizer/detokenizer.

Run: python3 tools/primo/test_primo_basic.py

The ground truth is a file a real Primo B saved (reference/detect-tokenized.prg)
and, when the fetched library is present, every BASIC program in it: each line
is detokenized and tokenized again and must come back byte for byte.
"""
import glob
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import primo_basic as pb
import ptp2disk

REFERENCE = os.path.join(HERE, "reference", "detect-tokenized.prg")
LIBRARY = os.path.join(HERE, "..", "..", "primo-library", "ptp")


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def spit(path, text):
    with open(path, "w") as f:
        f.write(text)


def tok(word):
    return bytes([0x80 + pb.KEYWORDS.index(word)])


class TestTable(unittest.TestCase):
    def test_size_and_known_tokens(self):
        self.assertEqual(len(pb.KEYWORDS), 0xFB - 0x80 + 1)
        # the tokens lists, and two the reference file proves
        for code, word in {0x88: "DATA", 0x95: "ELSE", 0xA2: "OPEN", 0xAE: "SCREEN", 0xBC: "TAB(",
                           0xD3: "OR", 0xD4: ">", 0xD5: "=", 0xD6: "<", 0xE5: "PEEK"}.items():
            self.assertEqual(pb.KEYWORDS[code - 0x80], word, hex(code))

    def test_program_base_matches_the_converter(self):
        self.assertEqual(pb.BASIC_PROGRAM_BASE, ptp2disk.BASIC_PROGRAM_BASE)


class TestTokenizeText(unittest.TestCase):
    def test_keywords_and_plain_text(self):
        self.assertEqual(pb.tokenize_text('PRINT "A"'), tok("PRINT") + b' "A"')
        self.assertEqual(pb.tokenize_text("IF A=1 THEN 20"),
                         tok("IF") + b" A" + tok("=") + b"1 " + tok("THEN") + b" 20")

    def test_strings_are_verbatim(self):
        self.assertEqual(pb.tokenize_text('PRINT "IF THEN"'), tok("PRINT") + b' "IF THEN"')

    def test_rem_and_data_are_verbatim(self):
        self.assertEqual(pb.tokenize_text("REM FOR IF"), tok("REM") + b" FOR IF")
        self.assertEqual(pb.tokenize_text("DATA 1,IF,3:END"), tok("DATA") + b" 1,IF,3:" + tok("END"))
        self.assertEqual(pb.tokenize_text('DATA "A:B",2:END'), tok("DATA") + b' "A:B",2:' + tok("END"))

    def test_quote_shorthand_is_stored_as_colon_rem_quote(self):
        self.assertEqual(pb.tokenize_text("A=1 'HI"), b"A" + tok("=") + b"1 " + bytes([0x3A, 0x80 + pb.KEYWORDS.index("REM"), 0xFB]) + b"HI")

    def test_only_upper_case_is_tokenized(self):
        self.assertEqual(pb.tokenize_text("print"), b"print")

    def test_no_word_boundary_check(self):
        self.assertEqual(pb.tokenize_text("TOTAL"), tok("TO") + b"TAL")

    def test_caret_is_not_the_power_token(self):
        self.assertEqual(pb.tokenize_text("A^2"), b"A^2")

    def test_detokenize(self):
        self.assertEqual(pb.detokenize_bytes(pb.tokenize_text("FOR I=1 TO 9")), "FOR I=1 TO 9")


class TestSource(unittest.TestCase):
    def test_host_comments_blank_lines_and_spacing(self):
        lines = pb.parse_source("# a host comment\n\n10 PRINT 1\n  20    END  \n")
        self.assertEqual(lines, [(10, "PRINT 1"), (20, "END")])

    def test_line_numbers_must_ascend(self):
        with self.assertRaises(pb.BasicError):
            pb.parse_source("20 END\n10 END\n")

    def test_line_needs_a_number(self):
        with self.assertRaises(pb.BasicError):
            pb.parse_source("PRINT 1\n")

    def test_line_length_limit_counts_the_number(self):
        ok = "A=" + "1" * (pb.MAX_LINE - len("10 A="))
        self.assertEqual(len(pb.parse_source("10 " + ok)), 1)
        with self.assertRaises(pb.BasicError):
            pb.parse_source("10 " + ok + "1")


class TestContainer(unittest.TestCase):
    def test_pointers_are_absolute(self):
        payload = pb.build_payload([(10, "END"), (20, "END")], base=0x43EA)
        end = tok("END")
        self.assertEqual(payload, bytes([0xF0, 0x43, 10, 0]) + end + b"\x00" + bytes([0xF6, 0x43, 20, 0]) + end + b"\x00\x00\x00")

    def test_prg_wraps_one_basic_block(self):
        payload = pb.build_payload([(10, "END")])
        prg = pb.build_prg(payload)
        self.assertEqual(prg[:3], bytes([0xD1, 0, 0]))
        self.assertEqual(prg[3] | prg[4] << 8, len(payload))
        self.assertEqual(prg[-1], 0xC9)
        self.assertEqual(pb.prg_payload(prg), payload)

    def test_prg_payload_rejects_other_files(self):
        with self.assertRaises(pb.BasicError):
            pb.prg_payload(bytes([0xD9, 0, 0, 1, 0, 0, 0xC9]))

    def test_payload_lines_survive_truncation(self):
        payload = pb.build_payload([(10, "END"), (20, "END")])
        self.assertEqual([n for n, _ in pb.payload_lines(payload[:-4])], [10])


class TestGroundTruth(unittest.TestCase):
    def test_saved_by_a_real_primo_round_trips(self):
        prg = slurp(REFERENCE)
        lines = [(n, pb.to_source(t)) for n, t in pb.payload_lines(pb.prg_payload(prg))]
        self.assertEqual(len(lines), 14)
        self.assertEqual(pb.build_prg(pb.build_payload(lines)), prg)

    @unittest.skipUnless(os.path.isdir(LIBRARY), "fetched library not present (tools/primo/fetch-library.sh)")
    def test_library_lines_round_trip(self):
        total, bad = 0, set()
        for path in sorted(glob.glob(os.path.join(LIBRARY, "**", "*.ptp"), recursive=True)):
            try:
                _, _, blocks, _, _ = ptp2disk.parse_ptp(slurp(path))
            except Exception:
                continue
            for btype, _, payload in blocks:
                if btype != 0xF1:
                    continue
                for _, toks in pb.payload_lines(payload):
                    total += 1
                    if pb.tokenize_text(pb.to_source(toks)) != toks:
                        bad.add(os.path.basename(path))
        self.assertGreater(total, 30000)            # 36,777 lines when this was written
        # the three known exceptions, one line each: a junk CR line, a program whose author
        # typed F OR Z and the ROM stored it without the space, and a control-character hack
        self.assertEqual(bad, {"cmd_leiras.ptp", "mo_varosai.ptp", "leonidas.ptp"})


class TestLint(unittest.TestCase):
    def lint(self, src):
        return pb.lint(pb.parse_source(src))

    def test_clean_program(self):
        self.assertEqual(self.lint('10 FOR I=1 TO 3: PRINT "TOTAL";I: NEXT I\n20 GOTO 10\n'), [])

    def test_variable_containing_a_keyword_is_flagged(self):
        problems = self.lint("10 TOTAL=1\n")
        self.assertEqual(len(problems), 1)
        self.assertIn("TO+TAL", problems[0])

    def test_keyword_with_channel_mark_is_not_flagged(self):
        self.assertEqual(self.lint('10 CMD PRINT#2,I\n20 CMD INPUT#2,A\n'), [])
        self.assertIn("TO+TAL", self.lint("10 TOTAL#=1\n")[0])   # negative control

    def test_two_character_collision(self):
        self.assertTrue(any("AB" in p for p in self.lint("10 AB=1: ABC=2\n")))
        self.assertEqual(self.lint("10 A$=\"X\": A=1\n"), [])      # a $ variable is another variable

    def test_dangling_targets(self):
        problems = self.lint("10 GOTO 99\n20 IF A=1 THEN 98\n30 ON ERROR GOTO 97\n")
        self.assertEqual(len(problems), 3)
        self.assertEqual(self.lint("10 ON ERROR GOTO 0\n"), [])

    def test_strings_and_comments_are_not_linted(self):
        self.assertEqual(self.lint('10 PRINT "TOTAL GOTO 99"\n20 REM TOTAL GOTO 99\n'), [])


class TestCli(unittest.TestCase):
    def run_cli(self, *args):
        return subprocess.run([sys.executable, os.path.join(HERE, "primo_basic.py"), *args],
                              capture_output=True, text=True)

    def test_tokenize_then_list(self):
        with tempfile.TemporaryDirectory() as d:
            src, out = os.path.join(d, "a.bas"), os.path.join(d, "A.PRG")
            spit(src, "10 PRINT \"HI\"\n20 END\n")
            r = self.run_cli("tokenize", src, out)
            self.assertEqual(r.returncode, 0, r.stderr)
            listing = self.run_cli("list", out)
            self.assertEqual(listing.stdout.strip().splitlines(), ['10 PRINT "HI"', "20 END"])

    def test_upload_refuses_a_program_lint_objects_to(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.bas")
            spit(src, "10 GOTO 99\n")
            r = self.run_cli("upload", src, "A", "127.0.0.1")
            self.assertEqual(r.returncode, 1)
            self.assertIn("does not exist", r.stderr)


if __name__ == "__main__":
    unittest.main()
