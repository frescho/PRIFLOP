; The turbo installer's core, shared by install.asm (TURBO.PRG, CMD LOAD
; after a stock CDOS boot) and boot.asm (inside the boot file). Straight-line
; code, not a subroutine: CLEAR moves the stack, so whoever includes it ends
; in READY instead of returning.
;
; It places the module directly under CDOS -- lowering BASIC's top and doing
; CLEAR 50, or reusing the room an earlier boot left -- copies and relocates
; it, gives it CDOS's own output port (body 0x06DB), keeps CDOS's entries
; 0x0E, 0x10, 0x12 in it and points them at it. IX is kept.
;
; The includer defines CDOSV, MEMTOP, CLEAR, `mbase` (a word) and `image`
; (the module image, then its relocation list ended by 0xFFFF), and the
; equates SIZE, OUTPORT_OFS, OLDTAB_OFS (turbo_syms.inc).

    ld hl,(MEMTOP)      ; is a module of ours right above the top already,
    inc hl              ; from an earlier boot? A reboot with everything
    ld (mbase),hl       ; resident reinstalls CDOS in place and keeps the
    ld a,(image)        ; top, so lowering it
    cp (hl)             ; again stacks one more module on every Reset+BRK.
    jr nz,ic_new        ; Recognised by its first instruction and operand,
    inc hl              ; relocated to where it sits (offset 1 is always a
    ld e,(hl)           ; relocation, build_turbo.py checks). Not "the top
    inc hl              ; is one module below CDOS": once the top was
    ld d,(hl)           ; anywhere else, that stacked a module per boot
    ld hl,(image + 1)   ; for good (the real Primo, 2026-10-06:
    ld bc,(mbase)       ; 55795 -> 53998 -> 53399).
    add hl,bc
    or a
    sbc hl,de
    jr z,ic_copy

ic_new:

    ld hl,(MEMTOP)      ; the module goes right under the top
    ld de,SIZE - 1
    or a
    sbc hl,de
    ld (mbase),hl
    dec hl              ; BASIC ends below it. CLEAR first, as CDOS's own
    ld (MEMTOP),hl      ; loader does: it moves BASIC's stack and strings
    ld de,50            ; below the new top. During CMD LOAD the stack lies
    call CLEAR          ; just under CDOS, where the module goes; copying
                        ; first wrecked it (2026-10-05, the real Primo).
ic_copy:
    ld de,(mbase)
    ld hl,image
    ld bc,SIZE
    ldir

    push ix             ; IX is BASIC's system-variable base (0x4042): the
    ld ix,image + SIZE  ; display routine needs it back (2026-10-05, MAME)
ic_reloc:
    ld l,(ix+0)
    ld h,(ix+1)
    ld a,h
    and l
    inc a
    jr z,ic_port          ; 0xFFFF: the end
    ld de,(mbase)
    add hl,de           ; the word to adjust
    ld a,(hl)
    add a,e
    ld (hl),a
    inc hl
    ld a,(hl)
    adc a,d
    ld (hl),a
    inc ix
    inc ix
    jr ic_reloc

ic_port:
    ld hl,(CDOSV)
    ld bc,06DBh         ; CDOS's own OUT (n),A operand
    add hl,bc
    ld a,(hl)
    ld hl,(mbase)
    ld bc,OUTPORT_OFS
    add hl,bc
    ld (hl),a

    ld hl,(CDOSV)       ; entries 0x0E, 0x10, 0x12: keep, then replace
    ld bc,0B42h + 0Eh
    add hl,bc
    push hl
    ld de,(mbase)
    ex de,hl
    ld bc,OLDTAB_OFS
    add hl,bc
    ex de,hl
    ld bc,6
    ldir
    pop hl
    ld de,(mbase)       ; SVC is at offset 0
    ld b,3
ic_hook:
    ld (hl),e
    inc hl
    ld (hl),d
    inc hl
    djnz ic_hook

    pop ix
