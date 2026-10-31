; KEYS: the code of each key pressed, in hex, until BRK -- CMD LOAD"KEYS".
; Step 6g: which codes the cursor keys give on this
; machine. KKBHND (0x0025) waits for a key and returns it in A; DSPHND
; (0x0015) prints A and keeps every register (software booklet, part II ch.3).

    org 07800h

DSPHND  equ 0015h
KKBHND  equ 0025h

start:
    ld hl,title
    call puts
next:
    call KKBHND
    push af
    rrca
    rrca
    rrca
    rrca
    call hex
    pop af
    push af
    call hex
    ld a,' '
    call DSPHND
    pop af
    cp 01h              ; BRK
    jr nz,next
    ld a,0Dh
    jp DSPHND           ; and back to BASIC

hex:
    and 0Fh
    add a,'0'
    cp '9'+1
    jr c,hex1
    add a,7
hex1:
    jp DSPHND

puts:
    ld a,(hl)
    or a
    ret z
    call DSPHND
    inc hl
    jr puts

title:
    defb "KEYS: each key's code in hex, BRK ends", 0Dh, 0
