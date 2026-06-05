    DEVICE NOSLOT64K
    ORG $4000

VDP_CTRL EQU $99

; ROM header (slot scan reads $4000/$4001 = "AB")
    DB  "AB"            ; $4000  cartridge ID
    DW  Init            ; $4002  INIT entry point
    DW  0               ; $4004  STATEMENT (unused)
    DW  0               ; $4006  DEVICE    (unused)
    DW  0               ; $4008  TEXT      (unused)
    DS  6, 0            ; $400A  reserved

; INIT: paint the Screen 0 backdrop green, then halt forever.
;
; The V20 BIOS calls DISSCR (R1 bit 6 = 0) before the slot scan, so the display
; is off when our INIT runs. On the V9918 a disabled display forces black output
; regardless of R7. BIOS would normally call ENASCR after CALSLT returns, but we
; halt forever, so we re-enable the display ourselves.
;
; We do an explicit Screen 0 (40-col text) init via R0/R1/R2/R4/R7. BIOS already
; cleared VRAM, so the name table and pattern-table char 0 are zeroed; every
; text cell then renders as the backdrop colour (R7 low nibble) = green. Solid
; green, no garbage.
Init:
    DI
    in  a, (VDP_CTRL)   ; reset VDP byte flip-flop before any control write

    ld  a, $00          ; R0 = no M3, no ext video, no hsync IRQ
    out (VDP_CTRL), a
    ld  a, $80          ; write to R0
    out (VDP_CTRL), a

    ld  a, $D0          ; R1 = 16K VRAM + display ON + no IRQ + M1=text + no sprites
    out (VDP_CTRL), a
    ld  a, $81          ; write to R1
    out (VDP_CTRL), a

    ld  a, $00          ; R2 = name table base / $400, so name table at $0000
    out (VDP_CTRL), a
    ld  a, $82          ; write to R2
    out (VDP_CTRL), a

    ld  a, $01          ; R4 = pattern table base / $800, so pattern table at $0800
    out (VDP_CTRL), a
    ld  a, $84          ; write to R4
    out (VDP_CTRL), a

    ld  a, $F2          ; R7 = text $F (white), backdrop $2 (med green)
    out (VDP_CTRL), a
    ld  a, $87          ; write to R7
    out (VDP_CTRL), a
.halt:
    halt
    jr  .halt           ; if an IRQ ever slips through, halt again

; pad to 16 KB with $FF
    DS  $8000 - $, $FF

    SAVEBIN "diag.rom", $4000, $4000
