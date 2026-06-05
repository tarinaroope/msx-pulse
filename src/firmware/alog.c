#include "alog.h"
#include "usb_serial.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "cart_serve.pio.h"  /* cart_addr_log_program */

#include <stdio.h>

#define ALOG_PIO  pio0
#define ALOG_SM   0u

__attribute__((aligned(4)))
static volatile uint32_t alog_raw[ALOG_TOTAL];
static uint  alog_pio_offset;
static int   alog_dma_ch = -1;

static void alog_arm_dma(void) {
    dma_channel_config c = dma_channel_get_default_config(alog_dma_ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, true);
    channel_config_set_dreq(&c, pio_get_dreq(ALOG_PIO, ALOG_SM, false));
    dma_channel_configure(alog_dma_ch, &c,
        (void *)alog_raw,
        &ALOG_PIO->rxf[ALOG_SM],
        ALOG_TOTAL,                         /* finite count, decremented (not the endless ring boot_capture uses) */
        true);
}

void alog_init(void) {
    alog_pio_offset = pio_add_program(ALOG_PIO, &cart_addr_log_program);
    pio_sm_config c = cart_addr_log_program_get_default_config_custom(alog_pio_offset);
    pio_sm_init(ALOG_PIO, ALOG_SM, alog_pio_offset, &c);
    pio_sm_set_consecutive_pindirs(ALOG_PIO, ALOG_SM, 0, 32, false);
    pio_sm_clear_fifos(ALOG_PIO, ALOG_SM);

    alog_dma_ch = dma_claim_unused_channel(true);
    alog_arm_dma();
    pio_sm_set_enabled(ALOG_PIO, ALOG_SM, true);
}

void alog_reset(void) {
    pio_sm_set_enabled(ALOG_PIO, ALOG_SM, false);
    dma_channel_abort(alog_dma_ch);
    pio_sm_clear_fifos(ALOG_PIO, ALOG_SM);
    pio_sm_restart(ALOG_PIO, ALOG_SM);
    pio_sm_exec(ALOG_PIO, ALOG_SM, pio_encode_jmp(alog_pio_offset));
    alog_arm_dma();
    pio_sm_set_enabled(ALOG_PIO, ALOG_SM, true);
}

uint16_t alog_captured(void) {
    if (alog_dma_ch < 0) return 0;
    uintptr_t cur  = (uintptr_t)dma_hw->ch[alog_dma_ch].write_addr;
    uintptr_t base = (uintptr_t)alog_raw;
    uint32_t n = (uint32_t)((cur - base) / 4u);
    if (n > ALOG_TOTAL) n = ALOG_TOTAL;
    return (uint16_t)n;
}

uint32_t alog_peek(uint16_t idx) {
    if (idx >= ALOG_TOTAL) return 0;
    return alog_raw[idx];
}

void alog_print_status(usb_out_ring_t *usb_out) {
    char line[USB_OUT_LINE_LEN];
    uint16_t n = alog_captured();
    snprintf(line, sizeof(line),
             "ALOG: %u/%u events captured (%u slots x %u)\r\n",
             (unsigned)n, (unsigned)ALOG_TOTAL,
             (unsigned)ALOG_SLOT_COUNT, (unsigned)ALOG_PER_SLOT);
    usb_serial_write_line(usb_out, line);
}

