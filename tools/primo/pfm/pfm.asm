; PFM, the PRIFLOP Manager: the Primo's file selector.
; Copyright (C) 2026 sd2iec_pico2w contributors; GPL v2.
;
; One 0xD9 record at ORG and a 0xC3 autostart; CMD LOAD with no name loads it
; (our DOS sends the name PFM). It talks to the drive only through the DOS's
; service call (RST 28H) and to the screen and keyboard only through the
; ROM's jump table.
;
; It is not resident and never returns into the load that started it. To
; load a file it writes CMD LOAD"name" into BASIC's input buffer, puts
; BASIC's stack back as it is at the prompt, and enters the ROM where the
; line input returns a typed line: the load is the statement's own, exactly
; as if typed, and nothing of PFM has to survive it. Q leaves the same way,
; with NEW, because PFM itself overwrote the program area.
;
; Settings: PRIFLOP.CFG is read by name through the byte channel, only the
; keys PFM owns are rewritten, everything else is copied as it was. The file
; is written twice: first as PRIFLOP.TMP, read back and compared, then as
; PRIFLOP.CFG, read back again. Nothing is kept in memory but the old file:
; the new one is generated again for every pass (`gen`).

ORG_PFM equ 4400h
    org ORG_PFM

; the ROM's jump table (Primo software booklet, part II ch.3)
DSPHND  equ 0015h       ; A to the screen: a character, or a control code; all kept
KKBHND  equ 0025h       ; wait for a key: A = its code, the rest kept
DIRCUR  equ 0080h       ; the cursor to row D (0-15), column E (0-41)
; BASIC (TRS-80 Level II's layout): the line input's return, HL = the line
; less one, NC; and the pointers
LINRET  equ 1A7Eh
STRSPC  equ 40A0h       ; the string space; at the prompt the stack is under it
INPBUF  equ 40A7h       ; the input buffer

; the screen's control codes
C_NORM  equ 01h         ; normal: no inverse, no pre-clear
C_INV   equ 04h         ; inverse on
C_INVX  equ 14h         ; inverse off
C_PCLR  equ 06h         ; pre-clear on: a character overwrites the cell
C_CLS   equ 0Ch

; the keys, as the ROM's key handler gives them (measured on the B64 with
; KEYS.PRG, 2026-10-11: the arrows; in the simulation: RETURN and BRK)
K_BRK   equ 01h
K_LEFT  equ 08h
K_DOWN  equ 0Ah
K_RET   equ 0Dh
K_UP    equ 1Fh
K_RIGHT equ 19h

; the DOS's services (RST 28H, A = the index)
S_SETDEV equ 06h        ; C = the device
S_GETDEV equ 08h        ; A = the device
S_STATUS equ 0Ch        ; A = the status: 40h EOI
S_OPEN   equ 14h        ; the name at HL (length first), secondary C
S_CLOSE  equ 16h        ; secondary C
S_CMD    equ 18h        ; the command at HL (length first)
S_ACPTR  equ 1Ch        ; a byte in A
S_CIOUT  equ 1Eh        ; C out
S_LISTEN equ 20h        ; device C
S_TALK   equ 22h
S_SECL   equ 24h        ; secondary C after LISTEN
S_SECT   equ 26h        ; secondary C after TALK
S_UNL    equ 28h
S_UNT    equ 2Ah
; after OPEN, a command and CLOSE: NC NZ all right (A = 00 or 01), Z the
; drive's code in A (its message at HL), C no drive or a bus error

ROWS    equ 14          ; files per page, on rows 0-13
NAMEL   equ 16
E_TYPE  equ 16          ; an entry: the name, 0-padded; its type (3);
E_BLK   equ 19          ; its blocks; the name's length
E_LEN   equ 21
ESIZE   equ 22
CFGMAX  equ 1024        ; the largest config file PFM reads
SEC     equ 2           ; the secondary address of the config file

start:
    ld hl,bss           ; the variables are not in the file: whatever the
    ld de,bss+1         ; last program left there, so all of them 0 first
    ld bc,bssend-bss-1
    ld (hl),0
    ldir
    ld a,S_GETDEV
    rst 28h
    ld (dev),a
    ld (dev0),a
    ld (v_dev),a
    xor a
    ld (msg+1),a        ; no message
    ld (copen),a
    ld a,1
    ld (v_turbo),a      ; PFM came through the turbo
    call csize
    call scrinit
dir0:                   ; a new directory: its first page
    call cinval
    ld hl,0
    ld (skip),hl
    xor a
    ld (pageno),a
    ld (cur),a
dir1:
    call getpage
    ld a,(count)        ; the cursor on the page
    ld b,a
    ld a,(cur)
    cp b
    jr c,dir2
    ld a,b
    or a
    jr z,dir2
    dec a
dir2:
    ld (cur),a
    call draw
key:
    call getkey
    ld hl,keytab
key1:
    ld b,(hl)
    inc b
    dec b
    jr z,key
    inc hl
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    cp b
    jr nz,key1
    ex de,hl
    jp (hl)

keytab:
    defb K_DOWN
    defw k_down
    defb ' '
    defw k_down
    defb K_UP
    defw k_up
    defb K_RIGHT
    defw k_next
    defb 'N'
    defw k_next
    defb K_LEFT
    defw k_prev
    defb 'P'
    defw k_prev
    defb K_RET
    defw k_ret
    defb 'C'
    defw k_sett
    defb 'Q'
    defw k_quit
    defb 0

k_down:
    ld a,(count)
    ld b,a
    ld a,(cur)
    inc a
    cp b
    jr nc,k_next0
    jr k_move
k_next0:                ; past the last line: the next page, if any
    xor a
    ld (cur),a
k_next:
    ld a,(more)
    or a
    jp z,key
    ld hl,(skip)
    ld de,ROWS
    add hl,de
    ld (skip),hl
    ld hl,pageno
    inc (hl)
    jr k_load
k_up:
    ld a,(cur)
    or a
    jr z,k_up1
    dec a
k_move:                 ; the bar to line A: only the two rows are drawn
    ld hl,cur
    ld c,(hl)
    ld (hl),a
    call drawrow        ; the old line, now plain
    ld a,(cur)
    ld c,a
    call drawrow
    jp key
k_up1:                  ; above the first line: the page before, at its end
    ld a,(pageno)
    or a
    jp z,key
    ld a,ROWS-1
    ld (cur),a
    jr k_prev1
k_prev:
    ld a,(pageno)
    or a
    jp z,key
    xor a
    ld (cur),a
k_prev1:
    ld hl,(skip)
    ld de,-ROWS
    add hl,de
    ld (skip),hl
    ld hl,pageno
    dec (hl)
k_load:
    jp dir1

k_ret:
    ld a,(count)
    or a
    jp z,key
    call curent         ; HL = the entry
    call isup
    jp z,k_back         ; "..": up one level
    push hl
    call isdir
    pop hl
    jr z,k_cd
    jp go_load
k_cd:                   ; a folder or a disk image: CD:name
    call cinval
    call cmdnew
    ld hl,s_cd
    call cmdstr
    call curent
    push hl
    ld de,E_LEN
    add hl,de
    ld b,(hl)
    pop hl
k_cd1:
    ld a,(hl)
    call cmdadd
    inc hl
    djnz k_cd1
    jr k_cd2
k_back:                 ; "..": up one level, CD:_
    call cinval
    call cmdnew
    ld hl,s_cdup
    call cmdstr
k_cd2:
    call cmdsend
    jp nc,dir0
    jp dir1             ; the message shows on the status line

k_sett:
    call cinval         ; the settings use the cache's memory
    call settings
    jp dir1

k_quit:
    call dclose
    ld de,(INPBUF)
    inc de
    call setpre
    ld hl,s_new
    call strcpy
    jp handover

; RETURN on a file: CMD LOAD"name", as typed
go_load:
    push hl             ; the entry: CLOSE leaves HL at the drive's message
    call dclose         ; (6l: the status was loaded as the name)
    ld de,(INPBUF)
    inc de
    call setpre
    ld hl,s_load
    call strcpy
    pop hl
    push hl
    ld bc,E_LEN
    add hl,bc
    ld b,(hl)
    pop hl
    inc b
    dec b
    jr z,gl2
gl1:
    ld a,(hl)
    ld (de),a
    inc hl
    inc de
    djnz gl1
gl2:
    ld a,22h            ; the closing quote
    ld (de),a
    inc de
    xor a
    ld (de),a
