/* Passive bus sampler for the live dashboard. Two layers:
 *  - Pure classifiers (no pico-sdk): turn accumulated seen-HIGH/seen-LOW GPIO
 *    masks into per-line verdicts, convert edge counts to Hz, classify the
 *    /RESET line. Unit-tested in tests/test_bus_probe.c.
 *  - Hardware layer (#ifndef NATIVE_TEST): a Core-1 windowed GPIO sampler plus
 *    a PWM7B edge-counter on GP31, publishing a snapshot for Core 0. */
#ifndef BUS_PROBE_H
#define BUS_PROBE_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LINE_ACTIVE   = 0,  /* toggled within the window (seen both H and L) */
    LINE_STUCK_HI = 1,  /* only ever HIGH */
    LINE_STUCK_LO = 2,  /* only ever LOW */
} line_state_t;

typedef enum {
    RST_RUNNING     = 0,  /* /RESET high (deasserted), cart not driving */
    RST_IN_RESET    = 1,  /* /RESET low, cart not driving (held/stuck) */
    RST_PULSING     = 2,  /* transitions seen within the window */
    RST_DRIVEN_CART = 3,  /* cart asserting /RESET (defensive; unused in dashboard) */
} reset_status_t;

/* Control-line indices into bus_probe_snapshot_t.ctrl[]. */
enum {
    CTRL_RD = 0, CTRL_WR, CTRL_MREQ, CTRL_IORQ, CTRL_M1, CTRL_SLTSL, CTRL_WAIT,
    CTRL_COUNT
};

typedef struct {
    line_state_t   addr[16];   /* A0..A15  = GP0..15  */
    line_state_t   data[8];    /* D0..D7   = GP16..23 */
    line_state_t   ctrl[CTRL_COUNT]; /* GP24..30 (CLK=GP31 handled by PWM) */
    uint32_t       clk_hz;     /* 0 = absent */
    reset_status_t reset;
    uint32_t       timestamp_ms;
} bus_probe_snapshot_t;

/* Pure classifiers (no pico-sdk). */

/* seen both -> ACTIVE; only HIGH -> STUCK_HI; only LOW -> STUCK_LO. */
line_state_t bus_probe_classify_bit(bool seen_hi, bool seen_lo);

/* Fill addr/data/ctrl from accumulated GP0..31 masks. CLK (GP31) is ignored
 * (its health is the frequency, set separately). Leaves clk_hz / reset /
 * timestamp untouched. */
void bus_probe_classify(uint32_t seen_hi, uint32_t seen_lo,
                        bus_probe_snapshot_t *out);

/* Convert edges counted over gate_us microseconds to Hz (rounded). gate_us must
 * be > 0. Uses a 64-bit intermediate to avoid overflow. */
uint32_t bus_probe_edges_to_hz(uint32_t edges, uint32_t gate_us);

/* /RESET status from its windowed samples plus whether the cart is driving it. */
reset_status_t bus_probe_reset_status(bool seen_hi, bool seen_lo,
                                      bool cart_driving);

#ifndef NATIVE_TEST
void bus_probe_init(void);        /* configure PWM7B on GP31; call once */
void bus_probe_sample_once(void); /* one windowed sample -> publish snapshot */
void bus_probe_get(bus_probe_snapshot_t *out); /* Core 0 reads latest snapshot */
#endif

#endif /* BUS_PROBE_H */
