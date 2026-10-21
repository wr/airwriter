// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk node on the board (W-1051): UART2 to TashTalk in U7, the pure
// node in lt_node.c, and what the web UI shows of it. Runs in DIP mode 2, or
// always on a bench build (CONFIG_AIRWRITER_LT_BENCH).
//
// A PAP job is the Mac driver's own ImageWriter stream (W-1053), printed
// through the printer every AppleTalk link shares (at_printer.h, W-1076).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "at_printer.h"

// What the node saw that the panel shows (W-1049), on the LocalTalk task.
typedef enum {
    LT_EV_ONLINE,       // the node ID is ours
    LT_EV_LOOKUP,       // an NBP LkUp for us was answered: a Chooser is looking
    LT_EV_NEW_NODE,     // a node's first frame (not an ENQ) since boot: a Mac is on the cable
} lt_event_t;

typedef struct {
    int         uart_num;
    int         tx_pin;      // LT_UART_TX, TashTalk's RX
    int         rx_pin;      // LT_UART_RX, TashTalk's TX
    int         cts_pin;     // LT_CTS, TashTalk's CTS out; -1 = no flow control
    const char *object;      // NBP object: the queue name
    const char *type;        // NBP type the Chooser looks up
    uint16_t    status;      // Option Card status word to start with (lt_node.h)
    localtalk_printer_t printer;   // NULL begin: jobs are captured and dropped
    void      (*on_event)(lt_event_t ev);  // may be NULL; must not block
} localtalk_cfg_t;

// Starts the node once; later calls do nothing. ESP_ERR_NOT_FOUND when
// TashTalk takes none of the start-up sync (U7 empty, or not running
// TashTalk): nothing is left running and the pins are free for ICSP, so a
// later call tries again (W-1081).
esp_err_t localtalk_start(const localtalk_cfg_t *cfg);
// The same sync with the node off, then UART2 and the pins given back:
// ESP_OK if TashTalk took it, ESP_ERR_NOT_FOUND if not. Only the pins and
// UART of cfg are used. ESP_OK at once while the node runs.
esp_err_t localtalk_probe(const localtalk_cfg_t *cfg);
int       localtalk_running(void);
// The AppleTalk port's switch on the settings page (W-1079). The node cannot
// stop once started (it keeps UART2 and its task), so off is quiet: a job is
// cut short, TashTalk answers for no node and frames are dropped, so the
// board leaves the Chooser. On again acquires a node as at start. Running
// stays true throughout: the cable test and the TashTalk flash treat a quiet
// node as a running one.
void      localtalk_set_quiet(int quiet);
int       localtalk_quiet(void);
// The Option Card status word (LT_IW_* in lt_node.h) for SendStatus.
void      localtalk_set_status(uint16_t status);
// State, counters and the latest lines, for GET /api/lt.
void      localtalk_json(char *buf, size_t cap);
// A line for /api/lt's event log (and the console), from any task.
void      localtalk_note(const char *line);
// Bench check: an AEP echo request to `node`; the reply shows in the log.
void      localtalk_echo(uint8_t node);
// The last PAP job's bytes as captured (up to the capture size), or NULL.
const uint8_t *localtalk_job(size_t *len);

// The cable tester (W-1055) borrows the running node's link: every frame
// TashTalk relays, FCS checked and stripped, also goes to `tap` (on the
// LocalTalk task), and localtalk_send_raw queues a frame for TashTalk as the
// node's own are. A NULL tap removes it.
void localtalk_set_tap(void (*tap)(void *ctx, const uint8_t *f, size_t n), void *ctx);
void localtalk_send_raw(const uint8_t *f, size_t n);

// 1 if TashTalk drains the node's UART within wait_ms. The node runs whether
// or not a chip is in U7, its bytes held in the UART behind CTS; the cable
// test asks before it sends (W-1063).
int  localtalk_tt_alive(int wait_ms);
