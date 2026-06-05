#pragma once
#include <stdint.h>
#include "ipc.h"   /* rail_snapshot_t */

#ifndef NATIVE_TEST
#include "pico/stdlib.h"
#include "hardware/adc.h"
#endif

void    adc_rails_init(void);
void    adc_rails_sample(void);                          /* round-robin: call each Core 0 loop */
void    adc_rails_snapshot(rail_snapshot_t *out);        /* call at 500 ms intervals */

/* Pure math, no pico-sdk. */
float   adc_rails_raw_to_v(uint32_t raw_avg);
float   adc_rails_calc_voltage(int rail, float vadc);   /* rail: 0=+5V 1=+12V 2=-12V */
uint8_t adc_rails_check_threshold(int rail, float v);   /* 0=OK 1=WARN 2=FAIL 3=ABSENT */
