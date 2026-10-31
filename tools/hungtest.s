; HUNGTEST: the C64 hangs mid-byte on purpose, for decision 11b (step 13b).
; Asks drive 8 to talk on channel 2, says "ready for data", then never
; acknowledges the byte and holds the bus ~20 s with interrupts off.
; The drive waits in its post-byte wait; it should pet the watchdog
; (tx_long_waits +1) instead of resetting (boots stays).
CHKIN  = $FFC6
CLRCHN = $FFCC
CI2PRA = $DD00          ; bit 5 DATA out (1 pulls low), bit 6 CLOCK in
        .org $C000
        ldx #2
        jsr CHKIN       ; TALK + secondary address, C64 now listener
        sei
wclk:   lda CI2PRA      ; wait for the drive to release CLOCK (ready to send)
        and #$40
        beq wclk
        lda CI2PRA      ; release DATA: ready for data; never pull it again
        and #$DF
        sta CI2PRA
        ldy #60         ; 60 x 256 x 256 x ~5 cycles: ~20 s on PAL
outer:  lda #0
        sta $FB
mid:    ldx #0
inner:  dex
        bne inner
        dec $FB
        bne mid
        dey
        bne outer
        jsr CLRCHN      ; UNTALK under ATN ends the drive's wait
        cli
        rts
