#include "cmd_dispatch.h"

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/structs/sio.h"

#include "alog.h"
#include "boot_detect.h"
#include "boot_diag.h"
#include "bus_probe.h"
#include "cart_serve.h"
#include "msx_runtime.h"
#include "oled.h"
#include "usb_serial.h"

#include <stdio.h>
#include <string.h>

#define GP_RESET_IN  32  /* MSX /RESET input (read by RESET cmd)         */
#define GP_RESET_DRV 33  /* 2N7002 gate — HIGH asserts /RESET on MSX bus */

static void cmd_pins(usb_out_ring_t *usb_out) {
    /* Connectivity probe: sample GP0-31 for 1 s, tracking per-pin "ever HIGH"
     * and "ever LOW". Each MSX-bus pin reports T=toggled, H=stuck HIGH,
     * L=stuck LOW. */
    uint32_t seen_high = 0;
    uint32_t seen_low  = 0;
    uint32_t start_us  = time_us_32();
    while ((time_us_32() - start_us) < 1000000u) {
        uint32_t v = sio_hw->gpio_in;
        seen_high |= v;
        seen_low  |= ~v;
    }
    char line[USB_OUT_LINE_LEN];
    usb_serial_write_line(usb_out, "PIN TOGGLE TEST (1.0s sample):\r\n");
    #define STATUS(pin) \
        ((((seen_high >> (pin)) & 1u) && ((seen_low >> (pin)) & 1u)) ? 'T' : \
         (((seen_high >> (pin)) & 1u) ? 'H' : 'L'))
    snprintf(line, sizeof(line),
             "A even: A4=%c A2=%c A0=%c A13=%c A8=%c A6=%c A10=%c A15=%c\r\n",
             STATUS(0), STATUS(1), STATUS(2),  STATUS(3),
             STATUS(4), STATUS(5), STATUS(6),  STATUS(7));
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "A odd:  A5=%c A3=%c A1=%c A14=%c A12=%c A7=%c A11=%c A9=%c\r\n",
             STATUS(8),  STATUS(9),  STATUS(10), STATUS(11),
             STATUS(12), STATUS(13), STATUS(14), STATUS(15));
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "D bus:  D6=%c D7=%c D4=%c D5=%c D2=%c D3=%c D0=%c D1=%c\r\n",
             STATUS(16), STATUS(17), STATUS(18), STATUS(19),
             STATUS(20), STATUS(21), STATUS(22), STATUS(23));
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "CTRL:   /RD=%c /WR=%c /MREQ=%c /IORQ=%c /M1=%c /SLTSL=%c CLK=%c\r\n",
             STATUS(24), STATUS(25), STATUS(26), STATUS(27),
             STATUS(28), STATUS(29), STATUS(31));
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line), "raw HI=$%08lX LO=$%08lX\r\n",
             (unsigned long)seen_high, (unsigned long)seen_low);
    usb_serial_write_line(usb_out, line);
    #undef STATUS
}

void msx_reset_pulse(void) {
    msx_runtime_pulse_reset();
}

static void cmd_reset(usb_out_ring_t *usb_out) {
    /* Diagnostic version of msx_reset_pulse(): sample the MSX /RESET line and
     * alog capture count at four points around the assert/sleep/deassert
     * sequence. Tells us whether the 2N7002 really pulls bus /RESET low
     * (polarity / drive check), and whether the Z80 actually stopped during the
     * 100 ms hold (capture count should stall while in reset). */
    char line[USB_OUT_LINE_LEN];
    alog_reset();                /* re-arm alog FIRST so we don't miss the boot scan */
    boot_detect_reset(); boot_capture_reset();
    /* Flush cart_serve pipeline (FIFOs + DMA) so the next MSX boot
     * doesn't see leftover bytes from the previous one. */
    cart_serve_reset_pipeline();
    uint16_t cap_pre = alog_captured();
    uint32_t r_pre   = gpio_get(GP_RESET_IN);

    gpio_put(GP_RESET_DRV, 1);   /* assert /RESET */
    sleep_us(50);                /* let line settle */
    uint32_t r_asserted = gpio_get(GP_RESET_IN);
    uint16_t cap_asserted = alog_captured();

    sleep_ms(50);
    uint32_t r_mid = gpio_get(GP_RESET_IN);
    uint16_t cap_mid = alog_captured();

    sleep_ms(50);                /* total assert hold = ~100 ms */
    gpio_put(GP_RESET_DRV, 0);   /* deassert */
    sleep_us(50);                /* let line settle */
    uint32_t r_post = gpio_get(GP_RESET_IN);
    uint16_t cap_post = alog_captured();

    snprintf(line, sizeof(line),
             "RESET /RESET: pre=%lu ass=%lu mid=%lu post=%lu\r\n",
             (unsigned long)r_pre, (unsigned long)r_asserted,
             (unsigned long)r_mid, (unsigned long)r_post);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "RESET cap:    pre=%u ass=%u mid=%u post=%u\r\n",
             (unsigned)cap_pre, (unsigned)cap_asserted,
             (unsigned)cap_mid, (unsigned)cap_post);
    usb_serial_write_line(usb_out, line);
    usb_serial_write_line(usb_out, "MSX /RESET asserted 100ms (alog re-armed)\r\n");
}

