; PCALL -- a file with 0xCD records, the calls CDOS makes in the middle of a
; load. No program on the drive has one (JETPAC is 0xD5 0xD9 0xC3), so this
; is the file that tests them: CMD LOAD"PCALL", turbo or not.
;
; The file (build_turbo.pcall()) is: this code (0xD9), a call to call1 (0xCD),
; 2 KB of a pattern at DATA (0xD9), a call to call2 (0xCD), 2 KB more (0xD9),
; and an autostart at start (0xC3). Each call counts itself and prints a line;
; start sums the 4 KB and prints PCALL OK only if the sum matches and both
; calls ran, in order. A load that loses a call, runs one twice or garbles the
; data after a call prints PCALL BAD. `result` is 1 OK, 2 bad (for the sims).
;
; The registers matter on the way back. A call gets the load address in DE
; and CDOS stores DE back afterwards (body 0x0932), so a call keeps it. The
; autostart returns into CMD LOAD (body 0x00AA), which takes C as a bus error,
; Z as an error code in A, and otherwise -- unless A = 0xD5 -- stores DE as
; BASIC's program end (0x40F9). The first PCALL returned what PRINTS left
; there, and the next command said OM Error (2026-10-06).

PRINTS  equ 2B75h       ; ROM: print the string at HL, ending 00H
DATA    equ 7400h
SIZE    equ 1000h

    org 7000h

call1:
    ld a,(calls)
    or a
    jr nz,c_wrong       ; call1 must come first
    inc a
    ld (calls),a
    ld hl,m_c1
    jr c_print
call2:
    ld a,(calls)
    cp 1
    jr nz,c_wrong
    inc a
    ld (calls),a
    ld hl,m_c2
c_print:
    push de             ; the load address, which CDOS takes back
    call PRINTS
    pop de
    ret
c_wrong:
    ld a,0FFh
    ld (calls),a
    ret

start:
    ld hl,DATA
    ld bc,SIZE
    ld de,0
sum:
    ld a,(hl)
    add a,e
    ld e,a
    jr nc,sum_nc
    inc d
sum_nc:
    inc hl
    dec bc
    ld a,b
    or c
    jr nz,sum
    ld hl,(want)
    or a
    sbc hl,de
    jr nz,bad
    ld a,(calls)
    cp 2
    jr nz,bad
    ld a,1
    ld (result),a
    ld hl,m_ok
    jr done
bad:
    ld a,2
    ld (result),a
    ld hl,m_bad
done:
    call PRINTS
    ld de,(40F9h)       ; CMD LOAD stores DE as the program end: keep it
    ld a,0D9h           ; the last record type, as a load returns it
    or a                ; NC NZ: no error
    ret

calls:  defb 0
result: defb 0
want:   defw 0          ; the data's sum, patched by the builder
m_c1:   defm "CALL 1", 13, 0
m_c2:   defm "CALL 2", 13, 0
m_ok:   defm "PCALL OK", 13, 0
m_bad:  defm "PCALL BAD", 13, 0