handover:
    ld a,C_NORM         ; the screen as BASIC leaves it: normal, cleared
    call DSPHND
    ld a,C_CLS
    call DSPHND
    ld hl,(STRSPC)      ; BASIC's stack at the prompt: a zero word under the
    ld sp,hl            ; strings, and the line input's return is taken
    ld hl,0
    push hl
    ld hl,-64           ; the wiper goes under the stack, into free memory,
    add hl,sp           ; and runs there: nothing of PFM is left
    ex de,hl
    push de
    ld hl,wipe
    ld bc,wipeend-wipe
    ldir
    ret

wipe:                   ; position-independent: PFM's memory zeroed, then
    ld bc,(wlen)        ; the line goes to BASIC
    ld hl,ORG_PFM
    ld de,ORG_PFM+1
    ld (hl),0
    ldir
    ld hl,(INPBUF)
    ld a,K_RET
    or a                ; NC: the line ended with RETURN, not BRK
    jp LINRET
wipeend:

; "CMD SET n:" at DE when the device was changed in the settings
setpre:
    ld a,(dev)
    ld b,a
    ld a,(dev0)
    cp b
    ret z
    ld hl,s_set
    call strcpy
    ld a,b
    cp 10
    jr c,sp1
    ld a,'1'
    ld (de),a
    inc de
    ld a,b
    sub 10
sp1:
    add a,'0'
    ld (de),a
    inc de
    ld a,':'
    ld (de),a
    inc de
    ret

; the 0-terminated string at HL to DE; DE after it, at its 0
strcpy:
    ld a,(hl)
    ld (de),a
    or a
    ret z
    inc hl
    inc de
    jr strcpy

; HL = the entry under the cursor
curent:
    ld a,(cur)
entry:                  ; HL = entry A of the page
    ld l,a
    ld h,0
    call x22
    ld de,(pgptr)
    add hl,de
    ret

x22:                    ; HL x ESIZE (22); DE lost
    add hl,hl           ; x2
    ld d,h
    ld e,l
    add hl,hl           ; x4
    add hl,hl           ; x8
    add hl,hl           ; x16
    add hl,de           ; x18
    add hl,de           ; x20
    add hl,de           ; x22
    ret

; Z if the entry at HL is "..", the one dopen puts first outside the root
isup:
    ld a,(hl)
    cp '.'
    ret nz
    inc hl
    ld a,(hl)
    dec hl
    cp '.'
    ret nz
    push hl
    ld de,E_LEN
    add hl,de
    ld a,(hl)
    pop hl
    cp 2
    ret

; Z if the entry at HL is a folder: of type DIR, or a disk image by its name
isdir:
    push hl
    ld de,E_TYPE
    add hl,de
    ex de,hl
    ld hl,s_dir
    call cmp3
    pop hl
    ret z
    ld de,E_LEN
    push hl
    add hl,de
    ld a,(hl)
    pop hl
    sub 4
    jr c,isd9           ; shorter than ".D64"
    ld e,a
    ld d,0
    add hl,de
    ld a,(hl)
    cp '.'
    jr nz,isd9
    inc hl
    ld a,(hl)
    call upcase
    cp 'D'
    jr nz,isd9
    inc hl
    ex de,hl            ; DE = the last two characters
    ld hl,s_imgs
isd1:
    ld a,(hl)
    or a
    jr z,isd9
    push de
    ld a,(de)
    call upcase
    cp (hl)
    inc hl
    jr nz,isd2
    inc de
    ld a,(de)
    call upcase
    cp (hl)
isd2:
    pop de
    ret z
    inc hl
    jr isd1
isd9:
    or 1                ; NZ
    ret

; Z if the three characters at DE equal those at HL
cmp3:
    ld b,3
c3a:
    ld a,(de)
    cp (hl)
    ret nz
    inc de
    inc hl
    djnz c3a
    ret

; a key, letters as capitals
getkey:
    call keywait
upcase:
    cp 'a'
    ret c
    cp 'z'+1
    ret nc
    sub 20h
    ret

; ---- the directory, cached: the entries read so far stay in memory, from
; `cache` up to under the stack, and the listing's channel stays open between
; keys, so a page forward reads only its own entries and a page back reads
; nothing. The cache holds entries cbase .. cbase+ccnt-1;
; when it is full, reading on drops what it holds; a page before cbase reads
; the listing again from its start. Outside the root the listing's entry 0
; is "..", put there by dopen.

getpage:                ; the page at `skip`: count, more and pgptr set
    ld (errsp),sp
    xor a
    ld (count),a
    ld (more),a
    ld hl,(skip)
    ld de,(cbase)
    or a
    sbc hl,de
    call c,cinval       ; before the cache: from the start
gp1:
    ld hl,(skip)        ; enough? cbase+ccnt >= skip+ROWS+1, or the end
    ld de,ROWS+1
    add hl,de
    ex de,hl
    call cend           ; HL = cbase+ccnt
    or a
    sbc hl,de
    jr nc,gp8
    ld a,(ceof)
    or a
    jr nz,gp8
    ld a,(copen)
    or a
    call z,dopen        ; the channel is opened, the cache emptied
    ld hl,(ccnt)
    ld de,(cmax)
    or a
    sbc hl,de
    call nc,slide       ; full
gp2:
    ld hl,(ccnt)        ; empty and before the page: an entry read and dropped
    ld a,h
    or l
    jr nz,gp3
    ld hl,(cbase)
    ld de,(skip)
    or a
    sbc hl,de
    jr nc,gp3
    ld hl,cache
    call rdent
    jr c,gp1
    ld hl,(cbase)
    inc hl
    ld (cbase),hl
    jr gp1
gp3:
    ld hl,(ccnt)        ; into the next slot
    call x22
    ld de,cache
    add hl,de
    call rdent
    jr c,gp1
    ld hl,(ccnt)
    inc hl
    ld (ccnt),hl
    jr gp1
gp8:
    call untalk
    ld hl,(skip)        ; the page: from skip - cbase in the cache ...
    ld de,(cbase)
    or a
    sbc hl,de
    push hl
    call x22
    ld de,cache
    add hl,de
    ld (pgptr),hl
    call cend           ; ... and cbase+ccnt-skip entries of it, at most ROWS
    ld de,(skip)
    or a
    sbc hl,de
    pop de
    ret c
    ret z
    ld a,h
    or a
    jr nz,gp9
    ld a,l
    cp ROWS+1
    jr nc,gp9
    ld (count),a
    ret
gp9:
    ld a,ROWS
    ld (count),a
    ld a,1              ; one past the page: there is more
    ld (more),a
    ret

; the cache full: what is before the page goes, the rest moves to its start
slide:
    call cend
    ld de,(skip)
    or a
    sbc hl,de           ; HL = cbase+ccnt-skip, the entries kept
    jr nc,slid1
    call cend           ; the page is past it all: nothing is kept
    ld (cbase),hl
    ld hl,0
    ld (ccnt),hl
    ret
slid1:
    ld (ccnt),hl
    call x22
    push hl             ; their bytes
    ld hl,(skip)
    ld de,(cbase)
    ld (cbase),hl
    or a
    sbc hl,de
    call x22
    ld de,cache
    add hl,de           ; from the page's first
    pop bc
    ld a,b
    or c
    ret z
    ldir
    ret

cend:                   ; HL = cbase+ccnt, the entry the channel gives next
    ld hl,(cbase)
    push de
    ld de,(ccnt)
    add hl,de
    pop de
    ret

; the listing opened, its header read for the title; the cache empty
dopen:
    ld hl,0
    ld (cbase),hl
    ld (ccnt),hl
    xor a
    ld (ceof),a
    ld hl,title
    ld b,NAMEL+1
dp0:
    ld (hl),a
    inc hl
    djnz dp0
    ld hl,dollar
    ld c,0
    ld a,S_OPEN
    rst 28h
    jp c,e_bus
    jp z,e_drv
    ld a,1
    ld (copen),a
    call dtalk
    call rb             ; the load address
    call rb
    call rline          ; the header: its name is the title
    ret c
    ld hl,line
    call quote
    ret c
    ld de,title
    ld b,NAMEL
    call qcopy
    xor a
    ld (de),a
    ld hl,s_root        ; the root's header is the volume's name: PRIFLOP
    call istitle        ; (lfsop_getvolumename), RAMDISK 9 on the RAM drive;
    ret z               ; anywhere else, a folder or a disk image, ".." first
    ld hl,s_root9       ; (a folder named so gets none)
    call istitle
    ret z
    ld hl,dotdot
    ld de,cache
    ld bc,ESIZE
    ldir
    ld hl,1
    ld (ccnt),hl
    or a
    ret

