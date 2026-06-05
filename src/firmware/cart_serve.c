#include "cart_serve.h"
#include "rom_lut.h"            /* rom_lut_build */
#include <string.h>

#ifndef NATIVE_TEST
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/clocks.h"
#include "hardware/structs/bus_ctrl.h"
#include "cart_serve.pio.h"
#endif

#define GP_LVC_OEn  35   /* All four 74LVC245 /OE; LOW = enabled */
#define GP_LVC_DIR  34   /* LVC_DIR: HIGH=MSX→MCU, LOW=MCU→MSX */
#define GP_WAIT     30   /* /WAIT bus pin (active LOW), direct drive via R23 */

#ifndef NATIVE_TEST
/* Two SMs, one per PIO. */
#define ADDR_PIO    pio1
#define SM_ADDR     0u   /* cart_addr_read: pushes 32-bit LUT pointer */
#define DATA_PIO    pio2
#define SM_READ     0u   /* cart_read: full read-cycle responder */
#endif

/* ROM image in logical order, for the DUMP USB command. */
__attribute__((aligned(16384)))
static uint8_t rom_sram[16384];

/* Pre-permuted lookup table indexed by raw GP[15:0] bits. 64K-aligned so
 * the PIO+DMA chain can compute (LUT_BASE_HIGH<<16)|raw_bits as a single
 * 32-bit pointer. */
__attribute__((aligned(65536)))
static uint8_t rom_lut[65536];

#ifndef NATIVE_TEST
static uint addr_prog_offset, read_prog_offset;
static int  dma_addr_fwd_ch, dma_data_fetch_ch;

/* Arm both DMA channels for one cycle of the lock-step chain, using the
 * already-claimed channel indices. Called from dma_chain_configure() at init
 * and from cart_serve_reset_pipeline() after an abort. */
static void dma_chain_arm(void) {
    /* cart_addr_read pushes exactly one 32-bit LUT pointer per cart read cycle
     * (gated on /SLTSL+/RD in PIO). Ch A copies that pointer to Ch B's
     * READ_ADDR_TRIG alias, which sets Ch B's read_addr and fires Ch B for one
     * byte into cart_read's TX FIFO. cart_read's `pull block` consumes the byte
     * and drives D0-D7.
     *
     * No ENDLESS mode and no DREQ-paced burst-fill: the chain is self-clocked by
     * /SLTSL+/RD events, so there's no stale-byte pipeline at startup and no way
     * for byte and address to skew. */
    {
        /* Ch B (data fetch): 1 byte from *(read_addr) into the PIO2 SM0 TX FIFO.
         * trans_count=1 with chain_to self re-arms it after each fire. trigger
         * is false here; Ch A's first write to al3_read_addr_trig starts it.
         * DREQ pacing on SM_READ TX-not-full keeps Ch B off a full TX FIFO. */
        dma_channel_config c = dma_channel_get_default_config(dma_data_fetch_ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(DATA_PIO, SM_READ, true));
        channel_config_set_chain_to(&c, dma_data_fetch_ch);  /* self → no chain */
        dma_channel_configure(dma_data_fetch_ch, &c,
            &DATA_PIO->txf[SM_READ],          /* dst: cart_read TX FIFO */
            rom_lut,                          /* src: overwritten per fire */
            1u,
            false);
    }

    /* Ch A (addr fwd): 32-bit pointer from PIO1 SM0 RX into Ch B's
     * READ_ADDR_TRIG. Writing AL3 (not AL2) triggers Ch B together with the
     * read_addr update, so Ch B fires exactly once per Ch A transfer. */
    {
        dma_channel_config c = dma_channel_get_default_config(dma_addr_fwd_ch);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        channel_config_set_dreq(&c, pio_get_dreq(ADDR_PIO, SM_ADDR, false));
        channel_config_set_chain_to(&c, dma_addr_fwd_ch);    /* self → no chain */
        dma_channel_configure(dma_addr_fwd_ch, &c,
            &dma_hw->ch[dma_data_fetch_ch].al3_read_addr_trig, /* dst: Ch B READ_ADDR + trigger */
            &ADDR_PIO->rxf[SM_ADDR],
            0xFFFFFFFFu,                                        /* ENDLESS */
            true);                                              /* starts on PIO1 RX DREQ */
    }

    /* Give DMA top read and write priority on the bus. */
    bus_ctrl_hw->priority = BUSCTRL_BUS_PRIORITY_DMA_R_BITS
                          | BUSCTRL_BUS_PRIORITY_DMA_W_BITS;
}

static void dma_chain_configure(void) {
    dma_addr_fwd_ch   = dma_claim_unused_channel(true);
    dma_data_fetch_ch = dma_claim_unused_channel(true);
    hard_assert(dma_addr_fwd_ch < dma_data_fetch_ch);   /* AHB priority order */
    dma_chain_arm();
}
#endif /* NATIVE_TEST */

