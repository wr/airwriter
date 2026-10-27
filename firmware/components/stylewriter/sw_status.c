// SPDX-License-Identifier: GPL-3.0-or-later
#include "sw_status.h"

sw_ipp_t sw_state_ipp(sw_state_t st)
{
    switch (st) {
    case SWS_NO_PAPER:     return (sw_ipp_t){ "media-empty-error",           "Needs attention: check paper" };
    case SWS_JAM:          return (sw_ipp_t){ "media-jam-error",             "Needs attention: clear the paper jam" };
    case SWS_CART_MISSING: return (sw_ipp_t){ "marker-supply-missing-error", "Needs attention: check the ink cartridge" };
    case SWS_INK_OUT:      return (sw_ipp_t){ "marker-supply-empty-error",   "Needs attention: replace the ink cartridge" };
    case SWS_DOOR:         return (sw_ipp_t){ "door-open-error",             "Needs attention: close the cover" };
    case SWS_WASTE:        return (sw_ipp_t){ "marker-waste-full-error",     "Needs attention: the waste ink pad is full" };
    // IPP has no word for a jammed cartridge or a fault the design does not name: "other",
    // as the offline state uses, reads as an error on a stopped printer all the same.
    case SWS_CART_JAM:     return (sw_ipp_t){ "other",                       "Needs attention: check the ink cartridge" };
    case SWS_FAULT:        return (sw_ipp_t){ "other",                       "Needs attention: check the printer" };
    case SWS_OFFLINE:      return (sw_ipp_t){ "other",                       "Offline: check power and cable" };
    case SWS_READY:
    case SWS_PRINTING:     break;
    }
    return (sw_ipp_t){ NULL, NULL };
}

int sw_state_ready(sw_state_t st)
{
    return st == SWS_READY || st == SWS_PRINTING;
}

int sw_state_fault(sw_state_t st)
{
    return st != SWS_READY && st != SWS_PRINTING && st != SWS_OFFLINE;
}

sw_state_t sw_idle_silent(sw_model_t m, int job_silent)
{
    return m == SW_ORIGINAL && !job_silent ? SWS_READY : SWS_OFFLINE;
}
