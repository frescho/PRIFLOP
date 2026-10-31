; PRIFLOP DOS -- our own DOS for the Primo, compatible with CDOS 1.0.
; Copyright (C) 2026 sd2iec_pico2w contributors; GPL v2.
;
; Written from CDOS's documented behaviour and from running
; CDOS in the simulation (tests/primo_dos_sim.py), not from its code.
;
; Assembled at org 0; the loader moves it under BASIC's top and adds the base
; to every absolute reference (build_dos.py finds them).
;
; Status: step B7 -- the service call, services 0x00-0x0C, the bus layer
; (0x1C-0x2A), the channel services (0x14-0x1A), and load (0x10) and save (0x0E) through PRIFLOP's
; turbo, with a raw load on the standard bus for plain files (mask 0).
; Verify (0x12) and the printer's 0x2C are not provided. The CMD statement: LOAD, SAVE, $, ERROR,
; NEW, KILL, RESET and SET, and the error hook with the DOS's messages;
; the byte channel: OPEN, CREATE, CONT, DEF, CLOSE, CLEAR, PRINT[#],
; INPUT#, GET[#], EOF() and the two output hooks. loader.asm installs it.

INP     equ 040h        ; input port: bit 5 CLOCK, bit 4 DATA; 1 = released
ICLK    equ 020h
IDAT    equ 010h
LATN    equ 002h        ; output port bits: 1 pulls the line
LCLK    equ 020h
LDAT    equ 010h
SYSNMI  equ 403Bh       ; BASIC's shadow of port 0; bit 7 = NMI enable
SYSSCR  equ 404Ah       ; the screen's first page

; timeouts, in polls of `wt` (44 T each while waiting); each matches CDOS's
; own in T-states, measured with `margins` (tests/primo_dos_sim.py)
T_DEV   equ 63          ; ATN: a device pulls DATA (T_AT)
T_ACK   equ 55          ; a byte sent: the listener pulls DATA (T_F)
T_EOI   equ 21          ; receiving: the talker pulls CLOCK, or it is EOI
T_GAP   equ 10          ; djnz loops of 13 T before a byte under ATN

; BASIC's ROM: TRS-80 Level II's routines, at the same addresses
R_EVAL  equ 2337h       ; the expression at HL; HL after it
R_STR   equ 29D7h       ; ... a string: HL = its descriptor, or TM
R_BYTE  equ 2B1Ch       ; the expression at HL as 0-255 in A, or FC; flags: the next character
R_STK   equ 1963h       ; C words free under the stack, or OM
R_IDCHK equ 2828h       ; ID in direct mode
R_INPUT equ 21EBh       ; INPUT's parser: the data at HL+1, the variables at (SP)
R_ERR   equ 19A2h       ; the error E
R_SN    equ 1997h       ; SN
R_ERRIN equ 1A0Ah       ; the error message's end: " in" the line HL, then READY
R_OUT   equ 032Ah       ; a character to the screen
R_NL    equ 20F9h       ; a new line, unless at the line's start
R_PRINT equ 2B75h       ; the text at HL, up to a 0
R_NUM   equ 0FAFh       ; HL as a number
R_LWAIT equ 065Fh       ; LIST's: a new line, then a key held; BRK breaks
R_LIST  equ 2B7Eh       ; LIST's: the line's text at HL into the input buffer, words for tokens
R_OK    equ 1A19h       ; READY, "Ok"
R_LINK  equ 1AF8h       ; the program's line links, set again
R_CLRUN equ 1B5Dh       ; RUN's start: the variables cleared, a new stack, HL = the program - 1
R_EXEC  equ 1D1Eh       ; the statement loop
R_READY equ 1A33h       ; READY, without "Ok"
CURLIN  equ 40A2h       ; the line running, 0FFFFh in direct mode
TXTTAB  equ 40A4h       ; the program
INPBUF  equ 40A7h       ; the input buffer
INPDEV  equ 40A9h       ; INPUT's: 0 = from a device (INPUT#)
ERRLIN  equ 40EAh       ; the line of the last error
VARTAB  equ 40F9h       ; the program's end
STREND  equ 40FDh       ; the free memory
STRSPC  equ 40A0h       ; the string space; the stack under it
RSTVEC  equ 400Dh       ; RST 28H's JP operand
OUTDEV  equ 409Ch       ; PRINT's device: 80h as to tape
TMPSTR  equ 40D3h       ; the string just made: its length, its address
R_PRT   equ 209Bh       ; PRINT's list, at HL
R_VAR   equ 260Dh       ; the variable at HL: DE = its value, its type set
R_TM    equ 0AF6h       ; TM
R_STR1  equ 2857h       ; A bytes in the string space, described at TMPSTR
R_INT   equ 0A9Ah       ; HL as the result, an integer
R_FIX   equ 0B37h       ; INT(): the accumulator, an integer where it fits
ACC     equ 4121h       ; the accumulator
E_BO    equ 90          ; the DOS's own: ten files open
E_FO    equ 92          ; the file is open already
E_NO    equ 94          ; the file is not open
E_BD    equ 96          ; the file is open the other way
E_FD    equ 42          ; read past the file's end
FNOW    equ 1           ; a file's mode: not for writing
FNOR    equ 2           ; not for reading; bit 7: read to its end
T_LOAD  equ 0A7h        ; BASIC's tokens
T_SAVE  equ 0ADh
T_SCRN  equ 0AEh
T_AUTO  equ 0B7h
E_MO    equ 40          ; BASIC's error codes
E_FC    equ 8
E_OM    equ 12
E_NP    equ 100         ; DEVICE NOT PRESENT; 100 + n: the drive's message n

    org 0

; ---- the CMD statement: HL at the word after CMD, A = it, the flags of
; RST 10H, the statement loop's return on the stack. A word that is not
; ours goes to the CMD vector found before ours, as it came.
p_cmd:
cmd:
    push hl
    push af
    ld hl,svc
    ld (RSTVEC),hl      ; RST 28H is ours again after every CMD, as CDOS's
    ld a,(cdev)
    ld (dev),a          ; the device of CMD SET, whatever service 0x06 set
    xor a
    ld (status),a       ; and the status cleared, as CDOS
    pop af
    push af
    ld hl,ctab
    ld b,NCMD
cmd1:
    cp (hl)
    inc hl
    jr z,cmd2
    inc hl
    inc hl
    djnz cmd1           ; not ours: HL at `oldcmd`
cmd2:
    ld e,(hl)
    inc hl
    ld d,(hl)
    pop af
    pop hl
    push de
    ret

ctab:                   ; the words and their handlers
    defb T_LOAD
    defw c_load
    defb T_SAVE
    defw c_save
    defb '$'
    defw c_dir
    defb 9Eh            ; ERROR
    defw c_err
    defb 0BBh           ; NEW
    defw c_new
    defb 0AAh           ; KILL
    defw c_kill
    defb 82h            ; RESET
    defw c_rset
    defb 83h            ; SET
    defw c_set
    defb 0A2h           ; OPEN
    defw c_open
    defb 0ABh           ; CREATE
    defw c_crea
    defb 0B3h           ; CONT
    defw c_cont
    defb 0B0h           ; DEF
    defw c_def
    defb 0A6h           ; CLOSE
    defw c_clos
    defb 0B8h           ; CLEAR
    defw c_clr
    defb 0B2h           ; PRINT
    defw c_prt
    defb 89h            ; INPUT
    defw c_inp
    defb 0A4h           ; GET
    defw c_get
NCMD    equ (ASMPC - ctab) / 3
oldcmd: defw 1997h      ; the CMD vector before ours; the loader writes it

; CMD LOAD [SCREEN] "n": the file into the program area. Unless its last
; record is the screen's, what was loaded is taken as the program, which
; ends where the last record ended (so does CDOS, even after machine code):
; RUN at once inside a program or after a record set the line running, else
; READY without "Ok". CMD LOAD alone loads PFM, the file selector
; CDOS answers it with MO.
c_load:
    rst 10h
    jr nz,ld0
    ex de,hl            ; no name: PFM
    ld hl,pfmname
    jr ld3
ld0:
    cp T_SCRN           ; SCREEN changes nothing
    jr z,ld1
    dec hl              ; RST 10H reads this character again, for its flags
ld1:
    rst 10h
    call name0
ld3:
    push de             ; the text after it
    push hl
    ld hl,(STRSPC)
    ld bc,-1Ah
    add hl,bc
    ld (limit),hl       ; the limit: under the strings, where CDOS sets it
    pop hl
    ld de,(TXTTAB)
    ld c,1Fh
    ld a,10h
    rst 28h             ; through the service call, as CDOS
    call ldchk
    pop hl
    cp 0D5h
    ret z               ; a screen last: on with the statement
    ld (VARTAB),de      ; else, as CDOS, whatever came last: a program ending
                        ; there
    call R_LINK
    ld hl,(CURLIN)
    inc hl
    ld a,h
    or l
    jr z,ld2
    call R_CLRUN        ; a line was running, or the file set one: RUN
    jp R_EXEC
ld2:
    call R_CLRUN
    jp R_READY

; CMD SAVE [AUTO|SCREEN] "n": the program, the program to start at once, or
; the screen, as CDOS writes them.
c_save:
    rst 10h
    ld de,d_auto
    cp T_AUTO
    jr z,sv1
    ld de,d_scr
    cp T_SCRN
    jr z,sv1
    ld de,d_bas
    dec hl
sv1:
    rst 10h
    push de             ; the descriptor
    call name0
    ex (sp),hl          ; the name; HL = the descriptor
    push de             ; the text after it
    push hl
    ld hl,(TXTTAB)
    ld (d_src),hl
    ex de,hl
    ld hl,(VARTAB)
    or a
    sbc hl,de
    ld (d_len),hl
    ld a,(SYSSCR)
    ld (d_scrp),a
    pop de
    pop bc
    pop hl
    push bc
    ld a,0Eh
    rst 28h
    call dsk
    pop hl
    ret

; CMD $ ["pattern"]: the directory, read raw into the free memory and
; listed as LIST lists a program (codes from 80h print as BASIC's words), a
; key held to go on. In direct mode the listing used the input buffer: READY.
c_dir:
    rst 10h
    ld bc,'$'*256+':'
    jr nz,dir1
    push hl
    ld hl,dollar
    jr dir2
dir1:
    call name
    push de
dir2:
    ex de,hl
    ld hl,(STRSPC)
    ld bc,-1Ch          ; two bytes lower than CMD LOAD's, as CDOS
    add hl,bc
    ld (limit),hl
    ex de,hl
    ld de,(STREND)
    ld c,0
    ld a,10h
    rst 28h
    call ldchk
    ld hl,(STREND)
dir3:
    ld a,(hl)           ; a line's link: 0 ends the listing
    inc hl
    or (hl)
    inc hl
    jr z,dir4
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    push hl
    ex de,hl
    call R_NUM          ; its number: the blocks
    ld a,' '
    call R_OUT
    pop hl
    call R_LIST         ; its text, into the input buffer
    push hl
    push hl             ; as deep as CDOS's: a BRK leaves the stack as it does
    ld hl,(INPBUF)
    call R_PRINT
    call R_LWAIT
    pop hl
    pop hl
    jr dir3
dir4:
    pop de              ; the text after it
    ld hl,(CURLIN)
    inc hl
    ld a,h
    or l
    ex de,hl
    ret nz
    pop af              ; direct: the statement loop's return, not wanted
    jp R_OK

; CMD ERROR: the drive's message, printed; never an error but no drive.
; CMD ERROR v1[,v2..]: its parts -- code, text, track, sector -- into the
; variables, as INPUT# reads them; only inside a program. Either shows the
; message a drive command held back (01, cmd18) instead of reading again.
c_err:
    rst 10h
    jr nz,er1
    push hl
    call errget
    call R_NL
    ld hl,errbuf
    call R_PRINT
    pop hl
    ret
er1:
    push hl
    call errget         ; read first, as CDOS: then ID in direct mode
    pop hl
    call R_IDCHK
    push hl
    xor a
    ld (INPDEV),a       ; as INPUT#: OD when the parts run out
    ld hl,errbuf
    ld de,(INPBUF)      ; into BASIC's input buffer, where INPUT reads:
    push de             ; LET copies the parts into the string space
er2:
    ld a,(hl)
    ld (de),a
    inc hl
    inc de
    or a
    jr nz,er2
    pop hl
    dec hl
    jp R_INPUT
errget:                 ; the message held back, or the drive's now
    ld hl,held
    ld a,(hl)
    ld (hl),0
    or a
    ret nz
    call s1a
    ret nc
    jp dnp

; CMD KILL "n" and CMD NEW "n": the drive's commands S: and N:.
c_kill:
    ld bc,'S'*256+':'
    jr kn1
c_new:
    ld bc,'N'*256+':'
kn1:
    rst 10h
    call name
    push de
    jr cmd18

; CMD RESET: channel 15 closed, then the drive's I.
c_rset:
    rst 10h
    jp nz,R_SN          ; nothing may follow, checked first, as CDOS
    push hl
    xor a
    ld (held),a
    call forget         ; the drive closes them all, CDOS forgets them
    ld c,0Fh
    call s16
    call dsk
    ld hl,initdr
cmd18:
    call s18
    call dsk
    cp 1
    jr nz,c18a
    ld (held),a         ; 01: held back for CMD ERROR, as CDOS does
c18a:
    pop hl
    ret

; CMD SET d[,p] or CMD SET ,p: the drive 8-11, the printer 4 or 5.
c_set:
    rst 10h
    jr z,e_mo
    cp ','
    jr z,set1
    call R_BYTE
    sub 8
    cp 4
    jr nc,e_fc
    add a,8
    ld (cdev),a
    ld (dev),a
    ld a,(hl)
    cp ','
    ret nz
set1:
    rst 10h
    call R_BYTE
    sub 4
    cp 2
    jr nc,e_fc
    add a,4
    ld (pdev),a
    ret

e_mo:
    ld e,E_MO
    defb 1              ; LD BC,nn: skips the next LD E,n
e_fc:
    ld e,E_FC
    defb 1
e_om:
    ld e,E_OM
    jp R_ERR

; after a load: C no drive; Z with 52 (the turbo's) or 0A8h (the raw load's)
; out of memory, 63h the end of a plain file, else the drive's code.
ldchk:
    jr c,dnp
    ret nz
    cp 63h
    ret z
    cp 52
    jr z,e_om
    cp 0A8h
    jr z,e_om
    jr dsk2
; after a service: NC NZ returns; C is DEVICE NOT PRESENT; Z, A = the
; drive's code n: the error 100 + n, its message in `errbuf`, held back for
; CMD ERROR until the error hook shows it (as CDOS: not under ON ERROR).
dsk:
    jr c,dnp
    ret nz
dsk2:
    add a,E_NP
    ld e,a
    ld a,1
    ld (held),a
    jp R_ERR
dnp:
    ld e,E_NP
    jp R_ERR

; the string expression at HL (Z: none, MO) as a name, length-prefixed,
; after a prefix of two characters in BC (none: name0), in the free memory
; 128 bytes under the stack: so a load that moves the program's end (CMD
; LOAD of machine code) does not take it into the program, as CDOS's name is
; not there either. Out: HL = the name, DE = the text after the expression.
name0:
    ld b,0
name:
    jr z,e_mo
    push bc
    call R_EVAL
    pop bc
    push hl             ; the text after it
    push bc
    call R_STR          ; HL = its descriptor
    ld a,(hl)
    srl a
    add a,44h
    ld c,a
    call R_STK          ; room under the stack, or OM
    pop bc
    ld a,(hl)           ; the length
    inc hl
    ld e,(hl)
    inc hl
    ld d,(hl)           ; DE = the characters
    cpl
    ld l,a
    cpl
    ld h,0FFh           ; HL = -1 - the length
    push de
    ld de,-82h
    add hl,de
    pop de
    add hl,sp
    push hl             ; the name
    ld (hl),a
    inc hl
    inc b
    dec b
    jr z,nm1
    ld (hl),b
    inc hl
    ld (hl),c
    inc hl
    ex (sp),hl
    inc (hl)
    inc (hl)
    ex (sp),hl
nm1:
    or a
    jr z,nm2
    ld c,a
    ld b,0
    ex de,hl
    ldir
nm2:
    pop hl
    pop de
    ret

; ---- the error hook (0x41A6): E = the code, HL = the ROM's two-letter
; table, A = 0. 90-99 are ours, two letters and " Error" as the ROM's; 100
; and up, our message whole; below 90, the hook found before ours.
ehook:
    ld a,e
    cp 90
    jr c,ehk1
    ld hl,etab-90
    cp E_NP
    jr nc,ehk2
    xor a               ; the ROM takes D from A
    ret
ehk1:
    xor a
oldhook:
    defb 0C9h,0,0       ; the hook before ours, 3 bytes; the loader copies them
ehk2:
    ld hl,etab+8        ; 100
    jr z,ehk3
    xor a
    ld (held),a         ; 100 + n: the drive's message, shown now
    ld hl,errbuf
ehk3:
    pop af              ; the ROM's own printing is not wanted
    ld a,7
    call R_OUT          ; the bell, as the ROM's
    call R_PRINT
    ld hl,(ERRLIN)
    jp R_ERRIN
etab:
    defm "BOFONOBD"     ; 90 BO, 92 FO, 94 NO, 96 BD
    defm "DEVICE NOT PRESENT"
    defb 0
dollar: defb 1, '$'
initdr: defb 1, 'I'
pfmname: defb 6, "//:PFM"  ; from the root, whatever folder the drive is in

; CMD SAVE's descriptors: AUTO writes 00 00 to the line
; running (CURLIN), then the program as d_bas does
d_auto: defb 0D9h
        defw CURLIN, 2, d_bas+1
d_bas:  defb 0D1h
        defw 0
d_len:  defw 0
d_src:  defw 0
        defb 0C9h
d_scr:  defb 0D5h
        defw 0, 1800h
        defb 0
d_scrp: defb 0
        defb 0C9h

; ---- the byte channel: files 2-14, ten open at a time, each on the drive
; it was opened on, and read only (OPEN), written only (CREATE, CONT) or
; either (DEF).

; CMD OPEN n,"name", CMD CREATE n,"name", CMD CONT n,"name", CMD DEF n,
; "name": the name goes to the drive with ",S,R", ",S,W", ",S,A" or as it
; is, on secondary n.
c_open:
    ld de,'R'*256+FNOW
    jr op1
c_crea:
    ld de,'W'*256+FNOR
    jr op1
c_cont:
    ld de,'A'*256+FNOR
    jr op1
c_def:
    ld de,0
op1:
    push de             ; the mode
    call fnum0
    rst 08h
    defb ','
    push hl
    ld a,(fno)
    call fent
    ld e,E_FO
    jp z,R_ERR          ; open already
    xor a
    call fent
    ld e,E_BO
    jp nz,R_ERR         ; ten open
    pop hl
    dec hl
    rst 10h
    call name0
    pop bc              ; the mode
    push de             ; the text after it
    push bc
    ld a,b
    or a
    jr z,op2
    push hl             ; the name's suffix: ",S," and the letter
    ld a,(hl)
    ld e,a
    add a,4
    ld (hl),a
    ld d,0
    inc hl
    add hl,de
    ld (hl),','
    inc hl
    ld (hl),'S'
    inc hl
    ld (hl),','
    inc hl
    ld (hl),b
    pop hl
op2:
    ld a,(fno)
    ld c,a
    call s14
    call dsk
    xor a
    call fent           ; the free entry
    ld a,(fno)
    ld (hl),a
    inc hl
    ld a,(dev)
    ld (hl),a
    inc hl
    pop bc
    ld (hl),c           ; the mode, no EOF yet
    pop hl
    ret

; CMD CLOSE n: closed on its own drive; a file not open is not an error.
c_clos:
    call fnum0
    push hl
    call fent
    jr nz,cl1
    ld (hl),0
    inc hl
    ld c,a
    ld a,(hl)
    ld (dev),a
    call s16
    call dsk
cl1:
    pop hl
    ret

; CMD CLEAR: every file forgotten, and on the drive channel 15 closed,
; which closes them all.
c_clr:
    rst 10h
    push hl
    call forget
    ld c,0Fh
    call s16
    call dsk
    pop hl
    ret
forget:                 ; every file forgotten
    ld hl,files
    ld b,NFILES*3
fg1:
    ld (hl),0
    inc hl
    djnz fg1
    ret

; CMD PRINT#n[,list]: BASIC's PRINT, its characters to file n. CMD PRINT
; [list]: the same to channel 15, a drive command. Both print as to tape
; (409Ch = 80h: commas as ",", no TAB); the hooks below send the
; characters and end with UNLISTEN.
c_prt:
    rst 10h
    cp '#'
    ld c,0Fh
    jr nz,pr2
    call fnum0
    jr z,pr1            ; nothing after the number
    rst 08h
    defb ','
pr1:
    call rent
    rra
    ld e,E_BD
    jp c,R_ERR          ; open for reading
pr2:
    call lsn2           ; LISTEN, secondary C
    jp c,dnp
    ld a,80h
    ld (OUTDEV),a
    ld (redir),a
    jp R_PRT

; CMD INPUT#n,v1[,v2..]: a line of file n -- up to a CR, the end of the
; file or 250 bytes -- read into BASIC's input buffer, then into the
; variables as INPUT# reads; only inside a program.
c_inp:
    call R_IDCHK        ; first, as CDOS
    rst 10h
    cp '#'
    jp nz,R_SN
    call rdy
    push hl             ; the variables
    ld hl,(INPBUF)
    ld b,250
in1:
    call grx
    cp 0Dh
    jr z,in2
    ld (hl),a
    inc hl
    ld a,(status)
    and 40h
    jr nz,in2           ; the file's end
    djnz in1
in2:
    ld (hl),0
    call s2a            ; UNTALK
    xor a
    ld (INPDEV),a
    ld hl,(INPBUF)
    dec hl
    jp R_INPUT

; CMD GET#n,v1$[,v2$..]: a byte of file n into each string variable. CMD
; GET v1$[,..]: of channel 15, the drive's message, its end for this
; statement only.
c_get:
    rst 10h
    cp '#'
    jr z,gt0
    push hl
    ld hl,chf
    ld (hl),0
    ld (fcur),hl
    pop hl
    ld c,0Fh
    call tlk
    jr gt1
gt0:
    call rdy
gt1:
    call R_VAR          ; DE = the variable
    rst 20h
    jp nz,R_TM          ; a string variable only
    call fdq            ; read to its end already: FD
    push hl
    push de
    call grx
    push af
    ld a,1
    call R_STR1         ; one byte in the string space, described at 40D3h
    pop af
    ld hl,(INPBUF)
    ld (hl),a           ; as CDOS, the byte passes through the input buffer
    ld hl,(TMPSTR+1)
    ld (hl),a
    pop de
    ld hl,TMPSTR
    ld bc,3
    ldir                ; the variable takes it
    pop hl
    dec hl
    rst 10h
    cp ','
    jp nz,s2a           ; UNTALK
    rst 10h
    jr gt1

; for INPUT# and GET#: the file number at HL, a comma, the file open for
; reading, not at its end, then TALK, secondary n.
rdy:
    call fnum0
    rst 08h
    defb ','
    call rent
    and FNOR
    ld e,E_BD
    jp nz,R_ERR         ; open for writing
    call fdq
tlk:                    ; TALK, secondary C
    push hl
    ld a,(dev)
    push bc
    ld c,a
    call s22            ; TALK
    pop bc
    call nc,s26         ; secondary n, the turnaround
    pop hl
    jp c,dnp
    ret

; FD if the file was read to its end. Keeps HL, DE.
fdq:
    push hl
    ld hl,(fcur)
    bit 7,(hl)
    pop hl
    ret z
    ld e,E_FD
    jp R_ERR

; a byte of the file; its end (EOI) is remembered for EOF() and the next read
grx:
    call s1c
    jp c,dnp
    push af
    ld a,(status)
    and 40h
    jr z,grx1
    push hl
    ld hl,(fcur)
    set 7,(hl)
    pop hl
grx1:
    pop af
    ret

; the file number at the word after HL: 2-14, else FC; none: MO. Out: A
; and (fno) = it, HL after it, the flags of what follows.
fnum0:
    rst 10h
    jp z,e_mo
    call R_BYTE
    push af
    call frng
    ld (fno),a
    pop af
    ret
frng:                   ; A: 2-14, else FC
    push af
    sub 2
    cp 13
    jp nc,e_fc
    pop af
    ret

; file (fno), open, else NO: C = its number, `dev` its drive, `fcur` its
; mode's address, A = the mode. Keeps HL and DE.
rent:
    push hl
    ld a,(fno)
    call fent
    ld e,E_NO
    jp nz,R_ERR
    ld c,a
    inc hl
    ld a,(hl)
    ld (dev),a
    inc hl
    ld (fcur),hl
    ld a,(hl)
    pop hl
    ret

; the entry of file A (0: a free one): HL; Z if there is one. Keeps A, C, DE.
fent:
    ld hl,files
    ld b,NFILES
fe1:
    cp (hl)
    ret z
    inc hl
    inc hl
    inc hl
    djnz fe1
    ret

; EOF(n), through the vector at 4161h: -1 when file n was read to its end.
efn:
    call R_FIX          ; INT(), an integer where it fits: so CDOS takes it
    rst 20h
    jp p,e_fc           ; still a floating number: FC, not OV
    ld hl,(ACC)
    ld a,h
    or a
    jp nz,e_fc
    ld a,l
    call frng
    call fent
    ld e,E_NO
    jp nz,R_ERR
    inc hl
    inc hl
    ld a,(hl)
    ld e,E_BD
    bit 1,a             ; FNOR: open for writing
    jp nz,R_ERR
    rla
    sbc a,a
    ld l,a
    ld h,a
    jp R_INT            ; an integer

; the character output's hook (41C1h): under CMD PRINT, the character goes
; to the drive (CIOUT) instead of the screen.
cout:
    ld a,(redir)
    or a
    ld a,c
    ret z
    call s1e
    jp c,dnp
    pop af              ; not back into the ROM's output
    ld a,c
    pop bc
    ret

; the end of PRINT's hook (41BEh), also on every error: UNLISTEN after a
; CMD PRINT. BC, DE and HL kept: the error routine holds its return in BC.
pend:
    ld a,(redir)
    or a
    ret z
    xor a
    ld (redir),a
    push bc
    call s28
    pop bc
    ret

; ---- the service call: RST 28H, A = an even index into `table`.
; Every register reaches the service as it came, and Z; C is clear (ADD HL).
p_svc:
svc:
    push hl
    push de
    ld e,a
    ld d,0
    ld hl,table
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    ex de,hl
    pop de
    ex (sp),hl
    ret

table:
    defw s00, s02, s04, s06, s08, s0a, s0c, s0e, s10, fail
    defw s14, s16, s18, s1a, s1c, s1e, s20, s22, s24, s26, s28, s2a, fail

    defb 0              ; service 0x00 points HL at a zero byte, as CDOS does
s00:
    ld a,10h            ; version 1.0
    ld hl,s00-1
    ret
s02:                    ; the handler for unknown records, the load limit
    ld (handler),de
    ld (limit),hl
    ret
s04:
    ld hl,(limit)
    ld de,(handler)
    ret
s06:
    ld a,c
    ld (dev),a
    ret
s08:
    ld a,(dev)
    ret
s0a:
    ld a,c
    ld (status),a
    ret
s0c:
    ld a,(status)
    ret

; ---- load and save, through PRIFLOP's turbo
p_xfer:

; 0x10 with mask 0: a plain file -- CMD $'s directory -- to DE without its
; two-byte load address, until EOI, below the limit, on the standard bus.
; Out, as CDOS's load through CDOS's own raw handler: Z with A = 63h, DE =
; where the next byte would go; Z with A = 0A8h (out of memory), DE = 65535
; less the bytes stored; Z with the drive's code, or C, as any load.
raw:
    ld c,0              ; OPEN on secondary 0 (the bus services keep DE)
    call s14
    ret c
    ret z
    ld hl,lbus          ; where a bus error goes
    push hl
    ld (lsp),sp
    ld a,(dev)
    ld c,a
    call s22            ; TALK
    ld c,0
    call s26            ; secondary 0x60, the turnaround
    call c,bail
    call rx             ; the load address, dropped
    call rx
    ex de,hl
    ld bc,0FFFFh        ; the bytes left, as CDOS counts them
raw1:
    ld a,(SYSSCR)       ; below the screen, only below the limit
    dec a
    cp h
    jr c,raw2
    ld de,(limit)
    ex de,hl
    scf
    sbc hl,de           ; C: the address is at or above the limit
    ex de,hl
    jr c,rawom
raw2:
    call rx
    ld (hl),a
    cpi                 ; HL+1, BC-1
    ld a,(status)
    bit 6,a
    jr z,raw1
    ex de,hl            ; EOI: the end
    ld a,63h
    jr rawe
rawom:
    ld d,b
    ld e,c
    ld a,0A8h           ; out of memory
rawe:
    cp a
    pop bc              ; the bus error's route
    push af
    call lclose         ; UNTALK, CLOSE: their error, else A, DE, flags
    jr c,rawx
    jr z,rawx
    pop af
    ret
rawx:
    pop bc
    ret
lbus:                   ; a bus error: as CDOS, the error channel first
    call s1a
lclose:                 ; UNTALK, CLOSE and the error channel
    call s2a
    ld c,0
    jp s16
rx:                     ; a byte; on a bus error, to the route on the stack
    call s1c
    ret nc
bail:
    ld sp,(lsp)
    ret

; 0x0E: save the descriptor at DE as the file named at HL.
; 0x10: load the file named at HL, the records of the types in mask C, base DE.
; Both go through the turbo: the drive is asked with
; `XT`, the index and the registers, then reads the name, the descriptor and
; CDOS-format variables itself and gives orders (order.asm) until the end.
; There is no standard-bus fallback (checkpoint 2), and the
; handler of service 0x02 is not called: the drive parses the records.
; Out, as CDOS: NZ NC, A = a load's last record type, DE = its end; Z, A =
; the drive's code, DE as it came; C, a bus error, A = status, HL and DE as
; they came. Unless C, HL points at the drive's message (`errbuf`).
; A drive that does not answer `00,TURBO` -- a 1541, PRIFLOP with its turbo
; off -- leaves Z with A = its own answer's code (30 or 31, SYNTAX ERROR) and no
; transfer.
s10:
    inc c
    dec c
    jp z,raw            ; mask 0: a plain file, on the standard bus
    ld a,10h
    call xt
    ret c
    ret z
    push af
    ld a,(obuf)
    cp 'E'              ; ended by the drive: the EOI that CDOS's status
    jr nz,s10a          ; shows after a load
    ld a,40h
    ld (status),a
s10a:
    pop af
    ret
s0e:
    ld a,0Eh
xt:                     ; ask the drive, then the turbo; A = the index
    ld (xtbuf+3),a      ; the command: `XT`, the index, HL, DE, BC, `.`
    ld (xtbuf+4),hl
    ld (xtbuf+6),de
    ld (xtbuf+8),bc
    push hl             ; HL and DE as they came: a bus error keeps both,
    push de             ; any other error DE, as CDOS
    ld hl,xtbuf
    call s18
    jr c,xback          ; no drive: C, A = status
    jr z,xerr           ; refused: Z, A = the drive's code
    or a
    jr nz,xref          ; 01: not the turbo's answer
    ld a,(errbuf+3)
    cp 'T'              ; "00,T": the turbo's
    jr z,xgo
    xor a
xref:
    cp a                ; Z, A = 00 or 01, with the drive's text
xerr:
    pop de
    pop hl
xmsg:
    ld hl,errbuf        ; HL: the drive's message, as CDOS
    ret
xback:
    pop de
    pop hl
    ret
xgo:
    call turbo
    jr c,xback
    jr z,xerr
    pop hl
    pop hl
    jr xmsg

xoff:                   ; for order.asm: interrupts and the NMI off, the port
    call nmi_off        ; in C
    ld a,(outport)
    ld c,a
    ret
x_end:                  ; for order.asm: leave turbo, NMI on, the error channel
    ld a,OP_QUIT
    call x_op
    xor a               ; CLOCK and DATA let go, as the shadow already says
    out (c),a
    call nmi_on
    jp s1a

    include "order.asm"
    include "xop.asm"
    include "xfer.asm"


; ---- the channel services
p_chan:
s14:                    ; OPEN: the name at HL on secondary F0h|C
    ld a,c
    or 0F0h
s14b:
    ld c,a
    xor a
    ld (status),a
    call lsn2
    ret c
    ld a,(hl)           ; the name: a length, then the bytes
    inc hl
    or a
    jr z,s14d
    ld b,a
s14c:
    ld c,(hl)
    inc hl
    call s1e
    ret c
    djnz s14c
s14d:
    call s28
    call stchk
    ret c
    jr s1a
s18:                    ; a command, length-prefixed at HL, on channel 15
    ld a,0Fh
    jr s14b

s16:                    ; CLOSE: secondary E0h|C
    ld a,c
    and 0EFh
    or 0E0h
    ld c,a
    call lsn2
    call s28
    ld a,(status)
    and 0BFh
    ld a,(status)
    scf
    ret nz
                        ; fall into s1a
; 0x1A: the error channel into `errbuf`; A = its number, Z if it is 2 or
; more, NC NZ if 00 or 01. C: a bus error.
s1a:
    ld a,(status)
    push af
    xor a
    ld (status),a
    ld a,(dev)
    ld c,a
    call s22
    jr c,s1ax
    ld c,0Fh
    call s26
    ld hl,errbuf
    ld b,ERRLEN
s1a1:
    call s1c
    jr c,s1ax
    inc b
    dec b
    jr z,s1a2           ; full: read on to the end, keep no more
    ld (hl),a
    inc hl
    dec b
s1a2:
    ld a,(status)
    bit 6,a
    jr z,s1a1
    dec hl
    ld (hl),0           ; the closing CR becomes the end mark
    call s2a
    pop af
    ld (status),a
    ld hl,errbuf
    ld a,(hl)
    sub '0'
    add a,a
    ld b,a
    add a,a
    add a,a
    add a,b
    inc hl
    add a,(hl)
    sub '0'
    dec hl
    cp 2
    ccf
    ret nc
    cp a
    ret
s1ax:
    pop bc
    ret

; LISTEN the device, secondary C, ATN released. C (A = status) if the
; status says something went wrong.
lsn2:
    push bc
    ld a,(dev)
    ld c,a
    call s20
    pop bc
    call s24
stchk:                  ; C (A = status) if the status says anything
    ld a,(status)
    or a
    scf
    ret nz
    ccf
    ret

; ---- the bus
p_bus:
s20:                    ; LISTEN device C
    ld a,20h
    jr s22b
s22:                    ; TALK device C
    ld a,40h
s22b:
    or c
    push af
    xor a
    ld (pendf),a        ; a byte held back is dropped
    pop af
                        ; fall into atn
; send A under ATN: CLOCK and ATN pulled, DATA released, and a device must
; answer by pulling DATA. A secondary (`sec`) leaves ATN as it is: after an
; error has released it, nobody answers, as with CDOS. The bus services keep
; DE and HL, as CDOS's do.
atn:
    push de
    ld e,LATN
    db 1                ; LD BC,nn: skips PUSH DE and LD E's opcode; 00 is a NOP
sec:
    push de
    ld e,0
    push bc
    call atn0
    pop bc
    pop de
    ret

s24:                    ; secondary 60h|C after LISTEN, then ATN released
    ld a,c
    or 60h
    call sec
atnrel:
    ld a,(shadow)
    and 0FFh-LATN
    jp setl

s26:                    ; secondary 60h|C after TALK, then the turnaround
    ld a,c
    or 60h
    call sec
    ret c
    push de
    call nmi_off
    ld a,LCLK+LDAT      ; DATA pulled, ATN released
    call setl
    ld b,4              ; a short pause: the drive takes CLOCK at once
s26a:
    djnz s26a
    ld a,LDAT           ; CLOCK released: the drive takes it
    call setl
    ld de,ICLK*256
    call waitl
    pop de
    jp nmi_on

s28:                    ; UNLISTEN; a byte held back goes first, with EOI
    ld a,3Fh
    jr unl
s2a:                    ; UNTALK
    call nmi_off
    ld a,LATN+LCLK
    call setl
    ld a,5Fh
unl:
    push af
    scf
    call flush
    pop af
    call atn
    call atnrel         ; ATN released, then the rest
    call dly
    xor a
    jp setl

s1e:                    ; send C; the byte before it goes out now (CIOUT)
    or a
    call flush
    ret c
    ld a,c
    ld (pendb),a
    ld a,0FFh
    ld (pendf),a
    ld a,c
    ret

; send the byte held back, if there is one; carry = with EOI. Keeps BC, DE.
flush:
    ld a,(pendf)
    inc a               ; carry kept; Z: a byte is waiting
    ret nz
    ld (pendf),a
    push bc
    push de
    ld a,(pendb)
    ld c,a
    call sendb
    pop de
    pop bc
    ret

atn0:
    ld c,a
    ld b,T_GAP          ; a pause after the last byte: a device that was not
atn1:                   ; addressed lets DATA go meanwhile, so that the check
    djnz atn1           ; below sees it gone, no sooner than CDOS's does
    call nmi_off
    ld a,(shadow)
    and LATN
    or e
    or LCLK
    call setl
    ld b,T_DEV
    ld de,IDAT*256
    call wt
    jr c,e_np
                        ; fall into sendb, NC: no EOI
; send C; carry = with EOI. NC: done. C: an error, A = status.
sendb:
    push af
    call nmi_off
    ld a,(shadow)
    and 0FFh-LDAT
    call setl
    call dst
    jr nz,e_npp         ; nobody holds DATA: nobody there
    ld a,(shadow)       ; CLOCK released: ready to send
    and LATN
    call setl
    pop af
    ld de,IDAT*257
    jr nc,sb1
    call waitl          ; with EOI: wait for the listener's acknowledge,
    ld e,0              ; DATA low and high again
    call waitl
    ld e,IDAT
sb1:
    call waitl          ; ready for data
    ld a,(shadow)
    or LCLK
    call setl
    ld b,8
sb2:
    call dst
    jr z,e_to           ; the listener pulled DATA: it gave up
    ld a,(shadow)
    rr c
    jr c,sb3
    or LDAT             ; a 0 bit pulls DATA
sb3:
    call setl           ; each line change here lasts longer than CDOS's
    and 0FFh-LCLK       ; CLOCK released: the bit is valid
    call setl
    or LCLK
    call setl
    and 0FFh-LDAT
    call setl
    djnz sb2
    push bc
    ld b,T_ACK          ; the listener's acknowledge
    ld de,IDAT*256
    call wt
    pop bc
    jr c,e_to
    jp nmi_on           ; NC

e_npp:
    pop af
e_np:
    ld a,80h            ; device not present
    jr e_set
e_to:
    ld a,03h            ; timeout
e_set:
    push hl
    ld hl,status
    or (hl)
    ld (hl),a
    pop hl
    call nmi_on
    xor a               ; every line released
    call setl
    ld a,(status)
fail:                   ; 0x12 (verify) and 0x2C (printer) are not provided: C
    scf
    ret

; 0x1C: receive a byte (ACPTR). A = the byte; EOI sets status bit 6.
s1c:
    push bc
    push de
    call nmi_off
    ld a,(shadow)       ; CLOCK released
    and 0FFh-LCLK
    call setl
    ld de,ICLK*257
    call waitl          ; the talker is ready to send
    ld c,0              ; C: 0, then 1 after an EOI
r1:
    ld a,(shadow)       ; DATA released: ready for data
    and 0FFh-LDAT
    call setl
    ld b,T_EOI
    ld de,ICLK*256
    call wt
    jr nc,r2
    inc c               ; no CLOCK in time: EOI, or after one, a timeout
    dec c
    jr nz,r_to
    ld a,(shadow)       ; acknowledge the EOI: DATA pulled a while
    or LDAT
    call setl
    call dly
    ld a,(status)
    or 40h
    ld (status),a
    inc c
    jr r1
r2:
    ld b,8
r3:
    in a,(INP)          ; CLOCK high ...
    and ICLK
    jr z,r3
    in a,(INP)          ; ... and still high at once: its DATA is the bit
    bit 5,a
    jr z,r3
    and IDAT
    add a,0F0h          ; DATA into carry
    rr c
r4:
    in a,(INP)          ; CLOCK low, twice
    and ICLK
    jr nz,r4
    in a,(INP)
    and ICLK
    jr nz,r4
    djnz r3
    ld a,(shadow)       ; the frame acknowledge: DATA pulled
    or LDAT
    call setl
    ld a,(status)
    bit 6,a
    jr z,r5
    call dly            ; the last byte: let go of the bus
    xor a
    call setl
r5:
    call nmi_on
    ld a,c
    pop de
    pop bc
    ret                 ; NC from nmi_on
r_to:
    pop de
    pop bc
    jp e_to

; ---- line primitives

; wait for (INP & D) == E, seen twice, for as long as it takes; between
; rounds of 256 polls it looks at the Reset button and turns the NMI on if it
; is pressed, so the user can always get out. B is lost.
waitl:
    ld b,0
    call wt
    ret nc
    in a,(0)
    and 2
    call nz,nmi_on
    jr waitl

; the same, at most B polls. NC: seen. C: not in time.
wt:
    in a,(INP)
    and d
    cp e
    jr nz,wt1
    in a,(INP)
    and d
    cp e
    ret z
wt1:
    djnz wt
    scf
    ret

; DATA, read until two reads agree: Z = low
dst:
    in a,(INP)
    and IDAT
    jr z,dst1
    in a,(INP)
    and IDAT
    ret nz
    jr dst
dst1:
    in a,(INP)
    and IDAT
    ret z
    jr dst

; a pause of about 40 us between line changes, so that each lasts at least
; as long as CDOS's own. A is kept.
dly:
    push bc
    ld b,6
dly1:
    djnz dly1
    pop bc
    ret

; the lines: A = the bits pulled
setl:
    push bc
    ld bc,(outport)
    out (c),a
    ld (shadow),a
    pop bc
    ret

nmi_off:
    di
    ld a,(SYSNMI)
    and 7Fh
    jr nmi1
nmi_on:
    ei
    ld a,(SYSNMI)
    or 80h
nmi1:
    ld (SYSNMI),a
    out (0),a
    ret

; ---- variables
p_data:
outport: defb 80h       ; the output port; the loader writes it
shadow: defb 0          ; the lines as last set
dev:    defb 8
cdev:   defb 8          ; CMD SET's drive: every CMD sets `dev` to it
pdev:   defb 4          ; CMD SET's printer, for the printer extension
held:   defb 0          ; 1: `errbuf` holds a message not shown yet: a drive
                        ; command's 01, or an error under ON ERROR
redir:  defb 0          ; under CMD PRINT: the characters go to the drive
fno:    defb 0          ; the file number of the statement
fcur:   defw 0          ; ... its mode's address in `files`, or `chf`
chf:    defb 0          ; CMD GET's channel 15: the end of its message seen
NFILES  equ 10
files:  defs NFILES*3   ; the open files: number (0: free), drive, mode
status: defb 0          ; 40h EOI, 03h timeout, 80h device not present
pendb:  defb 0          ; the byte CIOUT holds back
pendf:  defb 0          ; ... and whether there is one
handler: defw 0         ; service 0x02's; kept, never called (the turbo parses)
clkval: defb 0          ; the turbo's CLOCK output value now (xfer.asm)
xtbuf:  defb 10         ; the turbo's command: length, `XT`, index, HL, DE,
        defm "XT"       ; BC, and `.`, so that a 0x0D in B is not taken for
        defs 7          ; the line's end
        defm "."
ERRLEN  equ 40
errbuf: defs ERRLEN+1
lsp     equ errbuf+ERRLEN-1 ; the raw load's stack for a bus error's route, in
                        ; the buffer's tail: it reads the error channel only
                        ; before it starts and after it ends
; The drive reads the load limit at (0x4174) + 0x0B8F, where CDOS has it
; (point 6): the body is padded up to it, and build_dos.py
; checks it is there.
p_pad:
    defs 0B8Fh - 8 - (ASMPC - p_cmd)
; How a later boot finds this DOS, whatever its build (loader.asm): a
; signature at a fixed distance from the base, and where the vectors it
; chains to are kept.
p_anchor:
dossig: defm "PDOS"
        defw oldcmd, oldhook
limit:  defw 0
