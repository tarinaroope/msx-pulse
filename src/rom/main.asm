    DEVICE NOSLOT64K
    ORG $4000

VDP_DATA    EQU $98
VDP_CTRL    EQU $99

PSG_ADDR    EQU $A0     ; write: latch register number
PSG_DATA    EQU $A1     ; write: write to latched register
PSG_READ    EQU $A2     ; read:  read from latched register

SLOT_REG    EQU $A8     ; primary slot select register
KB_COLRD    EQU $A9     ; PPI port B: keyboard column read
KB_ROWSEL   EQU $AA     ; PPI port C: keyboard row select (bits 3:0)

VRAM_NAME   EQU $0000       ; Screen 0 name table (40×24)
VRAM_PTRN   EQU $0800       ; Screen 0 pattern table (256 × 8 bytes)
COLS        EQU 40

RAM_START   EQU $C000       ; first byte of guaranteed MSX1 main RAM
RAM_END     EQU $FFFF       ; last byte

VRAM_TEST_START EQU $1000   ; first VRAM byte safe to test (avoids display area)
VRAM_TEST_END   EQU $3FFF   ; last VRAM byte (12 KB)

    DB  "AB"            ; $4000  cartridge ID
    DW  Init            ; $4002  INIT entry point
    DW  0               ; $4004  STATEMENT (unused)
    DW  0               ; $4006  DEVICE    (unused)
    DW  0               ; $4008  TEXT      (unused)
    DS  6, 0            ; $400A  reserved

Init:
    DI
    ld  sp, $FFFF       ; stack at top of RAM before any testing

    ; Init the VDP directly instead of via BIOS CHGMOD ($005F), so the
    ; diagnostic still shows output if the main BIOS ROM is dead. Sets up
    ; Screen 0 (text, 40x24), loads the font into the pattern table, clears
    ; the name table.
    in  a, (VDP_CTRL)   ; sync VDP byte flip-flop before writes

    ld  a, $00          ; R0: M3=0, no external video, no hsync int
    ld  e, 0
    call VDP_WriteReg
    ld  a, $D0          ; R1: display ON, M1=1 (Text Mode), no frame int
    ld  e, 1
    call VDP_WriteReg
    ld  a, $00          ; R2: name table base = VRAM $0000
    ld  e, 2
    call VDP_WriteReg
    ld  a, $01          ; R4: pattern table base = VRAM $0800
    ld  e, 4
    call VDP_WriteReg
    ld  a, $F4          ; R7: text = white ($F), bg = dark blue ($4)
    ld  e, 7
    call VDP_WriteReg

    call LoadFont       ; copy 96 printable-ASCII glyphs to VRAM $0900+

    call ClearScreen

    ld  hl, Str_Banner
    ld  de, VRAM_NAME + COLS * 1 + 11   ; row 1, centred
    call PrintStr

    ; CPU register / ALU test
    ld  hl, Str_CpuLabel
    ld  de, VRAM_NAME + COLS * 3 + 2
    call PrintStr

    call TestCPU
    ld  hl, Str_Pass
    or  a
    jr  z, .cpu_done
    ld  hl, Str_Fail
.cpu_done:
    ld  de, VRAM_NAME + COLS * 3 + 22
    call PrintStr

    ; RAM test ($C000-$FFFF, 16 KB).
    ; The test loops use only LD/INC/DEC/JP, no CALL, so the stack at $FFFF
    ; stays untouched while testing. CALLs before and after are fine. If the
    ; stack bytes ($FFFD-$FFFE) are themselves bad, a post-test CALL crashes —
    ; accepted, since the fault was already detected.
    ld  hl, Str_RamLabel
    ld  de, VRAM_NAME + COLS * 5 + 2
    call PrintStr

    ; Pass 1: write $AA
    ld  d, $AA
    ld  hl, RAM_START
    ld  bc, RAM_END - RAM_START + 1
.ram_w1:
    ld  (hl), $AA
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .ram_w1

    ; Pass 1: verify $AA
    ld  hl, RAM_START
    ld  bc, RAM_END - RAM_START + 1
