#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/bootrom.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/adc.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include <stdio.h>

#include "ipc.h"
#include "msx_runtime.h"
#include "bus_probe.h"
#include "boot_diag.h"
#include "oled.h"
#include "usb_serial.h"
#include "adc_rails.h"
#include "buttons.h"
#include "ui.h"
#include "cmd_dispatch.h"

#define GP_LED       38

static usb_out_ring_t  usb_out;
static cmd_ring_t      cmd_ring;
static rail_snapshot_t rail_snap;

static void __not_in_flash_func(core1_dashboard_loop)(void) {
    /* During a boot-diagnostic round, drain the capture rings — drain-only, no
     * blocking, since blocking here loses events. Otherwise run the lightweight
     * passive dashboard sampler. boot_diag_tick() on Core 0 owns the
     * active/inactive switch. */
    while (true) {
        if (boot_diag_capture_active())
            boot_diag_core1_drain_step();
        else
            bus_probe_sample_once();
    }
}

static void __not_in_flash_func(core1_entry)(void) {
    multicore_fifo_pop_blocking();   /* go signal from Core 0 */
    core1_dashboard_loop();
}

int main(void) {
    /* The first-proto POR network (R1=10k, C6=100nF, τ=1ms) has marginal hold
     * time; on cold power-up the chip sometimes comes out of reset glitched and
     * never reaches the main loop. The watchdog auto-resets after 5 s if the
     * loop (which feeds it every iteration) isn't running. Pause-on-debug is
     * true so SWD stepping doesn't trip resets during dev. */
    watchdog_enable(5000, true);

    /* LED on — first GPIO write (FW-006). */
    gpio_init(GP_LED);
    gpio_set_dir(GP_LED, GPIO_OUT);
    gpio_put(GP_LED, 1);

    set_sys_clock_khz(150000, true);   /* must be first (FW-007) */

    /* OLED before the power check (FW-009). */
    oled_init();
    oled_set_state(DISP_BOOT);

    usb_serial_init();

    ui_init();

    /* Rails are sampled and classified by the Core 0 monitor; no hard halt
     * here, so USB-only bring-up (all MSX rails legitimately absent) still
     * reaches the main loop and TinyUSB. */
    adc_rails_init();
    adc_rails_sample();

    /* Init IPC ring spin-locks before anything that takes a ring pointer
     * (msx_runtime_serve_detect_init, Core 1 launch). Per FW-011. */
    usb_out.lock  = spin_lock_init(spin_lock_claim_unused(true));
    cmd_ring.lock = spin_lock_init(spin_lock_claim_unused(true));

    /* cart_serve is autonomous PIO+DMA with no ISR, so only USB and I2C need a
     * priority set. */
    irq_set_priority(USBCTRL_IRQ,  2);
    irq_set_priority(I2C0_IRQ,     2);

    /* Bring the bus up read-only: the cart watches, never drives. The MSX boots
     * its own BASIC and we don't park /RESET. */
    msx_runtime_passive_init();

    /* Clock-frequency counter (PWM7B on GP31). Must run before Core 1 starts
     * sampling. */
    bus_probe_init();

    buttons_init();
    /* SW2 (UP) + SW3 (DOWN) held >= 500 ms resets the MSX. Suppresses short-press
     * UP/DOWN events during combo intent so navigation doesn't flicker. */
    buttons_set_combo(BTN_UP, BTN_DOWN, 500);

    multicore_launch_core1(core1_entry);
    multicore_fifo_push_blocking(1);

    oled_set_state(DISP_MONITORING);

    uint32_t last_rail_update_ms = 0;
    uint32_t last_btn_poll_ms    = 0;
    uint8_t  fault_count         = 0;   /* consecutive rail-FAIL polls; debounces MSX PSU soft-start */
    uint32_t        last_oled_ms = 0;

    for (;;) {
        watchdog_update();

        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        /* tud_task + drain output ring + buffer RX commands. */
        usb_serial_task(&usb_out, &cmd_ring);

        /* Advance any in-flight boot-diagnostic round. Non-blocking except its
         * 100 ms /RESET pulse, which fits the 5 s watchdog budget. */
        boot_diag_tick(now_ms);

        adc_rails_sample();   /* one rail per iteration, round-robin */

        /* Only treat a FAIL as a power fault when the MSX is actually powered
         * (at least one positive rail not ABSENT). On USB-only every rail is
         * ABSENT, so a stray -12 V FAIL from PCB leakage shouldn't blink the
         * fault page; MONITORING and the boot observer cover the "MSX off/dead"
         * case.
         *
         * The MSX PSU ±12 V soft-start takes 50-500 ms and the cart MCU boots
         * faster, so the first rail check can land mid-ramp and read FAIL on
         * still-rising rails. Require 4 consecutive 500 ms FAIL polls (2 s)
         * before tripping POWER_FAULT, and auto-clear once the condition lifts
         * so a transient fault doesn't latch the display. */
        if (now_ms - last_rail_update_ms >= 500) {
            last_rail_update_ms = now_ms;
            adc_rails_snapshot(&rail_snap);
            bool msx_present = (rail_snap.status_5v != 3) ||
                               (rail_snap.status_12v != 3);
            bool any_fail    = (rail_snap.status_5v   == 2) ||
                               (rail_snap.status_12v  == 2) ||
                               (rail_snap.status_m12v == 2);
            const uint8_t FAULT_DEBOUNCE = 4;
            if (msx_present && any_fail) {
                if (fault_count < FAULT_DEBOUNCE) fault_count++;
                if (fault_count >= FAULT_DEBOUNCE) {
                    ui_notify_power_fault(true);
                }
            } else {
                fault_count = 0;
                ui_notify_power_fault(false);
            }

        }

        if (now_ms - last_btn_poll_ms >= 1) {
            last_btn_poll_ms = now_ms;
            btn_event_t btn = buttons_poll();
            /* Any button dismisses the MSX_HEALTH page back to MONITORING. */
            if (btn != BTN_NONE && oled_get_state() == DISP_MSX_HEALTH) {
                oled_set_state(DISP_MONITORING);
            } else {
                ui_handle_btn(btn);
            }
            if (buttons_combo_check()) msx_reset_pulse();
        }

        char cmd_buf[CMD_LINE_LEN];
        if (usb_serial_get_cmd(&cmd_ring, cmd_buf, sizeof(cmd_buf))) {
            parsed_cmd_t cmd = usb_serial_parse_cmd(cmd_buf);
            cmd_dispatch(&cmd, &usb_out, &rail_snap);
        }

        /* OLED refresh every 100 ms: pull the latest probe snapshot into the
         * UI and render. */
        if (now_ms - last_oled_ms >= 100) {
            last_oled_ms = now_ms;
            bus_probe_snapshot_t probe;
            bus_probe_get(&probe);
            ui_set_probe(&probe);
            oled_refresh(&rail_snap, NULL, 0, 0, false);
        }
    }
}
