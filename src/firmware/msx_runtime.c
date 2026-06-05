#include "msx_runtime.h"
#include "cart_serve.h"   /* cart_serve_init/arm, bus_buffers_enable, cart_serve_reset_pipeline */
#include "alog.h"
#include "boot_detect.h"  /* boot_detect_reset, boot_capture_init/reset */
#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define GP_RESET     32   /* MSX /RESET input  */
#define GP_RESET_DRV 33   /* 2N7002 gate: HIGH asserts /RESET on the MSX bus */

static bool s_detect;

void msx_runtime_serve_detect_init(const uint8_t *rom, size_t rom_len,
                                   usb_out_ring_t *out, bool with_detect) {
    s_detect = with_detect;

    /* Loads PIO programs, copies the ROM, builds the LUT, sets up the DMA
     * chain. SMs aren't enabled until cart_serve_arm() below. */
    cart_serve_init(rom, rom_len, out);

    /* /RESET drive: LOW de-asserts (FET off, /RESET floats). */
    gpio_init(GP_RESET_DRV);
    gpio_put(GP_RESET_DRV, 0);
    gpio_set_dir(GP_RESET_DRV, GPIO_OUT);

    /* /RESET input, read by Core 1 to arm the boot observer. */
    gpio_init(GP_RESET);
    gpio_set_dir(GP_RESET, GPIO_IN);

    /* On RP2350 the pad input buffer is off at reset and the isolation latch
     * is set (datasheet 9.3, p588 — unlike RP2040), so PIO/SIO read these
     * inputs as 0 until IE=1 and ISO=0. gpio_set_function sets both. The
     * output pins (GP16-23 data, GP34 DIR, GP30 /WAIT) get this via
     * cart_serve_init()'s pio_gpio_init(), but the input-only pins below
     * don't — skip this loop and /MREQ, /IORQ, /SLTSL, CLOCK and the whole
     * address bus stay stuck at 0, the bus-monitor PIO hangs on
     * `wait 1 gpio 26`, and cart_serve never sees a read. GP30 is left alone
     * because cart_read owns it as a PIO side-set output; touching it here
     * would steal it back and /WAIT would never drive. */
    for (int pin = 0;  pin <= 15; pin++) gpio_set_function(pin, GPIO_FUNC_SIO); /* A0–A15 */
    for (int pin = 24; pin <= 31; pin++) {
        if (pin == 30) continue; /* GP30 (/WAIT) is PIO-owned by cart_wait_drive */
        gpio_set_function(pin, GPIO_FUNC_SIO); /* /RD /WR /MREQ /IORQ /M1 /SLTSL CLK */
    }

    /* No internal pulls on the bus inputs or the /RESET + DIR pads. */
    for (int pin = 0;  pin <= 15; pin++) gpio_disable_pulls(pin);
    for (int pin = 24; pin <= 31; pin++) gpio_disable_pulls(pin);
    gpio_disable_pulls(34);
    gpio_disable_pulls(32);

    /* Enable the LVC245 buffers. Mode A drives data through them, Mode B
     * reads the bus through them, so this is the same either way. */
    bus_buffers_enable();

    /* Arm cart_serve in both modes: the MSX has to boot into our ROM so the
     * phase-8 header reads at 4000/4001 are genuine (serve + detect). */
    cart_serve_arm();

    /* Arm ALOG capture (PIO0 SM0 + DMA): samples GP0-31 on /SLTSL falling
     * edge, no ISR latency. */
    alog_init();

    if (with_detect) {
        boot_detect_reset();
        boot_capture_init();
    }
}

void msx_runtime_pulse_reset(void) {
    /* Assert /RESET before re-arming alog so the Z80 stops driving the bus
     * first. Otherwise the old boot's /SLTSL events (e.g. the ROM busy-loop
     * at $4333) land in alog[0..N] between alog_reset() and the assertion,
     * eating capacity the fresh boot needs for the $4000/$4001 header read.
     * 1 ms is enough for the Z80 to stop (MSX /RESET is synchronous). */
    gpio_put(GP_RESET_DRV, 1);
    sleep_ms(1);

    /* Re-arm alog and flush the cart_serve pipeline with the Z80 stopped. */
    alog_reset();
    cart_serve_reset_pipeline();
    if (s_detect) { boot_detect_reset(); boot_capture_reset(); }

    sleep_ms(99);                /* finish the 100 ms /RESET hold */
    gpio_put(GP_RESET_DRV, 0);
}

void msx_runtime_hold_reset(void) {
    gpio_put(GP_RESET_DRV, 1);   /* HIGH parks the Z80 in /RESET */
}

#define GP_LVC_DIR   34   /* data shifter direction: HIGH = MSX->MCU (monitor) */

void msx_runtime_passive_init(void) {
    /* /RESET drive LOW = de-asserted; dashboard mode never parks the MSX. */
    gpio_init(GP_RESET_DRV);
    gpio_put(GP_RESET_DRV, 0);
    gpio_set_dir(GP_RESET_DRV, GPIO_OUT);

    /* /RESET input. */
    gpio_init(GP_RESET);
    gpio_set_dir(GP_RESET, GPIO_IN);

    /* Bus pins to SIO so their input buffers come on (RP2350 9.3: IE=0/ISO=1
     * at reset): A0..A15, D0..D7, control GP24..30. GP31 (CLOCK) is left for
     * bus_probe_init()'s PWM7B. GP30 (/WAIT) stays SIO since cart_serve isn't
     * armed here. */
    for (int pin = 0;  pin <= 15; pin++) gpio_set_function(pin, GPIO_FUNC_SIO);
    for (int pin = 16; pin <= 23; pin++) gpio_set_function(pin, GPIO_FUNC_SIO);
    for (int pin = 24; pin <= 30; pin++) gpio_set_function(pin, GPIO_FUNC_SIO);

    for (int pin = 0;  pin <= 15; pin++) gpio_disable_pulls(pin);
    for (int pin = 16; pin <= 23; pin++) gpio_disable_pulls(pin);
    for (int pin = 24; pin <= 30; pin++) gpio_disable_pulls(pin);
    gpio_disable_pulls(GP_RESET);
    gpio_disable_pulls(GP_LVC_DIR);

    /* Data shifter direction MSX->MCU (monitor): GP34 HIGH. */
    gpio_init(GP_LVC_DIR);
    gpio_put(GP_LVC_DIR, 1);
    gpio_set_dir(GP_LVC_DIR, GPIO_OUT);

    /* Enable the shared LVC245 /OE so the bus is visible. */
    bus_buffers_enable();
}

void msx_runtime_pulse_reset_passive(void) {
    gpio_put(GP_RESET_DRV, 1);   /* assert /RESET (FET on) */
    sleep_ms(100);
    gpio_put(GP_RESET_DRV, 0);
}
