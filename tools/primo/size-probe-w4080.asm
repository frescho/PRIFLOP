; Phase 3 diagnostic: isolate whether there's a load-size ceiling,
; independent of any content/format question. jetpac.ptp's converted
; JETPAC.PRG (~15KB, screen+code) loads the starting picture partway,
; then nothing -- consistent with a transfer-size or timing limit, not
; a data/format bug (ptp2disk.py's own output already verified correct
; structurally and CRC-checked at the source).
;
; Design: banner-print code at the very front (proven working already,
; hello.asm), then filler out to an exact total size, all one single
; 0xD9 disk data block via deploy.py -- same mechanism as every other
; machine-code test so far, differing only in size. If the whole file
; (all the way to the closing block) doesn't arrive, CDOS never issues
; the autostart CALL, so nothing prints -- a clean pass/fail per size,
; not a partial/ambiguous result.
;
; Filler is 0xAA, a recognisable, non-zero, non-ASCII marker -- visible
; if anyone ever inspects RAM directly, and distinct from the 0x00 CDOS
; itself might use for "untouched" memory.

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
    ret

message:
    defb "SIZE TEST W4080 OK"
    defb 00Dh
    defb 000h

    defs 4038, 0AAh
