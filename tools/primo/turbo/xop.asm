; PRIFLOP Primo turbo -- one exchange with its retries, and the order buffer.
; Shared by phase A's module (turbo.asm) and our own DOS (dos.asm); used by
; order.asm, on top of xfer.asm.

RETRIES equ 8

; x_op: one exchange of the kind in A, frame (nfr), up to RETRIES tries.
; Out: C = failed; else the next frame number is set.
x_op:
    exx                 ; the operation goes to D', which xchg reads
    ld d,a
    exx
    ld b,RETRIES
xr_try:
    push bc
    call xchg
    pop bc
    jr z,xr_ok
    djnz xr_try
    scf
    ret
xr_ok:
    ld hl,nfr
    inc (hl)
    ret                 ; NC (from xchg)

nfr:    defb 0          ; the next frame number
obuf:   defs 4          ; an order: op, address, length
