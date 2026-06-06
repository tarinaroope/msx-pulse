/* 2+1 level menu: state machine, page registry, renderer. Passive instrument;
 * the settings level has one active row, "Run boot diag", which launches
 * boot_diag (Inc 2).
 *
 * The mock header must come before any header that transitively pulls in
 * ipc.h (same rule as oled.c). */
#ifndef NATIVE_TEST
#include "pico/stdlib.h"
#else
#include "pico_stdlib_mock.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ui.h"
#include "oled_internal.h"   /* fb_*, CHAR_W, hb_full, hb_hollow   */
#include "bus_probe.h"
#include "boot_diag.h"
#include "boot_detect.h"

typedef struct {
    const char *title;
    void      (*render)(uint32_t now_ms, const rail_snapshot_t *rails);
} ui_page_t;

typedef struct {
    const ui_page_t *pages;
    uint8_t          n_pages;
} ui_pages_t;

typedef struct {
    const char *label;
    bool        is_back;
} ui_settings_row_t;

static void render_rails  (uint32_t now_ms, const rail_snapshot_t *rails);
static void render_clkrst (uint32_t now_ms, const rail_snapshot_t *rails);
static void render_control(uint32_t now_ms, const rail_snapshot_t *rails);
static void render_address(uint32_t now_ms, const rail_snapshot_t *rails);
static void render_data   (uint32_t now_ms, const rail_snapshot_t *rails);

static const ui_page_t carousel_pages[] = {
    { "Rails",     render_rails   },
    { "Clk/Reset", render_clkrst  },
    { "Control",   render_control },
    { "Address",   render_address },
    { "Data",      render_data    },
};

static const ui_settings_row_t settings_rows[] = {
    { "Run boot diag", false },
    { "back",          true  },
};

#define N_SETTINGS_ROWS ((uint8_t)(sizeof(settings_rows) / sizeof(settings_rows[0])))

static const ui_pages_t carousel = { carousel_pages, 5 };

static ui_level_t             s_level;
static uint8_t                s_page_idx;
static uint8_t                s_selector_idx;
static bool                   s_power_fault;
static const ui_pages_t      *s_active_pages;

static bus_probe_snapshot_t s_probe;

void ui_set_probe(const bus_probe_snapshot_t *p) { s_probe = *p; }

typedef enum { DV_NONE = 0, DV_PROGRESS = 1, DV_RESULTS = 2 } ui_diag_view_t;
#define N_DIAG_PAGES 6u
static ui_diag_view_t s_diag_view;
static uint8_t        s_diag_page;

static const char *status_text(const rail_snapshot_t *rails) {
    bool msx_absent = (rails->status_5v == 3) && (rails->status_12v == 3);
    if (s_level == UI_LEVEL_SETTINGS) return "settings";
    if (s_power_fault)                return "POWER FAULT";
    if (msx_absent)                   return "no MSX";
    return "watching";
}

static void render_settings(void) {
    int base = OLED_PAGES - 1 - (int)N_SETTINGS_ROWS;
    for (int i = 0; i < (int)N_SETTINGS_ROWS; i++) {
        char line[22];
        snprintf(line, sizeof(line), "%c%s",
                 (i == (int)s_selector_idx) ? '>' : ' ',
                 settings_rows[i].label);
        fb_puts(base + i, 0, line);
    }
}

/* Row 0 top-right indicator: "MAIN" on the dashboard, "n/N" on detail pages.
 * Nothing in the settings level (the status text already reads "settings"). */
static void render_top_right_indicator(void) {
    if (s_level == UI_LEVEL_MAIN) {
        fb_puts(0, OLED_W - 4 * CHAR_W, "MAIN");
    } else if (s_level == UI_LEVEL_DETAIL) {
        char line[8];
        uint8_t n_detail = s_active_pages->n_pages - 1u;
        snprintf(line, sizeof(line), "%u/%u",
                 (unsigned)s_page_idx, (unsigned)n_detail);
        int len = (int)strlen(line);
        int x   = OLED_W - len * CHAR_W;
        if (x < 0) x = 0;
        fb_puts(0, x, line);
    }
}

static void render_settings_footer(void) {
    if (s_level == UI_LEVEL_SETTINGS) {
        fb_puts_centered(7, "up/dn  hold sel=back");
    }
}

static char line_glyph(line_state_t s) {
    switch (s) { case LINE_ACTIVE: return '~';
                 case LINE_STUCK_HI: return 'H';
                 default: return 'L'; }
}

static void render_rails(uint32_t now_ms, const rail_snapshot_t *rails) {
    (void)now_ms;
    char line[22];
    fb_hline(1);
    snprintf(line, sizeof(line), "+5V :  %6.2f V", (double)rails->v5);   fb_puts(2,0,line);
    snprintf(line, sizeof(line), "+12V:  %6.2f V", (double)rails->v12);  fb_puts(3,0,line);
    snprintf(line, sizeof(line), "-12V:  %6.2f V", (double)rails->vm12); fb_puts(4,0,line);
    fb_puts(7, 0, "raw - you judge");
}

static const char *reset_word(reset_status_t r) {
    switch (r) { case RST_RUNNING: return "RUNNING";
                 case RST_IN_RESET: return "IN RESET";
                 case RST_PULSING: return "PULSING";
                 default: return "DRIVEN(cart)"; }
}

static void render_clkrst(uint32_t now_ms, const rail_snapshot_t *rails) {
    (void)now_ms; (void)rails;
    char line[22];
    if (s_probe.clk_hz == 0) {
        fb_puts(2, 0, "CLK : ABSENT");
    } else {
        snprintf(line, sizeof(line), "CLK : %lu.%02lu MHz",
                 (unsigned long)(s_probe.clk_hz / 1000000u),
                 (unsigned long)((s_probe.clk_hz / 10000u) % 100u));
        fb_puts(2, 0, line);
    }
    snprintf(line, sizeof(line), "RST : %s", reset_word(s_probe.reset));
    fb_puts(4, 0, line);
}

static void render_control(uint32_t now_ms, const rail_snapshot_t *rails) {
    (void)now_ms; (void)rails;
    static const char *nm[CTRL_COUNT] =
        { "/RD","/WR","/MREQ","/IORQ","/M1","/SLT","/WAIT" };
    char line[22];
    int row = 1;
    for (int i = 0; i < CTRL_COUNT; i += 2) {
        if (i + 1 < CTRL_COUNT)
            snprintf(line, sizeof(line), "%-5s %c   %-5s %c",
                     nm[i], line_glyph(s_probe.ctrl[i]),
                     nm[i+1], line_glyph(s_probe.ctrl[i+1]));
        else
            snprintf(line, sizeof(line), "%-5s %c",
                     nm[i], line_glyph(s_probe.ctrl[i]));
        fb_puts(row++, 0, line);
    }
    fb_puts(7, 0, "~ act  H hi  L lo");
}

static void render_address(uint32_t now_ms, const rail_snapshot_t *rails) {
    (void)now_ms; (void)rails;
    char line[22]; char *p;
    p = line; for (int i = 0; i < 8; i++)  *p++ = line_glyph(s_probe.addr[i]);
    *p = '\0'; fb_puts(2, 0, "A0-7 :"); fb_puts(2, 7*CHAR_W, line);
    p = line; for (int i = 8; i < 16; i++) *p++ = line_glyph(s_probe.addr[i]);
    *p = '\0'; fb_puts(3, 0, "A8-15:"); fb_puts(3, 7*CHAR_W, line);
    fb_puts(7, 0, "~ act  H hi  L lo");
}

static void render_data(uint32_t now_ms, const rail_snapshot_t *rails) {
    (void)now_ms; (void)rails;
    char line[22]; char *p = line;
    for (int i = 0; i < 8; i++) *p++ = line_glyph(s_probe.data[i]);
    *p = '\0';
    fb_puts(2, 0, "D0-7 :"); fb_puts(2, 7*CHAR_W, line);
    fb_puts(6, 0, "idles floating");
    fb_puts(7, 0, "~ act  H hi  L lo");
}

