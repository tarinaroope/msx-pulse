#include "boot_diag.h"
#include "boot_detect.h"   /* BP_* + pure name/status/category helpers */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

void boot_diag_classify_anomalies(const bus_probe_snapshot_t *s, uint32_t events,
                                  boot_diag_result_t *r) {
    uint16_t f = 0;
    if (s->clk_hz == 0u) {
        f |= (1u << ANOM_CLK_ABSENT);
    } else {
        if (s->clk_hz < 3000000u || s->clk_hz > 4000000u) f |= (1u << ANOM_CLK_OUT_OF_RANGE);
        if (events == 0u) f |= (1u << ANOM_NO_BUS_CYCLES);
    }
    if (s->reset == RST_IN_RESET)               f |= (1u << ANOM_RESET_STUCK);
    if (s->ctrl[CTRL_WAIT] == LINE_STUCK_LO)    f |= (1u << ANOM_WAIT_STUCK_LOW);
    r->anomaly_flags = f;
}

bool boot_diag_anomaly_is_fatal(uint16_t anomaly_flags) {
    const uint16_t FATAL = (1u << ANOM_CLK_ABSENT) | (1u << ANOM_RESET_STUCK)
                         | (1u << ANOM_NO_BUS_CYCLES);
    return (anomaly_flags & FATAL) != 0u;
}

bool boot_diag_round_should_stop(uint16_t phase_flags, uint16_t anomaly_flags) {
    if ((phase_flags & 0x1FFu) == 0x1FFu) return true;
    return boot_diag_anomaly_is_fatal(anomaly_flags);
}

/* Fused verdict, checked top-down — first match wins. */
boot_verdict_t boot_diag_verdict(const boot_diag_result_t *r) {
    if (r->anomaly_flags & (1u << ANOM_CLK_ABSENT))    return VERDICT_CLOCK;
    if (r->anomaly_flags & (1u << ANOM_RESET_STUCK))   return VERDICT_RESET;
    if (r->anomaly_flags & (1u << ANOM_NO_BUS_CYCLES)) return VERDICT_CPU_OR_DECODE;
    if (r->stuck_data_hi || r->stuck_data_lo)          return VERDICT_DATA_LINE;
    if (r->bus_undriven)                               return VERDICT_ROM_NO_DRIVE;
    if (r->cp_failed)                                  return VERDICT_ROM_DATA;
    if (r->stuck_addr_mask)                            return VERDICT_ADDR_LINE;
    if ((r->phase_flags & 0x1FFu) == 0x1FFu)           return VERDICT_OK;
    if (r->first_missing == BP_SLOT_RAM_PROBE)         return VERDICT_RAM_PATH;
    return VERDICT_BOOT_STALL;
}

const char *boot_diag_verdict_text(boot_verdict_t v) {
    switch (v) {
    case VERDICT_OK:            return "CPU/RAM/ROM/BASIC path OK (VDP/PSG/kbd not proven)";
    case VERDICT_CLOCK:         return "Clock - crystal / clock gen / CPU clock pin";
    case VERDICT_RESET:         return "Reset held - RC / Schmitt / short to GND";
    case VERDICT_CPU_OR_DECODE: return "CPU not fetching - Z80 or addr-decode / ROM /CS";
    case VERDICT_DATA_LINE:     return "Data line stuck - buffer (LVC245) or driving chip";
    case VERDICT_ROM_NO_DRIVE:  return "Bus not driven - BIOS ROM / /CS / decode / cold joint";
    case VERDICT_ROM_DATA:      return "BIOS ROM data wrong - ROM or data path";
    case VERDICT_ADDR_LINE:     return "Address line stuck - line / address decode";
    case VERDICT_RAM_PATH:      return "RAM visibility - slot/expansion or RAM (bit-map: future)";
    case VERDICT_BOOT_STALL:    return "Boot stalled - see stall phase + diagnosis";
    default:                    return "?";
    }
}

const char *boot_diag_anomaly_name(boot_anomaly_t a) {
    switch (a) {
    case ANOM_CLK_ABSENT:       return "no clock";
    case ANOM_CLK_OUT_OF_RANGE: return "clock off-freq";
    case ANOM_RESET_STUCK:      return "reset held";
    case ANOM_NO_BUS_CYCLES:    return "no bus cycles";
    case ANOM_WAIT_STUCK_LOW:   return "/WAIT stuck low";
    default:                    return "?";
    }
}

static int appendf(char *buf, size_t buflen, int off, const char *fmt, ...) {
    if (off < 0 || (size_t)off >= buflen) return off;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(buf + off, buflen - (size_t)off, fmt, ap);
    va_end(ap);
    if (w < 0) return off;
    off += w;
    return (size_t)off >= buflen ? (int)(buflen - 1) : off;
}

