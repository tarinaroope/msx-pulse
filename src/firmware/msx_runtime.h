/* Shared hardware bring-up + MSX reset pulse, used by main.c and the
 * MSX-in-the-loop hardware test so both exercise identical init. */
#ifndef MSX_RUNTIME_H
#define MSX_RUNTIME_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ipc.h"   /* usb_out_ring_t */

/* Bring up bus pins (with RP2350-E9 mitigation), /RESET GPIOs, LVC245 buffers,
 * cart_serve (init+arm) and alog. With with_detect true, also init the
 * boot-phase detector capture + state machine (serve + detect). out may be NULL
 * (no USB trace ring; used by the test binary). */
void msx_runtime_serve_detect_init(const uint8_t *rom, size_t rom_len,
                                   usb_out_ring_t *out, bool with_detect);

/* Pulse the MSX /RESET line for 100 ms after flushing the cart_serve pipeline,
 * re-arming alog, and (if detect was initialized) resetting the detector.
 * Mirrors the production reset path. Safe to call repeatedly. */
void msx_runtime_pulse_reset(void);

/* Assert /RESET and leave it asserted, parking the Z80. The auto-reset latch
 * uses this to hold the Z80 in reset while the MSX is off or before the cart
 * has fired its arming pulse. Released by the next msx_runtime_pulse_reset().
 * Idempotent. */
void msx_runtime_hold_reset(void);

/* Passive dashboard bring-up: bus pins read-only (SIO + E9 mitigation), LVC245
 * /OE enabled, data shifter DIR = MSX->MCU, cart_serve not armed, /RESET not
 * driven. The MSX boots natively and the cart only watches. GP31 (CLOCK) is
 * left for bus_probe_init() to claim as PWM7B. */
void msx_runtime_passive_init(void);

/* Passive-only /RESET pulse (100 ms assert then release on GP33). Touches
 * nothing else: no cart_serve flush, no alog re-arm, no detector reset, since
 * msx_runtime_passive_init() doesn't initialise those. Used by boot_diag, which
 * owns the detector/capture lifecycle. Assumes GP33 is already an output. */
void msx_runtime_pulse_reset_passive(void);

#endif /* MSX_RUNTIME_H */
