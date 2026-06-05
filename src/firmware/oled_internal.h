#pragma once
/* Private constants and test-visible declarations for oled.c. In the firmware
 * build only the geometry constants are exported; everything else stays static
 * in oled.c. Under NATIVE_TEST the internals are extern-visible so tests can
 * exercise the framebuffer helpers and render_* logic without touching I2C. */
#include <stdint.h>
#include <stdbool.h>
#include "ipc.h"             /* rail_snapshot_t, bus_transaction_t */

#define OLED_W       128
#define OLED_H       64
#define OLED_PAGES   (OLED_H / 8)   /* 8 */
#define CHAR_W       6              /* 5 px glyph + 1 px gap */

/* Framebuffer helpers and heartbeat glyphs — external linkage so ui.c can
 * call them from both the firmware build and native tests. */
void fb_clear(void);
void fb_putchar(int page, int x, char c);
void fb_putglyph(int page, int x, const uint8_t glyph[5]);
void fb_puts(int page, int x, const char *s);
void fb_puts_centered(int page, const char *s);
void fb_hline(int page);
extern const uint8_t hb_full[5];
extern const uint8_t hb_hollow[5];

#ifdef NATIVE_TEST
extern uint8_t      fb[OLED_PAGES][OLED_W];
extern bool         oled_present;

const char *status_tag(uint8_t s);

void render_boot(void);
void render_firmware_update(void);
#endif