void bus_buffers_enable(void) {
#ifndef NATIVE_TEST
    gpio_init(GP_LVC_OEn);
    gpio_set_dir(GP_LVC_OEn, GPIO_OUT);
    gpio_put(GP_LVC_OEn, 0);
#endif
}

void cart_serve_init(const uint8_t *rom_image, size_t rom_size, usb_out_ring_t *out) {
    (void)out;  /* reserved for diagnostic prints; not used in v1 */

    /* Copy the ROM image into logical-order SRAM for DUMP, pad the rest 0xFF. */
    size_t copy_len = (rom_size < sizeof(rom_sram)) ? rom_size : sizeof(rom_sram);
    if (rom_image && copy_len)
        memcpy(rom_sram, rom_image, copy_len);
    if (copy_len < sizeof(rom_sram))
        memset(rom_sram + copy_len, 0xFF, sizeof(rom_sram) - copy_len);

    rom_lut_build(rom_sram, sizeof(rom_sram), rom_lut);

#ifndef NATIVE_TEST
    /* The chain indexes rom_lut as (HIGH << 16) | raw_bits, so the base must be
     * 64K-aligned. aligned(65536) should guarantee it; assert as a tripwire. */
    hard_assert(((uintptr_t)rom_lut & 0xFFFFu) == 0u);

    /* Per-PIO GPIOBASE. PIO1 covers GP0-31; PIO2 covers GP16-47. */
    pio_set_gpio_base(ADDR_PIO, 0);
    pio_set_gpio_base(DATA_PIO, 16);

    addr_prog_offset = pio_add_program(ADDR_PIO, &cart_addr_read_program);
    read_prog_offset = pio_add_program(DATA_PIO, &cart_read_program);

    /* Address reader SM (PIO1 SM0): IN pins GP0-15, no OUT, no side-set.
     * Pre-load the TX FIFO with the LUT high half so the one-shot prologue
     * (`pull block; mov x, osr`) can latch it into X for the SM's lifetime. */
    {
        pio_sm_config c = cart_addr_read_program_get_default_config_custom(addr_prog_offset);
        sm_config_set_clkdiv(&c, 1.0f);   /* run at sys_clk */
        pio_sm_init(ADDR_PIO, SM_ADDR, addr_prog_offset, &c);
        pio_sm_set_consecutive_pindirs(ADDR_PIO, SM_ADDR, 0, 16, false);  /* inputs */
        pio_sm_clear_fifos(ADDR_PIO, SM_ADDR);
        pio_sm_put(ADDR_PIO, SM_ADDR, (uint32_t)((uintptr_t)rom_lut >> 16));
    }

    /* cart_read SM (PIO2 SM0): owns the full Z80 read cycle. Drives GP16-23
     * (data, pindirs flip on /SLTSL+/RD), GP30 (/WAIT side-set) and GP34
     * (LVC_DIR set pin). Hands GPIO ownership from SIO to PIO for every pin
     * it writes. */
    {
        for (int i = 16; i <= 23; i++) pio_gpio_init(DATA_PIO, i);
        pio_gpio_init(DATA_PIO, GP_WAIT);
        pio_gpio_init(DATA_PIO, GP_LVC_DIR);

        pio_sm_config c = cart_read_program_get_default_config_custom(read_prog_offset);
        sm_config_set_clkdiv(&c, 1.0f);
        int rc = pio_sm_set_config(DATA_PIO, SM_READ, &c);
        hard_assert(rc == PICO_OK);
        pio_sm_clear_fifos(DATA_PIO, SM_READ);
        pio_sm_restart(DATA_PIO, SM_READ);
        pio_sm_exec(DATA_PIO, SM_READ, pio_encode_jmp(read_prog_offset));

        /* Drive GP30 (/WAIT) and GP34 (LVC_DIR) HIGH before flipping their
         * pindirs to output. The pad reset state is 0, so setting pindir first
         * would briefly assert /WAIT LOW and put the LVC into drive-toward-MSX
         * mode at boot. Pad output HIGH, then pindir OUT, leaves both idle HIGH
         * cleanly. */
        pio_sm_set_pins_with_mask64(DATA_PIO, SM_READ,
                                    ((uint64_t)1u << GP_WAIT) | ((uint64_t)1u << GP_LVC_DIR),
                                    ((uint64_t)1u << GP_WAIT) | ((uint64_t)1u << GP_LVC_DIR));

        /* GP16-23 idle as inputs (the LVC drives them when DIR=HIGH); the SM
         * flips them to output for the duration of each read cycle. */
        pio_sm_set_consecutive_pindirs(DATA_PIO, SM_READ, 16, 8, false);
        pio_sm_set_consecutive_pindirs(DATA_PIO, SM_READ, GP_WAIT, 1, true);
        pio_sm_set_consecutive_pindirs(DATA_PIO, SM_READ, GP_LVC_DIR, 1, true);
    }

    dma_chain_configure();

    /* GP_LVC_OEn idles HIGH; bus_buffers_enable() pulls it LOW. */
    gpio_init(GP_LVC_OEn);
    gpio_set_dir(GP_LVC_OEn, GPIO_OUT);
    gpio_put(GP_LVC_OEn, 1);
#endif /* NATIVE_TEST */
}

void cart_serve_arm(void) {
#ifndef NATIVE_TEST
    /* Enable cart_read first: it stalls at `wait 0 gpio 29` holding /WAIT and
     * LVC_DIR HIGH via side-set/set. Then cart_addr_read, which starts pushing
     * LUT pointers (paced by the already-armed Ch A). The DMA chain delivers a
     * byte to cart_read's TX FIFO when /SLTSL+/RD fires and the SM reaches
     * `pull block`. */
    pio_sm_set_enabled(DATA_PIO, SM_READ, true);
    pio_sm_set_enabled(ADDR_PIO, SM_ADDR, true);
#endif
}

void cart_serve_disable_drive(void) {
#ifndef NATIVE_TEST
    pio_sm_set_enabled(ADDR_PIO, SM_ADDR, false);
    pio_sm_set_enabled(DATA_PIO, SM_READ, false);

    dma_channel_abort(dma_addr_fwd_ch);
    dma_channel_abort(dma_data_fetch_ch);

    /* GP34 back to SIO, driven HIGH (receive). */
    gpio_set_function(GP_LVC_DIR, GPIO_FUNC_SIO);
    gpio_set_dir(GP_LVC_DIR, GPIO_OUT);
    gpio_put(GP_LVC_DIR, 1);

    /* GP16-23 back to SIO inputs (pad output drivers off). */
    for (int i = 16; i <= 23; i++) {
        gpio_set_function(i, GPIO_FUNC_SIO);
        gpio_set_dir(i, GPIO_IN);
    }

    /* GP30 (/WAIT) back to SIO input; the MSX bus pull-up holds it HIGH and
     * R23 protects against 5V exposure. */
    gpio_set_function(GP_WAIT, GPIO_FUNC_SIO);
    gpio_set_dir(GP_WAIT, GPIO_IN);
#endif
}

void cart_serve_reset_pipeline(void) {
#ifndef NATIVE_TEST
    /* Stop both SMs so nothing pushes or pulls during the reset. */
    pio_sm_set_enabled(ADDR_PIO, SM_ADDR, false);
    pio_sm_set_enabled(DATA_PIO, SM_READ, false);

    dma_channel_abort(dma_addr_fwd_ch);
    dma_channel_abort(dma_data_fetch_ch);

    /* Clear the PIO FIFOs to flush bytes left over from the previous MSX boot.
     * That's the whole point of this function. */
    pio_sm_clear_fifos(ADDR_PIO, SM_ADDR);
    pio_sm_clear_fifos(DATA_PIO, SM_READ);

    /* Restart the SMs (clears OSR/ISR/shift counters) and jump to the program
     * entry so the cart_addr_read prologue (`pull block; mov x, osr`) runs again
     * and re-latches LUT_HIGH. */
    pio_sm_restart(ADDR_PIO, SM_ADDR);
    pio_sm_restart(DATA_PIO, SM_READ);
    pio_sm_exec(ADDR_PIO, SM_ADDR, pio_encode_jmp(addr_prog_offset));
    pio_sm_exec(DATA_PIO, SM_READ, pio_encode_jmp(read_prog_offset));

    /* Re-prime the SM_ADDR TX FIFO with LUT_HIGH for the prologue. */
    pio_sm_put(ADDR_PIO, SM_ADDR, (uint32_t)((uintptr_t)rom_lut >> 16));

    dma_chain_arm();

    /* Restore D-bus, /WAIT and LVC_DIR to idle in case cart_read was aborted
     * mid-cycle with pindirs flipped OUT. */
    pio_sm_set_pins_with_mask64(DATA_PIO, SM_READ,
                                ((uint64_t)1u << GP_WAIT) | ((uint64_t)1u << GP_LVC_DIR),
                                ((uint64_t)1u << GP_WAIT) | ((uint64_t)1u << GP_LVC_DIR));
    pio_sm_set_consecutive_pindirs(DATA_PIO, SM_READ, 16, 8, false);

    /* Re-enable in the original order, cart_read first. */
    pio_sm_set_enabled(DATA_PIO, SM_READ, true);
    pio_sm_set_enabled(ADDR_PIO, SM_ADDR, true);
#endif
}

const uint8_t *cart_serve_sram(void) {
    return rom_sram;
}
