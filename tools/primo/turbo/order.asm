; PRIFLOP Primo turbo -- the order interpreter, shared by phase A's module
; (turbo.asm) and our own DOS (tools/primo/dos/dos.asm). The orders are
; listed at the top of turbo.asm; the protocol is in docs/user/PRIMO.md.
;
; Entered at `turbo` once the drive has answered `XT` with `00,TURBO`. The
; host provides:
;   xoff     interrupts and the NMI off, the output port in C
;   nmi_on   interrupts and the NMI on
;   x_end    leave turbo, NMI on, read the error channel: CDOS's flags
;            (C a bus error, Z with A = the drive's code, else NZ NC)
; and xop.asm (x_op, nfr, obuf) and xfer.asm (the frame layer).
;
; Out, as CDOS's load and save: NZ NC, A = the 'E' or 'J' order's length
; byte (a load's last record type), DE = its address (a load's end); Z with
; A = the drive's code; C, a bus error.

turbo:
    call xoff           ; lines released, frame 0
    xor a
    ld (clkval),a
    ld (nfr),a
    out (c),a
    call poll_low       ; the drive pulls DATA once it is in turbo ...
    jr c,x_fail
    call flip           ; ... and releases it on this first edge
order:
    ld a,OP_READ        ; the next order, and a 'W' order's data with it
    call x_op
    jr c,x_fail
    ld a,(obuf)
    cp 'W'
    jr z,order          ; stored
    ld hl,(obuf+1)
    cp 'R'
    jr nz,or_k
    ld a,OP_WRITE
    call x_op
    jr nc,order
x_fail:                 ; could not finish: leave, report a bus error
    call x_end
    scf
    ret
or_k:
    cp 'K'
    jr nz,or_end
    call nmi_on
    call jphl
    call xoff           ; frame number and CLOCK as they were: the drive,
    jr order            ; idle meanwhile, restarts at whatever level CLOCK is
or_end:
    call x_end
    ret c
    ret z               ; the error channel says what went wrong
    ld hl,(obuf+1)
    ld d,h              ; DE = the order's address: 'E' returns it; 'J'
    ld e,l              ; calls with it, as CDOS does -- CMD LOAD makes the
    ld a,(obuf)         ; program end of the DE that comes back (garbage hung
    cp 'J'              ; BASIC after the first real autostart, 2026-10-05)
    call z,jphl         ; 'J': call the program; a JP let its RET reach CMD
    ld a,1              ; LOAD with its own carry (DEVICE NOT PRESENT)
    or a                ; NZ NC, A = the length byte: the last record type,
    ld a,(obuf+3)       ; or a verify's count
    ret
jphl:
    jp (hl)
