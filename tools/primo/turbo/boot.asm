; PRIFLOP Primo turbo -- inside the boot file (our own DOS, phase A).
;
; The boot file is BOOTU (boot80.prg, CDOS's own loader and body, with the
; port-detecting prologue, build_unified_boot.py), then this code, then the
; module image and its relocation list. build_turbo.boot_file() points the
; loader's banner call at `boot` (0x4658: CALL 2B75h -> CALL boot), which the
; loader reaches only after CDOS is installed -- the "Illegal BASIC version"
; path leaves before it.
;
; `boot` prints the banner (HL), as the call it replaces would have, then
; asks the drive with `XT+`. A PRIFLOP answers OK and switches its half on;
; any other drive answers an error (Z), and no drive at all a bus error (C).
; Only on OK is the module installed (inst_core.asm) and its line printed;
; otherwise nothing changes and nothing is printed: plain CDOS, every byte
; of BASIC's memory kept. Either way it ends in the loader's own last two
; steps, NEW and READY (0x465B), which reset the stack CLEAR moved.
;
; Assembled at the address that follows BOOTU in memory (build_turbo.py
; replaces the `org 0` below). Equates from turbo_syms.inc.

    include "turbo_syms.inc"

CDOSV   equ 4174h       ; CDOS's base
MEMTOP  equ 40B1h       ; BASIC's top of memory
PRINTS  equ 2B75h       ; ROM: print the string at HL, ending 00H
CLEAR   equ 1E83h       ; ROM: CLEAR with DE bytes of string space
LDRDONE equ 465Bh       ; the loader: CALL 1B4Dh (NEW) ; JP 1A19h (READY)

    org 0

boot:
    call PRINTS         ; the banner, as the replaced call would have
    ld hl,c_on          ; a PRIFLOP? (service 0x18: the command, then the
    ld a,18h            ; error channel; C = no answer, Z = an error)
    rst 28h
    jp c,LDRDONE
    jp z,LDRDONE
    include "inst_core.asm"
    ld hl,m_on
    call PRINTS
    jp LDRDONE

mbase:      defw 0
c_on:       defb 3
            defm "XT+"
m_on:       defm "PRIFLOP turbo on", 13, 0
image:
