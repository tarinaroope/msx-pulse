#ifndef BOOT_DETECT_H
#define BOOT_DETECT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* Ordered boot phases (Canon V20). Index order is milestone order. */
typedef enum {
    BP_RESET_VECTOR     = 0,  /* M1 fetch @ 0x0000 */
    BP_BIOS_ENTRY       = 1,  /* M1 fetch @ 0x02D7 (or PPI/slot writes) */
    BP_SLOT_RAM_PROBE   = 2,  /* repeated A8h writes / high-RAM mem writes */
    BP_BIOS_INIT_DONE   = 3,  /* M1 fetch @ 0x03F8 */
    BP_BASIC_HANDOFF    = 4,  /* M1 fetch @ 0x2680 */
    BP_BASIC_COLDSTART  = 5,  /* M1 fetch @ 0x7C76 (or FD9A write) */
    BP_BASIC_BIOS_VECS  = 6,  /* M1 fetch @ 003E/003B/006F/0069 */
    BP_VDP_INIT         = 7,  /* I/O write to port 99h (or 98h) */
    BP_CART_SCAN        = 8,  /* M1 fetch @ 0x7D75 or 4000/4001 header read */
    BP_PHASE_COUNT      = 9
} boot_phase_t;

/* Per-phase display status, derived from phase_flags + first_missing. */
typedef enum {
    BPS_OK          = 0,  /* milestone observed */
    BPS_STALLED     = 1,  /* first unobserved milestone (the stall point) */
    BPS_NOT_REACHED = 2,  /* unobserved, after the stall */
} boot_phase_status_t;

/* Decoded bus event fed to the state machine. addr is the full 16-bit address;
 * for I/O events the port is (addr & 0xFF). data is only trustworthy when
 * data_valid is set. The bd_mem/bd_io capture SMs sample a few cycles after
 * /MREQ|/IORQ fall, before cart_serve drives read data, so their read data is
 * stale (0xFF) and they leave data_valid false. The alog ring samples at /RD
 * rising and feeds header reads with data_valid true. The detector takes header
 * data only from valid events, but sets the phase from either. */
typedef enum {
    BEV_FETCH      = 0,   /* /M1 + /MREQ + /RD */
    BEV_MEM_READ   = 1,   /* /MREQ + /RD, not /M1 */
    BEV_MEM_WRITE  = 2,   /* /MREQ + /WR */
    BEV_IO_READ    = 3,   /* /IORQ + /RD */
    BEV_IO_WRITE   = 4    /* /IORQ + /WR */
} bus_event_kind_t;

typedef struct {
    bus_event_kind_t kind;
    uint16_t         addr;
    uint8_t          data;
    bool             data_valid;  /* data trustworthy? see comment above */
} boot_bus_event_t;

typedef struct {
    uint16_t phase_flags;        /* bit (1<<boot_phase_t) set == observed */
    bool     early_ppi_writes;   /* AB=82 / A8 / AA seen (phase-1 side effect) */
    bool     basic_workarea_init;/* write to FD9A seen (phase-5 side effect) */
    bool     header_read;        /* 4000==41 && 4001==42 seen */
    uint8_t  header_4000;        /* observed value, 0xFF if unseen */
    uint8_t  header_4001;        /* observed value, 0xFF if unseen */
    int8_t   last_confirmed;     /* highest set phase, -1 if none */
    int8_t   first_missing;      /* lowest unset phase index (0..PHASE_COUNT) */
} boot_detect_state_t;

/* Pure state machine, no hardware dependencies. */
void                       boot_detect_reset(void);
void                       boot_detect_feed(const boot_bus_event_t *ev);
const boot_detect_state_t *boot_detect_get_state(void);
const char                *boot_phase_name(boot_phase_t p);
const char                *boot_phase_short_name(boot_phase_t p);
boot_phase_status_t        boot_phase_status(boot_phase_t p);
boot_phase_status_t        boot_phase_status_of(uint16_t phase_flags, int8_t first_missing,
                                                boot_phase_t p);
const char                *boot_detect_failure_category(void);
const char                *boot_detect_failure_category_passive(void);
/* Full multi-line report. Returns bytes written (excl. NUL). */
int                        boot_detect_format_report(char *buf, size_t buflen);

/* Capture layer, implemented in boot_capture.c. */
void boot_capture_init(void);   /* load PIO progs, claim DMA, arm rings */
void boot_capture_reset(void);  /* flush rings + re-arm (call on /RESET) */
void boot_capture_stop(void);   /* disable capture SMs + abort DMA; no re-arm */
uint32_t boot_capture_drain(void); /* decode pending cycles -> boot_detect_feed; returns #events */

/* Capture-path health since the last reset.
 *  discarded   - entries dropped by the overrun guard. Accurate while draining
 *                tightly; if late_drains>0, silent loss may exceed this.
 *  late_drains - drains whose interval exceeded the ring's time-budget, i.e. a
 *                cadence at which the DMA can wrap the ring and lose events.
 *  max_gap_us  - longest observed gap between drain calls (us).
 * In the intended tight Core-1 drain loop late_drains stays near 0. */
void boot_capture_health(uint32_t *discarded, uint32_t *late_drains,
                         uint32_t *max_gap_us);

#endif /* BOOT_DETECT_H */
