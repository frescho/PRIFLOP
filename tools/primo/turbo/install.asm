; PRIFLOP Primo turbo -- the installer, the start of TURBO.PRG.
;
; CMD LOAD"TURBO" loads this, the module image and its relocation list to
; 0x7000 and starts it (the file's closing 0xC3 record). It:
;
;   1. checks that CDOS is there and that table entry 0x10 is still CDOS's
;      own load (0x08CD in its body) -- otherwise it is already installed,
;      or something else is, and it changes nothing;
;   2. copies the module to the top of RAM, directly under CDOS, and adds
;      its address to every absolute reference (build_turbo.py's list);
;   3. copies CDOS's own output port byte (body 0x06DB: 0x80 on a B64, 0x40
;      on an A64) into the module -- the port detection, for free;
;   4. keeps CDOS's entries 0x0E, 0x10, 0x12 in the module and points them
;      at it;
;   5. (first, in fact) lowers BASIC's top of memory below it and does
;      CLEAR 50 (or reuses the room an earlier install left: inst_core.asm,
;      shared with boot.asm); at the end prints its banner, NEW, READY --
;      the same steps, ROM calls and order as CDOS's own loader.
;
; The equates SIZE, SVC, OUTPORT_OFS, OLDTAB_OFS come from turbo_syms.inc,
; written by build_turbo.py; the module image and the relocation list
; (16-bit offsets, ended by 0xFFFF) are appended right after this code.

    include "turbo_syms.inc"

CDOSV   equ 4174h       ; CDOS's base
MEMTOP  equ 40B1h       ; BASIC's top of memory
PRINTS  equ 2B75h       ; ROM: print the string at HL, ending 00H
CLEAR   equ 1E83h       ; ROM: CLEAR with DE bytes of string space
NEW     equ 1B4Dh       ; ROM: part of NEW, as CDOS's loader calls it
READY   equ 1A19h       ; ROM: READY

    org 7000h

install:
    ld hl,(CDOSV)
    ld de,0B42h + 10h   ; table entry 0x10
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    ld hl,(CDOSV)
    ld bc,08CDh
    add hl,bc
    or a
    sbc hl,de
    ld hl,m_already
    jp nz,say           ; not CDOS's own load: leave everything as it is

    include "inst_core.asm"
    ld hl,c_on          ; tell the drive: `XT+` turns its half on (a drive
    ld a,18h            ; that does not know it answers with an error; it
    rst 28h             ; changes nothing either way)
    ld hl,m_on          ; print, NEW, READY: CDOS's own loader's order
    call PRINTS         ; (0x690C-0x6974)
    call NEW
    jp READY
say:
    call PRINTS
    jp READY

mbase:      defw 0
c_on:       defb 3
            defm "XT+"
m_on:       defm "PRIFLOP turbo on", 13, 0
m_already:  defm "Turbo not installed: CDOS's load is not its own", 13, 0
image:
