; PTURBO -- a soak test for loading, verifying and saving on the Primo,
; turbo or not. CMD LOAD"PTURBO8" (or "PTURBO9" for the RAM drive) starts it;
; it runs in rounds until BRK is pressed: the key is polled after every
; transfer and remembered, and the round in progress is finished first.
;
; Each round, for each size in `sizes` (1, 255, 256, 257, 1000, 4096, 8192,
; 16384 bytes) it:
;   1. fills BUF with a pseudo-random pattern (xorshift16, seeded with the
;      round and the size, so no two files repeat);
;   2. saves it as a BASIC-type record (0xD1) to "@:PTn" -- CDOS service 0x0E;
;   3. clears BUF and the byte after it;
;   4. loads "PTn" back -- service 0x10 -- and checks the returned type (0xD1),
;      the returned end (DE = BUF + size), every byte against the pattern, and
;      that the byte after the end is still 0 (nothing written past it);
;   5. verifies it -- service 0x12, CMD TEST's -- which must count 0;
;   6. flips one byte and verifies again, which must count exactly 1, so a
;      verify that compares nothing cannot pass.
;   Under our own DOS (phase B; found by its signature "PDOS"
;   at the CMD vector + 0B87h, as M finds it), which has no service 0x12,
;   steps 5 and 6 verify by reading the file back instead: `rbv` opens it on
;   the standard bus (services 0x14, 0x22, 0x26, 0x1C), compares every byte
;   -- the header, the data against memory, the closing 0xC9 -- and counts
;   the differences as 0x12 does.
;   Then the screen, as CMD SAVE SCREEN / CMD LOAD / CMD TEST handle it: one
; 0xD5 record of the whole screen (0x1800 bytes from (404Ah) x 256). Whatever
; the screen shows is the pattern: it is copied to BUF, saved to "@:PTS",
; cleared, loaded back and compared, then verified (0, and 1 with a byte
; flipped). Nothing prints in between unless something failed. Size index 8.
; It calls CDOS's services exactly as CMD LOAD / SAVE / TEST do, so with
; TURBO installed every transfer goes through the turbo, and without it the
; same program tests stock CDOS. Speed and retries are counted by the drive
; (/api/stats: pt_*); the Primo's own clock stops while transfers run with
; the NMI off.
;
; Screen: one line per round, "R <round> OK <files> ERR <errors>"; an error
; prints first as <phase><size index> <code>, phase S save, L load, T type or
; end, C compare, V verify 0, W verify 1; code 255 = a bus error (carry).

PRINTS  equ 2B75h       ; ROM: print the string at HL, ending 00H
PRCH    equ 032Ah       ; ROM: print the character in A
PRNUM   equ 0FAFh       ; ROM: print HL as a number
KYBHND  equ 001Dh       ; ROM: the key held now, 0 none, 1 BRK (non-blocking)
BUF     equ 8000h       ; 16 KB + 1 guard byte, under BASIC's top after TURBO
LIMIT   equ 0C800h      ; the load limit given to CDOS (service 0x02)
NSIZES  equ 8

    org 7000h

start:
    ld hl,(4174h)       ; our own DOS? then verify by reading back
    ld de,0B87h
    add hl,de
    ld de,m_pdos
    ld b,4
    ld a,1
sg_loop:
    ld c,a
    ld a,(de)
    cp (hl)
    ld a,c
    jr nz,sg_end
    inc hl
    inc de
    djnz sg_loop
    xor a
sg_end:
    ld (vfy),a
    ld a,(dev)
    ld c,a
    ld a,06h            ; CDOS: the device
    rst 28h
    ld hl,m_title
    call PRINTS
    ld a,(dev)
    ld l,a
    ld h,0
    call PRNUM
    ld a,13
    call PRCH
    ld hl,0
    ld (round),hl
    ld (oks),hl
    ld (errs),hl
    xor a
    ld (brk),a

rloop:
    ld hl,(round)
    inc hl
    ld (round),hl
    xor a
    ld (sidx),a