/* Append "D3 D5" style names for a data-bit mask; returns new offset. */
static int append_bits(char *buf, size_t buflen, int o, const char *pfx,
                       uint32_t mask, int nbits) {
    if (!mask) return o;
    o = appendf(buf, buflen, o, "%s", pfx);
    for (int i = 0; i < nbits; i++)
        if (mask & (1u << i)) o = appendf(buf, buflen, o, " %c%d", pfx[0] == 'D' ? 'D' : 'A', i);
    o = appendf(buf, buflen, o, "\n");
    return o;
}

int boot_diag_format_report(const boot_diag_result_t *r, char *buf, size_t buflen) {
    if (!buf || buflen == 0) return 0;
    int o = 0;
    o = appendf(buf, buflen, o, "Passive boot diagnostic (%u pass%s)\n\n",
                (unsigned)r->passes_run, r->passes_run == 1u ? "" : "es");
    o = appendf(buf, buflen, o, "VERDICT: %s\n\n", boot_diag_verdict_text(r->verdict));

    /* Bus integrity */
    o = appendf(buf, buflen, o, "Bus integrity (%lu reads):\n", (unsigned long)r->n_reads);
    if (!r->stuck_data_hi && !r->stuck_data_lo && !r->stuck_addr_mask
        && !r->bus_undriven && !r->cp_failed) {
        o = appendf(buf, buflen, o, "- OK\n");
    } else {
        o = append_bits(buf, buflen, o, "Data stuck HIGH:", r->stuck_data_hi, 8);
        o = append_bits(buf, buflen, o, "Data stuck LOW:",  r->stuck_data_lo, 8);
        o = append_bits(buf, buflen, o, "Addr stuck:",      r->stuck_addr_mask, 16);
        if (r->bus_undriven) o = appendf(buf, buflen, o, "- bus not driven on reads\n");
        if (r->cp_failed)    o = appendf(buf, buflen, o, "- opcode checkpoint(s) failed: 0x%02X\n",
                                         r->cp_failed);
    }

    /* Anomalies */
    if (r->anomaly_flags) {
        o = appendf(buf, buflen, o, "Anomalies%s:\n", r->fatal ? " (FATAL)" : "");
        for (int a = 0; a < ANOM_COUNT; a++)
            if (r->anomaly_flags & (1u << a))
                o = appendf(buf, buflen, o, "- %s\n", boot_diag_anomaly_name((boot_anomaly_t)a));
    }

    /* Phases */
    const char *next = (r->first_missing >= BP_PHASE_COUNT)
        ? "complete" : boot_phase_name((boot_phase_t)r->first_missing);
    o = appendf(buf, buflen, o, "\nNext missing phase: %s\nPhases:\n", next);
    static const char *st[] = { "OK", "STALLED", "--" };
    for (int i = 0; i < BP_PHASE_COUNT; i++)
        o = appendf(buf, buflen, o, "- %s: %s\n", boot_phase_short_name((boot_phase_t)i),
                    st[boot_phase_status_of(r->phase_flags, r->first_missing, (boot_phase_t)i)]);
    o = appendf(buf, buflen, o, "- PPI writes: %s  work-RAM: %s\n",
                r->early_ppi_writes ? "yes" : "no", r->basic_workarea_init ? "yes" : "no");
    return o;
}

/* Firmware orchestrator: a non-blocking Core-0 state machine. */
#ifndef NATIVE_TEST
#include "pico/stdlib.h"
#include "txn_capture.h"
#include "msx_runtime.h"
#include "alog.h"

static volatile bool      s_capture_active;
static bus_integrity_t    s_bi;            /* read stream, accumulated across passes */
static bool               s_inited;
static bool               s_request;
static boot_diag_phase_t  s_phase = BOOT_DIAG_IDLE;
static uint8_t            s_pass;
static uint32_t           s_deadline_ms;
static uint16_t           s_anom_accum;
static boot_diag_result_t s_result;

void boot_diag_start(void) {
    if (s_phase != BOOT_DIAG_IDLE && s_phase != BOOT_DIAG_DONE) return;
    s_request = true;
}
boot_diag_phase_t boot_diag_phase(void) { return s_phase; }
uint8_t boot_diag_current_pass(void) {
    return (s_phase == BOOT_DIAG_IDLE || s_phase == BOOT_DIAG_DONE)
         ? s_result.passes_run : (uint8_t)(s_pass + 1u);
}
const boot_diag_result_t *boot_diag_get_result(void) { return &s_result; }

bool boot_diag_capture_active(void) { return s_capture_active; }
void boot_diag_core1_drain_step(void) {
    boot_capture_drain();          /* phase rings → boot_detect            */
    txn_capture_drain(&s_bi);      /* read stream → bus_integrity (accum)  */
}