/* Boot-diagnostic render (Increment 2). */

static void render_diag_progress(void) {
    const char *pn;
    switch (boot_diag_phase()) {
        case BOOT_DIAG_PASS_BEGIN: pn = "resetting MSX";  break;
        case BOOT_DIAG_CAPTURING:  pn = "capturing boot"; break;
        case BOOT_DIAG_SETTLE:     pn = "analysing";      break;
        default:                   pn = "...";            break;
    }
    char line[22];
    fb_puts(2, 0, "Boot diagnostic");
    snprintf(line, sizeof(line), "Pass %u/%u",
             (unsigned)boot_diag_current_pass(), (unsigned)BOOT_DIAG_MAX_PASSES);
    fb_puts(4, 0, line);
    fb_puts(5, 0, pn);
    fb_puts(7, 0, "watching native boot");
}

/* Word-wrap into rows row_lo..7 (21 cols). '\n' forces a line break. */
static void wrap_rows(const char *msg, int row_lo) {
    char line[22]; int col = 0, row = row_lo; line[0] = '\0';
    const char *w = msg;
    while (*w && row <= 7) {
        if (*w == '\n') { fb_puts(row++, 0, line); col = 0; line[0] = '\0'; w++; continue; }
        if (*w == ' ')  { w++; continue; }
        const char *end = w; while (*end && *end != ' ' && *end != '\n') end++;
        int wl = (int)(end - w); if (wl > 21) wl = 21;
        if (col > 0 && col + 1 + wl > 21) { fb_puts(row++, 0, line); col = 0; line[0] = '\0'; }
        if (row > 7) break;
        if (col > 0) line[col++] = ' ';
        memcpy(line + col, w, (size_t)wl); col += wl; line[col] = '\0';
        w = end;
    }
    if (col > 0 && row <= 7) fb_puts(row, 0, line);
}

static void render_diag_summary(void) {
    const boot_diag_result_t *r = boot_diag_get_result();
    char line[22];
    snprintf(line, sizeof(line), "%u pass  %lu reads",
             (unsigned)r->passes_run, (unsigned long)r->n_reads);
    fb_puts(1, 0, line);

    const char *clk = (r->anomaly_flags & (1u << ANOM_CLK_ABSENT))       ? "ABSENT"
                    : (r->anomaly_flags & (1u << ANOM_CLK_OUT_OF_RANGE)) ? "off-freq"
                    : "present";
    snprintf(line, sizeof(line), "CLK  %s", clk); fb_puts(2, 0, line);

    const char *rst = (r->anomaly_flags & (1u << ANOM_RESET_STUCK)) ? "HELD" : "released";
    snprintf(line, sizeof(line), "RST  %s", rst); fb_puts(3, 0, line);

    bool bus_issue = r->stuck_data_hi || r->stuck_data_lo || r->stuck_addr_mask
                  || r->bus_undriven || r->cp_failed;
    snprintf(line, sizeof(line), "BUS  %s", bus_issue ? "issues (see 2/6)" : "OK");
    fb_puts(4, 0, line);

    if ((r->phase_flags & 0x1FFu) == 0x1FFu)
        fb_puts(5, 0, "BOOT complete");
    else if (r->last_confirmed < 0)
        fb_puts(5, 0, "BOOT no phases");
    else {
        snprintf(line, sizeof(line), "BOOT to %s",
                 boot_phase_short_name((boot_phase_t)r->last_confirmed));
        fb_puts(5, 0, line);
    }
}

static void append_bit_names(char *out, size_t cap, char letter, uint32_t mask, int nbits) {
    size_t o = 0; out[0] = '\0';
    for (int i = 0; i < nbits && o + 4 < cap; i++)
        if (mask & (1u << i)) o += (size_t)snprintf(out + o, cap - o, "%c%d ", letter, i);
}

static void render_diag_bus(void) {
    const boot_diag_result_t *r = boot_diag_get_result();
    char buf[22];
    if (!r->stuck_data_hi && !r->stuck_data_lo && !r->stuck_addr_mask
        && !r->bus_undriven && !r->cp_failed) {
        fb_puts(2, 0, "Bus + data: OK");
        if (r->cp_checked) fb_puts(4, 0, "opcodes verified");
        return;
    }
    int row = 1;
    if (r->stuck_data_hi) { append_bit_names(buf, sizeof(buf), 'D', r->stuck_data_hi, 8);
        char l[22]; snprintf(l, sizeof(l), "Dhi:%s", buf); fb_puts(row++, 0, l); }
    if (r->stuck_data_lo) { append_bit_names(buf, sizeof(buf), 'D', r->stuck_data_lo, 8);
        char l[22]; snprintf(l, sizeof(l), "Dlo:%s", buf); fb_puts(row++, 0, l); }
    if (r->stuck_addr_mask) { append_bit_names(buf, sizeof(buf), 'A', r->stuck_addr_mask, 16);
        char l[22]; snprintf(l, sizeof(l), "Astk:%s", buf); fb_puts(row++, 0, l); }
    if (r->bus_undriven) fb_puts(row++, 0, "bus not driven");
    if (r->cp_failed)    { char l[22]; snprintf(l, sizeof(l), "opcode fail 0x%02X", r->cp_failed);
                           fb_puts(row++, 0, l); }
}

static void render_diag_anomalies(void) {
    const boot_diag_result_t *r = boot_diag_get_result();
    if (r->anomaly_flags == 0u) { fb_puts(3, 0, "Anomalies: none"); return; }
    int row = 1;
    for (int a = 0; a < ANOM_COUNT && row <= 7; a++)
        if (r->anomaly_flags & (1u << a))
            fb_puts(row++, 0, boot_diag_anomaly_name((boot_anomaly_t)a));
}

static const char *phase_status_glyph(boot_phase_status_t st) {
    switch (st) { case BPS_OK: return "OK"; case BPS_STALLED: return "<<"; default: return "--"; }
}
static void render_diag_phases_range(int lo, int hi) {
    const boot_diag_result_t *r = boot_diag_get_result();
    char line[22]; int row = 1;
    for (int i = lo; i <= hi; i++) {
        boot_phase_status_t st =
            boot_phase_status_of(r->phase_flags, r->first_missing, (boot_phase_t)i);
        snprintf(line, sizeof(line), "%-15s %s",
                 boot_phase_short_name((boot_phase_t)i), phase_status_glyph(st));
        fb_puts(row++, 0, line);
    }
}
static void render_diag_phases_lo(void) { render_diag_phases_range(0, 4); }
static void render_diag_phases_hi(void) { render_diag_phases_range(5, 8); }

static void render_diag_causes(void) {
    const boot_diag_result_t *r = boot_diag_get_result();
    char buf[400];
    boot_diag_possible_causes(r, buf, sizeof(buf));
    wrap_rows(buf, 1);   /* wrap_rows honors '\n' between findings */
}

void ui_init(void) {
    s_active_pages = &carousel;
    s_level        = UI_LEVEL_MAIN;
    s_page_idx     = 0;
    s_power_fault  = false;
    s_diag_view    = DV_NONE;
    s_diag_page    = 0;
    /* s_selector_idx initialised by SELECT-into-settings handler */
}

