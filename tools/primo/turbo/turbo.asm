; PRIFLOP Primo turbo -- the resident part, installed under CDOS by TURBO.
; A turbo on top of stock CDOS, at most 512 bytes
; (the user's limit for this interim turbo, 2026-10-05).
;
; Three entries of CDOS's RAM service table (RST 28H) are
; pointed here: 0x10 load, 0x12 verify, 0x0E save. Each first asks the drive,
; through CDOS's own standard-speed command service, whether it speaks turbo:
; the command `XT`, then the service index and the registers CDOS was called
; with (HL the name, DE the base or the save descriptor, C the record mask),
; on channel 15; the answer `00,TURBO` in the error channel. If not -- a
; 1541, another drive, PRIFLOP with the turbo off -- it jumps to the CDOS
; routine it replaced, with every register as it came, and nothing else
; changes.
;
; With turbo, the DRIVE does the thinking; it
; reads the name, the descriptor and CDOS's variables itself with 'R'. The
; Primo carries out the drive's orders until one ends the transfer:
;
;   'W' a, n   store n bytes at a; they come in the same  (a load)
;              frame as the order
;   'R' a, n   send the drive n bytes from a             (save, verify, and
;                                                         reading CDOS's own
;                                                         variables)
;   'K' a      call a, then go on                        (a 0xCD record)
;   'J' a, t   finish, then call a; if it returns, return (a 0xC3 record;
;              NC NZ, A = t, as CDOS                        t = the last
;                                                         record type)
;   'E' a, n   finish: DE = a, A = n, NZ NC              (a load's last type
;                                                         and end; a verify's
;                                                         count)
;
; Errors -- a missing file, "out of memory" (the drive checks a load against
; CDOS's limit itself) -- come from the drive's error channel, read at the
; finish as CDOS reads it.
;
; "Finish" means: leave turbo, NMI back on, and read the error channel at
; standard speed exactly as CDOS does at the end of its own routines, so its
; message buffer and its flags are what CDOS would leave.
;
; Assembled at org 0 and moved by the installer, which adds the load address
; to every absolute reference (build_turbo.py finds them).

SYSNMI  equ 403Bh       ; BASIC's shadow of port 0; bit 7 = NMI enable
CDOSV   equ 4174h       ; the CMD vector's operand: CDOS's base
ERRBUF  equ 0B96h       ; CDOS body: the error channel's text

    org 0

; the entry the installer puts into CDOS's table, three times. CDOS's service
; call (body 0x0B32) leaves the service's own index in A: 0x0E save, 0x10
; load, 0x12 verify.
svc:
    ld (xtbuf+3),a
    ld (xtbuf+4),hl
    ld (xtbuf+6),de
    ld (xtbuf+8),bc
    cp 0Eh
    jr z,sv_neg
    inc c
    dec c
    jr z,sv_stock       ; load with mask 0: a plain file (the directory)
sv_neg:
    ; ask the drive: `XT`, the index and the registers, on channel 15,
    ; with CDOS's own command service, which reads the answer into its
    ; error buffer (body 0x0B96)
    ld hl,xtbuf
    ld a,18h
    rst 28h
    jr c,sv_stock
    ld hl,(CDOSV)
    ld de,ERRBUF
    add hl,de
    ld de,xtok
    ld b,4
sv_cmp:
    ld a,(de)
    cp (hl)
    jr nz,sv_stock
    inc hl
    inc de
    djnz sv_cmp
    jr turbo
sv_stock:
    ld a,(xtbuf+3)      ; the old routine for this index
    ld e,a
    ld d,0
    ld hl,oldtab-0Eh
    add hl,de
    ld a,(hl)
    inc hl
    ld h,(hl)
    ld l,a
    push hl
    ld hl,(xtbuf+4)
    ld de,(xtbuf+6)
    ld bc,(xtbuf+8)
    ret                 ; into it, every register as it came

    include "order.asm"  ; turbo: the order interpreter, shared with our DOS

; xoff: NMI and interrupts off (as CDOS's 0x0686), the port in C.
xoff:
    di
    ld a,(SYSNMI)
    and 7Fh
    call nmi_w
    ld a,(outport)
    ld c,a
    ret

; x_end: leave turbo; NMI back on; read the error channel as CDOS would.
; Out: CDOS's flags (C bus error, Z with A = code, else NZ NC).
x_end:
    ld a,OP_QUIT
    call x_op
    xor a               ; let go of CLOCK and DATA: the last edge may have
    out (c),a           ; left CLOCK pulled, and the next session waits for it
    call nmi_on
    ld a,1Ah
    rst 28h
    ret

nmi_on:
    ld a,(SYSNMI)
    or 80h
    ei
nmi_w:
    ld (SYSNMI),a
    out (0),a
    ret

    include "xop.asm"    ; x_op, nfr, obuf, shared with our DOS
oldtab:                 ; the replaced routines, by index
old_save:   defw 0      ; 0x0E
old_load:   defw 0      ; 0x10
old_verify: defw 0      ; 0x12
xtok:   defm "00,T"          ; the answer starts so; an error text never does
xtbuf:  defb 10         ; the command: length, `XT`, index, HL, DE, BC, `.`
        defm "XT"
        defs 7
        defm "."        ; so a 0x0D in B is not taken for the line's end

outport: defb 080h      ; set by the installer from CDOS's own OUT operand
clkval:  defb 0         ; the CLOCK output value now (0 or CLK); DATA released

    include "xfer.asm"
turbo_end:
