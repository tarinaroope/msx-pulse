#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BTN_NONE,
    BTN_UP,
    BTN_DOWN,
    BTN_SELECT
} btn_event_t;

void        buttons_init(void);
btn_event_t buttons_poll(void);            /* call every ~1 ms; returns one event or BTN_NONE */
bool        buttons_held(btn_event_t btn); /* true after 500 ms continuous hold */

/* Two-button combo. When a and b are both held continuously for hold_ms,
 * buttons_combo_check() returns true once. While both are pressed, short-press
 * events for a and b are suppressed in buttons_poll(). Short events for a and b
 * are also held back until COMBO_WINDOW_MS (100 ms) so a quick combo press
 * doesn't briefly fire shorts before the second button registers. */
void        buttons_set_combo(btn_event_t a, btn_event_t b, uint32_t hold_ms);
bool        buttons_combo_check(void);