sloop:
    ld a,(sidx)
    add a,'0'
    ld (n_save+5),a
    ld (n_load+3),a
    ld a,(sidx)
    add a,a
    ld e,a
    ld d,0
    ld hl,sizes
    add hl,de
    ld e,(hl)
    inc hl
    ld d,(hl)
    ld (k),de
    ld (desc+3),de
    ld (vlen),de
    ld hl,BUF
    ld (vmem),hl
    ld a,0D1h
    ld (vtype),a
    ld hl,(round)       ; seed: round x 8 + size index, never 0
    add hl,hl
    add hl,hl
    add hl,hl
    ld a,(sidx)
    or l
    or 1
    ld l,a
    ld (seed),hl

    call fill
    ld hl,n_save        ; 1. save
    ld de,desc
    ld a,0Eh
    rst 28h
    ld b,'S'
    call result
    jp c,next

    ld hl,BUF           ; 2. clear BUF and the guard byte
    ld (hl),0
    ld d,h
    ld e,l
    inc de
    ld bc,(k)
    ldir

    ld de,ret0          ; 3. load: CDOS's handler and limit, as CMD LOAD
    ld hl,LIMIT
    ld a,02h
    rst 28h
    ld hl,n_load
    ld de,BUF
    ld c,1Fh
    ld a,10h
    rst 28h
    ld b,'L'
    call result
    jr c,next
    cp 0D1h             ; the type ...
    jr nz,bad_end
    ld hl,(k)           ; ... and the end
    ld bc,BUF
    add hl,bc
    or a
    sbc hl,de
    jr nz,bad_end

    call check          ; every byte, and the guard
    ld b,'C'
    ld a,0
    jr c,fail

    ld hl,n_load        ; 4. verify: 0 differences
    ld de,BUF
    ld c,0Eh
    call vrf
    ld b,'V'
    call result
    jr c,next
    or a
    jr nz,fail

    ld hl,(k)           ; 5. one byte changed: exactly 1
    srl h
    rr l
    ld de,BUF
    add hl,de
    ld a,(hl)
    cpl
    ld (hl),a
    push hl
    ld hl,n_load
    ld de,BUF
    ld c,0Eh
    call vrf
    pop hl
    push af
    ld a,(hl)
    cpl
    ld (hl),a
    pop af
    ld b,'W'
    call result
    jr c,next
    cp 1
    jr nz,fail
sz_ok:
    ld hl,(oks)
    inc hl
    ld (oks),hl
    jr next

bad_end:
    ld b,'T'
fail:
    call err
next:
    ld a,(sidx)
    inc a
    ld (sidx),a
    cp NSIZES
    jp c,sloop
    call scrtest

    ld hl,m_r           ; the round's line
    call PRINTS
    ld hl,(round)
    call PRNUM
    ld hl,m_ok
    call PRINTS
    ld hl,(oks)
    call PRNUM
    ld hl,m_err
    call PRINTS
    ld hl,(errs)
    call PRNUM
    ld a,13
    call PRCH
    call KYBHND         ; held now, or pressed during the round
    cp 1
    jr z,stop
    ld a,(brk)
    or a
    jp z,rloop
stop:
    ld hl,m_stop
    call PRINTS
    ld de,start         ; as CDOS called us: DE = our address
    or a
ret0:
    ret

; scrtest: the screen round, size index 8 (see the top).
scrtest:
    ld a,NSIZES
    ld (sidx),a
    ld a,(404Ah)        ; the screen's high byte, as CDOS takes it
    ld h,a
    ld l,0
    ld (scr),hl
    ld (sdesc+5),hl
    ld (vmem),hl
    ld de,(slen)
    ld (sdesc+3),de
    ld (vlen),de
    ld a,0D5h
    ld (vtype),a
    ld de,BUF           ; 1. keep a copy
    ld bc,(slen)
    ldir
    ld hl,n_ssave       ; 2. save
    ld de,sdesc
    ld a,0Eh
    rst 28h
    ld b,'S'
    call result
    ret c
    ld hl,(scr)         ; 3. clear it
    ld (hl),0
    ld d,h
    ld e,l
    inc de
    ld bc,(slen)
    dec bc
    ldir
    ld de,ret0          ; 4. load, as CMD LOAD
    ld hl,LIMIT
    ld a,02h
    rst 28h
    ld hl,n_sload
    ld de,BUF           ; the BASIC base; a screen record does not use it
    ld c,1Fh
    ld a,10h
    rst 28h
    ld b,'L'
    call result
    ret c
    cp 0D5h             ; the type ...
    jr nz,s_badend
    ld hl,(scr)         ; ... and the end (0x0000 for a screen at 0xE800)
    ld bc,(slen)
    add hl,bc
    or a
    sbc hl,de
    jr nz,s_badend
    ld hl,(scr)         ; 5. every byte against the copy
    ld de,BUF
    ld bc,(slen)
