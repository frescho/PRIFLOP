"""a32fix: the 32 KB programs' address arithmetic made signed, nothing else."""
import os
import unittest

import a32fix
import build_library
import primo_basic
import ptp2disk

LIB = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))), "primo-library")


class TestFixText(unittest.TestCase):
    def test_the_screen_address_becomes_signed(self):
        self.assertEqual(a32fix.fix_text("C=256*PEEK(16562)+PEEK(16561)"),
                         "C=(PEEK(16562)-256)*256+PEEK(16561)")
        self.assertEqual(a32fix.fix_text("Q=PEEK(16442)*256+PEEK(16441)"),
                         "Q=(PEEK(16442)-256)*256+PEEK(16441)")

    def test_only_the_high_byte_argument_is_masked(self):
        self.assertEqual(a32fix.fix_text("POKE16561,C-256*INT(C/256),INT(C/256)"),
                         "POKE16561,C-256*INT(C/256),(INT(C/256)AND255)")

    def test_negative_control_other_text_is_left_alone(self):
        for t in ("A=256*PEEK(16000)+1", "PRINT INT(X/2)", 'PRINT"256*PEEK"', "X=INT(Y/256)"):
            self.assertEqual(a32fix.fix_text(t), t)


class TestFixPayload(unittest.TestCase):
    def test_unchanged_lines_keep_their_bytes_and_the_chain_is_relinked(self):
        src = primo_basic.parse_source('10 PRINT"HI"\n20 C=256*PEEK(16562)+PEEK(16561)\n30 END\n')
        payload = primo_basic.build_payload(src)
        out = a32fix.fix_payload(payload)
        old, new = primo_basic.payload_lines(payload), primo_basic.payload_lines(out)
        self.assertEqual(new[0], old[0])
        self.assertEqual(new[2], old[2])
        self.assertIn("(PEEK(16562)-256)*256", primo_basic.detokenize_bytes(new[1][1]))
        self.assertEqual(ptp2disk.relink_basic_lines(out), out)      # pointers already right

    @unittest.skipUnless(os.path.isdir(LIB), "primo-library not fetched")
    def test_only_the_named_programs_are_patched_in_the_library(self):
        files, _ = build_library.build(LIB)
        for path in ("JATEK/BANYASZ_BERCI.PRG", "JATEK/LOKOMOTIV.PRG"):
            self.assertIn("(PEEK(16562)-256)*256", primo_basic.detokenize_bytes(files[path]))
        # negative control: a program not on the list keeps 256*PEEK(16562) as it was
        others = [p for p, d in files.items() if p not in ("JATEK/BANYASZ_BERCI.PRG", "JATEK/LOKOMOTIV.PRG")
                  and "PEEK(16562)-256" in primo_basic.detokenize_bytes(d)]
        self.assertEqual(others, [])


if __name__ == "__main__":
    unittest.main()
