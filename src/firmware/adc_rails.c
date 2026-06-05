#include "adc_rails.h"

/* 3.3 V reference, 12-bit ADC (0..4095). */
float adc_rails_raw_to_v(uint32_t raw_avg) {
    return (float)raw_avg * (3.3f / 4095.0f);
}

/* Undo each rail's divider to recover the rail voltage (see FW-121 / HW14a).
 *   +5V:   10k:10k        V = Vadc * 2.0
 *   +12V:  33k:10k        V = Vadc * 4.3
 *   -12V:  10k(3V3):82k   V = Vadc * 9.2 - 27.06   (bridge against +3V3) */
float adc_rails_calc_voltage(int rail, float vadc) {
    switch (rail) {
        case 0: return vadc * 2.0f;
        case 1: return vadc * 4.3f;
        case 2: return vadc * 9.2f - 27.06f;
        default: return 0.0f;
    }
}

/* WARN at ±5%, FAIL at ±10% of nominal (FW-123 / FW-124). Returns
 * 0=OK 1=WARN 2=FAIL 3=ABSENT.
 *
 * ABSENT means the cart is on USB only and the MSX rails are floating. The
 * +5V/+12V dividers sag to near GND in that case, so v < 1.0V is unambiguous.
 * The -12V bridge instead pulls toward +3V3 and, depending on residual leakage
 * past R17, can settle anywhere from +3V3 down to ~+1V — hence the wide
 * v >= -2.0V band. A genuine undervolt fault sits far below that (a
 * half-driven rail is ≈ -6V), and the +5V/+12V FAIL points (4.5V / 10.8V) are
 * far above 1.0V, so ABSENT never collides with a real fault. */
static const float nominal[3] = { 5.0f, 12.0f, -12.0f };

uint8_t adc_rails_check_threshold(int rail, float v) {
    if (rail < 0 || rail > 2) return 2;

    if ((rail == 0 || rail == 1) && v < 1.0f) return 3;
    if (rail == 2 && v >= -2.0f) return 3;

    float nom  = nominal[rail];
    float diff = v - nom;
    float pct  = diff / nom;
    if (pct < 0.0f) pct = -pct;

    if (pct > 0.10f) return 2;
    if (pct > 0.05f) return 1;
    return 0;
}

#ifndef NATIVE_TEST

#include "hardware/adc.h"
#include "pico/time.h"

/* 64-sample exponential running average per rail: avg = (avg*63 + sample) / 64.
 * uint32_t holds avg*63 fine (worst case 4095*63 = 257,985). */
static uint32_t avg[3];
static int      current_rail;

void adc_rails_init(void) {
    adc_init();
    adc_gpio_init(40);  /* GP40/ADC0 = +5V  */
    adc_gpio_init(41);  /* GP41/ADC1 = +12V */
    adc_gpio_init(42);  /* GP42/ADC2 = -12V */
    avg[0] = avg[1] = avg[2] = 0;
    current_rail = 0;
}

void adc_rails_sample(void) {
    adc_select_input(current_rail);
    uint16_t raw = adc_read();
    avg[current_rail] = (avg[current_rail] * 63 + raw) / 64;
    current_rail = (current_rail + 1) % 3;
}

void adc_rails_snapshot(rail_snapshot_t *out) {
    float vadc0 = adc_rails_raw_to_v(avg[0]);
    float vadc1 = adc_rails_raw_to_v(avg[1]);
    float vadc2 = adc_rails_raw_to_v(avg[2]);

    out->v5    = adc_rails_calc_voltage(0, vadc0);
    out->v12   = adc_rails_calc_voltage(1, vadc1);
    out->vm12  = adc_rails_calc_voltage(2, vadc2);

    out->status_5v   = adc_rails_check_threshold(0, out->v5);
    out->status_12v  = adc_rails_check_threshold(1, out->v12);
    out->status_m12v = adc_rails_check_threshold(2, out->vm12);

    out->timestamp_ms = time_us_32() / 1000u;
}

#else /* NATIVE_TEST stubs */

void    adc_rails_init(void) {}
void    adc_rails_sample(void) {}
void    adc_rails_snapshot(rail_snapshot_t *out) { (void)out; }

#endif /* NATIVE_TEST */
