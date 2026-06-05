#include "bus_integrity.h"

/* Canon V20 reference checkpoints: the first BIOS fetches (docs/V20_BOOT_SEQUENCE.md).
 *   $0000 DI (F3) ; $0001 JP $02D7 (C3 D7 02) ; $02D7 LD A,$82 (3E 82).
 * These get read in the first microseconds of every boot, so they're a reliable
 * baseline. A stuck data bit or bad ROM corrupts them, and cp_failed then names
 * the checkpoint that didn't match. */
const bus_checkpoint_t V20_CHECKPOINTS[BUS_N_CHECKPOINTS] = {
    { 0x0000, 0xF3 },  /* DI                 */
    { 0x0001, 0xC3 },  /* JP                 */
    { 0x0002, 0xD7 },  /* $02D7 low byte     */
    { 0x0003, 0x02 },  /* $02D7 high byte    */
    { 0x02D7, 0x3E },  /* LD A,n             */
    { 0x02D8, 0x82 },  /* PPI mode word $82  */
};

void bus_integrity_reset(bus_integrity_t *b) {
    b->data_seen_hi = b->data_seen_lo = 0;
    b->addr_seen_hi = b->addr_seen_lo = 0;
    b->n_reads = b->n_allzero = b->n_allone = 0;
    b->cp_checked = b->cp_failed = 0;
}

void bus_integrity_feed(bus_integrity_t *b, uint16_t addr, uint8_t data) {
    b->n_reads++;
    b->data_seen_hi |= data;
    b->data_seen_lo |= (uint8_t)~data;
    b->addr_seen_hi |= addr;
    b->addr_seen_lo |= (uint16_t)~addr;
    if (data == 0x00u) b->n_allzero++;
    if (data == 0xFFu) b->n_allone++;
    for (uint32_t k = 0; k < BUS_N_CHECKPOINTS; k++) {
        if (V20_CHECKPOINTS[k].addr == addr) {
            b->cp_checked |= (uint8_t)(1u << k);
            if (data != V20_CHECKPOINTS[k].expected)
                b->cp_failed |= (uint8_t)(1u << k);
        }
    }
}

uint8_t bus_integrity_stuck_data_hi(const bus_integrity_t *b, uint32_t min_reads) {
    if (b->n_reads < min_reads) return 0;
    return (uint8_t)(b->data_seen_hi & (uint8_t)~b->data_seen_lo);  /* seen 1, never 0 */
}

uint8_t bus_integrity_stuck_data_lo(const bus_integrity_t *b, uint32_t min_reads) {
    if (b->n_reads < min_reads) return 0;
    return (uint8_t)(b->data_seen_lo & (uint8_t)~b->data_seen_hi);  /* seen 0, never 1 */
}

static uint32_t popcount16(uint16_t v) {
    uint32_t c = 0; while (v) { c += v & 1u; v >>= 1; } return c;
}

uint16_t bus_integrity_stuck_addr(const bus_integrity_t *b, uint32_t min_reads,
                                  uint32_t min_distinct_bits) {
    if (b->n_reads < min_reads) return 0;
    uint16_t toggled = (uint16_t)(b->addr_seen_hi & b->addr_seen_lo);
    if (popcount16(toggled) < min_distinct_bits) return 0;   /* tight loop → suppress */
    /* a bit that was seen high or low but never toggled, within the populated range */
    uint16_t seen = (uint16_t)(b->addr_seen_hi | b->addr_seen_lo);
    return (uint16_t)(seen & (uint16_t)~toggled);
}

bool bus_integrity_bus_undriven(const bus_integrity_t *b, uint32_t min_reads) {
    if (b->n_reads < min_reads) return false;
    /* >50% of reads were all-0 or all-1 → nothing driving / contention */
    uint32_t bad = b->n_allzero + b->n_allone;
    return bad * 2u > b->n_reads;
}