static inline bool reached(uint32_t now, uint32_t deadline) {
    return (int32_t)(now - deadline) >= 0;
}

static void freeze_result(void) {
    const boot_detect_state_t *bs = boot_detect_get_state();
    s_result.anomaly_flags       = s_anom_accum;
    s_result.stuck_data_hi       = bus_integrity_stuck_data_hi(&s_bi, BOOT_DIAG_MIN_READS);
    s_result.stuck_data_lo       = bus_integrity_stuck_data_lo(&s_bi, BOOT_DIAG_MIN_READS);
    s_result.stuck_addr_mask     = bus_integrity_stuck_addr(&s_bi, BOOT_DIAG_MIN_READS,
                                                            BOOT_DIAG_MIN_ADDR_BITS);
    s_result.bus_undriven        = bus_integrity_bus_undriven(&s_bi, BOOT_DIAG_MIN_READS);
    s_result.cp_checked          = s_bi.cp_checked;
    s_result.cp_failed           = s_bi.cp_failed;
    s_result.n_reads             = s_bi.n_reads;
    s_result.phase_flags         = bs->phase_flags;
    s_result.last_confirmed      = bs->last_confirmed;
    s_result.first_missing       = bs->first_missing;
    s_result.early_ppi_writes    = bs->early_ppi_writes;
    s_result.basic_workarea_init = bs->basic_workarea_init;
    s_result.passes_run          = s_pass;
    s_result.fatal               = boot_diag_anomaly_is_fatal(s_anom_accum);
    s_result.verdict             = boot_diag_verdict(&s_result);
}

void boot_diag_tick(uint32_t now_ms) {
    switch (s_phase) {
    case BOOT_DIAG_IDLE:
    case BOOT_DIAG_DONE:
        if (s_request) {
            s_request = false;
            if (!s_inited) {          /* one-time resource claim */
                alog_init();
                boot_capture_init();
                txn_capture_init();
                s_inited = true;
            }
            boot_detect_reset();
            bus_integrity_reset(&s_bi);
            s_pass = 0; s_anom_accum = 0;
            s_phase = BOOT_DIAG_PASS_BEGIN;
        }
        break;

    case BOOT_DIAG_PASS_BEGIN:
        boot_capture_reset();
        txn_capture_reset();
        s_capture_active = true;              /* arm Core 1 drain before /RESET so the
                                               * very first fetch (phase 0, addr 0x0000)
                                               * is drained before the ring can overflow */
        msx_runtime_pulse_reset_passive();    /* 100 ms blocking pulse */
        s_deadline_ms = now_ms + BOOT_DIAG_WINDOW_MS;
        s_phase = BOOT_DIAG_CAPTURING;
        break;

    case BOOT_DIAG_CAPTURING:
        if (reached(now_ms, s_deadline_ms)) {
            s_capture_active = false;         /* Core 1 returns to bus_probe */
            boot_capture_stop();
            txn_capture_stop();
            s_deadline_ms = now_ms + 60u;     /* let Core 1 publish a fresh probe sample */
            s_phase = BOOT_DIAG_SETTLE;
        }
        break;

    case BOOT_DIAG_SETTLE:
        if (reached(now_ms, s_deadline_ms)) {
            bus_probe_snapshot_t snap; bus_probe_get(&snap);
            boot_diag_result_t pass_r;
            boot_diag_classify_anomalies(&snap, s_bi.n_reads, &pass_r);
            s_anom_accum |= pass_r.anomaly_flags;
            s_pass++;
            const boot_detect_state_t *bs = boot_detect_get_state();
            bool stop = boot_diag_round_should_stop(bs->phase_flags, s_anom_accum)
                     || s_pass >= BOOT_DIAG_MAX_PASSES;
            if (stop) { freeze_result(); s_phase = BOOT_DIAG_DONE; }
            else      { s_phase = BOOT_DIAG_PASS_BEGIN; }
        }
        break;
    }
}
#endif /* !NATIVE_TEST */

/* Native build: state stubs + a test seam to inject a result. */
#ifdef NATIVE_TEST
static boot_diag_phase_t  s_phase = BOOT_DIAG_IDLE;
static boot_diag_result_t s_result;
void boot_diag_start(void)                  { s_phase = BOOT_DIAG_PASS_BEGIN; }
boot_diag_phase_t boot_diag_phase(void)     { return s_phase; }
uint8_t boot_diag_current_pass(void)        { return s_result.passes_run; }
const boot_diag_result_t *boot_diag_get_result(void) { return &s_result; }
void boot_diag_test_set_result(const boot_diag_result_t *r, boot_diag_phase_t ph) {
    s_result = *r; s_phase = ph;
}
#endif
