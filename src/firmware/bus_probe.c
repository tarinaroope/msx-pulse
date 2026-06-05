#include "bus_probe.h"

#ifndef NATIVE_TEST
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/structs/sio.h"

#define GP_CLK       31     /* MSX CLOCK — PWM7 B input        */
#define GP_RESET_DRV 33     /* 2N7002 gate (read to know if WE drive /RESET) */
#define BP_WINDOW_US 20000u /* 20 ms sampling window           */
#define BP_CLK_GATE_US 10000u /* 10 ms clock-count gate        */

static uint              s_clk_slice;
static spin_lock_t      *s_lock;
static bus_probe_snapshot_t s_snap;   /* published under s_lock */
#endif

line_state_t bus_probe_classify_bit(bool seen_hi, bool seen_lo) {
    if (seen_hi && seen_lo) return LINE_ACTIVE;
    if (seen_hi)            return LINE_STUCK_HI;
    return LINE_STUCK_LO;   /* seen_lo only, or never sampled */
}

void bus_probe_classify(uint32_t seen_hi, uint32_t seen_lo,
                        bus_probe_snapshot_t *out) {
    for (int i = 0; i < 16; i++)
        out->addr[i] = bus_probe_classify_bit((seen_hi >> i) & 1u,
                                              (seen_lo >> i) & 1u);
    for (int i = 0; i < 8; i++)
        out->data[i] = bus_probe_classify_bit((seen_hi >> (16 + i)) & 1u,
                                              (seen_lo >> (16 + i)) & 1u);
    /* ctrl[]: GP24..30 in CTRL_* order (RD,WR,MREQ,IORQ,M1,SLTSL,WAIT). */
    for (int i = 0; i < CTRL_COUNT; i++) {
        int bit = 24 + i;
        out->ctrl[i] = bus_probe_classify_bit((seen_hi >> bit) & 1u,
                                              (seen_lo >> bit) & 1u);
    }
}

uint32_t bus_probe_edges_to_hz(uint32_t edges, uint32_t gate_us) {
    if (gate_us == 0) return 0;
    return (uint32_t)(((uint64_t)edges * 1000000u) / gate_us);
}

reset_status_t bus_probe_reset_status(bool seen_hi, bool seen_lo,
                                      bool cart_driving) {
    if (cart_driving)       return RST_DRIVEN_CART;
    if (seen_hi && seen_lo) return RST_PULSING;
    if (seen_hi)            return RST_RUNNING;
    return RST_IN_RESET;
}

#ifndef NATIVE_TEST
void bus_probe_init(void) {
    s_lock = spin_lock_init(spin_lock_claim_unused(true));

    /* GP31 → PWM7 B input, free-running rising-edge count, TOP = 0xFFFF. */
    gpio_set_function(GP_CLK, GPIO_FUNC_PWM);
    s_clk_slice = pwm_gpio_to_slice_num(GP_CLK);   /* == 7 for GP31 */
    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_clkdiv_mode(&cfg, PWM_DIV_B_RISING);
    pwm_config_set_wrap(&cfg, 0xFFFF);
    pwm_init(s_clk_slice, &cfg, false);            /* start disabled */
}

void bus_probe_sample_once(void) {
    pwm_set_counter(s_clk_slice, 0);
    pwm_set_enabled(s_clk_slice, true);
    uint32_t clk_t0 = time_us_32();

    uint32_t seen_hi = 0, seen_lo = 0;
    uint32_t r_hi = 0, r_lo = 0;
    uint32_t edges = 0;
    bool gate_done = false;
    uint32_t t0 = time_us_32();
    while ((time_us_32() - t0) < BP_WINDOW_US) {
        uint32_t v = sio_hw->gpio_in;
        seen_hi |= v;
        seen_lo |= ~v;
        uint32_t r = sio_hw->gpio_hi_in & 1u;
        if (r) r_hi = 1; else r_lo = 1;
        if (!gate_done && (time_us_32() - clk_t0) >= BP_CLK_GATE_US) {
            pwm_set_enabled(s_clk_slice, false);
            edges = pwm_get_counter(s_clk_slice);
            gate_done = true;
        }
    }
    if (!gate_done) {
        pwm_set_enabled(s_clk_slice, false);
        edges = pwm_get_counter(s_clk_slice);
    }

    bus_probe_snapshot_t snap;
    bus_probe_classify(seen_hi, seen_lo, &snap);
    snap.clk_hz = bus_probe_edges_to_hz(edges, BP_CLK_GATE_US);
    bool cart_driving = gpio_get(GP_RESET_DRV);
    snap.reset = bus_probe_reset_status(r_hi, r_lo, cart_driving);
    snap.timestamp_ms = to_ms_since_boot(get_absolute_time());

    uint32_t save = spin_lock_blocking(s_lock);
    s_snap = snap;
    spin_unlock(s_lock, save);
}

void bus_probe_get(bus_probe_snapshot_t *out) {
    uint32_t save = spin_lock_blocking(s_lock);
    *out = s_snap;
    spin_unlock(s_lock, save);
}
#endif
