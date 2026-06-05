#ifndef CMD_DISPATCH_H
#define CMD_DISPATCH_H

#include "ipc.h"
#include "adc_rails.h"   /* rail_snapshot_t */
#include "usb_serial.h"  /* parsed_cmd_t    */

/* Run one parsed USB command. All side-effects (USB writes, OLED state changes,
 * cart_serve / ALOG calls, reset_usb_boot, etc.) happen inside this call. */
void cmd_dispatch(const parsed_cmd_t *cmd,
                  usb_out_ring_t *usb_out,
                  const rail_snapshot_t *rail_snap);

/* Pulse MSX /RESET low for 100 ms via GP_RESET_DRV (2N7002 gate). Flushes the
 * cart_serve pipeline and re-arms ALOG first so the next boot sees a virgin
 * cart. Shared by the USB RESET command and the SW2+SW3 button combo. Blocks
 * for ~100 ms. */
void msx_reset_pulse(void);

#endif /* CMD_DISPATCH_H */