void alog_print_slot(usb_out_ring_t *usb_out, uint16_t slot) {
    char line[USB_OUT_LINE_LEN];
    if (slot >= ALOG_SLOT_COUNT) {
        snprintf(line, sizeof(line),
                 "ALOG: slot out of range (0-%u)\r\n",
                 (unsigned)(ALOG_SLOT_COUNT - 1));
        usb_serial_write_line(usb_out, line);
        return;
    }
    uint16_t n_now = alog_captured();
    snprintf(line, sizeof(line),
             "ALOG slot %u (count=%u):\r\n",
             (unsigned)slot, (unsigned)n_now);
    usb_serial_write_line(usb_out, line);
    uint16_t base = (uint16_t)(slot * ALOG_PER_SLOT);
    for (uint16_t i = 0; i < ALOG_PER_SLOT; i++) {
        uint16_t idx = (uint16_t)(base + i);
        if (idx >= n_now) {
            snprintf(line, sizeof(line),
                     "  [%3u] -\r\n", (unsigned)idx);
        } else {
            uint32_t raw = alog_raw[idx];
            snprintf(line, sizeof(line),
                     "  [%3u] $%04lX  /RD=%lu /WR=%lu /M1=%lu  raw=$%08lX\r\n",
                     (unsigned)idx,
                     (unsigned long)(raw & 0xFFFFu),
                     (unsigned long)((raw >> 24) & 1u),
                     (unsigned long)((raw >> 25) & 1u),
                     (unsigned long)((raw >> 28) & 1u),
                     (unsigned long)raw);
        }
        usb_serial_write_line(usb_out, line);
    }
}

void alog_print_debug(usb_out_ring_t *usb_out) {
    /* Snapshot PIO0 SM0 + DMA + bus pins to see whether the SM is parked on a
     * wait, autopushing, or the DMA is stuck. RP2350 FDEBUG bits:
     *   TXSTALL[27:24], TXOVER[23:20], RXUNDER[19:16], RXSTALL[31:28].
     * SM0 is bit 0 of each group. */
    char line[USB_OUT_LINE_LEN];
    uint32_t sm_pc      = ALOG_PIO->sm[ALOG_SM].addr;
    uint32_t sm_instr   = ALOG_PIO->sm[ALOG_SM].instr;
    uint32_t fdebug     = ALOG_PIO->fdebug;
    uint32_t flevel     = ALOG_PIO->flevel;
    uint32_t ctrl       = ALOG_PIO->ctrl;
    uint32_t dma_wa     = dma_hw->ch[alog_dma_ch].write_addr;
    uint32_t dma_count  = dma_hw->ch[alog_dma_ch].transfer_count;
    uint32_t dma_ctrl   = dma_hw->ch[alog_dma_ch].ctrl_trig;
    uint32_t gpio_in    = sio_hw->gpio_in;
    uint16_t captured   = alog_captured();
    uint32_t off        = alog_pio_offset;
    bool sm_enabled = (ctrl & (1u << ALOG_SM)) != 0;
    bool rxstall    = (fdebug & (1u << (28 + ALOG_SM))) != 0;
    bool txstall    = (fdebug & (1u << (24 + ALOG_SM))) != 0;
    bool dma_busy   = (dma_ctrl & DMA_CH0_CTRL_TRIG_BUSY_BITS) != 0;

    usb_serial_write_line(usb_out, "ALOG DEBUG:\r\n");
    snprintf(line, sizeof(line),
             "  SM en=%d pc=%lu rel=%ld instr=$%04lX off=%lu\r\n",
             sm_enabled, (unsigned long)sm_pc,
             (long)((long)sm_pc - (long)off),
             (unsigned long)sm_instr, (unsigned long)off);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "  fdebug=$%08lX rxstall=%d txstall=%d flevel=$%08lX\r\n",
             (unsigned long)fdebug, rxstall, txstall,
             (unsigned long)flevel);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "  DMA ch%d busy=%d wa=$%08lX cnt=%lu cap=%u/%u\r\n",
             alog_dma_ch, dma_busy,
             (unsigned long)dma_wa, (unsigned long)dma_count,
             (unsigned)captured, (unsigned)ALOG_TOTAL);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "  gpio=$%08lX /SLTSL=%lu /RD=%lu /M1=%lu\r\n",
             (unsigned long)gpio_in,
             (unsigned long)((gpio_in >> 29) & 1u),
             (unsigned long)((gpio_in >> 24) & 1u),
             (unsigned long)((gpio_in >> 28) & 1u));
    usb_serial_write_line(usb_out, line);
    /* Clear FDEBUG stall bits so the next call shows fresh state. */
    ALOG_PIO->fdebug = fdebug;
}
