/* One-shot, re-armable latch deciding when the cart should pulse the MSX
 * /RESET line. Pure logic (no pico-sdk), so the policy is host-testable. The
 * main loop feeds it one bool per rail poll and performs the returned action.
 * See KI-2 plan + msx_runtime.h. */
#ifndef MSX_AUTORESET_H
#define MSX_AUTORESET_H

#include <stdbool.h>

typedef enum {
    AR_NONE,        /* do nothing this poll */
    AR_FIRE_PULSE,  /* MSX stably present and not yet reset this power cycle:
                     * caller should msx_reset_pulse() (re-arm + release) */
    AR_PARK,        /* MSX went absent: caller should re-assert /RESET so the
                     * next power-on is held from the instant it powers up */
} autoreset_action_t;

/* Reset the latch state. Call once at startup. Statics zero-init to the same
 * state, so this is mainly for clarity and test setup. */
void msx_autoreset_reset(void);

/* Feed one rail-poll observation, return the action the caller must perform.
 * msx_present is true when at least one MSX rail is not ABSENT. */
autoreset_action_t msx_autoreset_step(bool msx_present);

#endif /* MSX_AUTORESET_H */