; Z if `title` is the 0-terminated name at HL, spaces after it allowed
istitle:
    ld de,title
it1:
    ld a,(hl)
    or a
    jr z,it2
    ld a,(de)
    cp (hl)
    ret nz
    inc hl
    inc de
    jr it1
it2:
    ld a,(de)
    or a
    ret z
    cp ' '
    ret nz
    inc de
    jr it2

; the next line into `line` (40 characters kept), its number in `blk`; C at
; the listing's end: the channel is closed, `ceof` set
rline:
    ld a,(talking)
    or a
    call z,dtalk
    call rb             ; a line's link: 0 ends the listing
    ld e,a
    call rb
    or e
    jr z,rl9
    call rb             ; its number: the blocks
    ld (blk),a
    call rb
    ld (blk+1),a
    ld hl,line
    ld b,40
rl2:
    call rb             ; its text, up to 0
    or a
    jr z,rl3
    inc b
    dec b
    jr z,rl2
    ld (hl),a
    inc hl
    dec b
    jr rl2
rl3:
    ld (hl),0
    or a
    ret
rl9:
    call dclose
    ld a,1
    ld (ceof),a
    scf
    ret

; the next entry into the slot at HL; C at the end. Lines without a name
; (BLOCKS FREE) are passed over.
rdent:
    push hl
re1:
    call rline
    jr c,re9
    ld hl,line
    call quote
    jr c,re1
    pop de
    jp pent
re9:
    pop hl
    ret

untalk:                 ; the drive stops talking; the channel stays open
    ld a,(talking)
    or a
    ret z
    xor a
    ld (talking),a
    ld a,S_UNT
    rst 28h
    ret

dclose:                 ; the listing's channel closed, if open
    call untalk
    ld a,(copen)
    or a
    ret z
    xor a
    ld (copen),a
    ld c,0
    ld a,S_CLOSE
    rst 28h
    ret

cinval:                 ; the cache emptied, the channel closed
    call dclose
cinv0:
    ld hl,0
    ld (cbase),hl
    ld (ccnt),hl
    xor a
    ld (ceof),a
    ret

; the cache's size: from `cache` to 256 bytes under the stack, in entries;
; and how much the handover's wiper clears
csize:
    ld hl,-256
    add hl,sp
    ld de,cache
    or a
    sbc hl,de           ; the bytes there are ...
    jr c,cs00
    ld de,CFGMAX
    sbc hl,de
    add hl,de
    jr nc,cs01
cs00:
    ld hl,CFGMAX        ; ... at least the config buffer's
cs01:
    ld bc,0
cs0:
    ld de,ESIZE
    or a
    sbc hl,de
    jr c,cs1z
    inc bc
    jr cs0
cs1z:
    ld (cmax),bc
    ld h,b
    ld l,c
    call x22
    ld de,bssend-cache
    or a
    sbc hl,de
    add hl,de
    jr nc,cs2z
    ex de,hl            ; at least to bssend
cs2z:
    ld de,cache-ORG_PFM-1
    add hl,de
    ld (wlen),hl
    ret

dtalk:                  ; TALK, the listing's channel
    ld c,0
    call talk
    ld a,1
    ld (talking),a
    ret

talk:                   ; TALK, secondary C
    ld a,c
    ld (chan),a
    push bc
    ld a,(dev)
    ld c,a
    ld a,S_TALK
    rst 28h
    pop bc
    jp c,e_bus
    ld a,S_SECT
    rst 28h
    jp c,e_bus
    ret

rb:                     ; a byte; a bus error closes the channel and leaves
    ld a,S_ACPTR        ; through e_bus
    rst 28h
    ret nc
    xor a
    ld (talking),a
    ld a,S_UNT
    rst 28h
    ld a,(chan)
    ld c,a
    ld a,S_CLOSE
    rst 28h
    xor a
    ld (copen),a
    call cinv0
    jp e_bus

; the entry in `line` (HL after its first quote), its number in `blk`, into
; the slot at DE
pent:
    push hl
    ld h,d
    ld l,e
    ld b,ESIZE
pa2:
    ld (hl),0
    inc hl
    djnz pa2
    pop hl              ; HL = the name in the line, DE = the slot
    push de
    ld b,NAMEL
    call qcopy          ; C = its length
    ex (sp),hl          ; HL = the slot
    push hl
    ld de,E_LEN
    add hl,de
    ld (hl),c
    pop de              ; DE = the slot
    pop hl              ; HL = after the name
pa3:
    ld a,(hl)           ; spaces and a splat (unclosed file) before the type
    cp ' '
    jr z,pa4
    cp '*'
    jr nz,pa5
pa4:
    inc hl
    jr pa3
pa5:
    push de
    ex de,hl
    ld bc,E_TYPE
    add hl,bc
    ex de,hl
    ld b,3
pa6:
    ld a,(hl)
    or a
    jr z,pa7
    ld (de),a
    inc hl
    inc de
    djnz pa6
pa7:
    pop hl
    ld de,E_BLK
    add hl,de
    ld de,(blk)
    ld (hl),e
    inc hl
    ld (hl),d
    or a
    ret


; HL after the first quote of the 0-terminated text at HL; C if there is none
quote:
    ld a,(hl)
    inc hl
    or a
    scf
    ret z
    cp 22h
    jr nz,quote
    or a
    ret

; up to B characters from HL to DE, until a quote or the end; HL after the
; closing quote, C = the count
qcopy:
    ld c,0
qc1:
    ld a,(hl)
    or a
    ret z
    inc hl
    cp 22h
    ret z
    inc b
    dec b
    jr z,qc1            ; too long: dropped
    ld (de),a
    inc de
    inc c
    dec b
    jr qc1

; ---- errors: the message for the status line, and back to the caller of
; the operation that set `errsp`

e_bus:
    ld hl,s_dnp
    jr e_set
e_drv:                  ; the drive's message at HL
e_set:
    call setmsg
    ld sp,(errsp)
    scf
    ret

; the message at HL (0-terminated) to the status line
setmsg:
    push de
    push bc
    ld de,msgbuf
    ld b,40
sm1:
    ld a,(hl)
    or a
    jr z,sm2
    ld (de),a
    inc hl
    inc de
    djnz sm1
sm2:
    xor a
    ld (de),a
    ld a,1
    ld (msg+1),a
    pop bc
    pop de
    ret

; ---- drive commands: built in `cmdbuf` (length first), then sent

cmdnew:
    xor a
    ld (cmdbuf),a
    ret
cmdadd:                 ; A to the command; keeps everything
    push hl
    push de
    ld hl,cmdbuf
    ld e,(hl)
    inc (hl)
    ld d,0
    add hl,de
    inc hl
    ld (hl),a
    pop de
    pop hl
    ret
cmdstr:                 ; the 0-terminated string at HL to the command
    ld a,(hl)
    or a
    ret z
    call cmdadd
    inc hl
    jr cmdstr
cmdsend:                ; NC: all right; C: the message set
    ld hl,cmdbuf
cmdhl:                  ; the command at HL
    ld a,S_CMD
    rst 28h
    jr c,cs1
    ret nz
    cp 20               ; under 20 is not an error: 03 is X's status
    jr nc,cs2
    or a
    ret
cs2:
    call setmsg         ; HL: the drive's message
    scf
    ret
cs1:
    ld hl,s_dnp
    call setmsg
    scf
    ret

; ---- the screen: PFM draws its text into the video RAM itself, with the
; ROM's own letters, copied once at the start (scrinit). Through the ROM a
; character took 4 ms, 13 in inverse, and a page took seconds
; A text row is 12 lines of 32 bytes from
; (0x404A)*256; column c starts at pixel 4+6c; of a letter's 12 lines 1-10
; are kept, 0 and 11 are blank in all of them (measured in the simulation by
; drawing through the ROM).

draw:
    call cls
    ld hl,s_help
    call help
drawlist:
    ld c,0
dl1:
    push bc
    call drawrow
    pop bc
    inc c
    ld a,c
    cp ROWS
    jr c,dl1
status:                 ; row 14: the message, else the page
    ld d,14
    call clrrow
    ld a,(msg+1)
    or a
    jr z,st1
    xor a
    ld (msg+1),a
    ld hl,msgbuf
    jp puts
st1:
    ld hl,s_page
    call puts
    ld a,(pageno)
    inc a
    ld l,a
    ld h,0
    ld c,0              ; no leading spaces
    call putdec0
    ld a,(more)
    or a
    ret z
    ld hl,s_more
    jp puts

