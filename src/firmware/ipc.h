#pragma once
#include <stdint.h>

#ifndef NATIVE_TEST
#include "pico/sync.h"
#else
/* In NATIVE_TEST builds pico_stdlib_mock.h provides spin_lock_t and stubs.
 * Only define them here if that mock wasn't already included. */
#ifndef PICO_STDLIB_MOCK_H
typedef uint32_t spin_lock_t;
static inline uint32_t spin_lock_blocking(spin_lock_t *l) { (void)l; return 0; }
static inline void     spin_unlock(spin_lock_t *l, uint32_t s) { (void)l; (void)s; }
#endif
#endif

/* USB output ring: Core 1 writes complete lines; Core 0 drains via tud_cdc_write() */
#define USB_OUT_RING_LINES 32
#define USB_OUT_LINE_LEN   80
typedef struct {
    char     lines[USB_OUT_RING_LINES][USB_OUT_LINE_LEN];
    uint32_t write_idx;
    uint32_t read_idx;
    spin_lock_t *lock;
} usb_out_ring_t;

/* Command ring: USB input chars accumulate into lines enqueued here. Core 0
 * both pushes complete lines and pops/dispatches them. */
#define CMD_RING_LINES  8
#define CMD_LINE_LEN   64
typedef struct {
    char     lines[CMD_RING_LINES][CMD_LINE_LEN];
    uint32_t write_idx;
    uint32_t read_idx;
    spin_lock_t *lock;
} cmd_ring_t;

/* Display state. Single-word write is atomic on Cortex-M33, so no lock needed. */
typedef enum {
    DISP_BOOT,
    DISP_MONITORING,
    DISP_FIRMWARE_UPDATE,
    DISP_MSX_HEALTH
} display_state_t;

/* Rail snapshot: Core 0 writes under spin-lock, Core 1 reads for the OLED. */
typedef struct {
    float    v5, v12, vm12;
    uint8_t  status_5v, status_12v, status_m12v;  /* 0=OK 1=WARN 2=FAIL 3=ABSENT */
    uint32_t timestamp_ms;
} rail_snapshot_t;

/* Bus transaction type codes */
#define TX_IF      0    /* Instruction fetch  (/M1 + /MREQ + /RD) */
#define TX_MR      1    /* Memory read        (/MREQ + /RD, not /M1) */
#define TX_MW      2    /* Memory write       (/MREQ + /WR) */
#define TX_IR      3    /* I/O read           (/IORQ + /RD) */
#define TX_IW      4    /* I/O write          (/IORQ + /WR) */
#define TX_IN      5    /* Interrupt ack      (/M1 + /IORQ) */
#define TX_INVALID 0xFF

typedef struct {
    uint32_t    raw;
    uint32_t    timestamp_us;
    uint8_t     type;
    uint16_t    addr;
    uint8_t     data;
    const char *annotation;
} bus_transaction_t;

/* Bit positions in 32-bit PIO capture word (in pins, 32 at IN_BASE=GP0) */
#define RAW_BIT_RD    (1u << 24)   /* /RD  active low */
#define RAW_BIT_WR    (1u << 25)   /* /WR  active low */
#define RAW_BIT_MREQ  (1u << 26)   /* /MREQ active low */
#define RAW_BIT_IORQ  (1u << 27)   /* /IORQ active low */
#define RAW_BIT_M1    (1u << 28)   /* /M1   active low */
#define RAW_BIT_SLTSL (1u << 29)   /* /SLTSL active low */