static void cmd_boot(usb_out_ring_t *usb_out) {
    boot_diag_start();
    usb_serial_write_line(usb_out,
        "Boot diagnostic started; watch OLED. Run BOOT again when DONE for the report.\r\n");
    const boot_diag_result_t *r = boot_diag_get_result();
    if (r->passes_run == 0u) return;
    char buf[800];
    boot_diag_format_report(r, buf, sizeof(buf));
    const char *p = buf;
    while (*p) {
        const char *nl = strchr(p, '\n');
        char line[USB_OUT_LINE_LEN];
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line) - 2) len = sizeof(line) - 3;
        memcpy(line, p, len); line[len] = '\0';
        size_t L = strlen(line); line[L] = '\r'; line[L + 1] = '\n'; line[L + 2] = '\0';
        usb_serial_write_line(usb_out, line);
        if (!nl) break;
        p = nl + 1;
    }
}

static void cmd_help(usb_out_ring_t *usb_out) {
    usb_serial_write_line(usb_out, "Commands:\r\n");
    usb_serial_write_line(usb_out, "  HELP               print commands\r\n");
    usb_serial_write_line(usb_out, "  STATUS             firmware state\r\n");
    usb_serial_write_line(usb_out, "  RESET              assert MSX /RESET 100ms\r\n");
    usb_serial_write_line(usb_out, "  DUMP $AAAA         hex dump 256B of ROM\r\n");
    usb_serial_write_line(usb_out, "  RAILS              ADC rail voltages\r\n");
    usb_serial_write_line(usb_out, "  UPDATE             enter USB bootloader\r\n");
    usb_serial_write_line(usb_out, "  EMU OFF            disable ROM emulator drive (passive monitor; power-cycle to re-enable)\r\n");
    usb_serial_write_line(usb_out, "  PINS               1s connectivity probe (each MSX-bus pin: T/H/L)\r\n");
    usb_serial_write_line(usb_out, "  ALOG [N|RESET|DEBUG]  /SLTSL capture log (no arg: status; N: slot; RESET: re-arm; DEBUG: pipeline state)\r\n");
    usb_serial_write_line(usb_out, "  BOOT               run passive boot diagnostic; prints last report\r\n");
    usb_serial_write_line(usb_out, "  DASH               live dashboard snapshot\r\n");
}

static void cmd_status(usb_out_ring_t *usb_out, const rail_snapshot_t *rail_snap) {
    char line[USB_OUT_LINE_LEN];
    snprintf(line, sizeof(line), "MSXDOC v0.1  (serve + boot-detect)\r\n");
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line), "+5V:%.2fV %s  +12V:%.1fV %s  -12V:%.1fV %s\r\n",
             rail_snap->v5,   rail_snap->status_5v   == 0 ? "OK" : rail_snap->status_5v   == 1 ? "WARN" : "FAIL",
             rail_snap->v12,  rail_snap->status_12v  == 0 ? "OK" : rail_snap->status_12v  == 1 ? "WARN" : "FAIL",
             rail_snap->vm12, rail_snap->status_m12v == 0 ? "OK" : rail_snap->status_m12v == 1 ? "WARN" : "FAIL");
    usb_serial_write_line(usb_out, line);
}

static void cmd_rails(usb_out_ring_t *usb_out, const rail_snapshot_t *rail_snap) {
    char line[USB_OUT_LINE_LEN];
    static const char *const stag[] = {"OK","WARN","FAIL","n/c"};
    snprintf(line, sizeof(line), "+5V:  %5.2fV  %s\r\n",
             rail_snap->v5,  stag[rail_snap->status_5v < 4 ? rail_snap->status_5v : 2]);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line), "+12V: %5.1fV  %s\r\n",
             rail_snap->v12, stag[rail_snap->status_12v < 4 ? rail_snap->status_12v : 2]);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line), "-12V: %5.1fV  %s\r\n",
             rail_snap->vm12, stag[rail_snap->status_m12v < 4 ? rail_snap->status_m12v : 2]);
    usb_serial_write_line(usb_out, line);
}

