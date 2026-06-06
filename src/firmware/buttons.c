#include "buttons.h"

#ifndef NATIVE_TEST
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#else
#include "pico_stdlib_mock.h"
#endif

#define GP_BTN_UP   39
#define GP_BTN_DOWN 43
#define GP_BTN_SEL  44

/* 20 ms debounce, 500 ms hold, 100 ms combo window, 200 ms SELECT hold-to-fire.
 * COMBO_WINDOW_MS defers the short-press of any button that's part of a combo:
 * its short fires only once this window has passed since the debounced press,
 * or if the button is released early. That gives a near-simultaneous second
 * press time to cancel the deferred short. SELECT isn't in the default combo,
 * so it's unaffected.
 * SELECT_HOLD_MS: SELECT (GP44) shares the QSPI flash chip-select and ghost-
 * pulses LOW on flash access, so a 20 ms debounce alone lets phantom presses
 * through. SELECT is therefore hold-to-fire — it emits its event only after a
 * continuous hold this long, which outlasts any runtime CS-low burst. */
#define DEBOUNCE_MS     20
#define HOLD_MS         500
#define COMBO_WINDOW_MS 100
#define SELECT_HOLD_MS  200

typedef struct {
    uint     gpio;
    btn_event_t event;
    uint32_t last_change_ms;
    uint32_t press_start_ms;
    bool     stable;
    bool     prev_raw;
    bool     held_reported;
    bool     pending_short;   /* debounced press observed but short not yet dispatched */
    bool     armed;           /* a press is honoured only after one observed release */
    bool     hold_fired;      /* SELECT hold-to-fire event already dispatched this press */
} btn_state_t;

static btn_state_t btns[3];

static struct {
    btn_event_t a, b;
    uint32_t    hold_ms;
    uint32_t    both_pressed_since_ms;
    bool        intent_active;
    bool        fired;
    bool        consumed;
} g_combo;

void buttons_init(void) {
    const uint gpios[3] = { GP_BTN_UP, GP_BTN_DOWN, GP_BTN_SEL };
    const btn_event_t evts[3] = { BTN_UP, BTN_DOWN, BTN_SELECT };
    for (int i = 0; i < 3; i++) {
        gpio_init(gpios[i]);
        gpio_set_dir(gpios[i], GPIO_IN);
        gpio_pull_up(gpios[i]);
        btns[i].gpio          = gpios[i];
        btns[i].event         = evts[i];
        btns[i].stable         = true;   /* assume released at boot */
        btns[i].prev_raw       = true;
        btns[i].last_change_ms = 0;
        btns[i].held_reported  = false;
        btns[i].pending_short  = false;
        btns[i].hold_fired     = false;
        /* SELECT (GP44) shares the flash chip-select (QSPI_SS) through R9
         * (1 kΩ), which overpowers the ~50 kΩ internal pull-up. GP44 idles HIGH
         * and pulses LOW on every flash access; during cold-XIP boot the QMI
         * holds CS low, so gpio_get(SELECT) reads LOW with nothing pressed — a
         * phantom press that used to jump the UI into settings. So start SELECT
         * un-armed: a press counts only after the line is seen stably released
         * (HIGH past the full debounce window); one stray idle-high sample
         * won't arm it. Once XIP warms and CS idles high, SELECT arms and real
         * presses work. UP/DOWN are ordinary GPIOs, released-high at boot, so
         * they start armed. */
        btns[i].armed          = (evts[i] != BTN_SELECT);
    }
    g_combo.a = BTN_NONE;
    g_combo.b = BTN_NONE;
    g_combo.hold_ms = 0;
    g_combo.intent_active = false;
    g_combo.fired = false;
    g_combo.consumed = true;
}

void buttons_set_combo(btn_event_t a, btn_event_t b, uint32_t hold_ms) {
    g_combo.a = a;
    g_combo.b = b;
    g_combo.hold_ms = hold_ms;
    g_combo.intent_active = false;
    g_combo.fired = false;
    g_combo.consumed = true;
}

bool buttons_combo_check(void) {
    if (g_combo.fired && !g_combo.consumed) {
        g_combo.consumed = true;
        return true;
    }
    return false;
}

static int find_btn(btn_event_t e) {
    if (e == BTN_NONE) return -1;
    for (int i = 0; i < 3; i++) {
        if (btns[i].event == e) return i;
    }
    return -1;
}

static bool is_combo_btn(btn_event_t e) {
    return g_combo.hold_ms > 0u && (e == g_combo.a || e == g_combo.b);
}

