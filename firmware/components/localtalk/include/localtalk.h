// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk nodes on the board (W-1051): a UART to a socket's TashTalk, the pure node in
// lt_node.c, and what the web UI shows of it. Runs with the AppleTalk port on, or always on a
// bench build (CONFIG_AIRWRITER_LT_BENCH).
//
// W-1186: a side per socket (ports.h: 0 J2, 1 J4), each a node of its own on its own network,
// with its own address, offers and place in the printer list (at_link_of_sock). Rev A has the
// one, J4's. One task and one lock serve both. Every call that takes `sock` is that side's.
//
// A PAP job is the Mac driver's own ImageWriter stream (W-1053), printed
// through the printer every AppleTalk link shares (at_printer.h, W-1076).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "at_printer.h"
#include "pap_client.h"

#define LT_SIDES 2

// What the node saw that the panel shows (W-1049), on the LocalTalk task.
typedef enum {
    LT_EV_ONLINE,       // the node ID is ours
    LT_EV_LOOKUP,       // an NBP LkUp for us was answered: a Chooser is looking
    LT_EV_NEW_NODE,     // a node's first frame (not an ENQ) since boot: a Mac is on the cable
                        // (a printer's first, its answer to our lookup, is not)
} lt_event_t;

typedef struct {
    int         uart_num;
    // W-1186: the UART is the socket's own, lent by serial_out while the socket does LocalTalk
    // (rev 2026.11), so its line inversion, the printer line's, is cleared here.
    int         lent;
    int         tx_pin;      // LT_UART_TX, TashTalk's RX
    int         rx_pin;      // LT_UART_RX, TashTalk's TX
    int         cts_pin;     // LT_CTS, TashTalk's CTS out; -1 = no flow control
    const char *object;      // NBP object: the queue name
    const char *type;        // NBP type the Chooser looks up
    uint16_t    status;      // Option Card status word to start with (lt_node.h)
    localtalk_printer_t printer;   // NULL begin: jobs are captured and dropped
    void      (*on_event)(int sock, lt_event_t ev);  // may be NULL; must not block
} localtalk_cfg_t;

// Starts socket `sock`'s node, or brings a parked one back on its TashTalk (cfg's UART and
// pins; the node keeps its set-up), acquiring a node there if it was on. Nothing when it runs
// already. ESP_ERR_NOT_FOUND when TashTalk takes none of the start-up sync (no chip, or not
// running TashTalk): the UART and the pins are given back, free for ICSP, so a later call
// tries again (W-1081).
esp_err_t localtalk_start(int sock, const localtalk_cfg_t *cfg);
// The same sync with the node off, then the UART and the pins given back: ESP_OK if TashTalk
// took it, ESP_ERR_NOT_FOUND if not. Only the pins and UART of cfg are used. ESP_OK at once
// while the side runs.
esp_err_t localtalk_probe(int sock, const localtalk_cfg_t *cfg);
// The side's node is up on its socket's TashTalk: quiet or listening included, parked not.
int       localtalk_running(int sock);
int       localtalk_any(void);              // any side runs
// W-1179: the socket takes the printer job: the node goes quiet and its UART and pins are
// given back (the driver deleted). The switch and the node's set-up are kept for
// localtalk_start. Nothing for a side that isn't running.
void      localtalk_park(int sock);
// Frames TashTalk relayed with a good FCS, from any node, quiet or not, since start: a socket
// whose count moves has a LocalTalk network on it.
uint32_t  localtalk_frames(int sock);
// W-1185: when the socket's node last queued a frame for TashTalk, its own or the cable test's
// (localtalk_send_raw), in esp_timer milliseconds; 0 if never. Its frames may reach its own
// socket's sense through a LocalTalk box, so the sense waits them out.
uint32_t  localtalk_tx_at(int sock);
// W-1179: listening. The node sends nothing (TashTalk answers for no node, and lookups, the PAP
// client and echoes send nothing) while frames are still heard and counted, until main lets it
// once LocalTalk has been heard on its socket or W-1185 its sense is held clear: frames sent
// into a printer print as junk, selected or not. Letting it acquires a node as at start. Set before start, the node starts
// listening. The switch is remembered meanwhile, and the cable test's frames
// (localtalk_send_raw) still go.
void      localtalk_set_hold(int sock, int hold);
int       localtalk_holding(int sock);
// W-1185: asked before each of a node's frames, on the LocalTalk task; 0 drops it. Main's
// hold follows on its next check, but a printer switched on in a socket stops the frames as
// soon as its sense holds it. The cable test's frames (localtalk_send_raw) aren't asked.
void      localtalk_set_gate(int (*may_send)(int sock));
// A side running, not quiet, and answering as the ImageWriter (W-1082): what the
// AppleTalk Port card and /api/status call on. A node running for a
// LaserWriter alone is not.
int       localtalk_serving(void);
// The Option Card status word (LT_IW_* in lt_node.h) for SendStatus.
void      localtalk_set_status(int sock, uint16_t status);
// State, counters and the latest lines, for GET /api/lt.
void      localtalk_json(int sock, char *buf, size_t cap);
// W-1186: one port's row for the LocalTalk card, the members of an object without its braces:
// "phase" ("online", "acquiring", "listening", or "off" for a node quiet or not running),
// "node", "on" and "laser" as /api/lt's, and "macs" as /api/lt's. Returns its length.
size_t    localtalk_json_port(int sock, char *buf, size_t cap);
// Bench check: an AEP echo request to `node`; the reply shows in the log.
void      localtalk_echo(int sock, uint8_t node);
// The last PAP job's bytes as captured (up to the capture size), or NULL.
const uint8_t *localtalk_job(size_t *len);

