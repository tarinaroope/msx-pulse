#pragma once

/* CFG_TUSB_MCU is set by the pico-sdk board header via tinyusb_board */
#define CFG_TUSB_RHPORT0_MODE  OPT_MODE_DEVICE
#define CFG_TUSB_OS            OPT_OS_NONE
#define CFG_TUSB_DEBUG         0
#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN     __attribute__((aligned(4)))

/* Device class support */
#define CFG_TUD_ENABLED        1
#define CFG_TUD_CDC            1
#define CFG_TUD_CDC_RX_BUFSIZE 256
#define CFG_TUD_CDC_TX_BUFSIZE 256
#define CFG_TUD_CDC_EP_BUFSIZE 64

/* Unused classes */
#define CFG_TUD_HID     0
#define CFG_TUD_MSC     0
#define CFG_TUD_MIDI    0
#define CFG_TUD_VENDOR  0