help:                   ; the key help at HL, on row 15
    push hl
    ld d,15
    call clrrow
    pop hl
    jp puts

clrrow:                 ; row D cleared, the cursor at its start
    xor a
    call fillrow
    ld e,0
    jp goto

cls:
    ld a,C_NORM
    call putc
    ld a,C_CLS
    jp putc

; row C: entry C, inverse under the cursor
drawrow:
    ld d,c
    ld hl,count
    ld a,c
    cp (hl)
    ld a,0
    jr nc,dr0           ; no entry: cleared
    ld a,(cur)
    cp c
    ld a,0
    jr nz,dr0
    dec a               ; the bar
dr0:
    call fillrow
    ld e,0
    call goto
    ld a,c
    cp (hl)
    ret nc
    ld a,(cur)
    cp c
    jr nz,dr2
    ld a,C_INV
    call putc
dr2:
    ld a,c
    call entry
    ld a,' '
    call putc
    ld b,NAMEL
    call putn
    ld a,' '
    call putc
    ld b,3
    call putn           ; HL at the type, then the blocks
    ld a,' '
    call putc
    ld e,(hl)
    inc hl
    ld d,(hl)
    ex de,hl
    call putdec
    ld a,C_INVX
    jp putc

; the 0-terminated string at HL, HL after it; B counts down the columns
; left in the row
puts:
    ld a,(hl)
    inc hl
    or a
    ret z
    call putc
    dec b
    jr puts

; B characters from HL: 0 as a space, the shifted capitals 0xC1-0xDA as
; capitals (the drive's asc2pet gives a folder's name so), codes the screen
; would take as control or user characters as '?'; HL after them
putn:
    ld a,(hl)
    inc hl
    or a
    jr nz,pn1
    ld a,' '
pn1:
    cp ' '
    jr c,pn2
    cp 7Fh
    jr c,pn3
    cp 0C1h
    jr c,pn2
    cp 0DBh
    jr nc,pn2
    sub 80h
    jr pn3
pn2:
    ld a,'?'
pn3:
    call putc
    djnz putn
    ret

; spaces to the end of the row: B is what puts left; more than 41 is none
spaces:
    ld a,b
    or a
    ret z
    cp 42
    ret nc
sps1:
    ld a,' '
    call putc
    djnz sps1
    ret

; HL in decimal, five places, right-aligned (putdec0 with C = 0: no
; leading spaces); BC kept
putdec:
    ld c,' '
putdec0:
    push bc
    ld de,10000
    call pd1
    ld de,1000
    call pd1
    ld de,100
    call pd1
    ld de,10
    call pd1
    ld a,l
    add a,'0'
    call putc
    pop bc
    ret
pd1:
    ld a,'0'-1
pd2:
    inc a
    or a
    sbc hl,de
    jr nc,pd2
    add hl,de
    cp '0'
    jr nz,pd3
    ld a,c              ; a leading zero
    or a
    ret z
    jp putc
pd3:
    ld c,'0'
    jp putc

; the ROM's letters 0x20-0x7E, drawn by the ROM on rows 13-15 and copied
; from there; then the screen is PFM's
scrinit:
    ld a,(404Ah)        ; the screen's high byte
    ld (vbase),a
    push af
    ld a,C_NORM
    call DSPHND
    ld a,C_CLS
    call DSPHND
    ld hl,cache+255     ; the ROM draws the letters unseen: 0x404A pointed at
    ld l,0              ; a screen in the cache's memory, cleared, if all of
    ld de,16*384        ; it fits there, 300 bytes under the stack
    add hl,de
    ex de,hl
    ld hl,-300
    add hl,sp
    or a
    sbc hl,de
    jr c,si0            ; it does not: on the screen, as the ROM sees it
    ld hl,cache+255
    ld l,0
    ld a,h
    ld (404Ah),a
    ld (vbase),a
    ld d,h
    ld e,1
    ld (hl),0
    ld bc,16*384-1
    ldir
si0:
    ld de,13*256
    ld a,' '
si1:
    ld b,a
    ld a,e
    or a
    call z,DIRCUR       ; a row's start
    ld a,b
    call DSPHND
    inc e
    ld a,e
    cp 42
    jr c,si2
    ld e,0
    inc d
si2:
    ld a,b
    inc a
    cp 7Fh
    jr c,si1
    ld hl,13*256
    ld (spos),hl
    ld hl,glyphs
    ld (gptr),hl
si3:
    ld de,(spos)
    call celladr        ; HL = the cell's line 0, A = its shift
    ld c,a
    ld de,32
    add hl,de           ; line 1
    ld de,(gptr)
    ld b,10
si4:
    push bc
    push hl
    ld a,(hl)
    inc hl
    ld l,(hl)
    ld h,a
    ld a,c
    or a
    jr z,si6
    ld b,a
si5:
    add hl,hl
    djnz si5
si6:
    ld a,h
    and 0FCh
    ld (de),a
    inc de
    pop hl
    ld bc,32
    add hl,bc
    pop bc
    djnz si4
    ld (gptr),de
    ld hl,(spos)
    inc l
    ld a,l
    cp 42
    jr c,si7
    ld l,0
    inc h
si7:
    ld (spos),hl
    ld hl,glyphs+95*10
    or a
    sbc hl,de
    jr nz,si3
    pop af              ; the screen back
    ld (404Ah),a
    ld (vbase),a
    jp cls

goto:                   ; the cursor to row D, column E; all kept
    ld (ccol),de
    ret

; A to the screen, as DSPHND took it: a character, C_INV, C_INVX, C_NORM,
; C_CLS, or 08 (back one place, cleared); all registers kept
putc:
    push af
    push bc
    push de
    push hl
    call pc0
    pop hl
    pop de
    pop bc
    pop af
    ret
pc0:
    cp ' '
    jr nc,pc2
    cp C_INV
    jr z,pcinv
    cp C_INVX
    jr z,pcnorm
    cp C_NORM
    jr z,pcnorm
    cp K_LEFT
    jr z,pcbs
    cp C_CLS
    ret nz
    ld a,(vbase)        ; CLS: all of it, the cursor home
    ld h,a
    ld l,0
    ld d,h
    ld e,1
    ld (hl),l
    ld bc,16*12*32-1
    ldir
    ld hl,0
    ld (ccol),hl
    dec l
    ld a,l
    ld (frow),a         ; no row is tracked
pcnorm:
    xor a
    jr pcn1
pcinv:
    ld a,0FCh
pcn1:
    ld (cinv),a
    ret
pcbs:
    ld de,(ccol)
    ld a,e
    or a
    jr nz,pb1
    ld a,d              ; the row before, at its end
    or a
    ret z
    dec d
    ld e,42
pb1:
    dec e
    ld (ccol),de
    ld a,' '
    call pc2
    ld hl,ccol
    dec (hl)
    ret
pc2:
    cp 7Fh
    jr c,pc3
    ld a,'?'
pc3:
    ld c,a
    ld de,(ccol)
    ld a,e
    cp 42
    jr c,pc4
    ld e,0              ; past a row's end: the next (the edit's two rows)
    inc d
    ld (ccol),de
pc4:
    ld a,d
    cp 16
    ret nc
    ld a,(frow)         ; on the row filled last ...
    cp d
    jr nz,pc6
    ld hl,fcol
    ld a,e
    cp (hl)
    jr c,pc5
    ld a,c              ; ... past what was drawn on it, a space on the
    cp ' '              ; fill's own colour is there already
    jr nz,pc5
    ld a,(ffill)
    ld hl,cinv
    cp (hl)
    jr z,pc9
pc5:
    ld hl,fcol
    ld a,e
    inc a
    cp (hl)
    jr c,pc6
    ld (hl),a
pc6:
    ld a,c
    call glyph
pc9:
    ld hl,ccol
    inc (hl)
    ret

; letter A at row D, column E
glyph:
    sub ' '
    ld l,a
    ld h,0
    push de
    ld d,h
    ld e,l
    add hl,hl           ; x2
    add hl,hl           ; x4
    add hl,de           ; x5
    add hl,hl           ; x10
    ld de,glyphs
    add hl,de
    ld de,gbuf+1
    ld bc,10
    ldir
    pop de
    call celladr        ; HL = the cell's line 0, A = its shift: 0, 2, 4, 6
    ld c,a
    ld b,0
    push hl
    ld hl,masks
    add hl,bc
    ld a,(hl)
    ld (mkh),a
    inc hl
    ld a,(hl)
    ld (mkl),a
    pop hl
    ld a,6              ; the shift: past 6-s of the six steps
    sub c
    add a,a
    add a,a
    ld (gjr+1),a
    ld a,(cinv)
    ld c,a
    ld de,gbuf
    ld b,12
