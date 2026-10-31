; PRIFLOP DOS -- the boot file's loader (step B7).
; Copyright (C) 2026 sd2iec_pico2w contributors; GPL v2.
;
; The boot ROM stores the boot file from 0x4500 on and jumps to its first
; byte. The file is this loader, then the body's relocations -- one byte
; each, the distance from the one before (from the image's start for the
; first); 0FFh steps 254 bytes without a relocation, 0 ends the list --
; then the body image (dos.asm, assembled at 0) without its padding, which
; is zeros (build_dos.boot_file()). Less to load: the boot is no slower
; than CDOS's.
;
; In order:
;   0. The image made whole: its tail moved up, the padding zeroed.
;   1. The BASIC ROM: the bytes CDOS's loader compares, as our DOS calls
;      the same routines. Another ROM: a message, nothing installed.
;   2. The port, from the two copies of it in the machine's boot ROM
;      (tier 1); 80h if they disagree or are unknown.
;   3. `XT+`, sent by the body's own code where it lies now, relocated
;      there for the purpose: a PRIFLOP switches its turbo on and answers
;      OK. Anything else -- another drive, no drive -- is a message, and
;      nothing resident is touched (the user's decision).
;   4. Where to go. Our DOS of any build (its signature, dossig), CDOS 1.0
;      (its RST 28H and service 0x00 where CDOS has them), with phase A's
;      module right under it, or nothing: then under BASIC's top. Over a
;      DOS, ours ends where that one ended and chains to the vectors that
;      one had saved, never to itself, so a reboot never stacks a copy.
;      BASIC's top moves only if it lay right under that region.
;   5. CLEAR 50 (the stack moves under the new top first), the copy, the
;      relocation to its place, the vectors.
;   6. The banner, NEW, READY: CDOS's order.
;   7. If the drive answered step 3's XT+ with 00, OK,01,00 -- PRIFLOP.CFG
;      has PFM_AUTOSTART=ON (primo_turbo.c) -- BASIC gets the line CMD LOAD
;      after NEW instead of READY, and the bare CMD LOAD starts PFM.
;
; Equates from dos_syms.inc (build_dos.boot_file()): BODYSZ, the body's
; offsets B_*, phase A's module A_SIZE, A_OP, A_OPW, where the list and
; the image are (RELS, IMG), and the padding left out (PADAT, PADSZ, TAILSZ).

    include "dos_syms.inc"

MEMTOP  equ 40B1h       ; BASIC's top
CMDV    equ 4174h       ; the CMD vector's operand
RSTOPD  equ 400Dh       ; RST 28H's
PRINTS  equ 2B75h       ; ROM: the text at HL, up to a 0
CLEAR   equ 1E83h       ; ROM: CLEAR, DE bytes of strings
NEWP    equ 1B4Dh       ; ROM: NEW
READY   equ 1A19h       ; ROM: READY
LINRET  equ 1A7Eh       ; ROM: the line input's return, HL = the line less one
STRSPC  equ 40A0h       ; the string space; at the prompt the stack is under it
INPBUF  equ 40A7h       ; the input buffer

    org 4500h

go:
    ld hl,IMG+PADAT+TAILSZ-1 ; 0. the image whole
    ld de,IMG+BODYSZ-1
    ld bc,TAILSZ
    lddr
    xor a
    ld b,PADSZ
pad:
    ld (de),a
    dec de
    djnz pad

    ld hl,1A01h         ; 1. the ROM
    ld de,romid
    ld b,4
    call same
    jr nz,badrom
    ld hl,2B1Ch
    ld b,3
    call same
badrom:
    ld hl,m_rom
    jr nz,fail

    ld a,(029Fh)        ; 2. the port
    ld b,a
    ld a,(0687h)
    cp b
    jr nz,port80
    cp 40h
    jr z,port
port80:
    ld a,80h
port:
    ld (IMG+B_OUTPORT),a

    ld de,IMG           ; 3. the turbo
    call reloc
    ld hl,xton
    call IMG+B_S18
    ld hl,m_nodrv
    jr c,fail
    ld hl,m_noturbo
    jr nz,where
    call PRINTS         ; refused: the drive's own message after ours
    ld hl,IMG+B_ERRBUF
    call PRINTS
    ld hl,m_cr
fail:
    call PRINTS
    jp done

where:                  ; 4. by default: under the top, its last byte left
    ld hl,(CMDV)        ; free, as CDOS leaves it. The vectors there now are
    ld (chcmd),hl       ; the ones to chain to.
    ld hl,41A6h
    ld de,chhook
    ld bc,3
    ldir
    ld hl,(MEMTOP)
    inc hl
    ld (rstart),hl
    dec hl
    dec hl
    ld (rend),hl

    ld hl,(CMDV)        ; ours, any build?
    ld de,B_SIG
    add hl,de
    ld de,sigtxt
    ld b,4
    call same
    jr nz,cdos
    ld e,(hl)           ; where it keeps the vectors it chains to
    inc hl
    ld d,(hl)
    inc hl
    push hl
    ex de,hl
    ld de,chcmd
    ldi
    ldi
    pop hl
    ld e,(hl)
    inc hl
    ld d,(hl)
    ex de,hl
    ld de,chhook
    ld bc,3
    ldir
    ld hl,(CMDV)
    ld (rstart),hl
    ld de,BODYSZ-1
    jr region

cdos:                   ; CDOS 1.0: its RST 28H at its service call, and
    ld hl,(CMDV)        ; its table's entry 0x00 where CDOS has it
    ld de,0B32h
    add hl,de
    ex de,hl
    ld hl,(RSTOPD)
    or a
    sbc hl,de
    jr nz,place
    ld hl,(CMDV)
    ld de,0B42h
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    ld hl,(CMDV)
    ld bc,0652h
    add hl,bc
    or a
    sbc hl,de
    jr nz,place
    ld hl,(CMDV)        ; the vectors CDOS saved: the CMD vector at 0084h,
    ld de,0084h         ; the error hook's three bytes at 055Eh
    add hl,de
    ld de,chcmd
    ldi
    ldi
    ld hl,(CMDV)
    ld de,055Eh
    add hl,de
    ld de,chhook
    ld bc,3
    ldir
    ld hl,(CMDV)
    ld de,-A_SIZE       ; phase A's module right under it joins the region:
    add hl,de           ; its first byte, and its first operand relocated to
    ld a,(hl)           ; where it lies (inst_core.asm's own test)
    cp A_OP
    jr nz,cdos1
    push hl
    inc hl
    ld e,(hl)
    inc hl
    ld d,(hl)
    pop hl
    push hl
    ld bc,A_OPW
    add hl,bc
    or a
    sbc hl,de
    pop hl
    jr z,cdos2
cdos1:
    ld hl,(CMDV)
cdos2:
    ld (rstart),hl
    ld hl,(CMDV)
    ld de,0BB3h         ; CDOS's last byte
region:
    add hl,de
    ld (rend),hl

place:                  ; ours ends where the region ends, its base never
    ld hl,(rend)        ; ending in 97h (ASM takes that for "no DOS")
    ld de,1-BODYSZ
    add hl,de
    ld a,l
    cp 97h
    jr nz,pl1
    dec hl
pl1:
    ld (base),hl
    ld de,(rstart)      ; BASIC's top moves only if it lay right under it
    dec de
    ld hl,(MEMTOP)
    or a
    sbc hl,de
    jr nz,pl2
    ld hl,(base)
    dec hl
    ld (MEMTOP),hl
pl2:
    ld de,50            ; 5. CLEAR first: it moves the stack under the top
    call CLEAR
    ld hl,(base)
    ld de,IMG
    or a
    sbc hl,de
    ex de,hl
    call reloc          ; from where it lies to its place
    ld hl,IMG
    ld de,(base)
    ld bc,BODYSZ
    ldir

    ld hl,vtab          ; the vectors: JP to the body
    ld b,6
vec:
    ld e,(hl)
    inc hl
    ld d,(hl)
    inc hl
    ld a,0C3h
    ld (de),a
    inc de
    push hl
    ld a,(hl)
    inc hl
    ld h,(hl)
    ld l,a
    push de
    ld de,(base)
    add hl,de
    pop de
    ex de,hl
    ld (hl),e
    inc hl
    ld (hl),d
    pop hl
    inc hl
    inc hl
    djnz vec

    ld hl,(base)        ; and what it chains to
    ld de,B_OLDCMD
    add hl,de
    ex de,hl
    ld hl,chcmd
    ldi
    ldi
    ld hl,(base)
    ld de,B_OLDHOOK
    add hl,de
    ex de,hl
    ld hl,chhook
    ld bc,3
    ldir

    ld hl,m_ok          ; 6.
    call PRINTS

    ld hl,IMG+B_ERRBUF+6 ; 7. PFM at the start: the drive answered XT+ with
    ld de,s_t01         ; 00, OK,01,00, PRIFLOP.CFG's PFM_AUTOSTART=ON
    ld b,4              ; (primo_turbo.c); 00, OK,00,00 otherwise
    call same
    jr nz,done
    call NEWP           ; on: BASIC gets the line CMD LOAD as if typed, as
    ld hl,(STRSPC)      ; PFM hands its lines over (pfm.asm, handover):
    ld sp,hl            ; the stack as at the prompt, the line in the input
    ld hl,0             ; buffer, the ROM where the line input returns
    push hl
    ld de,(INPBUF)
    inc de
    ld hl,s_cmdl
    ld bc,9
    ldir
    ld hl,(INPBUF)
    ld a,0Dh
    or a
    jp LINRET
done:
    call NEWP
    jp READY

; Z if the B bytes at HL are those at DE; HL and DE after them if so
same:
    ld a,(de)
    cp (hl)
    ret nz
    inc hl
    inc de
    djnz same
    ret

; DE added to every word the relocation list names in the image
reloc:
    ld hl,IMG
    ld bc,RELS
rl1:
    ld a,(bc)
    inc bc
    or a
    ret z
    push de
    ld e,a
    ld d,0
    add hl,de
    pop de
    inc a
    jr z,rl2            ; 0FFh: 254 bytes on, nothing to relocate
    ld a,(hl)
    add a,e
    ld (hl),a
    inc hl
    ld a,(hl)
    adc a,d
    ld (hl),a
    dec hl
    jr rl1
rl2:
    dec hl
    jr rl1

vtab:                   ; the JP of each vector, and its target in the body
    defw 4173h, 0       ; CMD
    defw 400Ch, B_SVC   ; RST 28H
    defw 41A6h, B_EHOOK ; the error hook
    defw 4161h, B_EFN   ; EOF()
    defw 41BEh, B_PEND  ; the end of PRINT
    defw 41C1h, B_COUT  ; the character output

romid:  defb 0E5h, 2Ah, 0EAh, 40h       ; at 1A01h
        defb 0CDh, 37h, 23h             ; at 2B1Ch
sigtxt: defm "PDOS"
xton:   defb 3
        defm "XT+"
m_ok:   defb 0Ch
        defm "PRIFLOP DOS, turbo"
m_cr:   defb 13, 0
m_rom:  defm "Not this BASIC ROM", 13, 0
m_nodrv: defm "No drive", 13, 0
m_noturbo: defm "No PRIFLOP turbo: ", 0
s_t01:  defm ",01,"     ; in the answer to XT+: PFM at the start
s_cmdl: defm "CMD LOAD", 0
rstart: defw 0          ; the region to take: its first byte
rend:   defw 0          ; ... and its last
base:   defw 0          ; where ours goes
chcmd:  defw 0          ; the CMD vector to chain to
chhook: defs 3          ; ... and the error hook
