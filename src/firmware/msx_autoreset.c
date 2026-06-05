/* KI-2 reset latch. See msx_autoreset.h and the KI-2 plan.
 *
 * Fire one /RESET pulse the first time the MSX is seen stably present, so the
 * cart releases the Z80 only after the detector + cart_serve pipeline are
 * re-armed. Re-park /RESET when the MSX goes absent, so the next power-on is
 * held from the moment it powers up. One-shot per power cycle, re-armable.
 */
#include "msx_autoreset.h"

/* Consecutive present polls (500 ms each) before firing. 2 is ~1 s, enough to
 * ride out the MSX ±12 V soft-start ramp. */
#define AR_PRESENT_POLLS 2

static unsigned char s_present_count;  /* clamped at AR_PRESENT_POLLS */
static bool          s_fired;          /* one-shot for this power cycle */

void msx_autoreset_reset(void) {
    s_present_count = 0;
    s_fired         = false;
}

autoreset_action_t msx_autoreset_step(bool msx_present) {
    if (msx_present) {
        if (s_present_count < AR_PRESENT_POLLS) s_present_count++;
        if (s_present_count >= AR_PRESENT_POLLS && !s_fired) {
            s_fired = true;
            return AR_FIRE_PULSE;
        }
        return AR_NONE;
    }

    /* MSX absent. Re-park only on the transition into absence (or after a
     * fire); steady-state absence returns AR_NONE so we don't re-assert on
     * every poll. */
    bool was_active = (s_present_count > 0) || s_fired;
    s_present_count = 0;
    s_fired         = false;
    return was_active ? AR_PARK : AR_NONE;
}