gy1:
    ld a,(de)
    xor c
    inc de
    push de
    ld d,a
    ld e,0
gjr:
    jr gs0              ; the displacement is set above
gs0:
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    srl d
    rr e
    ld a,(mkh)
    and (hl)
    or d
    ld (hl),a
    inc hl
    ld a,(mkl)
    and (hl)
    or e
    ld (hl),a
    ld de,31
    add hl,de
    pop de
    djnz gy1
    ret

masks:                  ; what a cell leaves of its two bytes, by its shift
    defb 03h, 0FFh, 0C0h, 0FFh, 0F0h, 03Fh, 0FCh, 00Fh

; HL = the first byte of row D, column E, line 0; A = the cell's shift in
; that byte (BC lost)
celladr:
    ld a,e
    add a,a
    add a,e
    add a,a             ; 6 x column
    add a,4
    ld c,a
    and 7
    ld b,a
    ld a,c
    rrca
    rrca
    rrca
    and 1Fh
    ld c,a
    ld a,d
    add a,a
    add a,d             ; 3 x row ...
    ld h,a
    ld l,0
    srl h
    rr l                ; ... x 128: row x 384
    ld a,(vbase)
    add a,h
    ld h,a
    ld a,l
    or c
    ld l,a
    ld a,b
    ret

; row D filled with A: 0, or 0FFh for inverse (the left margin stays dark);
; it is then tracked: spaces past what is drawn on it are not drawn
fillrow:
    push bc
    push de
    push hl
    push af
    ld a,d
    ld (frow),a
    xor a
    ld (fcol),a
    pop af
    push af
    and 0FCh
    ld (ffill),a
    ld e,0
    call celladr
    pop af
    push af
    ld d,h
    ld e,l
    inc de
    ld (hl),a
    push hl
    ld bc,12*32-1
    ldir
    pop hl
    pop af
    or a
    jr z,fr2
    ld de,32
    ld b,12
fr1:
    ld (hl),0Fh
    add hl,de
    djnz fr1
fr2:
    pop hl
    pop de
    pop bc
    ret

; ---- settings

; the fields: their labels, in order of the `dirty` bits
F_MODE  equ 0
F_SSID  equ 1
F_PASS  equ 2
F_CTRY  equ 3
F_ADMIN equ 4
F_AUTO  equ 5           ; PFM_AUTOSTART: our DOS starts PFM at boot (loader.asm)
F_TURBO equ 6
F_HIDE  equ 7
F_DEV   equ 8           ; `dirty` is a word: this one is bit 0 of dirty+1
NFIELDS equ 9
CFGBITS equ 3Fh         ; the fields kept in PRIFLOP.CFG

settings:
    ld (errsp),sp
    ld hl,0
    ld (dirty),hl
    xor a
    ld (scur),a
    call cfgread
    call xstat
sdraw:
    call cls
    ld d,0
    ld a,0FFh
    call fillrow
    ld de,0
    call goto
    ld a,C_INV
    call putc
    ld b,41
    ld hl,s_stitle
    call puts
    call spaces
    ld a,C_INVX
    call putc
    ld hl,s_shelp
    call help
sdlist:
    ld c,0
sd1:
    push bc
    call drawfield
    pop bc
    inc c
    ld a,c
    cp NFIELDS
    jr c,sd1
    call status1
skey:
    call getkey
    cp K_DOWN
    jr z,sdown
    cp ' '
    jr z,sdown
    cp K_UP
    jr z,sup
    cp K_RET
    jp z,schange
    cp 'W'
    jp z,swrite
    cp 'R'
    jp z,srestart
    cp K_BRK
    ret z
    cp 'Q'
    ret z
    cp 'B'
    ret z
    jr skey
sdown:
    ld a,(scur)
    inc a
    cp NFIELDS
    jr nc,skey
    ld (scur),a
    jr sdlist
sup:
    ld a,(scur)
    or a
    jr z,skey
    dec a
    ld (scur),a
    jr sdlist

status1:                ; the message, if any, on row 14
    ld d,14
    call clrrow
    ld a,(msg+1)
    or a
    ret z
    xor a
    ld (msg+1),a
    ld hl,msgbuf
    jp puts

; field C on row C+2: a mark if changed, its label, its value
drawfield:
    ld a,c
    add a,2
    ld d,a
    ld a,(scur)
    cp c
    ld a,0
    jr nz,df0
    dec a               ; the bar
df0:
    call fillrow
    ld e,0
    call goto
    ld a,(scur)
    cp c
    jr nz,df1
    ld a,C_INV
    call putc
df1:
    ld hl,(dirty)
    ld b,c
    inc b
df2:
    srl h
    rr l
    djnz df2
    ld a,' '
    jr nc,df3
    ld a,'*'
df3:
    call putc
    ld a,c
    ld hl,s_labels
    call nth
    ld b,40
    call puts           ; padded: the value at column 21
    ld a,b
    sub 20
    ld b,a
    call spaces1
    ld a,c
    call fvalue         ; HL = the value's text
    ld b,19
df4:
    ld a,(hl)
    or a
    jr z,df5
    call putc
    inc hl
    djnz df4
df5:
    call spaces1
    ld a,C_INVX
    jp putc
spaces1:                ; B spaces, B may be 0
    inc b
    dec b
    ret z
    jp sps1

; HL = string A of the 0-separated list at HL
nth:
    or a
    ret z
nth1:
    ld b,a
nth2:
    ld a,(hl)
    inc hl
    or a
    jr nz,nth2
    ld a,b
    dec a
    jr nz,nth1
    ret

; HL = the text of field A's value
fvalue:
    or a
    jr nz,fv1
    ld a,(v_mode)
    ld hl,s_modes
    jr nth
fv1:
    dec a
    jr nz,fv2
    ld hl,v_ssid
    ret
fv2:
    dec a
    jr nz,fv3
    ld a,(v_pass)
    jr fvsecret
fv3:
    dec a
    jr nz,fv4
    ld hl,v_ctry
    ld a,(hl)
    or a
    ret nz
    ld hl,s_world
    ret
fv4:
    dec a
    jr nz,fv5
    ld a,(v_admin)
fvsecret:               ; A: the first character, or 0
    ld hl,s_none
    or a
    ret z
    ld hl,s_stars
    ret
fv5:
    dec a
    jr nz,fv5a
    ld a,(v_auto)
    jr fvonoff
fv5a:
    dec a
    jr nz,fv6
    ld a,(v_turbo)
fvonoff:
    ld hl,s_onoff
    jr nth
fv6:
    dec a
    jr nz,fv7
    ld a,(v_hide)
    jr fvonoff
fv7:
    ld a,(v_dev)        ; the device, two places
    ld hl,numbuf
    ld (hl),' '
    cp 10
    jr c,fv8
    ld (hl),'1'
    sub 10
fv8:
    add a,'0'
    inc hl
    ld (hl),a
    inc hl
    ld (hl),0
    dec hl
    dec hl
    ret

; RETURN: toggle, cycle or edit the field under the cursor
schange:
    ld a,(scur)
    ld c,a              ; the dirty bit
    ld b,a
    inc b
    ld hl,0
    scf
sc0:
    adc hl,hl
    djnz sc0
    ld (dbit),hl
    ld a,c
    or a
    jr nz,sc1
    ld hl,v_mode
    jr sctoggle
sc1:
    cp F_SSID
    jr nz,sc2
    ld hl,v_ssid
    ld b,32
    ld c,0
    jr scedit
sc2:
    cp F_PASS
    jr nz,sc3
    ld hl,v_pass
    ld b,63
    ld c,1
    jr scedit
sc3:
    cp F_CTRY
    jr nz,sc4
    ld hl,v_ctry
    ld b,2
    ld c,0
    jr scedit
sc4:
    cp F_ADMIN
    jr nz,sc5
    ld hl,v_admin
    ld b,63
    ld c,1
    call edit
    jp c,sdraw
    ld a,(v_admin)      ; no empty admin password: the old one stays
    or a
    jp z,sdraw
    jr scmark
sc5:
    cp F_AUTO
    jr nz,sc5a
    ld hl,v_auto
    jr sctoggle
sc5a:
    cp F_TURBO
    jr nz,sc6
    ld hl,v_turbo
    jr sctoggle
sc6:
    cp F_HIDE
    jr nz,sc7
    ld hl,v_hide
    jr sctoggle
