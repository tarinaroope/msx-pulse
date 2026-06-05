/* Pure analyzer of the Z80 read transaction stream. Fed {addr,data} sampled on
 * driven reads (/RD low, data valid), so verdicts are high-confidence and the
 * RP2350-E9 floating issue doesn't apply. No pico-sdk; tested in
 * tests/test_bus_integrity.c. The hardware capture lives in txn_capture.c. */
#ifndef BUS_INTEGRITY_H
#define BUS_INTEGRITY_H
#include <stdint.h>
#include <stdbool.h>

/* A V20-reference checkpoint: expected data byte at a known BIOS read address. */
typedef struct { uint16_t addr; uint8_t expected; } bus_checkpoint_t;

/* Number of reference checkpoints (see V20_CHECKPOINTS in bus_integrity.c). */
#define BUS_N_CHECKPOINTS 6u

typedef struct {
    uint8_t  data_seen_hi;   /* OR over reads of data            (bit set = seen 1) */
    uint8_t  data_seen_lo;   /* OR over reads of ~data           (bit set = seen 0) */
    uint16_t addr_seen_hi;   /* OR over reads of addr                               */
    uint16_t addr_seen_lo;   /* OR over reads of ~addr                              */
    uint32_t n_reads;        /* reads fed                                           */
    uint32_t n_allzero;      /* reads where data == 0x00                            */
    uint32_t n_allone;       /* reads where data == 0xFF                            */
    uint8_t  cp_checked;     /* bit k set: checkpoint k's address was read          */
    uint8_t  cp_failed;      /* bit k set: checkpoint k's data mismatched expected  */
} bus_integrity_t;

void bus_integrity_reset(bus_integrity_t *b);
void bus_integrity_feed(bus_integrity_t *b, uint16_t addr, uint8_t data);

/* Derived findings (pure). Stuck checks return 0 until n_reads >= min_reads. */
uint8_t  bus_integrity_stuck_data_hi(const bus_integrity_t *b, uint32_t min_reads);
uint8_t  bus_integrity_stuck_data_lo(const bus_integrity_t *b, uint32_t min_reads);
/* Stuck address bits: never-toggled bits, gated on min_reads AND enough address
 * diversity (≥ min_distinct_bits address bits seen toggling) to avoid the
 * tight-loop false positive. */
uint16_t bus_integrity_stuck_addr(const bus_integrity_t *b, uint32_t min_reads,
                                  uint32_t min_distinct_bits);
/* True if a large fraction of reads returned all-0x00 or all-0xFF (no device
 * driving the bus / contention). Needs n_reads >= min_reads. */
bool     bus_integrity_bus_undriven(const bus_integrity_t *b, uint32_t min_reads);

extern const bus_checkpoint_t V20_CHECKPOINTS[BUS_N_CHECKPOINTS];

#endif /* BUS_INTEGRITY_H */
