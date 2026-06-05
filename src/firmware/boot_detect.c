#include "boot_detect.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

static boot_detect_state_t s;

/* Recompute last_confirmed (highest set bit) and first_missing (lowest unset). */
static void recompute_derived(void) {
    s.last_confirmed = -1;
    for (int i = 0; i < BP_PHASE_COUNT; i++)
        if (s.phase_flags & (1u << i)) s.last_confirmed = (int8_t)i;
    s.first_missing = BP_PHASE_COUNT;
    for (int i = 0; i < BP_PHASE_COUNT; i++)
        if (!(s.phase_flags & (1u << i))) { s.first_missing = (int8_t)i; break; }
}

void boot_detect_reset(void) {
    memset(&s, 0, sizeof(s));
    s.header_4000 = 0xFF;
    s.header_4001 = 0xFF;
    recompute_derived();
}

const boot_detect_state_t *boot_detect_get_state(void) { return &s; }

static void set_phase(boot_phase_t p) {
    s.phase_flags |= (uint16_t)(1u << p);
    recompute_derived();
}

void boot_detect_feed(const boot_bus_event_t *ev) {
    switch (ev->kind) {
    case BEV_FETCH:
        switch (ev->addr) {
        case 0x0000: set_phase(BP_RESET_VECTOR);    break;
        case 0x02D7: set_phase(BP_BIOS_ENTRY);      break;
        case 0x03F8: set_phase(BP_BIOS_INIT_DONE);  break;
        case 0x2680: set_phase(BP_BASIC_HANDOFF);   break;
        case 0x7C76: set_phase(BP_BASIC_COLDSTART); break;
        case 0x003E: case 0x003B:
        case 0x006F: case 0x0069: set_phase(BP_BASIC_BIOS_VECS); break;
        case 0x7D75: set_phase(BP_CART_SCAN);       break;
        default: break;
        }
        break;
    case BEV_MEM_READ:
        if (ev->addr == 0x4000 || ev->addr == 0x4001) {
            set_phase(BP_CART_SCAN);
            /* Header data is trustworthy only from the alog ring (data_valid).
             * The bd_mem ring samples before cart_serve drives, so it reads
             * 0xFF; ignoring it stops that stale byte from clobbering the value
             * the alog ring already latched. Phase is set either way. */
            if (ev->data_valid) {
                if (ev->addr == 0x4000) s.header_4000 = ev->data;
                else                    s.header_4001 = ev->data;
                if (s.header_4000 == 0x41 && s.header_4001 == 0x42)
                    s.header_read = true;
            }
        }
        break;
    case BEV_MEM_WRITE:
        if (ev->addr == 0xFD9A) {
            s.basic_workarea_init = true;
            set_phase(BP_BASIC_COLDSTART);
        }
        if (ev->addr == 0xFFFF || ev->addr == 0xBF00 ||
            ev->addr == 0xFE00 || ev->addr >= 0xC000) {
            set_phase(BP_SLOT_RAM_PROBE);
        }
        break;
    case BEV_IO_WRITE: {
        uint8_t port = (uint8_t)(ev->addr & 0xFF);
        if (port == 0xAB || port == 0xA8 || port == 0xAA) {
            s.early_ppi_writes = true;
            set_phase(BP_BIOS_ENTRY);
            if (port == 0xA8) set_phase(BP_SLOT_RAM_PROBE);
        } else if (port == 0x99 || port == 0x98) {
            set_phase(BP_VDP_INIT);
        }
        break;
    }
    default: break;
    }
}

const char *boot_phase_name(boot_phase_t p) {
    switch (p) {
    case BP_RESET_VECTOR:    return "Reset vector fetch";
    case BP_BIOS_ENTRY:      return "BIOS early init";
    case BP_SLOT_RAM_PROBE:  return "Slot/RAM probe";
    case BP_BIOS_INIT_DONE:  return "BIOS init complete";
    case BP_BASIC_HANDOFF:   return "BIOS->BASIC handoff";
    case BP_BASIC_COLDSTART: return "BASIC cold start";
    case BP_BASIC_BIOS_VECS: return "BASIC BIOS vectors";
    case BP_VDP_INIT:        return "VDP init activity";
    case BP_CART_SCAN:       return "Cartridge scan";
    default:                 return "?";
    }
}