.ram_r1:
    ld  a, (hl)
    cp  d
    jp  nz, .ram_fail
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .ram_r1

    ; Pass 2: write $55
    ld  d, $55
    ld  hl, RAM_START
    ld  bc, RAM_END - RAM_START + 1
.ram_w2:
    ld  (hl), $55
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .ram_w2

    ; Pass 2: verify $55
    ld  hl, RAM_START
    ld  bc, RAM_END - RAM_START + 1
.ram_r2:
    ld  a, (hl)
    cp  d
    jp  nz, .ram_fail
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .ram_r2

.ram_pass:
    ld  hl, Str_Pass
    ld  de, VRAM_NAME + COLS * 5 + 22
    call PrintStr
    jr  .ram_done

.ram_fail:
    ; On entry: HL = failing address, D = expected, A = actual value read
    ld  e, a            ; save actual before A gets clobbered

    push hl
    push de
    ld  hl, Str_Fail
    ld  de, VRAM_NAME + COLS * 5 + 22
    call PrintStr
    pop  de
    pop  hl             ; HL = fail addr, D = expected, E = actual

    ; Detail line: "  $XXXX exp:$XX got:$XX"
    push de
    push hl
    ld  hl, VRAM_NAME + COLS * 6 + 2
    call VDP_SetWrite
    pop  hl
    pop  de

    ld  a, ' '
    out (VDP_DATA), a
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    call PrintHex16     ; HL = fail address (preserved by PrintHex16)

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'e'
    out (VDP_DATA), a
    ld  a, 'x'
    out (VDP_DATA), a
    ld  a, 'p'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, d
    call PrintHex8

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'g'
    out (VDP_DATA), a
    ld  a, 'o'
    out (VDP_DATA), a
    ld  a, 't'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, e
    call PrintHex8

.ram_done:

    ; VRAM test ($1000-$3FFF, 12 KB)
    ld  hl, Str_VramLabel
    ld  de, VRAM_NAME + COLS * 7 + 2
    call PrintStr

    ; Pass 1: write $AA
    ld  hl, VRAM_TEST_START
    call VDP_SetWrite
    ld  bc, VRAM_TEST_END - VRAM_TEST_START + 1
.vram_w1:
    ld  a, $AA
    out (VDP_DATA), a
    dec bc
    ld  a, b
    or  c
    jr  nz, .vram_w1

    ; Pass 1: verify $AA
    ld  d, $AA
    ld  hl, VRAM_TEST_START
    call VDP_SetRead
    ld  bc, VRAM_TEST_END - VRAM_TEST_START + 1
.vram_r1:
    in  a, (VDP_DATA)
    cp  d
    jp  nz, .vram_fail
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .vram_r1

    ; Pass 2: write $55
    ld  hl, VRAM_TEST_START
    call VDP_SetWrite
    ld  bc, VRAM_TEST_END - VRAM_TEST_START + 1
.vram_w2:
    ld  a, $55
    out (VDP_DATA), a
    dec bc
    ld  a, b
    or  c
    jr  nz, .vram_w2

    ; Pass 2: verify $55
    ld  d, $55
    ld  hl, VRAM_TEST_START
    call VDP_SetRead
    ld  bc, VRAM_TEST_END - VRAM_TEST_START + 1
.vram_r2:
    in  a, (VDP_DATA)
    cp  d
    jp  nz, .vram_fail
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .vram_r2

.vram_pass:
    ld  hl, Str_Pass
    ld  de, VRAM_NAME + COLS * 7 + 22
    call PrintStr
    jr  .vram_done

