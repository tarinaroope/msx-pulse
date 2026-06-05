/* Hardware capture of Z80 reads with valid data (see the .pio). Drains into the
 * pure bus_integrity analyzer. PIO1 SM0 + one DMA ring. */
#ifndef TXN_CAPTURE_H
#define TXN_CAPTURE_H
#include <stdint.h>
#include "bus_integrity.h"

void     txn_capture_init(void);   /* load PIO prog, claim DMA, arm ring (once) */
void     txn_capture_reset(void);  /* flush ring + re-arm (per pass) */
void     txn_capture_stop(void);   /* disable SM + abort DMA */
/* Decode pending read entries → bus_integrity_feed(bi,...); returns # fed. */
uint32_t txn_capture_drain(bus_integrity_t *bi);

#endif /* TXN_CAPTURE_H */
