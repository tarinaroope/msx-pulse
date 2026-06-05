#pragma once
#include <stdint.h>
#include <stddef.h>
#include "ipc.h"        /* usb_out_ring_t */

/* Enable the four 74LVC245 bus buffers (drives GP35 LOW). Needed whether the
 * cart drives D0-D7 or only reads the bus. Lives here because cart_serve owns
 * the LVC control GPIOs. */
void bus_buffers_enable(void);

/* Initialise PIO + DMA + GPIO state. Loads PIO programs, builds the 64KB
 * pre-permuted LUT, configures the two DMA channels, but does not enable the
 * state machines. Safe to call once per power-up. out is used for one-shot
 * diagnostic prints (LUT alignment, SM offsets); may be NULL in unit tests. */
void cart_serve_init(const uint8_t *rom_image, size_t rom_size, usb_out_ring_t *out);

/* Enable the three SMs in order: data writer -> CS handler -> addr reader.
 * After this the cart actively drives the MSX bus on /SLTSL+/MREQ+/RD cycles. */
void cart_serve_arm(void);

/* Stop driving the bus. Disables SMs, aborts DMA channels, returns GP16-23 +
 * GP34 ownership to SIO (GP34 high = receive). One-way; power-cycle to re-arm. */
void cart_serve_disable_drive(void);

/* Flush all in-flight pipeline state so the next /SLTSL+/RD cycle sees a virgin
 * pipeline. Called before asserting MSX /RESET so leftover bytes from the
 * previous boot don't bleed into the next one. No-op if SMs aren't running. */
void cart_serve_reset_pipeline(void);

/* Read-only access to the 16KB ROM image (logical order) for the DUMP command. */
const uint8_t *cart_serve_sram(void);
