#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "ipc.h"

#ifdef __GNUC__
#define USB_PRINTF_ATTR __attribute__((format(printf, 2, 3)))
#else
#define USB_PRINTF_ATTR
#endif

typedef enum {
    CMD_HELP,
    CMD_STATUS,
    CMD_RESET,
    CMD_DUMP_HEX,
    CMD_RAILS,
    CMD_UPDATE,
    CMD_EMU_OFF,    /* disable ROM emulator drive (cart becomes passive monitor) */
    CMD_PINS,       /* 1 s GPIO-toggle survey, catches dead bus pins */
    CMD_ALOG_STATUS,        /* "ALOG"       print capture status */
    CMD_ALOG_RESET,         /* "ALOG RESET" clear buffer + re-arm IRQ */
    CMD_ALOG_SLOT,          /* "ALOG N"     dump slot N (arg_addr = slot) */
    CMD_ALOG_DEBUG,         /* "ALOG DEBUG" dump PIO0 SM0 + DMA pipeline state */
    CMD_BOOT,               /* "BOOT"       print the boot-phase detector report */
    CMD_DASH,               /* "DASH"       print the live dashboard snapshot */
    CMD_UNKNOWN
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    uint16_t   arg_addr;
} parsed_cmd_t;

/* Pure parser, no hardware. */
parsed_cmd_t usb_serial_parse_cmd(const char *line);

/* Ring I/O, spin-lock only (no TinyUSB), so native-testable. */
void usb_serial_write_line(usb_out_ring_t *ring, const char *line);  /* Core 1 safe */
void usb_serial_printf(usb_out_ring_t *ring, const char *fmt, ...) USB_PRINTF_ATTR; /* Core 1 safe */
bool usb_serial_get_cmd(cmd_ring_t *ring, char *buf, size_t buflen);

#ifndef NATIVE_TEST
void usb_serial_init(void);
void usb_serial_task(usb_out_ring_t *out, cmd_ring_t *cmd);
#endif