sc7:
    ld a,(v_dev)        ; the device: 8, 9, 10, 11, 8 ...
    inc a
    cp 12
    jr c,sc8
    ld a,8
sc8:
    ld (v_dev),a
    jr scmark
sctoggle:
    ld a,(hl)
    xor 1
    and 1
    ld (hl),a
    jr scmark
scedit:
    call edit
    jp c,sdraw
scmark:
    ld hl,(dirty)
    ld de,(dbit)
    ld a,l
    or e
    ld l,a
    ld a,h
    or d
    ld h,a
    ld (dirty),hl
    jp sdraw

; a line typed into the B-byte field at HL (0-terminated after it); C = 1:
; shown as stars. RETURN keeps it (NC), BRK leaves the field as it was (C).
edit:
    push hl
    push bc
    ld d,11
    call clrrow
    ld b,41
    ld hl,s_edit
    call puts
    ld d,14             ; rows 13 and 14: 63 characters fit
    call clrrow
    ld d,13
    call clrrow
    pop bc
    ld hl,ebuf
    ld e,0              ; the count
ed1:
    call getkey0
    cp K_RET
    jr z,ed8
    cp K_BRK
    jr z,ed9
    cp K_LEFT
    jr nz,ed2
    inc e               ; take one back
    dec e
    jr z,ed1
    dec e
    dec hl
    call putc
    jr ed1
ed2:
    cp ' '
    jr c,ed1
    cp 7Fh
    jr nc,ed1
    ld d,a
    ld a,e
    cp b
    jr nc,ed1           ; full
    ld (hl),d
    inc hl
    inc e
    ld a,d
    bit 0,c
    jr z,ed3
    ld a,'*'
ed3:
    call putc
    jr ed1
ed8:
    ld (hl),0
    pop de              ; the field
    ld hl,ebuf
    call strcpy
    or a
    ret
ed9:
    pop hl
    scf
    ret

getkey0:                ; a key as typed, letters in both cases
keywait:                ; the ROM's cursor where PFM's is, as before
    push de
    ld de,(ccol)
    ld a,e
    cp 42
    jr c,kw1
    ld e,41
kw1:
    call DIRCUR
    pop de
    jp KKBHND

; R: the drive restarts (U and Shift-J): Wi-Fi and the admin password are
; read at its start; its answer never comes
srestart:
    ld hl,c_reset
    ld a,S_CMD
    rst 28h
    ld hl,s_restart
    call setmsg
    jp sdraw

; W: the changed fields go to the drive
swrite:
    ld hl,(dirty)
    ld a,h
    or l
    jp z,sdraw
    ld a,l
    and CFGBITS
    jr z,sw1
    call cfgwrite       ; C: the message says why
    jp c,sdraw
    call cfgread        ; the file as it is now: a second W in the same
    jp c,sdraw          ; visit starts from it, not from the one before
    ld a,(dirty)
    and 0FFh-CFGBITS
    ld (dirty),a
sw1:
    ld hl,(dirty)       ; the switches, then XW to keep them
    ld a,l
    and 0FFh-CFGBITS
    or h
    jp z,swdone
    ld hl,dirty
    bit F_TURBO,(hl)
    jr z,sw2
    ld a,(v_turbo)
    ld hl,c_xtoff
    or a
    jr z,sw1a
    ld hl,c_xton
sw1a:
    call cmdhl
    jp c,sdraw
sw2:
    ld hl,dirty
    bit F_HIDE,(hl)
    jr z,sw3
    ld a,(v_hide)
    ld hl,c_xeoff
    or a
    jr z,sw2a
    ld hl,c_xeon
sw2a:
    call cmdhl
    jp c,sdraw
sw3:
    ld hl,dirty+1
    bit F_DEV-8,(hl)
    jr z,sw4
    call cmdnew         ; U0> and the new device
    ld hl,s_u0
    call cmdstr
    ld a,(v_dev)
    call cmdadd
    ld hl,cmdbuf        ; the drive takes its new address at once: the
    ld a,S_CMD          ; message is not there to be read on the old one
    rst 28h
    ld a,(v_dev)
    ld (dev),a
    ld c,a
    ld a,S_SETDEV
    rst 28h
sw4:
    ld hl,c_xw
    call cmdhl
    jp c,sdraw
swdone:
    ld hl,s_saved
    call setmsg
    ld hl,0
    ld (dirty),hl
    jp sdraw

; "X": the drive's switches; `xmode` = the extension mode, `v_hide`
xstat:
    ld a,0FFh
    ld (xmode),a
    ld hl,c_x
    ld a,S_CMD
    rst 28h
    ret c
    ld a,(hl)           ; "03,E1-..."
    cp '0'
    ret nz
    inc hl
    ld a,(hl)
    cp '3'
    ret nz
    inc hl
    inc hl
    ld a,(hl)
    cp 'E'
    ret nz
    inc hl
    ld a,(hl)
    sub '0'
    ld (xmode),a
    inc hl
    ld a,(hl)
    cp '+'
    ld a,1
    jr z,xs1
    xor a
xs1:
    ld (v_hide),a
    ret

; ---- PRIFLOP.CFG

; read it into cfgbuf and take PFM's fields from it; `cfgok` 1 if it can be
; rewritten (a missing file counts: it is written new)
cfgread:
    ld (errsp),sp
    xor a               ; the firmware's defaults for a key the file lacks
    ld (cfgok),a        ; (netconfig_set_defaults)
    ld (v_pass),a
    ld (v_auto),a       ; PFM_AUTOSTART: off
    ld (v_ctry),a
    inc a
    ld (v_mode),a       ; AP
    ld hl,s_defssid
    ld de,v_ssid
    call strcpy
    ld hl,s_defadm
    ld de,v_admin
    call strcpy
    ld hl,0
    ld (cfglen),hl
    ld hl,n_cfg
    ld c,SEC
    ld a,S_OPEN
    rst 28h
    jp c,e_bus
    jr nz,cr1
    cp 62
    jp nz,e_drv         ; the drive's message
    jr cr8              ; none yet
cr1:
    ld c,SEC
    call talk
    ld hl,cfgbuf
    ld de,CFGMAX
cr2:
    ld a,d
    or e
    jr z,cr7            ; too large
    call rb
    ld (hl),a
    inc hl
    dec de
    ld a,S_STATUS
    rst 28h
    and 40h
    jr z,cr2
    ld de,cfgbuf
    or a
    sbc hl,de
    ld (cfglen),hl
    ld a,S_UNT
    rst 28h
    ld c,SEC
    ld a,S_CLOSE
    rst 28h
    call cfgparse
cr8:
    ld a,1
    ld (cfgok),a
    ret
cr7:
    ld a,S_UNT
    rst 28h
    ld c,SEC
    ld a,S_CLOSE
    rst 28h
    ld hl,s_toolarge
    jp e_set

; the fields from the lines of cfgbuf
cfgparse:
    call bufset
cp1:
    call atend
    ret z
    call scanl
    ld a,(kidx)
    or a
    jr z,cp9
    dec a
    jr nz,cp2
    ld hl,(vbeg)        ; MODE: AP or STA
    ld a,(hl)
    call upcase
    cp 'A'
    ld a,1
    jr z,cp1a
    xor a
cp1a:
    ld (v_mode),a
    jr cp9
cp2:
    ld hl,v_ssid
    ld b,32
    dec a
    jr z,cp8
    ld hl,v_pass
    ld b,63
    dec a
    jr z,cp8
    ld hl,v_ctry
    ld b,2
    dec a
    jr z,cp8
    ld hl,v_admin
    ld b,63
    dec a
    jr z,cp8
    ld hl,ebuf          ; PFM_AUTOSTART: ON is on, anything else off, as
    ld b,3              ; the DOS reads it at boot (loader.asm)
    call vcopy
    ld hl,ebuf
    ld b,0
    ld a,(hl)
    call upcase
    cp 'O'
    jr nz,cp7
    inc hl
    ld a,(hl)
    call upcase
    cp 'N'
    jr nz,cp7
    inc hl
    ld a,(hl)
    or a
    jr nz,cp7
    inc b
cp7:
    ld a,b
    ld (v_auto),a
    jr cp9
cp8:
    call vcopy
cp9:
    ld hl,(lnext)
    jr cp1

; the value (vbeg-vend), at most B bytes of it, to the field at HL, 0-terminated
vcopy:
    ld a,b
    ld (vmax),a
    ex de,hl
    ld hl,(vend)
    ld bc,(vbeg)
    or a
    sbc hl,bc
    ld a,h
    or a
    ld a,(vmax)
    jr nz,vc1           ; over 255: the most the field holds
    ld a,l
    ld hl,vmax
    cp (hl)
    jr c,vc1
    ld a,(hl)
