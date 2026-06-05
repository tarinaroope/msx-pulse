#include "boot_capture.h"
#include "ipc.h"   /* RAW_BIT_* */

bool boot_capture_decode_mem(uint32_t raw, boot_bus_event_t *out) {
    bool mreq = !(raw & RAW_BIT_MREQ);
    if (!mreq) return false;
    bool m1 = !(raw & RAW_BIT_M1);
    bool wr = !(raw & RAW_BIT_WR);
    out->addr = (uint16_t)(raw & 0xFFFFu);
    out->data = (uint8_t)((raw >> 16) & 0xFFu);
    out->kind = m1 ? BEV_FETCH : (wr ? BEV_MEM_WRITE : BEV_MEM_READ);
    /* Read data is sampled before cart_serve drives the bus (see
     * boot_detect.pio), so only fetch/write data is trustworthy. */
    out->data_valid = (out->kind != BEV_MEM_READ);
    return true;
}

/* An interrupt-acknowledge cycle (/M1 + /IORQ, /MREQ high) also slips through
 * this gate and looks like a BEV_IO_READ with undefined address bits. We don't
 * filter it: matching one of the ports the detector watches (99/98/AB/A8/AA) by
 * accident is vanishingly unlikely. */
bool boot_capture_decode_io(uint32_t raw, boot_bus_event_t *out) {
    bool iorq = !(raw & RAW_BIT_IORQ);
    if (!iorq) return false;
    bool wr = !(raw & RAW_BIT_WR);
    out->addr = (uint16_t)(raw & 0xFFFFu);
    out->data = (uint8_t)((raw >> 16) & 0xFFu);
    out->kind = wr ? BEV_IO_WRITE : BEV_IO_READ;
    out->data_valid = wr;   /* read data sampled early; writes have settled */
    return true;
}

#ifndef NATIVE_TEST
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "alog.h"
#include "boot_detect.pio.h"   /* bd_mem_capture_program, bd_io_capture_program */

#define BD_PIO        pio0
#define BD_SM_MEM     1u
#define BD_SM_IO      2u
#define BD_RING_BITS  14u                 /* 16 KB ring -> write-addr wrap        */
#define BD_RING_LEN   (1u << (BD_RING_BITS - 2))  /* 4096 u32 entries            */

__attribute__((aligned(1u << BD_RING_BITS))) static volatile uint32_t bd_mem_ring[BD_RING_LEN];
__attribute__((aligned(1u << BD_RING_BITS))) static volatile uint32_t bd_io_ring[BD_RING_LEN];

static uint bd_mem_off, bd_io_off;
static int  bd_mem_dma = -1, bd_io_dma = -1;
static uint32_t bd_mem_rd, bd_io_rd;   /* Core-1 read cursors (total counts)    */
static uint16_t bd_alog_rd;            /* cart-read cursor into alog ring        */

/* Running total of entries DMA has written to each ring, derived from the
 * write-address delta in bd_dma_total(). Reset alongside the read cursors. */
static uint32_t bd_mem_waddr_total, bd_io_waddr_total;
static uint32_t bd_mem_waddr_prev,  bd_io_waddr_prev;

/* Entries dropped by the overrun guard. Only meaningful while draining tightly
 * (see the cadence note below). */
static uint32_t bd_mem_disc, bd_io_disc;

/* Drain-cadence watchdog. The ring only holds ~3.5 ms of traffic (4096 entries
 * at the ~1.15 Mcyc/s a 3.58 MHz Z80 produces). Let the gap between drains grow
 * past that and the DMA laps the ring more than once; bd_dma_total's masked
 * write-address delta then aliases, so events vanish AND the overrun guard goes
 * blind because it's comparing against the aliased total. The write address
 * can't tell us how many whole wraps we missed, so rather than count lost
 * events we just flag the bad cadence and keep the worst gap seen. Both should
 * stay ~0; anything else means the drain loop is starving. */
#define BD_DRAIN_BUDGET_US 2000u   /* roughly ring-fill time at a pessimistic 2 Mcyc/s */
static uint32_t bd_late_drains, bd_max_gap_us;
static uint64_t bd_last_drain_us;

static void bd_arm_dma(int ch, volatile uint32_t *ring, uint sm) {
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_ring(&c, true, BD_RING_BITS);   /* wrap WRITE address     */
    channel_config_set_dreq(&c, pio_get_dreq(BD_PIO, sm, false));
    /* RP2350 quirk: trans_count bits[31:28] are a MODE field, so a raw
     * 0x10000000 sets MODE=TRIGGER_SELF with count 0 and the DMA does nothing.
     * dma_encode_endless_transfer_count() picks MODE=0xF (endless) instead, and
     * we track progress through the write address (bd_dma_total), not the count. */
    dma_channel_configure(ch, &c, (void *)ring, &BD_PIO->rxf[sm],
                          dma_encode_endless_transfer_count(), true);
}

static void bd_start_sm(uint sm, uint off,
                        pio_sm_config (*cfg)(uint)) {
    pio_sm_config c = cfg(off);
    pio_sm_init(BD_PIO, sm, off, &c);
    pio_sm_set_consecutive_pindirs(BD_PIO, sm, 0, 32, false);
    pio_sm_clear_fifos(BD_PIO, sm);
    pio_sm_set_enabled(BD_PIO, sm, true);
}

void boot_capture_init(void) {
    bd_mem_off = pio_add_program(BD_PIO, &bd_mem_capture_program);
    bd_io_off  = pio_add_program(BD_PIO, &bd_io_capture_program);
    bd_mem_dma = dma_claim_unused_channel(true);
    bd_io_dma  = dma_claim_unused_channel(true);
    bd_arm_dma(bd_mem_dma, bd_mem_ring, BD_SM_MEM);
    bd_arm_dma(bd_io_dma,  bd_io_ring,  BD_SM_IO);
    bd_start_sm(BD_SM_MEM, bd_mem_off, bd_mem_capture_program_get_default_config_custom);
    bd_start_sm(BD_SM_IO,  bd_io_off,  bd_io_capture_program_get_default_config_custom);
    bd_mem_rd = bd_io_rd = 0;
    bd_mem_waddr_total = bd_io_waddr_total = 0;
    bd_mem_waddr_prev  = bd_io_waddr_prev  = 0;
    bd_mem_disc = bd_io_disc = 0;
    bd_late_drains = bd_max_gap_us = 0;
    bd_last_drain_us = 0;
    bd_alog_rd = 0;
}

/* Flush and re-arm the capture rings only. Resetting the phase state machine is
 * the caller's job via boot_detect_reset() (msx_runtime_pulse_reset() and the
 * on-device tests do both). Keeping the two split lets a caller re-arm capture
 * without throwing away phases it has already seen. */
void boot_capture_reset(void) {
    if (bd_mem_dma < 0) return;   /* boot_capture_init() never ran — no-op */
    pio_sm_set_enabled(BD_PIO, BD_SM_MEM, false);
    pio_sm_set_enabled(BD_PIO, BD_SM_IO, false);
    dma_channel_abort(bd_mem_dma);
    dma_channel_abort(bd_io_dma);
    pio_sm_clear_fifos(BD_PIO, BD_SM_MEM);
    pio_sm_clear_fifos(BD_PIO, BD_SM_IO);
    pio_sm_restart(BD_PIO, BD_SM_MEM);
    pio_sm_restart(BD_PIO, BD_SM_IO);
    pio_sm_exec(BD_PIO, BD_SM_MEM, pio_encode_jmp(bd_mem_off));
    pio_sm_exec(BD_PIO, BD_SM_IO,  pio_encode_jmp(bd_io_off));
    bd_arm_dma(bd_mem_dma, bd_mem_ring, BD_SM_MEM);
    bd_arm_dma(bd_io_dma,  bd_io_ring,  BD_SM_IO);
    pio_sm_set_enabled(BD_PIO, BD_SM_MEM, true);
    pio_sm_set_enabled(BD_PIO, BD_SM_IO, true);
    bd_mem_rd = bd_io_rd = 0;
    bd_mem_waddr_total = bd_io_waddr_total = 0;
    bd_mem_waddr_prev  = bd_io_waddr_prev  = 0;
    bd_mem_disc = bd_io_disc = 0;
    bd_late_drains = bd_max_gap_us = 0;
    bd_last_drain_us = 0;
    bd_alog_rd = 0;
    /* alog itself is re-armed by msx_reset_pulse() -> alog_reset(). */
}

/* Stop the capture SMs and abort the DMA so the rings stop filling while the
 * dashboard probe has Core 1 between rounds. Does not re-arm (that's
 * boot_capture_reset()), and assumes boot_capture_init() already claimed the
 * DMA channels. */