const char *boot_phase_short_name(boot_phase_t p) {
    switch (p) {
    case BP_RESET_VECTOR:    return "Reset vector";
    case BP_BIOS_ENTRY:      return "BIOS early init";
    case BP_SLOT_RAM_PROBE:  return "Slot/RAM probe";
    case BP_BIOS_INIT_DONE:  return "BIOS init done";
    case BP_BASIC_HANDOFF:   return "BASIC handoff";
    case BP_BASIC_COLDSTART: return "BASIC coldstart";
    case BP_BASIC_BIOS_VECS: return "BASIC BIOS vecs";
    case BP_VDP_INIT:        return "VDP init";
    case BP_CART_SCAN:       return "Cart scan";
    default:                 return "?";
    }
}

boot_phase_status_t boot_phase_status(boot_phase_t p) {
    if (s.phase_flags & (uint16_t)(1u << p)) return BPS_OK;
    if ((int)p == s.first_missing)           return BPS_STALLED;
    return BPS_NOT_REACHED;
}

boot_phase_status_t boot_phase_status_of(uint16_t phase_flags, int8_t first_missing,
                                         boot_phase_t p) {
    if (phase_flags & (uint16_t)(1u << p)) return BPS_OK;
    if ((int)p == (int)first_missing)      return BPS_STALLED;
    return BPS_NOT_REACHED;
}

const char *boot_detect_failure_category(void) {
    switch (s.last_confirmed) {
    case -1:                 return "CPU/reset/clock/BIOS ROM sel/addr/data bus";
    case BP_RESET_VECTOR:    return "BIOS ROM fetch, jump exec, addr decoding";
    case BP_BIOS_ENTRY:      return "Early BIOS setup, PPI access, slot reg access";
    case BP_SLOT_RAM_PROBE:  return "Slot sel, expanded-slot, RAM visibility/integrity";
    case BP_BIOS_INIT_DONE:  return "BASIC ROM mapping, slot config, ROM bus visibility";
    case BP_BASIC_HANDOFF:   return "BASIC ROM mapping/visibility at 7C76";
    case BP_BASIC_COLDSTART: return "BASIC work RAM, stack area, system variables";
    case BP_BASIC_BIOS_VECS: return "BIOS service routine, screen init, VDP path";
    case BP_VDP_INIT:        return "BASIC init after display, keyboard/input, work RAM";
    case BP_CART_SCAN:
        return s.header_read ? "Reached cartridge discovery - boot OK to header"
                             : "Cart slot sel, cart ROM mapping, cart bus response";
    default:                 return "?";
    }
}

const char *boot_detect_failure_category_passive(void) {
    if (s.last_confirmed == BP_CART_SCAN)
        return "Native boot reached cart scan - CPU/RAM/ROM/BASIC path OK";
    return boot_detect_failure_category();
}

static int appendf(char *buf, size_t buflen, int off, const char *fmt, ...) {
    if (off < 0 || (size_t)off >= buflen) return off;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(buf + off, buflen - (size_t)off, fmt, ap);
    va_end(ap);
    if (w < 0) return off;
    off += w;
    return (size_t)off >= buflen ? (int)(buflen - 1) : off;
}

int boot_detect_format_report(char *buf, size_t buflen) {
    if (!buf || buflen == 0) return 0;
    int o = 0;
    const char *last = (s.last_confirmed < 0)
        ? "none" : boot_phase_name((boot_phase_t)s.last_confirmed);
    const char *next = (s.first_missing >= BP_PHASE_COUNT)
        ? "complete" : boot_phase_name((boot_phase_t)s.first_missing);
    o = appendf(buf, buflen, o, "Boot phase detector result\n\n");
    o = appendf(buf, buflen, o, "Last confirmed phase: %s\n", last);
    o = appendf(buf, buflen, o, "Next missing phase: %s\n\nObserved:\n", next);
    static const char *yn[] = { "no", "yes" };
    for (int i = 0; i < BP_PHASE_COUNT; i++)
        o = appendf(buf, buflen, o, "- %s: %s\n",
                    boot_phase_name((boot_phase_t)i),
                    yn[(s.phase_flags >> i) & 1u]);
    o = appendf(buf, buflen, o, "- Early PPI/slot writes: %s\n", yn[s.early_ppi_writes]);
    o = appendf(buf, buflen, o, "- BASIC work-area init: %s\n", yn[s.basic_workarea_init]);
    o = appendf(buf, buflen, o, "- Header read (AB): %s\n", yn[s.header_read]);
    o = appendf(buf, buflen, o, "- Header bytes: %02X %02X\n",
                s.header_4000, s.header_4001);
    o = appendf(buf, buflen, o, "\nLikely failure area:\n%s\n",
                boot_detect_failure_category());
    return o;
}