.vram_fail:
    ; On entry: HL = failing VRAM address, D = expected, A = actual
    ld  e, a            ; save actual before A gets clobbered

    push hl
    push de
    ld  hl, Str_Fail
    ld  de, VRAM_NAME + COLS * 7 + 22
    call PrintStr
    pop  de
    pop  hl             ; HL = fail addr, D = expected, E = actual

    ; Detail line: "  $XXXX exp:$XX got:$XX"
    push de
    push hl
    ld  hl, VRAM_NAME + COLS * 8 + 2
    call VDP_SetWrite
    pop  hl
    pop  de

    ld  a, ' '
    out (VDP_DATA), a
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    call PrintHex16     ; HL = fail address (preserved by PrintHex16)

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'e'
    out (VDP_DATA), a
    ld  a, 'x'
    out (VDP_DATA), a
    ld  a, 'p'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, d
    call PrintHex8

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'g'
    out (VDP_DATA), a
    ld  a, 'o'
    out (VDP_DATA), a
    ld  a, 't'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, e
    call PrintHex8

.vram_done:

    ; PSG register test (AY-3-8910).
    ; Only R0 is tested ($55 then $AA write+readback) — enough to show the
    ; chip is alive and the latch path ($A0/$A1/$A2) works. R7 (mixer) would
    ; add register-decode coverage, but its bits 7:6 set IOA/IOB direction;
    ; writing junk there breaks the later keyboard scan. Doing it safely needs
    ; read-modify-write, not worth it for v0.1.
    ld  hl, Str_PsgLabel
    ld  de, VRAM_NAME + COLS * 9 + 2
    call PrintStr

    ; Test R0: write $55, read back
    ld  a, 0
    out (PSG_ADDR), a
    ld  a, $55
    out (PSG_DATA), a
    ld  a, 0
    out (PSG_ADDR), a       ; re-latch R0 for read
    in  a, (PSG_READ)
    cp  $55
    ld  b, 0                ; B = register number
    ld  c, $55              ; C = expected
    ld  d, a                ; D = actual
    jp  nz, .psg_fail

    ; Test R0: write $AA, read back
    ld  a, 0
    out (PSG_ADDR), a
    ld  a, $AA
    out (PSG_DATA), a
    ld  a, 0
    out (PSG_ADDR), a
    in  a, (PSG_READ)
    cp  $AA
    ld  b, 0
    ld  c, $AA
    ld  d, a
    jp  nz, .psg_fail

    ; Restore R0 = 0
    ld  a, 0
    out (PSG_ADDR), a
    xor a
    out (PSG_DATA), a

.psg_pass:
    ld  hl, Str_Pass
    ld  de, VRAM_NAME + COLS * 9 + 22
    call PrintStr
    jr  .psg_done

.psg_fail:
    ; B = register number, C = expected, D = actual.
    ; The print routines don't touch B/C/D, so they survive across the prints.
    ld  hl, Str_Fail
    ld  de, VRAM_NAME + COLS * 9 + 22
    call PrintStr

    ; Detail line: "  R00 exp:$XX got:$XX"
    ld  hl, VRAM_NAME + COLS * 10 + 2
    call VDP_SetWrite

    ld  a, ' '
    out (VDP_DATA), a
    out (VDP_DATA), a
    ld  a, 'R'
    out (VDP_DATA), a
    ld  a, b
    call PrintHex8

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'e'
    out (VDP_DATA), a
    ld  a, 'x'
    out (VDP_DATA), a
    ld  a, 'p'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, c
    call PrintHex8

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'g'
    out (VDP_DATA), a
    ld  a, 'o'
    out (VDP_DATA), a
    ld  a, 't'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, d
    call PrintHex8

