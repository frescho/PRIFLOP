#!/usr/bin/env python3
"""Tests for basic_sim.py -- the interpreter test_ptest.py relies on. If these
are wrong, a green PTEST run means nothing, so the Microsoft-BASIC rules the
program depends on are pinned down here with tiny programs.

Run: python3 tools/primo/test_basic_sim.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import basic_sim as bs
import primo_basic as pb


def run(src, keys=None, setup=None):
    sim = bs.Sim(pb.parse_source(src), keys=keys)
    if setup:
        setup(sim)
    sim.run()
    return sim


def out(src, **kw):
    return run(src, **kw).screen.text()


class TestStatements(unittest.TestCase):
    def test_false_if_skips_the_rest_of_the_line(self):
        self.assertEqual(out('10 A=0: IF A THEN PRINT "X": PRINT "Y"\n20 PRINT "Z"'), "Z")
        self.assertEqual(out('10 A=1: IF A THEN PRINT "X": PRINT "Y"\n20 PRINT "Z"'), "X\nY\nZ")

    def test_nested_if_false_skips_the_line(self):
        self.assertEqual(out('10 A=1: B=0\n20 IF A THEN IF B THEN PRINT "X": PRINT "Y"\n30 PRINT "Z"'), "Z")

    def test_if_then_line_number(self):
        self.assertEqual(out('10 IF 1 THEN 30\n20 PRINT "NO"\n30 PRINT "YES"'), "YES")

    def test_for_next_and_step(self):
        self.assertEqual(out("10 FOR I=1 TO 3: PRINT I;: NEXT I"), " 1  2  3")
        self.assertEqual(out("10 FOR I=3 TO 1 STEP -1: PRINT I;: NEXT I"), " 3  2  1")

    def test_return_discards_for_frames_opened_after_gosub(self):
        src = '10 GOSUB 100\n20 PRINT "BACK"\n30 END\n100 FOR I=1 TO 5\n110 IF I=2 THEN RETURN\n120 NEXT I\n'
        self.assertEqual(out(src), "BACK")

    def test_for_on_a_variable_in_use_replaces_its_frame(self):
        src = "10 FOR I=1 TO 3\n20 FOR J=1 TO 2\n30 IF J=1 THEN 60\n40 NEXT J\n50 NEXT I\n60 FOR I=1 TO 2: PRINT I;: NEXT I"
        self.assertEqual(out(src), " 1  2")

    def test_empty_for_loop_is_refused_not_guessed(self):
        with self.assertRaises(AssertionError):
            run("10 FOR I=1 TO 0: NEXT I")

    def test_print_semicolon_and_numbers(self):
        self.assertEqual(out('10 PRINT "A";1;"B"'), "A 1 B")
        self.assertEqual(out('10 PRINT "A";: PRINT "B"'), "AB")
        self.assertEqual(out("10 PRINT -5"), "-5")

    def test_end_stops(self):
        self.assertEqual(out('10 PRINT "A": END\n20 PRINT "B"'), "A")


class TestExpressions(unittest.TestCase):
    def value(self, expr, var="X"):
        return run(f"10 {var}={expr}").get_var(var)

    def test_relations_are_minus_one_and_zero(self):
        self.assertEqual(self.value("3>2"), -1)
        self.assertEqual(self.value("3<2"), 0)
        self.assertEqual(self.value('"A"<"B"'), -1)

    def test_not_and_or_are_bitwise_on_minus_one(self):
        self.assertEqual(self.value("NOT (1=1)"), 0)
        self.assertEqual(self.value("NOT (1=2)"), -1)
        self.assertEqual(self.value("(1=1) OR (1=2)"), -1)
        self.assertEqual(self.value("(1=1) AND (1=2)"), 0)

    def test_precedence(self):
        self.assertEqual(self.value("2+3*4"), 14)
        self.assertEqual(self.value("-2+5"), 3)
        self.assertEqual(self.value("10-4-3"), 3)
        self.assertEqual(self.value("1+1=2 AND 2>1"), -1)

    def test_str_has_a_leading_space_for_non_negatives(self):
        self.assertEqual(self.value("STR$(5)", "X$"), " 5")
        self.assertEqual(self.value("STR$(-5)", "X$"), "-5")
        self.assertEqual(self.value("RIGHT$(STR$(10005),4)", "X$"), "0005")

    def test_string_functions(self):
        self.assertEqual(self.value('MID$("ABCDEF",2,3)', "X$"), "BCD")
        self.assertEqual(self.value('MID$("ABC",5,2)', "X$"), "")
        self.assertEqual(self.value('MID$("ABCDEF",4)', "X$"), "DEF")
        self.assertEqual(self.value('LEFT$("ABC",2)', "X$"), "AB")
        self.assertEqual(self.value('RIGHT$("ABC",2)', "X$"), "BC")
        self.assertEqual(self.value('CHR$(65)+"B"', "X$"), "AB")
        self.assertEqual(self.value('ASC("A")'), 65)
        self.assertEqual(self.value('LEN("ABC")'), 3)
        self.assertEqual(self.value('VAL("12")+1'), 13)

    def test_int_floors(self):
        self.assertEqual(self.value("INT(7/2)"), 3)
        self.assertEqual(self.value("INT(-7/2)"), -4)

    def test_variables_are_significant_to_two_characters(self):
        sim = run("10 ABC=1: ABD=2")
        self.assertEqual(sim.get_var("ABC"), 2)
        self.assertEqual(run("10 A$=\"X\": A=1").get_var("A$"), "X")


class TestErrors(unittest.TestCase):
    def test_unhandled_error_halts_with_code_and_line(self):
        with self.assertRaises(bs.Halt) as cm:
            run('10 A=ASC("")')
        self.assertEqual((cm.exception.code, cm.exception.line), (bs.ERR_FC, 10))

    def test_handler_records_err_and_erl_and_resume_next_stays_on_the_line(self):
        src = ('10 ON ERROR GOTO 900\n20 A=ASC(""): B=7: PRINT "AFTER"\n30 PRINT EC;EL\n40 END\n'
               '900 EC=ERR: EL=ERL: RESUME NEXT\n')
        sim = run(src)
        self.assertEqual(sim.screen.text().splitlines(), ["AFTER", " 8  20"])
        self.assertEqual(sim.get_var("B"), 7)

    def test_resume_next_at_the_end_of_a_line_goes_to_the_next_line(self):
        src = '10 ON ERROR GOTO 900\n20 A=ASC("")\n30 PRINT "OK"\n40 END\n900 RESUME NEXT\n'
        self.assertEqual(out(src), "OK")

    def test_error_inside_an_if_body_resumes_after_the_if_statement(self):
        src = '10 ON ERROR GOTO 900\n20 IF 1 THEN A=ASC(""): PRINT "SECOND"\n30 END\n900 RESUME NEXT\n'
        self.assertEqual(out(src), "SECOND")

    def test_on_error_goto_zero_disables_the_handler(self):
        src = '10 ON ERROR GOTO 900\n20 ON ERROR GOTO 0\n30 A=ASC("")\n900 RESUME NEXT\n'
        with self.assertRaises(bs.Halt):
            run(src)

    def test_error_inside_the_handler_is_fatal(self):
        with self.assertRaises(bs.Halt):
            run('10 ON ERROR GOTO 900\n20 A=ASC("")\n30 END\n900 B=ASC(""): RESUME NEXT\n')

    def test_string_over_255_is_an_error(self):
        with self.assertRaises(bs.Halt) as cm:
            run('10 A$="0123456789"\n20 A$=A$+A$+A$+A$+A$+A$\n30 A$=A$+A$+A$+A$+A$+A$')
        self.assertEqual(cm.exception.code, bs.ERR_LS)

    def test_undefined_line_and_type_mismatch(self):
        with self.assertRaises(bs.Halt) as cm:
            run("10 GOTO 99")
        self.assertEqual(cm.exception.code, bs.ERR_UL)
        with self.assertRaises(bs.Halt) as cm:
            run('10 A=1+"X"')
        self.assertEqual(cm.exception.code, bs.ERR_TM)

    def test_unknown_syntax_is_a_syntax_error_not_a_guess(self):
        with self.assertRaises(bs.Halt) as cm:
            run("10 FROBNICATE 3")
        self.assertEqual(cm.exception.code, bs.ERR_SN)


class TestCdos(unittest.TestCase):
    def test_write_then_read_back_lines(self):
        src = ('10 CMD SET 9: CMD CREATE 3,"F": CMD PRINT #3,"ONE": CMD PRINT #3,"TWO": CMD CLOSE 3\n'
               '20 CMD OPEN 3,"F": CMD INPUT #3,A$: CMD INPUT #3,B$: PRINT A$;B$;EOF(3): CMD CLOSE 3')
        sim = run(src)
        self.assertEqual(sim.screen.text(), "ONETWO-1")
        self.assertEqual(sim.cdos.drives[9]["F"], ("seq", b"ONE\rTWO\r"))

    def test_semicolon_suppresses_the_cr_and_chr_writes_a_raw_byte(self):
        sim = run('10 CMD CREATE 3,"F": CMD PRINT #3,CHR$(0);CHR$(255);: CMD PRINT #3,"A";: CMD CLOSE 3')
        self.assertEqual(sim.cdos.drives[8]["F"], ("seq", b"\x00\xffA"))

    def test_get_reads_raw_bytes_and_eof_flags_the_end(self):
        sim = run('10 CMD CREATE 3,"F": CMD PRINT #3,"A";: CMD CLOSE 3\n20 CMD OPEN 3,"F": CMD GET #3,C$: PRINT C$;EOF(3)')
        self.assertEqual(sim.screen.text(), "A-1")

    def test_reading_past_the_end_is_error_42(self):
        with self.assertRaises(bs.Halt) as cm:
            run('10 CMD CREATE 3,"F": CMD CLOSE 3: CMD OPEN 3,"F": CMD INPUT #3,A$')
        self.assertEqual(cm.exception.code, 42)

    def test_channel_errors(self):
        for src, code in (('10 CMD PRINT #3,"X"', 94), ('10 CMD CLOSE 3', 94),
                          ('10 CMD CREATE 3,"F": CMD CREATE 3,"G"', 92),
                          ('10 CMD CREATE 3,"F": CMD CLOSE 3: CMD CREATE 3,"F"', 190 + 63),
                          ('10 CMD OPEN 3,"NOPE"', 190 + 62)):
            with self.assertRaises(bs.Halt, msg=src) as cm:
                run(src)
            self.assertEqual(cm.exception.code, code, src)

    def test_scratch_removes_the_file_and_reports_status_one(self):
        sim = run('10 CMD CREATE 3,"F": CMD CLOSE 3: CMD PRINT "S0:F": CMD ERROR E,M$,T,S')
        self.assertEqual(sim.cdos.drives[8], {})
        self.assertEqual((sim.get_var("E"), sim.get_var("M$")), (1, "FILES SCRATCHED"))
        # the error channel is cleared by reading it
        sim2 = run('10 CMD PRINT "S0:F": CMD ERROR E,M$,T,S: CMD ERROR E,M$,T,S')
        self.assertEqual(sim2.get_var("E"), 0)

    def test_screen_save_load_test(self):
        src = ('10 PRINT "HELLO": CMD SAVE SCREEN "S": CLS: PRINT "X"\n'
               '20 CMD LOAD "S": CMD TEST "S": PRINT "PASSED"')
        self.assertEqual(out(src).splitlines(), ["HELLO", "PASSED"])
        with self.assertRaises(bs.Halt) as cm:
            run('10 PRINT "HELLO": CMD SAVE SCREEN "S": CLS: PRINT "HELLO?": CMD TEST "S"')
        self.assertEqual((cm.exception.code, cm.exception.count), (42, 1))

    def test_a_missing_drive_answers_160(self):
        with self.assertRaises(bs.Halt) as cm:
            run('10 CMD SET 9: CMD CREATE 3,"F"', setup=lambda s: s.cdos.absent.add(9))
        self.assertEqual(cm.exception.code, 160)

    def test_screen_scrolls_at_the_bottom(self):
        sim = run("10 FOR I=1 TO 17: PRINT I: NEXT I")
        self.assertEqual(sim.screen.scrolls, 2)          # 17 lines and the final CR on a 16-row screen


if __name__ == "__main__":
    unittest.main()
