/* Passive boot-diagnostic round: pure triage, fused verdict, and the Core-0
 * state machine (orchestrator in boot_diag.c, #ifndef NATIVE_TEST).
 * See docs/superpowers/specs/2026-06-03-increment-2-*. */
#ifndef BOOT_DIAG_H
#define BOOT_DIAG_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "bus_probe.h"
#include "bus_integrity.h"

#define BOOT_DIAG_MAX_PASSES    3u
#define BOOT_DIAG_WINDOW_MS     8000u   /* V20 reaches $7D75 cart-scan in ~5-6 s */
#define BOOT_DIAG_MIN_READS     64u     /* reads needed before stuck verdicts trusted */
#define BOOT_DIAG_MIN_ADDR_BITS 6u      /* address-diversity gate for stuck-addr */

/* Layer-A gross anomalies (from the bus_probe snapshot). */
typedef enum {
    ANOM_CLK_ABSENT       = 0,  /* clk_hz == 0                    (fatal) */
    ANOM_CLK_OUT_OF_RANGE = 1,  /* clk present, outside ~3-4 MHz         */
    ANOM_RESET_STUCK      = 2,  /* /RST low after release         (fatal) */
    ANOM_NO_BUS_CYCLES    = 3,  /* clk present, 0 reads captured  (fatal) */
    ANOM_WAIT_STUCK_LOW   = 4,  /* /WAIT classified STUCK_LO             */
    ANOM_COUNT            = 5
} boot_anomaly_t;

/* Fused component-level verdict (the headline). */
typedef enum {
    VERDICT_OK            = 0,  /* CPU/RAM/ROM/BASIC path OK (VDP/PSG/kbd unproven) */
    VERDICT_CLOCK         = 1,
    VERDICT_RESET         = 2,
    VERDICT_CPU_OR_DECODE = 3,  /* clock OK, no bus cycles */
    VERDICT_DATA_LINE     = 4,  /* stuck data bit (named)  */
    VERDICT_ROM_NO_DRIVE  = 5,  /* bus undriven on reads   */
    VERDICT_ROM_DATA      = 6,  /* opcode checkpoint mismatch */
    VERDICT_ADDR_LINE     = 7,  /* stuck address bit (named) */
    VERDICT_RAM_PATH      = 8,  /* stalled at slot/RAM probe */
    VERDICT_BOOT_STALL    = 9,  /* stalled mid-boot, data OK */
    VERDICT_COUNT         = 10
} boot_verdict_t;

/* Round phases (Core-0 state machine + UI progress view). */
typedef enum {
    BOOT_DIAG_IDLE = 0, BOOT_DIAG_PASS_BEGIN, BOOT_DIAG_CAPTURING,
    BOOT_DIAG_SETTLE, BOOT_DIAG_DONE
} boot_diag_phase_t;

/* Frozen aggregate result. */
typedef struct {
    /* Layer A */
    uint16_t anomaly_flags;
    /* Layer B (derived from bus_integrity) */
    uint8_t  stuck_data_hi;     /* Dk stuck HIGH */
    uint8_t  stuck_data_lo;     /* Dk stuck LOW  */
    uint16_t stuck_addr_mask;   /* Ak stuck      */
    bool     bus_undriven;
    uint8_t  cp_checked;
    uint8_t  cp_failed;
    uint32_t n_reads;
    /* Layer C */
    uint16_t phase_flags;
    int8_t   last_confirmed;
    int8_t   first_missing;
    bool     early_ppi_writes;
    bool     basic_workarea_init;
    /* round bookkeeping + fused output */
    uint8_t  passes_run;
    bool     fatal;
    boot_verdict_t verdict;
} boot_diag_result_t;

/* Pure layer. */
void  boot_diag_classify_anomalies(const bus_probe_snapshot_t *s, uint32_t events,
                                   boot_diag_result_t *r);  /* sets anomaly_flags */
bool  boot_diag_anomaly_is_fatal(uint16_t anomaly_flags);
bool  boot_diag_round_should_stop(uint16_t phase_flags, uint16_t anomaly_flags);
/* Fuse all layers of a populated result into a verdict code (top-down rules). */
boot_verdict_t boot_diag_verdict(const boot_diag_result_t *r);
const char *boot_diag_verdict_text(boot_verdict_t v);
const char *boot_diag_anomaly_name(boot_anomaly_t a);
int   boot_diag_format_report(const boot_diag_result_t *r, char *buf, size_t buflen);

/* State queries (native + firmware). */
void                      boot_diag_start(void);
boot_diag_phase_t         boot_diag_phase(void);
uint8_t                   boot_diag_current_pass(void);
const boot_diag_result_t *boot_diag_get_result(void);

/* Orchestrator (firmware only). */
#ifndef NATIVE_TEST
void boot_diag_tick(uint32_t now_ms);
bool boot_diag_capture_active(void);
void boot_diag_core1_drain_step(void);
#else
void boot_diag_test_set_result(const boot_diag_result_t *r, boot_diag_phase_t ph);
#endif

#endif /* BOOT_DIAG_H */