.psg_done:

    ; Slot register ($A8)
    ld  hl, Str_SlotLabel
    ld  de, VRAM_NAME + COLS * 12 + 2
    call PrintStr

    in  a, (SLOT_REG)
    ld  b, a                ; B = slot register value (preserved across all prints)

    ; Raw hex value at col 22
    ld  hl, VRAM_NAME + COLS * 12 + 22
    call VDP_SetWrite
    ld  a, '$'
    out (VDP_DATA), a
    ld  a, b
    call PrintHex8

    ; Decoded page map on row 13: "P0:x P1:x P2:x P3:x"
    ld  hl, VRAM_NAME + COLS * 13 + 2
    call VDP_SetWrite

    ld  a, 'P'
    out (VDP_DATA), a
    ld  a, '0'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, b
    and $03                 ; bits 1:0 = page 0 slot
    add a, '0'
    out (VDP_DATA), a

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'P'
    out (VDP_DATA), a
    ld  a, '1'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, b
    rrca
    rrca
    and $03                 ; bits 3:2 = page 1 slot
    add a, '0'
    out (VDP_DATA), a

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'P'
    out (VDP_DATA), a
    ld  a, '2'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, b
    rrca
    rrca
    rrca
    rrca
    and $03                 ; bits 5:4 = page 2 slot
    add a, '0'
    out (VDP_DATA), a

    ld  a, ' '
    out (VDP_DATA), a
    ld  a, 'P'
    out (VDP_DATA), a
    ld  a, '3'
    out (VDP_DATA), a
    ld  a, ':'
    out (VDP_DATA), a
    ld  a, b
    rrca
    rrca
    rrca
    rrca
    rrca
    rrca
    and $03                 ; bits 7:6 = page 3 slot
    add a, '0'
    out (VDP_DATA), a

    ; Keyboard matrix
    ld  hl, Str_KbLabel
    ld  de, VRAM_NAME + COLS * 15 + 2
    call PrintStr

    ld  hl, VRAM_NAME + COLS * 16 + 2
    call VDP_SetWrite       ; VRAM position for raw row data

    ld  c, 0                ; C = matrix row counter (0-10)
.kb_loop:
    ; select row C in PPI port C bits 3:0, keep bits 7:4
    in  a, (KB_ROWSEL)
    and $F0
    or  c
    out (KB_ROWSEL), a

    in  a, (KB_COLRD)
    call PrintHex8          ; A = nibble char after this

    ld  a, c
    cp  10
    jr  z, .kb_last         ; no space after last row
    ld  a, ' '
    out (VDP_DATA), a
.kb_last:
    inc c
    ld  a, c
    cp  11
    jr  nz, .kb_loop

    ld  hl, Str_KbRead
    ld  de, VRAM_NAME + COLS * 15 + 22
    call PrintStr
.kb_done:

Halt:
    halt
    jr  Halt

Str_Banner:   DB "=== MSXDOC v0.1 ===", 0    ; 19 chars
Str_CpuLabel: DB "CPU ALU/REGS........", 0    ; 20 chars
Str_RamLabel: DB "RAM $C000-$FFFF.....", 0    ; 20 chars
Str_Pass:     DB "PASS", 0
Str_Fail:     DB "FAIL", 0
Str_VramLabel: DB "VRAM $1000-$3FFF....", 0  ; 20 chars
Str_PsgLabel:  DB "PSG AY-3-8910.......", 0  ; 20 chars
Str_SlotLabel: DB "SLOT REGISTER $A8...", 0  ; 20 chars
Str_KbLabel:   DB "KB MATRIX $A9/$AA...", 0  ; 20 chars
Str_KbRead:    DB "READ", 0

; VDP_WriteReg: write A to VDP register E (0..7). Clobbers A. Byte flip-flop
; must already be synced.
VDP_WriteReg:
    out (VDP_CTRL), a
    ld  a, e
    or  $80
    out (VDP_CTRL), a
    ret

; LoadFont: write 96 x 8 bytes of row-major glyph data to the pattern table
; starting at ASCII $20 (VRAM $0800 + $20*8 = $0900). Font comes from the
; embedded font.bin at the bottom of the file.
LoadFont:
    ld  hl, VRAM_PTRN + $20 * 8
    call VDP_SetWrite
    ld  hl, Font
    ld  bc, 96 * 8
.loop:
    ld  a, (hl)
    out (VDP_DATA), a
    inc hl
    dec bc
    ld  a, b
    or  c
    jr  nz, .loop
    ret

; VDP_SetWrite: set VRAM write address from HL.
VDP_SetWrite:
    ld  a, l
    out (VDP_CTRL), a
    ld  a, h
    or  $40
    out (VDP_CTRL), a
    ret

; VDP_SetRead: set VRAM read address from HL.
VDP_SetRead:
    ld  a, l
    out (VDP_CTRL), a
    ld  a, h
    and $3F
    out (VDP_CTRL), a
    ret