static void cmd_dump_hex(usb_out_ring_t *usb_out, uint16_t arg_addr) {
    uint16_t offset = arg_addr;
    if (offset >= 0x4000u) offset -= 0x4000u;
    if (offset > 0x3F00u)  offset  = 0x3F00u;
    const uint8_t *sram = cart_serve_sram();
    for (int row = 0; row < 16; row++) {
        char line[USB_OUT_LINE_LEN];
        uint16_t base = offset + (uint16_t)(row * 16);
        int n = snprintf(line, sizeof(line), "$%04X:", 0x4000u + base);
        for (int b = 0; b < 16 && n < (int)sizeof(line) - 4; b++)
            n += snprintf(line + n, sizeof(line) - (size_t)n, " %02X", sram[base + b]);
        strcat(line, "\r\n");
        usb_serial_write_line(usb_out, line);
    }
}

static void cmd_dash(usb_out_ring_t *usb_out) {
    bus_probe_snapshot_t s;
    bus_probe_get(&s);
    char line[USB_OUT_LINE_LEN];
    if (s.clk_hz)
        snprintf(line, sizeof(line), "CLK: %lu Hz   RESET: %u\r\n",
                 (unsigned long)s.clk_hz, s.reset);
    else
        snprintf(line, sizeof(line), "CLK: ABSENT   RESET: %u\r\n", s.reset);
    usb_serial_write_line(usb_out, line);
    static const char gl[3] = { '~','H','L' };
    char a[22]; for (int i=0;i<16;i++) a[i]=gl[s.addr[i]]; a[16]='\0';
    snprintf(line, sizeof(line), "A0-15: %s\r\n", a);
    usb_serial_write_line(usb_out, line);
    char d[10]; for (int i=0;i<8;i++) d[i]=gl[s.data[i]]; d[8]='\0';
    snprintf(line, sizeof(line), "D0-7 : %s\r\n", d);
    usb_serial_write_line(usb_out, line);
    snprintf(line, sizeof(line),
             "CTRL : RD%c WR%c MREQ%c IORQ%c M1%c SLT%c WAIT%c\r\n",
             gl[s.ctrl[CTRL_RD]],   gl[s.ctrl[CTRL_WR]],  gl[s.ctrl[CTRL_MREQ]],
             gl[s.ctrl[CTRL_IORQ]], gl[s.ctrl[CTRL_M1]],  gl[s.ctrl[CTRL_SLTSL]],
             gl[s.ctrl[CTRL_WAIT]]);
    usb_serial_write_line(usb_out, line);
}

void cmd_dispatch(const parsed_cmd_t *cmd,
                  usb_out_ring_t *usb_out,
                  const rail_snapshot_t *rail_snap) {
    switch (cmd->type) {
        case CMD_EMU_OFF:
            cart_serve_disable_drive();
            usb_serial_write_line(usb_out,
                "ROM emulator DISABLED — power-cycle to re-enable\r\n");
            break;
        case CMD_PINS:           cmd_pins(usb_out);                            break;
        case CMD_RESET:          cmd_reset(usb_out);                           break;
        case CMD_ALOG_STATUS:    alog_print_status(usb_out);                   break;
        case CMD_ALOG_RESET:
            alog_reset();
            usb_serial_write_line(usb_out, "ALOG: cleared, PIO+DMA re-armed\r\n");
            break;
        case CMD_ALOG_DEBUG:     alog_print_debug(usb_out);                    break;
        case CMD_ALOG_SLOT:      alog_print_slot(usb_out, cmd->arg_addr);      break;
        case CMD_UPDATE:
            oled_set_state(DISP_FIRMWARE_UPDATE);
            reset_usb_boot(0, 0);                                              /* never returns */
            break;
        case CMD_HELP:           cmd_help(usb_out);                            break;
        case CMD_STATUS:         cmd_status(usb_out, rail_snap);               break;
        case CMD_RAILS:          cmd_rails(usb_out, rail_snap);                break;
        case CMD_DUMP_HEX:       cmd_dump_hex(usb_out, cmd->arg_addr);         break;
        case CMD_BOOT:              cmd_boot(usb_out);                         break;
        case CMD_DASH:           cmd_dash(usb_out);                            break;
        default: break;
    }
}
