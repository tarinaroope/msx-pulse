#pragma once
#include <stdint.h>
#include <stddef.h>

/* Address bus is wired 1:1 (GPn = MSX An, see scripts/gen_wiring.py), so the
 * permute is the identity. Kept as an inline helper to keep the LUT-build site
 * readable and symmetric with permute_data. */
static inline uint16_t reverse_permute(uint16_t a) {
    return a;
}

/* Data bus is wired 1:1 too (GP16+n = MSX Dn), so the GPIO byte is already the
 * data byte. Identity, kept as a helper to mirror reverse_permute. */
static inline uint8_t permute_data(uint8_t b) {
    return b;
}

/* Pure, native-testable. Builds the 64KB raw-bit-order lookup table the
 * ROM-emulator ISR uses to answer /SLTSL reads without runtime bit permutation.
 *
 * lut[raw_bits] = permute_data(sram[logical - 0x4000]) for every logical
 * address in [0x4000, 0x7FFF]; out-of-window entries are 0xFF (a Z80 read of
 * 0xFF is RST 38h, a safe no-op). raw_bits == reverse_permute(logical).
 *
 * sram_len may be shorter than 16 KB; missing tail bytes fill with 0xFF. */
void rom_lut_build(const uint8_t *sram, size_t sram_len, uint8_t lut[65536]);