static bool is_debounced_pressed(int i, uint32_t now_ms) {
    return !btns[i].prev_raw && (now_ms - btns[i].last_change_ms >= DEBOUNCE_MS);
}

btn_event_t buttons_poll(void) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    /* Pass 1: read raw state, debounce, detect new presses. */
    for (int i = 0; i < 3; i++) {
        bool raw = gpio_get(btns[i].gpio);

        if (raw != btns[i].prev_raw) {
            btns[i].prev_raw       = raw;
            btns[i].last_change_ms = now_ms;
            btns[i].held_reported  = false;
        }

        /* Arm only after a stable release: HIGH continuously past the debounce
         * window. A stray idle-high sample (QSPI CS between accesses) must not
         * arm SELECT. See buttons_init. */
        if (btns[i].prev_raw && (now_ms - btns[i].last_change_ms >= DEBOUNCE_MS))
            btns[i].armed = true;

        bool debounced_pressed = is_debounced_pressed(i, now_ms);

        if (debounced_pressed && btns[i].stable && btns[i].armed) {
            btns[i].stable         = false;
            btns[i].press_start_ms = now_ms;
            btns[i].hold_fired     = false;
            if (is_combo_btn(btns[i].event)) {
                btns[i].pending_short = true;
            } else if (btns[i].event == BTN_SELECT) {
                /* SELECT is hold-to-fire (Pass 4): don't dispatch on debounce. */
            } else {
                /* Non-combo button: fire the short immediately. */
                return btns[i].event;
            }
        }
        if (!debounced_pressed) {
            btns[i].stable        = true;
            btns[i].pending_short = false;  /* released before window elapsed: drop */
        }
    }

    /* Pass 2: combo state machine. While both combo buttons are held down
     * (debounced), suppress their pending shorts and arm the fire-after-
     * hold_ms latch. */
    int ai = find_btn(g_combo.a);
    int bi = find_btn(g_combo.b);
    if (ai >= 0 && bi >= 0 && g_combo.hold_ms > 0u) {
        bool a_p = is_debounced_pressed(ai, now_ms);
        bool b_p = is_debounced_pressed(bi, now_ms);
        if (a_p && b_p) {
            if (!g_combo.intent_active) {
                g_combo.intent_active = true;
                g_combo.both_pressed_since_ms = now_ms;
                g_combo.fired = false;
                g_combo.consumed = false;
            }
            /* Keep suppressing the shorts while the combo is held. */
            btns[ai].pending_short = false;
            btns[bi].pending_short = false;
            if (!g_combo.fired &&
                (now_ms - g_combo.both_pressed_since_ms >= g_combo.hold_ms)) {
                g_combo.fired    = true;
                g_combo.consumed = false;
            }
        } else {
            /* One or both released: intent ends. A fired-but-unconsumed latch
             * stays set until buttons_combo_check() reads it. */
            g_combo.intent_active = false;
        }
    }

    /* Pass 3: dispatch any combo-button short whose deferral window expired
     * without the combo stealing it. */
    for (int i = 0; i < 3; i++) {
        if (btns[i].pending_short &&
            (now_ms - btns[i].press_start_ms >= COMBO_WINDOW_MS)) {
            btns[i].pending_short = false;
            return btns[i].event;
        }
    }

    /* Pass 4: SELECT hold-to-fire. SELECT fires only after a continuous
     * SELECT_HOLD_MS hold (a genuine press outlasts any flash-CS ghost burst).
     * Fires once per press; re-press clears hold_fired in Pass 1. */
    for (int i = 0; i < 3; i++) {
        if (btns[i].event == BTN_SELECT && !btns[i].stable && !btns[i].hold_fired &&
            is_debounced_pressed(i, now_ms) &&
            (now_ms - btns[i].press_start_ms >= SELECT_HOLD_MS)) {
            btns[i].hold_fired = true;
            return btns[i].event;
        }
    }

    return BTN_NONE;
}

bool buttons_held(btn_event_t btn) {
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    for (int i = 0; i < 3; i++) {
        if (btns[i].event != btn) continue;
        bool pressed = !btns[i].prev_raw &&
                       (now_ms - btns[i].last_change_ms >= DEBOUNCE_MS);
        if (pressed && (now_ms - btns[i].press_start_ms >= HOLD_MS) &&
            !btns[i].held_reported) {
            btns[i].held_reported = true;
            return true;
        }
    }
    return false;
}