// The cable tester (W-1055) borrows a running node's link: every frame
// TashTalk relays, FCS checked and stripped, also goes to `tap` (on the
// LocalTalk task), and localtalk_send_raw queues a frame for TashTalk as the
// node's own are. A NULL tap removes it.
void localtalk_set_tap(int sock, void (*tap)(void *ctx, const uint8_t *f, size_t n), void *ctx);
void localtalk_send_raw(int sock, const uint8_t *f, size_t n);

// W-1082: what the cables' nodes are for, every side's alike (the page has one switch).
// serve: answer as the ImageWriter (the AppleTalk Port card's switch); laser: a LaserWriter
// is looked for or shared on the cable. Neither is quiet: a job is cut short, TashTalk answers
// for no node and frames are dropped, so the board leaves the Chooser (a node cannot stop once
// started; on again acquires a node as at start). laser alone keeps the node and its ID and
// answers as nothing. Kept for a side started later.
void localtalk_set_roles(int serve, int laser);
// W-1088: what old Macs on a side's cable are offered (lt_node_offer), kept for a
// node started later or anew. Any task.
void localtalk_set_offers(int sock, const lt_offers_t *o);
// W-1186: the printer list's record id behind a side's registration `reg` (lt_offers_t.ids),
// 0 for none. On the LocalTalk task, the lock held (at_relay_t's calls).
int  localtalk_offer_id(int sock, int reg);
// NBP =:type@* to every node on every running side's cable; the answers go to the found
// callback (localtalk_set_found) with the side's link (at_link_of_sock).
void localtalk_lookup(const char *type);
void localtalk_set_found(void (*found)(int link, const nbp_tuple_t *t, const lt_route_t *r));
// W-1098: a node on a cable looked up one name (lt_ops_t's name_check).
void localtalk_set_name_check(void (*check)(int link, const nbp_tuple_t *t));
// The PAP client rides a side's node (NULL detaches). Its callbacks run
// on the LocalTalk task with the lock held: they must not block.
void localtalk_attach(int sock, pap_client_t *c);
// Runs fn under the nodes' lock, for calls into the client (papc_open ...).
// -1 without running it when the side is not running.
int  localtalk_locked(int sock, void (*fn)(void *arg), void *arg);

// 1 if TashTalk drains the side's UART within wait_ms. The node runs whether
// or not a chip is in U7, its bytes held in the UART behind CTS; the cable
// test asks before it sends (W-1063).
int  localtalk_tt_alive(int sock, int wait_ms);
