#ifndef ALOG_H
#define ALOG_H

#include <stdint.h>
#include "ipc.h"  /* usb_out_ring_t */

/* ALOG: PIO+DMA ring of GP0-31 snapshots taken on /SLTSL falling. PIO0 SM0
 * waits for /SLTSL low, captures GP0-31 in one PIO cycle (~6.7 ns at
 * sys_clk=150 MHz), and pushes to the RX FIFO. DMA drains the FIFO to
 * alog_raw[] (DMA_SIZE_32) until full, then stops. No CPU/ISR latency: the
 * capture moment is fixed by hardware. */

#define ALOG_SLOT_COUNT 50u
#define ALOG_PER_SLOT   5u
#define ALOG_TOTAL      (ALOG_SLOT_COUNT * ALOG_PER_SLOT)

void     alog_init(void);
void     alog_reset(void);
uint16_t alog_captured(void);
/* Read a captured raw word by index (idx < alog_captured()). */
uint32_t alog_peek(uint16_t idx);

/* USB-command printers. Each pushes 1-N lines into `usb_out`. */
void alog_print_status(usb_out_ring_t *usb_out);
void alog_print_slot(usb_out_ring_t *usb_out, uint16_t slot);
void alog_print_debug(usb_out_ring_t *usb_out);

#endif /* ALOG_H */
