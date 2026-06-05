#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "ipc.h"          /* rail_snapshot_t */
#include "buttons.h"      /* btn_event_t */
#include "bus_probe.h"    /* bus_probe_snapshot_t */

typedef enum {
    UI_LEVEL_MAIN     = 0,  /* page 0 of carousel */
    UI_LEVEL_DETAIL   = 1,  /* pages 1..N-1 of carousel */
    UI_LEVEL_SETTINGS = 2,  /* settings */
} ui_level_t;

void ui_init(void);

/* Drive the state machine from buttons. Safe to call with BTN_NONE (no-op).
 * Called from the Core-0 1 ms button-poll path in main.c. */
void ui_handle_btn(btn_event_t e);

/* Render the current UI state into the OLED framebuffer. Caller flushes
 * afterwards (oled.c already does). */
void ui_render(uint32_t now_ms, const rail_snapshot_t *rails);

/* Latch the POWER FAULT row-0 status text. Called by main.c's rail-debounce
 * loop. */
void ui_notify_power_fault(bool faulted);

/* Copy the latest bus_probe snapshot for the dashboard pages to render. Called
 * by main.c each refresh, and by native tests with crafted data. */
void ui_set_probe(const bus_probe_snapshot_t *p);

/* Test introspection, always available. */
ui_level_t ui_get_level(void);
uint8_t    ui_get_page_index(void);     /* 0 = MAIN */
uint8_t    ui_get_page_count(void);     /* MAIN + detail pages */
uint8_t    ui_get_selector_highlight(void);  /* SETTINGS only */

uint8_t ui_get_diag_view(void);   /* 0=none, 1=progress, 2=results */
uint8_t ui_get_diag_page(void);   /* 0..5 within the results carousel */
