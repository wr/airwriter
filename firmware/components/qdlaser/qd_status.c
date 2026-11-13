// SPDX-License-Identifier: GPL-3.0-or-later
// W-1102: a QuickDraw LaserWriter's state and how it shows (qd_status.h).
#include "qd_status.h"
#include <stddef.h>

qd_state_t qd_state_from(const qd_spec_t *sp, const qd_raw_t *r, int manual)
{
    if (r->s02 & 0x08) return QDS_JAM;
    if (r->s02 & 0x04) return QDS_COVER;
    if (r->s02 & 0x20) return QDS_NO_CASSETTE;
    if (r->s01 & 0x02) return QDS_ENGINE;
    if (manual) {
        if (!(r->s2a & sp->bit_manual)) return QDS_MANUAL;
    } else if (!(r->s2a & sp->bit_cassette)) {
        return QDS_NO_PAPER;
    }
    if (r->s01 & 0x20) return QDS_PRINTING;
    return QDS_READY;
}

qd_ipp_t qd_state_ipp(qd_state_t st)
{
    switch (st) {
    case QDS_NO_PAPER:    return (qd_ipp_t){ "media-empty-error", "Needs attention: check paper" };
    case QDS_NO_CASSETTE: return (qd_ipp_t){ "input-tray-missing-error",
                                             "Needs attention: check the paper cassette" };
    case QDS_MANUAL:      return (qd_ipp_t){ "media-needed-report",
                                             "Needs attention: load the manual feed slot" };
    case QDS_JAM:         return (qd_ipp_t){ "media-jam-error", "Needs attention: clear the paper jam" };
    case QDS_COVER:       return (qd_ipp_t){ "door-open-error", "Needs attention: close the cover" };
    case QDS_ENGINE:      return (qd_ipp_t){ "other-error", "Needs attention: check the printer" };
    case QDS_OFFLINE:     return (qd_ipp_t){ "other", "Offline" };
    default:              return (qd_ipp_t){ NULL, NULL };
    }
}

int qd_state_ready(qd_state_t st)
{
    return st == QDS_READY || st == QDS_PRINTING;
}

int qd_state_fault(qd_state_t st)
{
    return !qd_state_ready(st) && st != QDS_OFFLINE;
}

uint8_t qd_lights(qd_state_t st, int blink_on)
{
    switch (st) {
    case QDS_READY:       return 0x01;
    case QDS_PRINTING:    return blink_on ? 0x01 : 0x00;
    case QDS_NO_PAPER:
    case QDS_NO_CASSETTE:
    case QDS_MANUAL:      return 0x02;
    case QDS_JAM:
    case QDS_COVER:       return 0x04;
    default:              return 0x00;
    }
}
