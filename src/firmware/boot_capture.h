#ifndef BOOT_CAPTURE_H
#define BOOT_CAPTURE_H
#include <stdint.h>
#include <stdbool.h>
#include "boot_detect.h"

/* Pure decoders: raw GP0-31 word -> event. Return false if this ring's
 * qualifier wasn't actually asserted (idle/garbage word). */
bool boot_capture_decode_mem(uint32_t raw, boot_bus_event_t *out);
bool boot_capture_decode_io(uint32_t raw, boot_bus_event_t *out);

#endif /* BOOT_CAPTURE_H */
