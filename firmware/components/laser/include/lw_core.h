// SPDX-License-Identifier: GPL-3.0-or-later
//
// The AppleTalk printers' policy (W-1082, W-1088 printers design), pure:
// answers and the clock in, what to do next and what the phone is told out.
// laser.c acts on it; the host tests drive it with a fake clock.
//
// It keeps the printers heard (NBP answers to =:LaserWriter@* and
// =:ImageWriter@*, by name, kind and link) and tells the printer list about
// each once, a second after it is first heard, unless it answered from
// another AirWriter's node (=:AirWriter@*): that one is a relay. It looks at
// start and every 30 s for 5 minutes, then every 5 minutes; "Look for
// printers" starts the 30 s run again. The printer list says which printers
// are its (targets, by record id): each heard on two links rides the cable's
// (then the network's, the tunnel's, the zone's), is asked its status in turn
// every 5 s (2 s while printing), and, a LaserWriter, gets the query job at
// its first answer after boot or after it went quiet, tried again after 60 s
// twice, then every 5 minutes. No answer for 30 s is offline.
//
// W-1098: a printer switched on checks that no one has its name before it
// takes it, looking that name up a few times about 2 s apart (NBP's
// registration, Inside AppleTalk 7-9). Each such check of a LaserWriter or
// ImageWriter name not heard lately on its link puts off one look until
// LW_CHECK_SETTLE_MS after it, when the name answers: a printer comes back,
// new or renamed, in seconds, and nothing is sent while it is off. A check
// from the address another name was heard at is that printer renamed: the
// old name is gone there at once, since its status (which names nothing)
// would come from the printer under its new name.
#pragma once
#include <stdint.h>
#include "lt_node.h"
#include "lw_status.h"

#define LW_HEARD_MAX       8
#define LWC_TARGETS        8
#define LWC_AW_MAX         8
#define LW_LOOK_MS         30000
#define LW_LOOK_SLOW_MS    300000
#define LW_LOOK_BURST_MS   300000     // looks every 30 s this long after start or a look-now
#define LW_TELL_MS         1000       // heard this long before it is told: AirWriters answer too
#define LW_STATUS_MS       5000
#define LW_STATUS_JOB_MS   2000
#define LW_GONE_MS         30000
#define LW_FORGET_MS       600000     // a printer unheard this long leaves the heard list
#define LW_QUERY_RETRY_MS  60000
#define LW_QUERY_TRIES     3          // then every LW_LOOK_SLOW_MS
#define LW_CHECK_SETTLE_MS 3000       // a name check's look, this long after the last check

// The same numbers as at_printer.h's at_link_t.
enum { LW_LINK_CABLE = 0, LW_LINK_TUNNEL, LW_LINK_ZONE, LW_LINK_RADIO };
enum { LWC_KIND_LW = 1, LWC_KIND_IW = 2 };

#define LWC_NONE 0xFF
// IPP printer-state values.
enum { LWC_IDLE = 3, LWC_PROCESSING = 4, LWC_STOPPED = 5 };

typedef struct {
    uint8_t    used, link, kind, told, aw;        // aw: answered from an AirWriter's node
    uint8_t    name[NBP_NAME_MAX + 1];            // Pascal, MacRoman
    lt_route_t route;                             // its listening socket
    uint64_t   first, heard;                      // first and last answer (NBP or status)
} lw_heard_t;

// A printer of the list's, as it gives it.
typedef struct {
    uint8_t id, kind;
    uint8_t name[NBP_NAME_MAX + 1];
} lwc_target_in_t;

typedef struct {
    uint8_t    used, id, kind;
    uint8_t    name[NBP_NAME_MAX + 1];
    uint8_t    online, queried, printing, fails;
    uint64_t   answered, asked, query_at;
    char       status[240];                       // its last status, UTF-8
    lw_state_t st;
} lwc_target_t;

typedef struct {
    lw_heard_t   heard[LW_HEARD_MAX];
    lwc_target_t t[LWC_TARGETS];
    lwc_target_t was[LWC_TARGETS];                // lwc_targets' scratch, off the caller's stack
    struct { uint8_t link, node; uint16_t net; uint64_t at; } aw[LWC_AW_MAX];
    uint64_t     looked, burst_from;
    uint64_t     check_look_at;                   // a name check's look, or 0
    struct {                                      // for /api/laser
        uint32_t n, looks, dropped;               // checks, the looks they made, names ended
        uint8_t  last[NBP_NAME_MAX + 1], link;
    } checks;
    uint32_t     looks;                           // every look, at start and since
    uint8_t      look_due;
    uint8_t      next;                            // whose status is asked next, in turn
} lwc_t;

typedef struct {
    uint8_t look;                         // send the lookups on every running link
    uint8_t status_ti;                    // SendStatus to t[status_ti], or LWC_NONE
    uint8_t query_ti;                     // run the query job at t[query_ti], or LWC_NONE
    uint8_t tell;                         // tell the list about heard[tell], or LWC_NONE
} lwc_want_t;

typedef struct {
    int         online;
    int         idx;                      // the heard[] entry it rides, or -1
    uint8_t     ipp_state;                // LWC_IDLE, LWC_PROCESSING, LWC_STOPPED
    const char *reason;                   // printer-state-reasons
    const char *message;                  // printer-state-message ("" for none)
                                          // (with a warning, busy or a fault: its words)
    uint8_t     busy;
} lwc_view_t;

void       lwc_init(lwc_t *c, uint64_t now);
void       lwc_look_now(lwc_t *c, uint64_t now);
// The list's AppleTalk printers. One with the same id and name as before keeps
// its state; the rest start offline.
void       lwc_targets(lwc_t *c, const lwc_target_in_t *in, int n, uint64_t now);
int        lwc_target(const lwc_t *c, int id);   // t[] index, or -1
void       lwc_found(lwc_t *c, const nbp_tuple_t *t, uint8_t link, const lt_route_t *r, uint64_t now);
// A node on `link` looked up one name (lt_ops_t's name_check).
void       lwc_name_check(lwc_t *c, const nbp_tuple_t *t, uint8_t link, uint64_t now);
void       lwc_status(lwc_t *c, int ti, const uint8_t *pstr, uint64_t now);
// The query job ended: answered (1) or not (0).
void       lwc_queried(lwc_t *c, int ti, int ok, uint64_t now);
void       lwc_job(lwc_t *c, int ti, int printing);
lwc_want_t lwc_tick(lwc_t *c, uint64_t now);
lwc_view_t lwc_view(const lwc_t *c, int ti, uint64_t now);
