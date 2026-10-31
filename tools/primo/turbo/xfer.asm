; PRIFLOP Primo turbo -- the frame layer, 1 bit per CLOCK edge.
; (version 3, 2026-10-05).
;
; The Primo sets the pace: it flips CLOCK, the drive's PIO program
; (src/pico/primo_turbo.pio) answers on DATA within a microsecond, and the
; Primo reads DATA about 12 us later. Nothing here is timed. Where the drive
; needs time (reading the file, checking a frame, writing flash) it says
; "ready" by pulling DATA, and the Primo polls for it.
;
; Every exchange, either direction, starts the same way: the Primo sends the
; frame number and its complement, and the drive answers whether it accepts
; it (see xchg). On any doubt the Primo resyncs: both lines released, ~3 ms
; still; after 2 ms without an edge the drive flushes and starts over
; (version 1 lost frames without this).
;
; Registers, everywhere in this file:
;   C       the output port (0x80 on a B64, 0x40 on an A64: `outport`)
;   D, E    the two CLOCK output values of a run of data edges (D first)
;   B'C'    the Fletcher-16 sums s1, s2 (alternate set)
;   E'      the byte count of a payload (alternate set)
; Interrupts and the NMI must be off; the callers see to it.
;
; Output port bits are inverted by the 7406: 1 pulls the line low.
; Input port 0x40 is not: 1 = the line is high (released).

INPORT  equ 040h
CLK     equ 020h        ; output bit 5: pull CLOCK
DAT     equ 010h        ; output bit 4: pull DATA; input bit 4: DATA high
RESYNC  equ 300         ; idle loops of 26 T: ~3.1 ms at 2.5 MHz

OP_READ  equ 'r'        ; the drive sends: an order, and a 'W' order's data
OP_WRITE equ 'w'        ; the Primo sends: the bytes an 'R' order asked for
OP_QUIT  equ 'q'        ; leave turbo

; The host defines `outport` (the output port) and `clkval` (the CLOCK output
; value now, 0 or CLK; DATA released): turbo.asm, or dos.asm.

; ---------------------------------------------------------------------------
; Primitives

; flip: one CLOCK edge, DATA released.
flip:
    ld a,(clkval)
    xor CLK
    ld (clkval),a
    out (c),a
    ret

; rel_flip_poll: release DATA, flip CLOCK, wait for the drive to pull DATA.
; Out: NC = it did; C = timeout (65536 polls of ~20 us, ~1.3 s). Clobbers A, DE.
rel_flip_poll:
    call flip
; poll_low: the same without the flip.
poll_low:
    ld de,0
pl_loop:
    in a,(INPORT)
    and DAT
    ret z               ; AND clears carry
    dec de
    ld a,d
    or e
    jr nz,pl_loop
    scf
    ret

; flip_read: flip CLOCK, wait as rx_raw does, read DATA. Out: Z = DATA low.
flip_read:
    call flip
    push af
    pop af
    in a,(INPORT)
    and DAT
    ret

; data_clocks: D = the value after the next flip, E = the value now.
data_clocks:
    ld a,(clkval)
    ld e,a
    xor CLK
    ld d,a
    ret

; fl_count: add A to the Fletcher-16 sums, count the payload down.
; Z after the last byte.
fl_count:
    exx
    add a,b
    ld b,a
    add a,c
    ld c,a
    dec e
    exx
    ret

; rx_raw: receive one byte, LSB first, 8 edges D,E,D,E... Out: A = B = byte.
; Each read comes ~30 T (12 us) after its edge: the previous bit is finished
; first (bit 0 waits with PUSH/POP). B starts as 0x80, a sentinel that
; reaches bit 0 when seven bits are in. ~55 T a bit.
rx_raw:
    ld b,80h
    out (c),d           ; edge for bit 0
    push af
    pop af
    in a,(INPORT)       ; bit 0
rxr_loop:
    out (c),e           ; edges 1, 3, 5, 7
    and DAT
    add a,0F0h          ; carry = DATA
    rr b                ; bits 0, 2, 4, 6
    in a,(INPORT)       ; bits 1, 3, 5, 7
    bit 0,b
    jr nz,rxr_last      ; bit 7 is in A
    out (c),d           ; edges 2, 4, 6
    and DAT
    add a,0F0h
    rr b                ; bits 1, 3, 5
    in a,(INPORT)       ; bits 2, 4, 6
    jr rxr_loop
rxr_last:
    and DAT
    add a,0F0h
    rr b
    ld a,b
    ret