s_cmp:
    ld a,(de)
    cp (hl)
    jr nz,s_diff
    inc hl
    inc de
    dec bc
    ld a,b
    or c
    jr nz,s_cmp
    ld hl,n_sload       ; 6. verify: 0 differences
    ld de,BUF
    ld c,0Eh
    call vrf
    ld b,'V'
    call result
    ret c
    or a
    jr nz,s_fail
    ld hl,(slen)        ; 7. one byte changed: exactly 1
    srl h
    rr l
    ld de,(scr)
    add hl,de
    ld a,(hl)
    cpl
    ld (hl),a
    push hl
    ld hl,n_sload
    ld de,BUF
    ld c,0Eh
    call vrf
    pop hl
    push af
    ld a,(hl)
    cpl
    ld (hl),a
    pop af
    ld b,'W'
    call result
    ret c
    cp 1
    jr nz,s_fail
s_ok:
    ld hl,(oks)
    inc hl
    ld (oks),hl
    ret
s_badend:
    ld b,'T'
    jr s_fail
s_diff:
    ld b,'C'
    xor a
s_fail:
    jp err

; vrf: verify the file named at HL: CDOS's service 0x12, or with `vfy` 0
; read back (rbv). A = the differences, NC NZ; C a bus error; Z an error code.
vrf:
    ld a,(vfy)
    or a
    jr z,rbv
    ld a,12h
    rst 28h
    ret

; rbv: the file named at HL read back on the standard bus, secondary 2, and
; compared with what it must hold: vtype, offset 0, vlen, vlen bytes from
; vmem, 0C9h, then EOI. A = the differences (at most 255; a length that
; differs, or a timeout, is 255).
rbv:
    ld c,2              ; OPEN: the drive's message decides
    ld a,14h
    rst 28h
    ret c
    ret z
    xor a
    ld (rb_bad),a
    ld h,a
    ld l,a
    ld (rb_i),hl
    ld a,(vtype)
    ld (rb_hdr),a
    ld hl,(vlen)
    ld (rb_hdr+3),hl
    ld c,0              ; status 0, so EOI is this file's
    ld a,0Ah
    rst 28h
    ld a,(dev)
    ld c,a
    ld a,22h            ; TALK, secondary 2
    rst 28h
    jr c,rb_to
    ld c,2
    ld a,26h
    rst 28h
    jr c,rb_to
rb_loop:
    ld a,1Ch            ; a byte
    rst 28h
    jr c,rb_to
    ld b,a
    ld hl,(rb_i)        ; what it must be
    ld de,5
    or a
    sbc hl,de
    jr c,rb_hd
    ld de,(vlen)
    or a
    sbc hl,de
    jr c,rb_dt
    ld a,h
    or l
    jr nz,rb_no         ; past the 0C9h: never right
    ld a,0C9h
    jr rb_cp
rb_hd:
    add hl,de           ; DE = 5: the index again
    ld de,rb_hdr
    add hl,de
    ld a,(hl)
    jr rb_cp
rb_dt:
    add hl,de
    ld de,(vmem)
    add hl,de
    ld a,(hl)
rb_cp:
    cp b
    jr z,rb_nx
rb_no:
    ld a,(rb_bad)
    inc a
    jr z,rb_nx          ; 255 stays
    ld (rb_bad),a
rb_nx:
    ld hl,(rb_i)
    inc hl
    ld (rb_i),hl
    ld a,0Ch            ; EOI: the file's end
    rst 28h
    and 40h
    jr z,rb_loop
    ld hl,(vlen)        ; the length: 5 + vlen + 1
    ld de,6
    add hl,de
    ld de,(rb_i)
    or a
    sbc hl,de
    jr z,rb_end
rb_to:
    ld a,0FFh
    ld (rb_bad),a
