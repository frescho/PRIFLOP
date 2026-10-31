; M -- what the boot left in memory, in one line: CMD LOAD"M" instead of
; typing the PEEKs (the user, 2026-10-06).
;
;   TOP 55795 CDOS 56395 MOD 1 HOOK
;
;   TOP 56429 DOS 56430 PRIFLOP
;
; The second form after a boot of our own DOS (phase B), recognised by its
; signature; the first after CDOS, with or without phase A's module.
;
; TOP is BASIC's top (0x40B1), CDOS its base (0x4174), MOD how many turbo
; modules fit between them -- 1 after a turbo boot, 0 after plain CDOS, more
; if boots stacked them (a `?` after it: the gap is not a whole number of
; modules) -- and HOOK says that CDOS's load entry (table 0x10) points at the
; module right above the top, i.e. the turbo is in use.
;
; The file (build_turbo.memprg()) is this code (0xD9 at 0x7000) and an
; autostart (0xC3). It returns as PCALL does: DE the program end, so CMD LOAD
; leaves BASIC's program alone. SIZE comes from turbo_syms.inc.

    include "turbo_syms.inc"

PRINTS  equ 2B75h       ; ROM: print the string at HL, ending 00H
CDOSV   equ 4174h
MEMTOP  equ 40B1h
SIGOFS  equ 0B87h       ; our DOS's signature, 8 bytes under its load limit

    org 7000h

start:
    ld de,line
    ld hl,t_top
    call copy
    ld hl,(MEMTOP)
    call dec
    ld hl,(CDOSV)       ; our own DOS (phase B): its signature, at a fixed
    ld bc,SIGOFS        ; distance from its base (tools/primo/dos/dos.asm)
    add hl,bc
    ld bc,t_sig
    push de
    ld e,4
sig:
    ld a,(bc)
    cp (hl)
    jr nz,notours
    inc hl
    inc bc
    dec e
    jr nz,sig
notours:
    pop de
    jr nz,cdos
    ld hl,t_dos
    call copy
    ld hl,(CDOSV)
    call dec
    ld hl,t_own
    call copy
    jr nohook
cdos:
    ld hl,t_cdos
    call copy
    ld hl,(CDOSV)
    call dec
    ld hl,t_mod
    call copy
    ld hl,(CDOSV)       ; the gap: CDOS - 1 - TOP bytes
    dec hl
    ld bc,(MEMTOP)
    or a
    sbc hl,bc
    push de
    ld de,SIZE
    ld bc,0             ; BC = gap / SIZE, HL = the remainder
divide:
    or a
    sbc hl,de
    jr c,divided
    inc bc
    jr divide
divided:
    add hl,de
    ld a,h
    or l
    pop de
    push af
    ld h,b
    ld l,c
    call dec
    pop af
    jr z,whole
    ld a,'?'
    ld (de),a
    inc de
whole:
    ld hl,(CDOSV)       ; table entry 0x10 at the module above the top?
    ld bc,0B42h + 10h
    add hl,bc
    ld c,(hl)
    inc hl
    ld b,(hl)
    ld hl,(MEMTOP)
    inc hl
    or a
    sbc hl,bc
    jr nz,nohook
    ld hl,t_hook
    call copy
nohook:
    ld a,13
    ld (de),a
    inc de
    xor a
    ld (de),a
    ld hl,line
    call PRINTS
    ld de,(40F9h)       ; CMD LOAD stores DE as the program end: keep it
    ld a,0D9h           ; the last record type, as a load returns it
    or a                ; NZ NC: no error
    ret

copy:                   ; the string at HL, ending 00H, to DE
    ld a,(hl)
    or a
    ret z
    ld (de),a
    inc hl
    inc de
    jr copy

dec:                    ; HL in decimal to DE, no leading zeros
    push de
    ld de,digits
    ld bc,-10000
    call digit
    ld bc,-1000
    call digit
    ld bc,-100
    call digit
    ld bc,-10
    call digit
    ld a,l
    add a,'0'
    ld (de),a
    pop de
    ld hl,digits
    ld b,4
skip:
    ld a,(hl)
    cp '0'
    jr nz,copy5
    inc hl
    djnz skip
copy5:
    inc b               ; the digits left, at least one
digout:
    ld a,(hl)
    ld (de),a
    inc hl
    inc de
    djnz digout
    ret
digit:                  ; one digit: subtract BC (negative) while it goes
    ld a,'0' - 1
digit1:
    inc a
    add hl,bc
    jr c,digit1
    sbc hl,bc
    ld (de),a
    inc de
    ret

t_top:  defm "TOP ", 0
t_cdos: defm " CDOS ", 0
t_mod:  defm " MOD ", 0
t_hook: defm " HOOK", 0
t_dos:  defm " DOS ", 0
t_own:  defm " PRIFLOP", 0
t_sig:  defm "PDOS"
digits: defs 5
line:   defs 48
