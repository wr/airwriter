// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LaserWriter's policy (W-1082, laser printers design, "Finding and
// choosing a LaserWriter" and "Status and errors"), pure: answers and the
// clock in, what to do next and what the phone is told out. laser.c acts on
// it; the host tests drive it with a fake clock.
//
// It keeps the LaserWriters heard (NBP answers to =:LaserWriter@*, by name
// and link) and the one shared, remembered by its NBP name; heard on two
// links, the cable's wins. It looks when the card is opened and every 30 s
// while the card is open or a LaserWriter is shared, asks the shared one its
// status every 5 s (2 s while printing) and the others every 30 s while the
// card is open, and wants the query job at the shared printer's first answer
// after boot or after it went quiet. No answer for 30 s is offline.
#pragma once
#include <stdint.h>
#include "lt_node.h"
#include "lw_status.h"

#define LW_HEARD_MAX       8
#define LW_LOOK_MS         30000
#define LW_STATUS_MS       5000
#define LW_STATUS_JOB_MS   2000
#define LW_GONE_MS         30000
#define LW_CARD_MS         60000      // the card counts as open this long after a GET
#define LW_FORGET_MS       600000     // a printer unheard this long leaves the list
#define LW_QUERY_RETRY_MS  30000

// The same numbers as at_printer.h's at_link_t.
enum { LW_LINK_CABLE = 0, LW_LINK_TUNNEL, LW_LINK_ZONE, LW_LINK_RADIO };

#define LWC_NONE 0xFF
// IPP printer-state values.
enum { LWC_IDLE = 3, LWC_PROCESSING = 4, LWC_STOPPED = 5 };

typedef struct {
    uint8_t    used, link;
    uint8_t    name[NBP_NAME_MAX + 1];    // Pascal, MacRoman
    lt_route_t route;                     // its listening socket
    uint64_t   heard, asked;              // last answer (NBP or status), last status ask
    char       status[80];                // its last status string
} lw_heard_t;

typedef struct {
    lw_heard_t heard[LW_HEARD_MAX];
    uint8_t    shared;
    uint8_t    share_name[NBP_NAME_MAX + 1];
    uint8_t    online, queried, want_query, printing;
    uint64_t   looked, card_at, answered, query_at;
    char       status[80];                // the shared printer's last status
    lw_state_t st;
} lwc_t;

typedef struct {
    uint8_t look;                         // send =:LaserWriter@* on every running link
    uint8_t status_idx;                   // SendStatus to heard[status_idx], or LWC_NONE
    uint8_t query;                        // run the query job at the shared printer
} lwc_want_t;

typedef struct {
    int         shared, online;
    int         idx;                      // the shared printer's heard[] entry, or -1
    uint8_t     ipp_state;                // LWC_IDLE, LWC_PROCESSING, LWC_STOPPED
    const char *reason;                   // printer-state-reasons
    const char *message;                  // printer-state-message ("" for none)
    uint8_t     busy;
} lwc_view_t;

void       lwc_init(lwc_t *c, uint64_t now);
// GET /api/laser: the card is open.
void       lwc_card(lwc_t *c, uint64_t now);
// Share the LaserWriter of this NBP name (Pascal), or none with NULL.
void       lwc_share(lwc_t *c, const uint8_t *pascal_name, uint64_t now);
void       lwc_found(lwc_t *c, const nbp_tuple_t *t, uint8_t link, const lt_route_t *r, uint64_t now);
void       lwc_status(lwc_t *c, int idx, const uint8_t *pstr, uint64_t now);
// The query job ended: answered (1) or not (0, tried again after 30 s).
void       lwc_queried(lwc_t *c, int ok, uint64_t now);
void       lwc_job(lwc_t *c, int printing);
lwc_want_t lwc_tick(lwc_t *c, uint64_t now);
lwc_view_t lwc_view(const lwc_t *c, uint64_t now);