; ClearScreen: fill name table (40x24) with spaces.
ClearScreen:
    ld  hl, VRAM_NAME
    call VDP_SetWrite
    ld  bc, COLS * 24
.loop:
    ld  a, ' '
    out (VDP_DATA), a
    dec bc
    ld  a, b
    or  c
    jr  nz, .loop
    ret

; PrintStr: write null-terminated string to VRAM.
; HL = ROM string pointer, DE = VRAM destination.
PrintStr:
    push hl
    ld  h, d
    ld  l, e
    call VDP_SetWrite
    pop hl
.loop:
    ld  a, (hl)
    or  a
    ret z
    out (VDP_DATA), a
    inc hl
    jr  .loop

; PrintHex16: print HL as 4 hex chars at the current VRAM position. Preserves HL.
PrintHex16:
    push hl
    ld  a, h
    call PrintHex8
    ld  a, l
    call PrintHex8
    pop  hl
    ret

; PrintHex8: print A as 2 hex chars at the current VRAM position.
PrintHex8:
    push af
    rrca
    rrca
    rrca
    rrca
    call PrintNibble    ; high nibble
    pop  af
    ; fall through to PrintNibble for the low nibble

; PrintNibble: print low nibble of A as one hex char.
PrintNibble:
    and  $0F
    add  a, '0'
    cp   '9' + 1
    jr   c, .ok
    add  a, 7           ; bump to 'A'-'F'  ('A'-'0'-10 = 7)
.ok:
    out  (VDP_DATA), a
    ret

; TestCPU: Z80 ALU and register tests, no RAM required.
; Returns A=0 on pass, A=test number on first failure.
TestCPU:
    ; 1. 8-bit addition
    ld  a, $3C
    add a, $3C          ; $78
    cp  $78
    ld  a, 1
    ret nz

    ; 2. subtraction goes negative, so sign flag (M) should be set
    ld  a, $50
    sub $51             ; $FF, negative
    jp  p, .fail        ; positive means broken

    ; 3. Zero flag
    ld  a, $42
    sub $42
    ld  a, 3
    ret nz

    ; 4. AND
    ld  a, $F0
    and $0F             ; $00
    ld  a, 4
    ret nz

    ; 5. OR
    ld  a, $F0
    or  $0F             ; $FF
    cp  $FF
    ld  a, 5
    ret nz

    ; 6. XOR
    ld  a, $FF
    xor $AA             ; $55
    cp  $55
    ld  a, 6
    ret nz

    ; 7. 16-bit INC across byte boundary
    ld  hl, $12FF
    inc hl              ; $1300
    ld  a, h
    cp  $13
    ld  a, 7
    ret nz
    ld  a, l
    or  a
    ld  a, 7
    ret nz

    ; 8. CPL
    ld  a, $A5
    cpl                 ; $5A
    cp  $5A
    ld  a, 8
    ret nz

    ; 9. RLCA: rotate left, bit 7 into carry
    ld  a, $80
    rlca                ; A=$01, CF=1
    cp  $01
    ld  a, 9
    ret nz

    ; 10. RLA: rotate left through carry
    scf
    ld  a, $40
    rla                 ; A=$81
    cp  $81
    ld  a, 10
    ret nz

    ; 11. EX AF,AF'
    ld  a, $42
    ex  af, af'
    ld  a, $FF
    ex  af, af'
    cp  $42
    ld  a, 11
    ret nz

    ; 12. EXX
    ld  bc, $1234
    exx
    ld  bc, $ABCD
    exx
    ld  a, b
    cp  $12
    ld  a, 12
    ret nz
    ld  a, c
    cp  $34
    ld  a, 12
    ret nz

    xor a
    ret

.fail:
    ld  a, 1
    ret

; Embedded font: 96 x 8 bytes (768), row-major 6x8.
; Generated by scripts/gen_msx_font.py from src/firmware/oled.c font5x7[].
Font:
    INCBIN "font.bin"

    SAVEBIN "diag.rom", $4000, $4000