; tx_sums: send s1 and s2 as they stand.
tx_sums:
    exx
    ld a,b
    exx
    call tx_a
    exx
    ld a,c
    exx
tx_a:                   ; send A
    ld b,a

; tx_raw: send the byte in B, LSB first, 8 edges. Each bit: DATA set with
; CLOCK unchanged, then CLOCK flipped with DATA unchanged, so the drive samples
; a settled line. A sentinel 1 shifted in at the top ends the loop. ~60 T a bit.
tx_raw:
    scf
    rr b                ; bit 0 in carry
txr_loop:
    sbc a,a             ; FF for a 1
    cpl                 ; 00 for a 1: DATA released
    and DAT
    or e
    out (c),a
    xor CLK
    out (c),a           ; edges 0, 2, 4, 6
    srl b
    sbc a,a
    cpl
    and DAT
    or d
    out (c),a
    xor CLK
    out (c),a           ; edges 1, 3, 5, 7
    srl b               ; Z once the sentinel is out
    jr nz,txr_loop
    ret

; ---------------------------------------------------------------------------
; xchg: one exchange, frame number (nfr), operation in D' (x_op sets it; the
; alternate set's D' is nothing else's).
; Out: NZ = do it again (resynced if needed); Z = done, NC.
;
; Every exchange opens with the frame number and its complement (16 edges,
; the Primo sending), H (poll: the drive has them) and G (DATA released =
; it accepts that number). Both sides know what comes next without being
; told: the drive gave the order that leads to it. The payload's Fletcher-16
; starts with s1 = the frame number, so a wrong frame fails its check.
;
; Read: the drive sends an order (op, address, length: 4 bytes to obuf),
; for a 'W' order its data, then the check; D. The order's length is used
; before the check; that is safe, because a wrong one only misaligns this
; frame, which then fails its check, and the drive acts only on numbers that
; passed their complement.
; Write (the answer to an 'R' order): the Primo sends the order's bytes and
; the check; Q (the drive checks and stores), V (its verdict: DATA released
; = stored); D. A misread verdict heals itself: "stored" taken for "again"
; sends a duplicate the drive drops by its number; "again" taken for
; "stored" asks for the next frame, and the drive, still without the data,
; puts the same 'R' order in it (step D simulation, a CLOCK spike during a
; save).
; Quit: the number, H, G. The drive leaves turbo.
xchg:
    call data_clocks
    ld hl,nfr           ; (nothing below touches HL until the order)
    ld a,(hl)
    call tx_a
    ld a,(hl)
    cpl
    call tx_a
    call rel_flip_poll  ; H: the drive has the number
    jr c,resync         ; no answer
    call flip_read      ; G: DATA released = it accepts it
    jr z,resync
    call data_clocks
    ld a,(hl)
    exx
    ld b,a              ; s1 starts at the frame number
    ld c,0
    ld a,d              ; the operation
    exx
    cp OP_QUIT
    ret z
    cp OP_READ
    jr nz,x_write
    ld hl,obuf
    ld a,4
    call rd_n
    ld a,(obuf)
    cp 'W'
    jr nz,x_rcheck
    ld hl,(obuf+1)
    ld a,(obuf+3)
    call rd_n
x_rcheck:
    call rx_raw         ; s1 as the drive has it
    exx
    sub b
    ld h,a              ; (not D': that holds the operation)
    exx
    call rx_raw         ; s2
    exx
    sub c
    or h                ; Z = both match
    exx
    push af
    call flip           ; D: the drive releases DATA
    pop af
    ret z
; resync: both lines released, then still for ~3 ms. Out: NZ, NC.
resync:
    xor a
    ld (clkval),a
    out (c),a
    ld de,RESYNC
rs_loop:
    dec de
    ld a,d
    or e
    jr nz,rs_loop
    inc a
    ret

x_write:
    ld hl,(obuf+1)
    ld a,(obuf+3)
    exx
    ld e,a
    exx
xw_loop:
    ld a,(hl)
    inc hl
    ld b,a
    call fl_count
    push af
    call tx_raw
    pop af
    jr nz,xw_loop
    call tx_sums
    call rel_flip_poll  ; Q: the drive has checked and stored it
    jr c,resync
    call flip_read      ; V: released = stored
    xor DAT             ; Z = stored
    push af
    call flip           ; D
    pop af
    ret                 ; NZ: send it again (nothing to resync)

; rd_n: receive A bytes (1-255, 0 = 256) to HL, into the sums.
rd_n:
    exx
    ld e,a
    exx
rd_loop:
    call rx_raw
    ld (hl),a
    inc hl
    call fl_count
    jr nz,rd_loop
    ret
