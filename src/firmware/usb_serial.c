#include "usb_serial.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <stdarg.h>

/* Pure parser, testable natively. */

static void str_tolower(char *dst, const char *src, size_t n) {
    size_t i;
    for (i = 0; i < n - 1 && src[i]; i++)
        dst[i] = (char)tolower((unsigned char)src[i]);
    dst[i] = '\0';
}

parsed_cmd_t usb_serial_parse_cmd(const char *line) {
    parsed_cmd_t r = { CMD_UNKNOWN, 0 };
    if (!line) return r;

    char buf[64];
    str_tolower(buf, line, sizeof(buf));

    /* strip leading/trailing whitespace */
    char *p = buf;
    while (*p == ' ' || *p == '\t') p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
        *--end = '\0';

    if (strcmp(p, "help") == 0)       { r.type = CMD_HELP;      return r; }
    if (strcmp(p, "status") == 0)     { r.type = CMD_STATUS;    return r; }
    if (strcmp(p, "reset") == 0)      { r.type = CMD_RESET;     return r; }
    if (strcmp(p, "rails") == 0)      { r.type = CMD_RAILS;     return r; }
    if (strcmp(p, "update") == 0)     { r.type = CMD_UPDATE;    return r; }
    if (strcmp(p, "emu off") == 0)    { r.type = CMD_EMU_OFF;   return r; }
    if (strcmp(p, "pins") == 0)       { r.type = CMD_PINS;      return r; }

    if (strcmp(p, "boot") == 0)        { r.type = CMD_BOOT;        return r; }
    if (strcmp(p, "dash") == 0)       { r.type = CMD_DASH;      return r; }

    if (strncmp(p, "dump", 4) == 0) {
        r.type = CMD_DUMP_HEX;
        char *arg = p + 4;
        while (*arg == ' ') arg++;
        if (*arg == '$') arg++;
        r.arg_addr = (uint16_t)strtoul(arg, NULL, 16);
        return r;
    }

    if (strncmp(p, "alog", 4) == 0 && (p[4] == '\0' || p[4] == ' ')) {
        char *arg = p + 4;
        while (*arg == ' ') arg++;
        if (*arg == '\0') {
            r.type = CMD_ALOG_STATUS;
        } else if (strcmp(arg, "reset") == 0) {
            r.type = CMD_ALOG_RESET;
        } else if (strcmp(arg, "debug") == 0) {
            r.type = CMD_ALOG_DEBUG;
        } else {
            r.type = CMD_ALOG_SLOT;
            r.arg_addr = (uint16_t)strtoul(arg, NULL, 10);
        }
        return r;
    }

    return r;  /* CMD_UNKNOWN */
}

/* Ring I/O: spin-lock only, no TinyUSB, so it's testable natively. */

void usb_serial_write_line(usb_out_ring_t *ring, const char *line) {
    uint32_t save = spin_lock_blocking(ring->lock);
    uint32_t next = (ring->write_idx + 1) % USB_OUT_RING_LINES;
    if (next != ring->read_idx) {
        strncpy(ring->lines[ring->write_idx], line, USB_OUT_LINE_LEN - 1);
        ring->lines[ring->write_idx][USB_OUT_LINE_LEN - 1] = '\0';
        ring->write_idx = next;
    }
    spin_unlock(ring->lock, save);
}

void usb_serial_printf(usb_out_ring_t *ring, const char *fmt, ...) {
    char buf[USB_OUT_LINE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    usb_serial_write_line(ring, buf);
}

bool usb_serial_get_cmd(cmd_ring_t *ring, char *buf, size_t buflen) {
    uint32_t save = spin_lock_blocking(ring->lock);
    bool got = ring->read_idx != ring->write_idx;
    if (got) {
        strncpy(buf, ring->lines[ring->read_idx], buflen - 1);
        buf[buflen - 1] = '\0';
        ring->read_idx = (ring->read_idx + 1) % CMD_RING_LINES;
    }
    spin_unlock(ring->lock, save);
    return got;
}

/* Hardware side (pico-sdk), dropped from NATIVE_TEST builds. */

#ifndef NATIVE_TEST

#include "tusb.h"
#include "pico/multicore.h"

void usb_serial_init(void) {
    tusb_init();
}

#define VERSION_BANNER "MSXDOC v0.1 -- type HELP for commands\r\n"

void usb_serial_task(usb_out_ring_t *out, cmd_ring_t *cmd) {
    tud_task();

    /* Banner once per connect. */
    static bool was_connected = false;
    bool connected = tud_cdc_connected();
    if (connected && !was_connected) {
        tud_cdc_write(VERSION_BANNER, sizeof(VERSION_BANNER) - 1);
        tud_cdc_write_flush();
    }
    was_connected = connected;

    /* Drain the output ring. Core 1 writes lines into it; Core 0 sends them. */
    while (out->read_idx != out->write_idx) {
        uint32_t save = spin_lock_blocking(out->lock);
        char line[USB_OUT_LINE_LEN];
        strncpy(line, out->lines[out->read_idx], sizeof(line));
        out->read_idx = (out->read_idx + 1) % USB_OUT_RING_LINES;
        spin_unlock(out->lock, save);
        if (tud_cdc_connected()) {
            tud_cdc_write(line, strlen(line));
            tud_cdc_write_flush();
        }
    }

    /* Accumulate incoming chars, push each complete line to the command ring. */
    {
        static char accum[CMD_LINE_LEN];
        static int  accum_len = 0;
        int c;
        while ((c = tud_cdc_read_char()) != -1) {
            if (c == '\n' || c == '\r') {
                if (accum_len > 0) {
                    accum[accum_len] = '\0';
                    uint32_t save = spin_lock_blocking(cmd->lock);
                    uint32_t next = (cmd->write_idx + 1) % CMD_RING_LINES;
                    if (next != cmd->read_idx) {
                        memcpy(cmd->lines[cmd->write_idx], accum,
                               (size_t)(accum_len + 1));
                        cmd->write_idx = next;
                    }
                    spin_unlock(cmd->lock, save);
                    accum_len = 0;
                }
            } else if (accum_len < CMD_LINE_LEN - 1) {
                accum[accum_len++] = (char)c;
            }
        }
    }
}

#endif /* NATIVE_TEST */
