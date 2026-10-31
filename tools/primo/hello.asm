; Phase 2 gate test: assemble here, deploy in one step, load and run on
; the real Primo, prove the CMD LOAD auto-run pipeline end to end.
;
; Load address 0x7800 -- chosen to sit clear of both CDOS's boot-time
; footprint (0x4500-0x53F8) and
;
; the low end of RAM where BASIC's own program/variables live. Confirmed
; safe on real hardware, 2026-09-07.
;
; A direct `CMD LOAD"HELLO"` at the READY prompt runs this automatically
; -- but not from a bare load-address header the way it looks like it
; should. deploy.py wraps this assembled code in the real block-tagged
; .PRI/.PRG format (data block + a 0xC3 closing block that CALLs the
; start address); that 0xC3 call is what actually triggers autostart.
; Getting this wrong the first time produced "SN Error in 32376" on real
; hardware -- see deploy.py's docstring for
; the full account.
;
; Character output is Primo's own documented ROM entry point (Sec.8.1,
; primobelsoleiras84_1.txt, "Display X'0015'"): CALL 0x0015 with the
; character in A, cursor position and screen control codes handled by
; the ROM; "minden regisztert ment" -- it preserves every register, so
; HL survives the call untouched.

    org 07800h

start:
    ld hl,message
print_loop:
    ld a,(hl)
    or a
    jr z,done
    call 00015h
    inc hl
    jr print_loop
done:
    ret                 ; hands control back to BASIC's READY prompt

message:
    defb "HELLO FROM THE PRIFLOP DEV LOOP"
    defb 00Dh           ; CR -- move to the next line
    defb 000h           ; our own loop's terminator, not sent to Display