rb_end:
    ld a,2Ah            ; UNTALK
    rst 28h
    ld c,2              ; CLOSE: its message as any service's
    ld a,16h
    rst 28h
    ret c
    ret z
    ld a,1              ; NC NZ
    or a
    ld a,(rb_bad)
    ret

; result: the flags of a CDOS service. C: a bus error, Z: an error code in A.
; Out: NC = fine (A kept); C = counted and printed.
; Also polls BRK, which is only seen while held: once per round missed a
; press made during the round (2026-10-06), so every transfer looks.
result:
    push af
    push bc
    push de
    push hl
    call KYBHND
    cp 1
    jr nz,res_nk
    ld (brk),a
res_nk:
    pop hl
    pop de
    pop bc
    pop af
    jr c,res_bus
    jr z,res_err
    scf
    ccf
    ret
res_bus:
    ld a,0FFh
res_err:
    call err
    scf
    ret

; err: print <B><size index> <A> and count it.
err:
    push af
    ld hl,(errs)
    inc hl
    ld (errs),hl
    ld a,b
    call PRCH
    ld a,(sidx)
    add a,'0'
    call PRCH
    ld a,' '
    call PRCH
    pop af
    ld l,a
    ld h,0
    call PRNUM
    ld a,13
    call PRCH
    ret

; fill: BUF[0 .. k) = the pattern for `seed`.
fill:
    ld hl,(seed)
    ld (xs),hl
    ld de,BUF
    ld bc,(k)
fl_loop:
    ld a,b
    or c
    ret z
    call xrnd
    ld (de),a
    inc de
    dec bc
    jr fl_loop

; check: BUF[0 .. k) against the pattern, BUF[k] = 0. C = a difference.
check:
    ld hl,(seed)
    ld (xs),hl
    ld de,BUF
    ld bc,(k)
ck_loop:
    ld a,b
    or c
    jr z,ck_guard
    call xrnd
    ex de,hl
    cp (hl)
    ex de,hl
    scf
    ret nz
    inc de
    dec bc
    jr ck_loop
ck_guard:
    ld a,(de)
    or a                ; NC
    ret z
    scf
    ret

; xrnd: the next pattern byte in A (16-bit xorshift, John Metcalf's).
xrnd:
    ld hl,(xs)
    ld a,h
    rra
    ld a,l
    rra
    xor h
    ld h,a
    ld a,l
    rra
    ld a,h
    rra
    xor l
    ld l,a
    xor h
    ld h,a
    ld (xs),hl
    ret

brk:    defb 0          ; BRK seen during the round
dev:    defb 8          ; patched to 9 in PTURBO9
vfy:    defb 1          ; 0 under our own DOS (phase B; set at the start), which
                        ; has no service 0x12: the verify steps read back (rbv)
round:  defw 0
oks:    defw 0
errs:   defw 0
sidx:   defb 0
k:      defw 0
seed:   defw 0
xs:     defw 0
vmem:   defw 0          ; what verifying compares: memory, length, record type
vlen:   defw 0
vtype:  defb 0
rb_i:   defw 0          ; rbv: bytes read
rb_bad: defb 0          ; ... the differences
rb_hdr: defb 0, 0, 0, 0, 0
sizes:  defw 1, 255, 256, 257, 1000, 4096, 8192, 16384
desc:   defb 0D1h       ; save descriptor: type, offset 0, length, source
        defw 0
        defw 0
        defw BUF
        defb 0C9h
scr:    defw 0
slen:   defw 1800h      ; the screen's size (the sim patches it smaller)
sdesc:  defb 0D5h       ; CMD SAVE SCREEN's descriptor: offset 0, length, source
        defw 0
        defw 0
        defw 0
        defb 0C9h
n_ssave: defb 5
        defm "@:PTS"
n_sload: defb 3
        defm "PTS"
n_save: defb 5
        defm "@:PT0"
n_load: defb 3
        defm "PT0"
m_pdos: defm "PDOS"
m_title: defm 13, "PTURBO: load, verify, save; BRK stops. Drive ", 0
m_r:    defm "R ", 0
m_ok:   defm " OK ", 0
m_err:  defm " ERR ", 0
m_stop: defm "stopped", 13, 0