void ui_handle_btn(btn_event_t e) {
    if (e == BTN_NONE) return;

    if (s_diag_view == DV_PROGRESS) return;   /* ignore buttons during a round */

    if (s_diag_view == DV_RESULTS) {
        if (e == BTN_DOWN) { if (s_diag_page + 1u < N_DIAG_PAGES) s_diag_page++; }
        else if (e == BTN_UP) { if (s_diag_page > 0) s_diag_page--; }
        else if (e == BTN_SELECT) {         /* SELECT is hold-to-fire in buttons.c   */
            s_diag_view = DV_NONE;          /* (~200 ms), so a brief GP44 flash-CS    */
            s_level     = UI_LEVEL_MAIN;    /* phantom pulse can't reach here and exit.*/
            s_page_idx  = 0;
        }
        return;
    }

    if (s_level == UI_LEVEL_SETTINGS) {
        if (e == BTN_UP) {
            if (s_selector_idx > 0) s_selector_idx--;
        } else if (e == BTN_DOWN) {
            if (s_selector_idx + 1u < N_SETTINGS_ROWS) s_selector_idx++;
        } else if (e == BTN_SELECT) {
            if (settings_rows[s_selector_idx].is_back) {
                s_level = UI_LEVEL_MAIN;
            } else {
                boot_diag_start();
                s_diag_view = DV_PROGRESS;
                s_level = UI_LEVEL_MAIN;
            }
        }
        return;
    }

    uint8_t n = s_active_pages->n_pages;
    if (e == BTN_DOWN) {
        if (s_page_idx + 1u < n) {
            s_page_idx++;
            s_level = (s_page_idx == 0) ? UI_LEVEL_MAIN : UI_LEVEL_DETAIL;
        }
    } else if (e == BTN_UP) {
        if (s_page_idx > 0) {
            s_page_idx--;
            s_level = (s_page_idx == 0) ? UI_LEVEL_MAIN : UI_LEVEL_DETAIL;
        }
    } else if (e == BTN_SELECT) {
        if (s_level == UI_LEVEL_MAIN) {
            s_selector_idx = N_SETTINGS_ROWS - 1u;
            s_level = UI_LEVEL_SETTINGS;
        }
    }
}

void ui_render(uint32_t now_ms, const rail_snapshot_t *rails) {
    bool blink = (bool)((now_ms / 500u) & 1u);

    if (s_diag_view == DV_PROGRESS && boot_diag_phase() == BOOT_DIAG_DONE) {
        s_diag_view = DV_RESULTS;
        s_diag_page = 0;
    }

    fb_putglyph(0, 0, blink ? hb_full : hb_hollow);

    if (s_diag_view == DV_PROGRESS) {
        fb_puts(0, 2 * CHAR_W, "DIAGNOSTIC");
        render_diag_progress();
        return;
    }
    if (s_diag_view == DV_RESULTS) {
        static const char *titles[N_DIAG_PAGES] =
            { "Summary", "Bus+Data", "Anomalies", "Phases 1-5", "Phases 6-9", "Poss.cause" };
        fb_puts(0, 2 * CHAR_W, titles[s_diag_page]);
        char ind[8];
        snprintf(ind, sizeof(ind), "%u/%u", (unsigned)(s_diag_page + 1u), (unsigned)N_DIAG_PAGES);
        int x = OLED_W - (int)strlen(ind) * CHAR_W; if (x < 0) x = 0;
        fb_puts(0, x, ind);
        switch (s_diag_page) {
            case 0:  render_diag_summary();    break;
            case 1:  render_diag_bus();        break;
            case 2:  render_diag_anomalies();  break;
            case 3:  render_diag_phases_lo();  break;
            case 4:  render_diag_phases_hi();  break;
            default: render_diag_causes();     break;
        }
        fb_puts(7, 0, "up/dn  hold sel=exit");
        return;
    }

    const char *st = status_text(rails);
    char stbuf[16];
    snprintf(stbuf, sizeof(stbuf), "%s", st);
    fb_puts(0, 2 * CHAR_W, stbuf);
    render_top_right_indicator();
    if (s_level == UI_LEVEL_SETTINGS) {
        render_settings();
    } else {
        s_active_pages->pages[s_page_idx].render(now_ms, rails);
    }
    render_settings_footer();
}

void ui_notify_power_fault(bool faulted) {
    s_power_fault = faulted;
}

ui_level_t ui_get_level(void)              { return s_level;                  }
uint8_t    ui_get_page_index(void)         { return s_page_idx;               }
uint8_t    ui_get_page_count(void)         { return s_active_pages->n_pages;  }
uint8_t    ui_get_selector_highlight(void) { return s_selector_idx;           }

uint8_t ui_get_diag_view(void) { return (uint8_t)s_diag_view; }
uint8_t ui_get_diag_page(void) { return s_diag_page; }
