#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ipc.h"             /* rail_snapshot_t, bus_transaction_t, display_state_t */
bool oled_init(void);    /* Core 0 boot: probe I2C 0x3C then 0x3D; returns false if absent */
void oled_set_state(display_state_t state);
display_state_t oled_get_state(void);
void oled_refresh(const rail_snapshot_t *rails, const bus_transaction_t *recent,
                  uint8_t n_txns, int scroll_offset, bool trace_paused);
bool oled_is_present(void);             /* true if I²C probe succeeded in oled_init */