void boot_capture_stop(void) {
    pio_sm_set_enabled(BD_PIO, BD_SM_MEM, false);
    pio_sm_set_enabled(BD_PIO, BD_SM_IO,  false);
    dma_channel_abort(bd_mem_dma);
    dma_channel_abort(bd_io_dma);
}

/* Monotonic count of entries the DMA has written to a ring, read off the write
 * address. Each call adds the ring-modular delta since last time, which tracks
 * wrap-arounds correctly as long as the DMA hasn't lapped the reader by more
 * than BD_RING_LEN between calls — drain_ring's overrun guard covers that case. */
static uint32_t bd_dma_total(int ch, const volatile uint32_t *ring,
                              uint32_t *total, uint32_t *prev_offset) {
    uint32_t waddr = dma_hw->ch[ch].write_addr;
    uint32_t base  = (uint32_t)(uintptr_t)ring;
    /* write address wraps inside the aligned block, so mask to ring size */
    uint32_t cur_offset = ((waddr - base) & ((1u << BD_RING_BITS) - 1u)) >> 2;
    uint32_t delta = (cur_offset - *prev_offset) & (BD_RING_LEN - 1u);
    *total       += delta;
    *prev_offset  = cur_offset;
    return *total;
}

static uint32_t drain_ring(int ch, volatile uint32_t *ring, uint32_t *rd_cursor,
                           uint32_t *waddr_total, uint32_t *waddr_prev,
                           uint32_t *discarded,
                           bool (*dec)(uint32_t, boot_bus_event_t *)) {
    uint32_t total = bd_dma_total(ch, ring, waddr_total, waddr_prev);
    uint32_t rd = *rd_cursor;
    /* overrun guard: lagged more than a ring? jump to the newest BD_RING_LEN */
    if (total - rd > BD_RING_LEN) {
        *discarded += (total - rd) - BD_RING_LEN;
        rd = total - BD_RING_LEN;
    }
    uint32_t n = 0;
    for (; rd != total; rd++) {
        uint32_t raw = ring[rd & (BD_RING_LEN - 1)];
        boot_bus_event_t e;
        if (dec(raw, &e)) { boot_detect_feed(&e); n++; }
    }
    *rd_cursor = rd;
    return n;
}

uint32_t boot_capture_drain(void) {
    /* Cadence watchdog: flag drains that ran too long after the previous one
     * (data may have been lost to ring wrap). See BD_DRAIN_BUDGET_US note. */
    uint64_t now_us = time_us_64();
    if (bd_last_drain_us) {
        uint32_t gap = (uint32_t)(now_us - bd_last_drain_us);
        if (gap > bd_max_gap_us)        bd_max_gap_us = gap;
        if (gap > BD_DRAIN_BUDGET_US)   bd_late_drains++;
    }
    bd_last_drain_us = now_us;

    uint32_t n = 0;
    n += drain_ring(bd_mem_dma, bd_mem_ring, &bd_mem_rd,
                    &bd_mem_waddr_total, &bd_mem_waddr_prev, &bd_mem_disc,
                    boot_capture_decode_mem);
    n += drain_ring(bd_io_dma,  bd_io_ring,  &bd_io_rd,
                    &bd_io_waddr_total, &bd_io_waddr_prev, &bd_io_disc,
                    boot_capture_decode_io);
    /* phase-8: cart reads with valid data from the alog ring */
    uint16_t cap = alog_captured();
    for (; bd_alog_rd < cap; bd_alog_rd++) {
        uint32_t raw = alog_peek(bd_alog_rd);
        uint16_t a = (uint16_t)(raw & 0xFFFFu);
        if (a == 0x4000 || a == 0x4001) {
            /* alog samples at /RD rising → header data is authoritative. */
            boot_bus_event_t e = { BEV_MEM_READ, a, (uint8_t)((raw >> 16) & 0xFFu), true };
            boot_detect_feed(&e);
            n++;
        }
    }
    return n;
}

void boot_capture_health(uint32_t *discarded, uint32_t *late_drains,
                         uint32_t *max_gap_us) {
    if (discarded)   *discarded   = bd_mem_disc + bd_io_disc;
    if (late_drains) *late_drains = bd_late_drains;
    if (max_gap_us)  *max_gap_us  = bd_max_gap_us;
}
#endif /* NATIVE_TEST */