vc1:
    ld h,b
    ld l,c
    ld b,a
    inc b
    dec b
    jr z,vc3
vc2:
    ld a,(hl)
    ld (de),a
    inc hl
    inc de
    djnz vc2
vc3:
    xor a
    ld (de),a
    ret

; HL = cfgbuf, (bufend) = its end
bufset:
    ld hl,cfgbuf
    ld de,(cfglen)
    ex de,hl
    add hl,de
    ld (bufend),hl
    ex de,hl
    ret

; Z if HL is at bufend; keeps HL
atend:
    push de
    ld de,(bufend)
    ld a,h
    cp d
    jr nz,ae1
    ld a,l
    cp e
ae1:
    pop de
    ret

; the line at HL: (lnext) the next, (kidx) which of PFM's keys it sets (1-6,
; 0 none), (vbeg) and (vend) its value, without spaces at either end -- as
; netconfig_apply_line() reads it
scanl:
    ld (lbeg),hl
    ld bc,0             ; after the first '='
sl1:
    call atend
    jr z,sl3
    ld a,(hl)
    inc hl
    cp 0Ah
    jr z,sl2
    cp '='
    jr nz,sl1
    ld a,b
    or c
    jr nz,sl1
    ld b,h
    ld c,l
    jr sl1
sl2:
    ld (lnext),hl
    dec hl              ; the line ends before its \n
    jr sl4
sl3:
    ld (lnext),hl
sl4:
    xor a
    ld (kidx),a
    ld a,b
    or c
    ret z               ; no '=': not a setting
    ex de,hl            ; DE = the line's end
    ld h,b              ; the value: from after '=' ...
    ld l,c
    call skipsp
    ld (vbeg),hl
    ex de,hl            ; ... to the end, less spaces
sl5:
    push hl
    or a
    sbc hl,de
    pop hl
    jr z,sl6
    dec hl
    ld a,(hl)
    call isspace
    jr z,sl5
    inc hl
sl6:
    ld (vend),hl
    ld hl,(lbeg)        ; the key: from the line's start, spaces skipped ...
    call skipsp
    ex de,hl
    dec bc              ; ... to the '=', less spaces
    ld h,b
    ld l,c
sl7:
    push hl
    or a
    sbc hl,de
    pop hl
    ret z               ; an empty key
    dec hl
    ld a,(hl)
    call isspace
    jr z,sl7
    inc hl              ; HL = the key's end, DE its start
    ld b,h
    ld c,l
    ld hl,keys
    ld a,1
sl8:                    ; each of PFM's keys, against DE-BC
    push af
    push de
    call keyeq
    pop de
    jr z,sl9
    pop af
    inc a
    cp 7
    ret nc
    jr sl8
sl9:
    pop af
    ld (kidx),a
    ret

; Z if the key at DE (ending at BC) is the 0-terminated key at HL, ignoring
; case; HL after that key's 0 either way
keyeq:
    ld a,(hl)
    or a
    jr z,ke8
    push hl
    ld h,b
    ld l,c
    or a
    sbc hl,de
    pop hl
    jr z,ke7            ; the key ended first
    ld a,(de)
    call upcase
    cp (hl)
    inc hl
    inc de
    jr z,keyeq
ke7:
    ld a,(hl)           ; to the next key
    inc hl
    or a
    jr nz,ke7
    or 1
    ret
ke8:
    inc hl
    push hl
    ld h,b              ; both ended?
    ld l,c
    or a
    sbc hl,de
    pop hl
    ret

skipsp:                 ; HL past spaces, up to DE
    push hl
    or a
    sbc hl,de
    pop hl
    ret z
    ld a,(hl)
    call isspace
    ret nz
    inc hl
    jr skipsp

isspace:                ; Z for a space, a tab or CR
    cp ' '
    ret z
    cp 09h
    ret z
    cp 0Dh
    ret

; ---- the new file, generated byte by byte into (emitv): every line of the
; old one as it was, a line of a changed key replaced; changed keys the old
; file did not have, added at its end. `wrdone` collects the keys written.

gen:
    xor a
    ld (wrdone),a
    ld a,0Ah
    ld (lastb),a
    call bufset
gn1:
    call atend
    jr z,gn5
    call scanl
    ld a,(kidx)         ; a key PFM changed?
    or a
    jr z,gn3
    call kbit
    ld hl,dirty
    and (hl)
    jr z,gn3
    ld hl,wrdone
    or (hl)
    ld (hl),a
    ld a,(kidx)
    call genkey
    jr gn4
gn3:
    ld hl,(lbeg)        ; the line as it was, with its \n
gn3a:
    ld de,(lnext)
    push hl
    or a
    sbc hl,de
    pop hl
    jr z,gn4
    ld a,(hl)
    call emit
    inc hl
    jr gn3a
gn4:
    ld hl,(lnext)
    jr gn1
gn5:
    ld a,1              ; the keys not in the file
gn6:
    push af
    call kbit
    ld hl,dirty
    and (hl)
    jr z,gn7
    ld hl,wrdone
    and (hl)
    jr nz,gn7
    ld a,(lastb)        ; after a last line without its \n
    cp 0Ah
    call nz,crlf
    pop af
    push af
    call genkey
gn7:
    pop af
    inc a
    cp 7
    jr c,gn6
    ret

; A = the dirty bit of key A (1-6)
kbit:
    ld b,a
    xor a
    scf
kb1:
    rla
    djnz kb1
    ret

; KEY=value and CR LF, for key A
genkey:
    push af
    ld hl,keys
    dec a
    call nth
    call emits
    ld a,'='
    call emit
    pop af
    dec a
    jr nz,gk1
    ld a,(v_mode)       ; MODE
    ld hl,s_modes
    call nth
    jr gk9
gk1:
    ld hl,v_ssid
    dec a
    jr z,gk9
    ld hl,v_pass
    dec a
    jr z,gk9
    ld hl,v_ctry
    dec a
    jr z,gk9
    ld hl,v_admin
    dec a
    jr z,gk9
    ld a,(v_auto)       ; PFM_AUTOSTART
    ld hl,s_onoff
    call nth
gk9:
    call emits
crlf:
    ld a,0Dh
    call emit
    ld a,0Ah
    jp emit

emits:                  ; the 0-terminated string at HL
    ld a,(hl)
    or a
    ret z
    call emit
    inc hl
    jr emits

; a byte of the new file to (emitv); every register kept
emit:
    ld (lastb),a
    push hl
    ld hl,(emitv)
    ex (sp),hl
    ret

e_send:                 ; to the drive (LISTEN is on)
    push af
    push bc
    ld c,a
    ld a,S_CIOUT
    rst 28h
    jr nc,es1
    ld a,1
    ld (gerr),a
es1:
    pop bc
    pop af
    ret

e_cmp:                  ; against the file read back (TALK is on)
    push af
    push bc
    ld b,a
    ld a,(gerr)
    or a
    jr nz,ec9           ; already wrong: nothing more is read
    ld a,(eoi)
    or a
    jr nz,ec8           ; the file ended first
    ld a,S_ACPTR
    rst 28h
    jr c,ec8
    cp b
    jr nz,ec8
    ld a,S_STATUS
    rst 28h
    and 40h
    ld (eoi),a
    jr ec9
ec8:
    ld a,1
    ld (gerr),a
ec9:
    pop bc
    pop af
    ret

; the config, rewritten: PRIFLOP.TMP written and read back, PRIFLOP.CFG
; scratched, written and read back, PRIFLOP.TMP scratched. C: the message
; says what failed, and if PRIFLOP.CFG is gone, PRIFLOP.TMP holds the new one.
cfgwrite:
    ld (errsp),sp
    ld a,(cfgok)
    or a
    ld hl,s_noread
    jp z,e_set
    ld a,(xmode)        ; modes 2 and 4 add a suffix even to a PRG
    cp 2
    ld hl,s_xmode
    jp z,e_set
    cp 4
    jp z,e_set
    ld hl,c_stmp
    call cmdok
    ld hl,n_tmpw        ; ASCII capitals: stored as priflop.tmp
    call wfile
    ld hl,n_tmp
    call vfile
    ld hl,c_scfg
    call cmdok
    ld hl,n_cfgw        ; shifted capitals: stored as PRIFLOP.CFG
    call wfile
    ld hl,n_cfg
    call vfile
    ld hl,c_stmp
    call cmdok
    or a
    ret

