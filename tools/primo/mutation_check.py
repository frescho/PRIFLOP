#!/usr/bin/env python3
"""Checks that test_ptest.py can actually fail: breaks ptest.bas on purpose,
one defect at a time, runs the whole PTEST test module against each broken
copy, and reports any defect the tests let through.

    python3 tools/primo/mutation_check.py        (about 45 s)

A "survivor" means a defect no test notices -- write a test, or admit the code
is dead. Each pattern must occur exactly once in ptest.bas, so renumbering or
rewording a line there makes this script complain rather than silently skip.
"""
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_ptest as t

orig = open(t.SOURCE, encoding="latin-1").read()
MUTATIONS = {
 "FOR I collides again":      ('1400 FOR Y=1 TO LEN(R$): IF MID$(X$,Y,1)<>MID$(R$,Y,1) THEN 1420\n1410 NEXT Y\n1420 EV=-1: GV=-1: IF Y<=LEN(R$) THEN EV=ASC(MID$(R$,Y,1))\n1425 IF Y<=LEN(X$) THEN GV=ASC(MID$(X$,Y,1))\n1430 FL$="DATA REC"+STR$(B)+" POS"+STR$(Y):',
                               '1400 FOR I=1 TO LEN(R$): IF MID$(X$,I,1)<>MID$(R$,I,1) THEN 1420\n1410 NEXT I\n1420 EV=-1: GV=-1: IF I<=LEN(R$) THEN EV=ASC(MID$(R$,I,1))\n1425 IF I<=LEN(X$) THEN GV=ASC(MID$(X$,I,1))\n1430 FL$="DATA REC"+STR$(B)+" POS"+STR$(I):'),
 "73 counted as an error":   ('IF EN>=20 AND EN<>73 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300\n1100', 'IF EN>=20 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300\n1100'),
 "no drain of the status":   ('300 CMD SET DV: CMD ERROR EN,EM$,ET,ES: EF=0', '300 CMD SET DV: EF=0'),
 "record offset wrong":      ('B*5-INT(B*5/36)*36+1,123', 'B*5-INT(B*5/36)*36+2,123'),
 "drive 8 not restored":     ('710 CMD SET 8: EF=0:', '710 EF=0:'),
 "EOF check after last dropped": ('1170 IF NOT EOF(3) THEN FL$="LONG: DATA AFTER REC"+STR$(NR): GOTO 1300\n', ''),
 "EOF check before record dropped": ('FOR B=1 TO NR: IF EOF(3) THEN FL$="SHORT: EOF AT REC"+STR$(B): GOTO 1300', 'FOR B=1 TO NR: IF 0 THEN 1300'),
 "no cleanup on failure":    ('1300 CMD CLOSE 3: CMD PRINT "S0:"+N$: EF=0', '1300 CMD CLOSE 3: EF=0'),
 "no cleanup at round end":  ('430 FOR F=1 TO NF: CMD PRINT "S0:PT"+RIGHT$(STR$(F),1): NEXT F: EF=0', '430 EF=0'),
 "write error unchecked":    ('CMD PRINT #3,R$: IF EF THEN 1290', 'CMD PRINT #3,R$'),
 "retry pass not reported":  ('F2$="RETRY PASSED": GOTO 1300', 'GOTO 2050'),
 "screen redraw missing":    ('1330 FC=FC+1: GOSUB 6100:', '1330 FC=FC+1:'),
 "binary compares nothing":  ('3110 IF C<>V THEN', '3110 IF C=V+1000 THEN'),
 "binary close unchecked":   ('3056 IF EN>=20 AND EN<>73 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300\n', ''),
 "abort ignored while reading": ('1150 K$=INKEY$: IF K$="Q" OR K$="q" THEN AB=1: GOTO 1300\n', ''),
}
MUTATIONS["73 counted as an error"] = ('IF EN>=20 AND EN<>73 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300\n1100', 'IF EN>=20 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300\n1100')
MUTATIONS["73 counted as an error (binary)"] = ('3056 IF EN>=20 AND EN<>73 THEN', '3056 IF EN>=20 THEN')
del MUTATIONS["no drain of the status"]
survived = []
for name, spec in MUTATIONS.items():
    pairs = spec if isinstance(spec, list) else [spec]
    src = orig
    for old, new in pairs:
        assert src.count(old) == 1, f"mutation {name!r}: pattern found {src.count(old)} times"
        src = src.replace(old, new)
    with tempfile.NamedTemporaryFile("w", suffix=".bas", delete=False, encoding="latin-1") as f:
        f.write(src)
    t.SOURCE = f.name
    suite = unittest.defaultTestLoader.loadTestsFromModule(t)
    res = unittest.TextTestRunner(stream=io.StringIO(), verbosity=0).run(suite)
    os.unlink(f.name)
    bad = len(res.failures) + len(res.errors)
    print(f"{'KILLED ' if bad else 'SURVIVED'}  {name}  ({bad} tests failed)")
    if not bad:
        survived.append(name)
print("survivors:", survived or "none")
sys.exit(1 if survived else 0)
