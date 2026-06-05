#include "rom_lut.h"    /* includes reverse_permute, permute_data inlines */
#include <string.h>

void rom_lut_build(const uint8_t *sram, size_t sram_len, uint8_t lut[65536]) {
    memset(lut, 0xFF, 65536);
    for (uint32_t logical = 0x4000u; logical <= 0x7FFFu; logical++) {
        size_t   src_idx = (size_t)(logical - 0x4000u);
        uint8_t  byte    = (src_idx < sram_len) ? sram[src_idx] : 0xFFu;
        uint16_t raw_idx = reverse_permute((uint16_t)logical);
        lut[raw_idx] = permute_data(byte);
    }
}