cmdok:                  ; the command at HL; an error ends cfgwrite
    ld a,S_CMD
    rst 28h
    jp c,e_bus
    ret nz
    cp 20
    ret c
    jp e_drv

wfile:                  ; the new config as the file named at HL
    ld c,SEC
    ld a,S_OPEN
    rst 28h
    jp c,e_bus
    jp z,e_drv
    ld a,(dev)
    ld c,a
    ld a,S_LISTEN
    rst 28h
    jp c,e_bus
    ld c,SEC
    ld a,S_SECL
    rst 28h
    jp c,e_bus
    ld hl,e_send
    ld (emitv),hl
    xor a
    ld (gerr),a
    call gen
    ld a,S_UNL          ; the last byte goes with EOI
    rst 28h
    ld a,(gerr)
    or a
    jp nz,e_bus
    ld c,SEC
    ld a,S_CLOSE
    rst 28h
    jp c,e_bus
    jp z,e_drv
    ret

vfile:                  ; the file named at HL read back against the new config
    ld c,SEC
    ld a,S_OPEN
    rst 28h
    jp c,e_bus
    jp z,e_drv
    ld c,SEC
    call talk
    ld hl,e_cmp
    ld (emitv),hl
    xor a
    ld (gerr),a
    ld (eoi),a
    call gen
    ld a,S_UNT
    rst 28h
    ld c,SEC
    ld a,S_CLOSE
    rst 28h
    ld a,(gerr)
    ld hl,s_verify
    or a
    jp nz,e_set
    ld a,(eoi)          ; it must end where the new one ends
    or a
    jp z,e_set
    ret

; ---- constants

dollar:     defb 1, '$'
s_root:     defm "PRIFLOP"
            defb 0
s_root9:    defm "RAMDISK 9"
            defb 0
dotdot:     defm ".."           ; an entry: the name, 0-padded, type, blocks, length
            defs NAMEL-2
            defm "DIR"
            defw 0
            defb 2
s_help:     defm "RET load up/dn move <> page C conf Q quit"
            defb 0
s_page:     defm "Page "
            defb 0
s_more:     defm "  (more)"
            defb 0
s_dnp:      defm "DEVICE NOT PRESENT"
            defb 0
s_dir:      defm "DIR"
s_imgs:     defm "647181NP"     ; after ".D": the disk images' last two letters
            defb 0
s_cd:       defm "CD:"
            defb 0
s_cdup:     defm "CD:_"
            defb 0
s_load:     defm "CMD LOAD"
            defb 22h, 0
s_new:      defm "NEW"
            defb 0
s_set:      defm "CMD SET "
            defb 0
s_stitle:   defm "PFM  Settings"
            defb 0
s_shelp:    defm "RET change  W write  R restart  BRK back"
            defb 0
s_labels:   defm "Wi-Fi mode"
            defb 0
            defm "Network name"
            defb 0
            defm "Wi-Fi password"
            defb 0
            defm "Country"
            defb 0
            defm "Web admin password"
            defb 0
            defm "PFM at boot"
            defb 0
            defm "Turbo"
            defb 0
            defm "Hide extensions"
            defb 0
            defm "Device number"
            defb 0
s_modes:    defm "STA"
            defb 0
            defm "AP"
            defb 0
s_onoff:    defm "OFF"
            defb 0
            defm "ON"
            defb 0
s_none:     defm "(none)"
            defb 0
s_defssid:  defm "PRIFLOP"
            defb 0
s_defadm:   defm "admin"
            defb 0
s_stars:    defm "****"
            defb 0
s_world:    defm "(world)"
            defb 0
s_edit:     defm "New value; RETURN keeps it, BRK cancels:"
            defb 0
s_restart:  defm "Drive restarting"
            defb 0
s_saved:    defm "Written. R restarts the drive for Wi-Fi."
            defb 0
s_noread:   defm "PRIFLOP.CFG could not be read"
            defb 0
s_toolarge: defm "PRIFLOP.CFG is too large"
            defb 0
s_xmode:    defm "Extension mode 2 or 4: not written"
            defb 0
s_verify:   defm "READ BACK DIFFERS"
            defb 0
s_u0:       defm "U0>"
            defb 0
keys:       defm "MODE"
            defb 0
            defm "SSID"
            defb 0
            defm "PASSWORD"
            defb 0
            defm "COUNTRY"
            defb 0
            defm "ADMIN"
            defb 0
            defm "PFM_AUTOSTART"
            defb 0
c_x:        defb 1
            defm "X"
c_xw:       defb 2
            defm "XW"
c_xton:     defb 3
            defm "XT+"
c_xtoff:    defb 3
            defm "XT-"
c_xeon:     defb 3
            defm "XE+"
c_xeoff:    defb 3
            defm "XE-"
c_reset:    defb 2
            defm "U"
            defb 0CAh           ; Shift-J: the drive's hard reset
; "//:" names the root whatever folder the drive is in (parse_path in
; parser.c): the config is the root's, the one the firmware reads
c_stmp:     defb 15
            defm "S//:PRIFLOP.TMP"
c_scfg:     defb 15
            defm "S//:PRIFLOP.CFG"
n_tmp:      defb 14
            defm "//:PRIFLOP.TMP"
n_cfg:      defb 14
            defm "//:PRIFLOP.CFG"
n_tmpw:     defb 18
            defm "//:PRIFLOP.TMP,P,W"
; the letters shifted (0xC1-0xDA): the drive's name conversion (pet2asc in
; lfsops.c) makes them capitals; plain ASCII capitals would become small
; letters, a file the drive does not read at its start
n_cfgw:     defb 18
            defm "//:"
            defb 0D0h, 0D2h, 0C9h, 0C6h, 0CCh, 0CFh, 0D0h
            defm "."
            defb 0C3h, 0C6h, 0C7h
            defm ",P,W"

; ---- variables: not in the file, set before use
bss:
dev:        defs 1      ; the drive now
dev0:       defs 1      ; the drive PFM started with
skip:       defs 2      ; the entries before this page
pageno:     defs 1
cur:        defs 1      ; the cursor's line on the page
count:      defs 1      ; the entries on the page
more:       defs 1      ; 1: there is another page
blk:        defs 2
errsp:      defs 2
msg:        defs 2      ; msg+1: 1 = a message waits for the status line
title:      defs NAMEL+1
line:       defs 41
msgbuf:     defs 41
cmdbuf:     defs 1+3+NAMEL+2
cmax:       defs 2      ; the cache: its size in entries,
cbase:      defs 2      ; the first entry it holds,
ccnt:       defs 2      ; how many,
ceof:       defs 1      ; 1: the listing's end is in it
copen:      defs 1      ; 1: the listing's channel is open,
talking:    defs 1      ; and talking
pgptr:      defs 2      ; the page's first entry
wlen:       defs 2      ; what the handover's wiper clears, less one
ccol:       defs 1      ; the screen: the cursor's column,
crow:       defs 1      ; its row (after ccol: ld (ccol),de)
cinv:       defs 1      ; 0FCh: inverse
frow:       defs 1      ; the row filled last,
fcol:       defs 1      ; drawn up to this column,
ffill:      defs 1      ; with this colour
vbase:      defs 1      ; the screen's high byte
mkh:        defs 1      ; a cell's masks
mkl:        defs 1
spos:       defs 2
gptr:       defs 2
gbuf:       defs 12     ; a letter's 12 lines; the first and last stay 0 (start)
glyphs:     defs 95*10  ; the ROM's letters 0x20-0x7E, lines 1-10
scur:       defs 1
dirty:      defs 2      ; a bit a field (F_*)
dbit:       defs 2
xmode:      defs 1
cfgok:      defs 1
v_mode:     defs 1
v_turbo:    defs 1
v_hide:     defs 1
v_dev:      defs 1
v_auto:     defs 1
v_ssid:     defs 33
v_pass:     defs 64
v_ctry:     defs 3
v_admin:    defs 64
ebuf:       defs 64
numbuf:     defs 3
cfglen:     defs 2
bufend:     defs 2
lbeg:       defs 2
lnext:      defs 2
vbeg:       defs 2
vend:       defs 2
kidx:       defs 1
vmax:       defs 1
chan:       defs 1
wrdone:     defs 1
lastb:      defs 1
emitv:      defs 2
gerr:       defs 1
eoi:        defs 1
cfgbuf:     defs CFGMAX ; the config file, or the directory's cache from here
bssend:                 ; on, up to under the stack
cache       equ cfgbuf
