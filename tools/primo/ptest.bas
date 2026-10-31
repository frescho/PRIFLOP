# PTEST -- drive reliability test for the Primo. The counterpart of CTEST.PRG
# (tools/reltest.c) on the C64: same menu, same rounds, same "write, read
# back, compare, report" shape, so a result on one machine can be set beside
# the other.
#
# Lines starting with '#' are host comments; primo_basic.py drops them, so
# they cost the Primo nothing (BASIC has about 9 KB for program, variables
# and strings together). Build and upload:
#
#     python3 tools/primo/primo_basic.py upload tools/primo/ptest.bas PTEST HOST PASSWORD
#
# On the Primo:   CMD SET 8 : CMD LOAD "PTEST"   then   RUN
#
# What differs from CTEST, and why (all from the CDOS description):
#
#  * A C64 program can SAVE a whole memory block in one call. BASIC on the
#    Primo cannot: CMD SAVE only writes the resident program and CMD SAVE
#    SCREEN the 6 KB screen. So the sized files (8K/16K/24K, as on the C64)
#    go through the byte channel instead -- CMD CREATE / CMD PRINT# / CMD
#    OPEN / CMD INPUT# -- which is the path the size-probe bug lived on. The
#    block path (LOAD/SAVE/TEST) is covered by the screen test.
#  * No reference file is needed for the screen test: the pattern is drawn
#    by the program, saved, redrawn and compared with CMD TEST, so it works
#    on the RAM drive, which is empty after every boot.
#  * CMD PRINT#, CMD INPUT# and CMD GET# do not poll the drive's error
#    channel (CDOS, chapter 9), so a drive fault on them shows up as
#    "160 DEVICE NOT PRESENT" or as bad data, not as a DOS message. After
#    every CLOSE the test asks the error channel itself (CMD ERROR).
#  * ON ERROR GOTO + RESUME NEXT: the handler only records ERR/ERL in EC/EL
#    and sets EF; each risky statement is followed on the same line by
#    IF EF THEN 1290.
#  * A drive says "73, DOS VERSION" until something else replaces it
#    (PRIFLOP does; so does a real 1541). That is not an error, so the
#    checks that read the error channel accept 73 along with 0..19.
#  * CMD SET 8 on exit, otherwise the next LOAD from the prompt would go to
#    the drive under test.
#  * Files are always removed with the drive's scratch command
#    (CMD PRINT "S0:name"): CMD KILL is documented for PRG files only and
#    these are SEQ.
#  * RND mode of CTEST is called MIX here and is deterministic (size depends
#    on round and file number), so a failure can be reproduced.
#  * BREAK stops BASIC with a channel possibly open: type CMD CLEAR and then
#    CMD SET 8. Q stops the test cleanly.
#
# Record layout: 4 digits (record number) + 123 characters cut from a
# repeating 0-9A-Z string at an offset that moves 5 per record, so a shifted
# or repeated record is caught, plus the CR that CMD PRINT# appends = 128
# bytes. 64/128/192 records = 8/16/24 KB, the C64 test's sizes.
#
# A FOR loop left by GOTO leaves its frame on the stack, and a later FOR on
# the same variable discards that frame and everything stacked above it --
# GOSUB return addresses included (Microsoft BASIC does this; basic_sim.py
# reproduces it, and found the first version of this program doing exactly
# that). So every loop that can be left by GOTO has a variable of its own:
# B (records), K (binary bytes), Y (finding the bad character), F (files, at
# the top level only). I is used only by loops that always run to the end.
#
# Variables: DV drive, SZ size mode, NF files, RI/RP repeat, SC screen test,
# BN binary test, RN round, FC failures, PA pass flag, AB stop flag, EF error
# flag (EC code, EL line), L$() last eight log lines, LI ring position.
#
10 CLEAR 2000: DIM L$(7): ON ERROR GOTO 9000
20 P$="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ": P$=P$+P$+P$+P$+P$: Z$="8K 16K24KMIX"
30 DV=9: SZ=1: NF=3: RI=0: RP=1: SC=1: BN=1
100 CLS: PRINT "PTEST - PRIMO DRIVE TEST": PRINT
110 PRINT "1 DRIVE .....";DV
120 PRINT "2 FILE SIZE . ";MID$(Z$,SZ*3+1,3)
130 PRINT "3 FILES .....";NF
140 PRINT "4 REPEAT ....";: IF RP=0 THEN PRINT " INFINITE": GOTO 160
150 PRINT RP
160 PRINT "5 SCREEN .... ";MID$("OFFON ",SC*3+1,3)
170 PRINT "6 BINARY .... ";MID$("OFFON ",BN*3+1,3)
180 PRINT: PRINT "ENTER=START  Q=QUIT": PRINT "FREE";FRE(0)
200 K$=INKEY$: IF K$="" THEN 200
210 IF K$="1" THEN DV=17-DV: GOTO 100
220 IF K$="2" THEN SZ=SZ+1: SZ=SZ-INT(SZ/4)*4: GOTO 100
230 IF K$="3" THEN NF=NF-INT(NF/5)*5+1: GOTO 100
240 IF K$="4" THEN RI=RI+1: RI=RI-INT(RI/4)*4: RP=VAL(MID$("1350",RI+1,1)): GOTO 100
250 IF K$="5" THEN SC=1-SC: GOTO 100
260 IF K$="6" THEN BN=1-BN: GOTO 100
270 IF K$=CHR$(13) OR K$=" " THEN 300
280 IF K$="Q" OR K$="q" THEN ON ERROR GOTO 0: CMD SET 8: CLS: END
290 GOTO 200
# ---- run: rounds of files, then the screen and binary tests ----
300 CMD SET DV: EF=0: AB=0: FC=0: RN=0: LI=0: FOR I=0 TO 7: L$(I)="": NEXT I
310 GOSUB 6100
320 RN=RN+1: IF RP>0 THEN IF RN>RP THEN 700
330 OK=0: FOR F=1 TO NF
340 Z=SZ: IF SZ=3 THEN Z=RN+F: Z=Z-INT(Z/3)*3
350 NR=64*(Z+1): N$="PT"+RIGHT$(STR$(F),1): GOSUB 1000: IF PA THEN OK=OK+1
360 IF AB THEN 700
370 NEXT F
380 M$="ROUND"+STR$(RN)+":"+STR$(OK)+"/"+STR$(NF)+" OK, FAIL"+STR$(FC): PRINT M$: GOSUB 6000
390 IF SC THEN GOSUB 2000
400 IF AB THEN 700
410 IF BN THEN GOSUB 3000
420 IF AB THEN 700
430 FOR F=1 TO NF: CMD PRINT "S0:PT"+RIGHT$(STR$(F),1): NEXT F: EF=0
440 GOTO 320
700 FOR F=1 TO NF: CMD PRINT "S0:PT"+RIGHT$(STR$(F),1): NEXT F
710 CMD SET 8: EF=0: M$="TEST COMPLETE": IF AB THEN M$="STOPPED"
720 PRINT: PRINT M$;"  FAILURES";FC: PRINT "ANY KEY FOR MENU"
730 K$=INKEY$: IF K$="" THEN 730
740 GOTO 100
# ---- one sized file: write NR records, read them back, compare ----
1000 PA=0: F2$="": FL$="": M$=N$+" W": PRINT M$;: CMD PRINT "S0:"+N$: EF=0
1010 CMD CREATE 3,N$: IF EF THEN 1290
1020 FOR B=1 TO NR: GOSUB 1500: CMD PRINT #3,R$: IF EF THEN 1290
1030 IF B-INT(B/16)*16=0 THEN PRINT ".";: M$=M$+"."
1040 K$=INKEY$: IF K$="Q" OR K$="q" THEN AB=1: GOTO 1300
1050 NEXT B
1060 CMD CLOSE 3: IF EF THEN 1290
1070 CMD ERROR EN,EM$,ET,ES: IF EF THEN 1290
1080 IF EN>=20 AND EN<>73 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300
1100 M$=M$+" R": PRINT " R";: CMD OPEN 3,N$: IF EF THEN 1290
1110 FOR B=1 TO NR: IF EOF(3) THEN FL$="SHORT: EOF AT REC"+STR$(B): GOTO 1300
1120 CMD INPUT #3,X$: IF EF THEN 1290
1130 GOSUB 1500: IF X$<>R$ THEN 1400
1140 IF B-INT(B/16)*16=0 THEN PRINT ".";: M$=M$+"."
1150 K$=INKEY$: IF K$="Q" OR K$="q" THEN AB=1: GOTO 1300
1160 NEXT B
1170 IF NOT EOF(3) THEN FL$="LONG: DATA AFTER REC"+STR$(NR): GOTO 1300
1180 CMD CLOSE 3: IF EF THEN 1290
1190 PRINT " OK": M$=M$+" OK": GOSUB 6000: PA=1: RETURN
# ---- failure exit shared by every test; N$ names the file to remove ----
1290 FL$="ERR"+STR$(EC)+" LINE"+STR$(EL): F2$=""
1292 IF EC=160 THEN F2$="DEVICE NOT PRESENT"
1294 IF EC>=190 THEN F2$="DRIVE ERROR"+STR$(EC-190)
1300 CMD CLOSE 3: CMD PRINT "S0:"+N$: EF=0
1310 IF AB THEN RETURN
1320 M$="*** "+N$+" "+FL$: GOSUB 6000: IF F2$<>"" THEN M$="    "+F2$: GOSUB 6000
1330 FC=FC+1: GOSUB 6100: PRINT: PRINT "ANY KEY=GO ON  Q=STOP"
1340 K$=INKEY$: IF K$="" THEN 1340
1350 IF K$="Q" OR K$="q" THEN AB=1
1360 RETURN
# ---- a record differs: find the first bad character ----
1400 FOR Y=1 TO LEN(R$): IF MID$(X$,Y,1)<>MID$(R$,Y,1) THEN 1420
1410 NEXT Y
1420 EV=-1: GV=-1: IF Y<=LEN(R$) THEN EV=ASC(MID$(R$,Y,1))
1425 IF Y<=LEN(X$) THEN GV=ASC(MID$(X$,Y,1))
1430 FL$="DATA REC"+STR$(B)+" POS"+STR$(Y): F2$="EXP"+STR$(EV)+" GOT"+STR$(GV)+" LEN"+STR$(LEN(X$))
1440 GOTO 1300
# ---- record B ----
1500 R$=RIGHT$(STR$(B+10000),4)+MID$(P$,B*5-INT(B*5/36)*36+1,123): RETURN
# ---- screen test: draw, save, reload, compare with the file; draw again, compare again ----
2000 PA=0: F2$="": FL$="": N$="PTS1": CMD PRINT "S0:PTS1": EF=0: CLS: GOSUB 2500
2010 CMD SAVE SCREEN "PTS1": IF EF THEN 1290
2020 CLS: CMD LOAD "PTS1": IF EF THEN 1290
2030 CMD TEST "PTS1": IF EF THEN 2800
2040 CLS: GOSUB 2500: CMD TEST "PTS1": IF EF THEN 2810
2050 CMD PRINT "S0:PTS1": EF=0: M$="SCREEN OK": GOSUB 6000: GOSUB 6100: PA=1: RETURN
2500 FOR I=0 TO 14: PRINT MID$(P$,I*7+1,41): NEXT I: PRINT MID$(P$,113,41);: RETURN
# a difference: repeat the compare without the handler, so the system's own
# message shows how many bytes differ (it stops the program; if the repeat
# passes, the difference was not repeatable and is reported as such)
2800 FL$="DIFF AFTER LOAD": GOTO 2820
2810 FL$="DIFF VS FILE"
2820 ON ERROR GOTO 0: CMD TEST "PTS1": ON ERROR GOTO 9000: F2$="RETRY PASSED": GOTO 1300
# ---- binary test: every byte value up and back down (512 bytes) ----
3000 PA=0: F2$="": FL$="": N$="PTB": M$="PTB BIN": PRINT M$;: CMD PRINT "S0:PTB": EF=0
3010 CMD CREATE 3,N$: IF EF THEN 1290
3020 FOR K=0 TO 511: V=K: IF K>255 THEN V=511-K
3030 CMD PRINT #3,CHR$(V);: IF EF THEN 1290
3040 NEXT K
3050 CMD CLOSE 3: IF EF THEN 1290
3055 CMD ERROR EN,EM$,ET,ES: IF EF THEN 1290
3056 IF EN>=20 AND EN<>73 THEN FL$="WRITE: DRIVE ERROR"+STR$(EN): F2$=EM$: GOTO 1300
3060 CMD OPEN 3,N$: IF EF THEN 1290
3070 FOR K=0 TO 511: V=K: IF K>255 THEN V=511-K
3080 IF EOF(3) THEN FL$="SHORT AT BYTE"+STR$(K): GOTO 1300
3090 CMD GET #3,C$: IF EF THEN 1290
3100 C=0: IF LEN(C$)>0 THEN C=ASC(C$)
3110 IF C<>V THEN FL$="BYTE"+STR$(K)+" EXP"+STR$(V)+" GOT"+STR$(C): GOTO 1300
3120 NEXT K
3130 IF NOT EOF(3) THEN FL$="LONG: DATA AFTER BYTE 511": GOTO 1300
3140 CMD CLOSE 3: IF EF THEN 1290
3150 CMD PRINT "S0:PTB": EF=0: PRINT " OK": M$=M$+" OK": GOSUB 6000: PA=1: RETURN
# ---- log: last eight lines, redrawn after anything that clears the screen ----
6000 L$(LI)=M$: LI=LI+1: LI=LI-INT(LI/8)*8: RETURN
6100 CLS: PRINT "PTEST DRIVE";DV;" SIZE ";MID$(Z$,SZ*3+1,3);" FILES";NF
6110 FOR I=0 TO 7: J=LI+I: J=J-INT(J/8)*8: IF L$(J)<>"" THEN PRINT L$(J)
6120 NEXT I: RETURN
# ---- error handler: record, resume after the failing statement ----
9000 EF=1: EC=ERR: EL=ERL: RESUME NEXT
