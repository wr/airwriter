// SPDX-License-Identifier: GPL-3.0-or-later
//
// The LocalTalk node, pure C: frames and the clock in, frames and log lines
// out. No FreeRTOS, no UART -- localtalk.c feeds it what TashTalk relays and
// sends what it asks for; the host tests in tools/rip drive it with a fake
// clock. Every timer is evaluated in lt_node_tick() against the caller's
// `now` (milliseconds, uint64 so it never wraps).
//
// What it does, in order:
//  1. LLAP: acquires a server node ID (128-254) by sending lapENQs to a
//     candidate and moving on whenever anything answers or uses it, then hands
//     the ID to TashTalk (ops.claim), which answers ENQ and RTS from then on.
//  2. NBP: answers LkUp for object:type@* -- the queue name and the type the
//     Chooser's driver looks up -- with the server listening socket (SLS).
//  3. PAP over ATP, as Apple's ImageWriter II/LQ LocalTalk Option Card does
//     it: answers SendStatus with the card's two-byte status word, accepts
//     one connection at a time (OpenConn), reads the job with SendData, keeps
//     the connection tickled, and answers CloseConn. The job's bytes -- raw
//     ImageWriter commands -- go to ops.job_data, and the next SendData waits
//     until ops.job_room has a whole read's worth of space: that is the
//     back-pressure from a slow or paused printer to the Mac.
// Also: AEP echo (socket 4), and the network number from RTMP when a router
// speaks.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include "lt_proto.h"

struct pap_client;              // pap_client.h (W-1082)

#define LT_SOCK_SLS   0x81      // PAP server listening socket, registered with NBP
#define LT_SOCK_CONN  0x82      // the one connection's ATP responding socket
#define LT_SOCK_PAPC    0x83    // W-1082: the PAP client's connection socket
#define LT_SOCK_PAPC_ST 0x84    // and the one its SendStatus goes from
// W-1088: registrations beside the node's own: printers offered to Macs here
// (listening sockets 0x85..0x88) and the AirWriter marker, whose type is
// LT_AIRWRITER_TYPE and which never opens.
#define LT_REG_MAX        5
#define LT_AIRWRITER_TYPE "AirWriter"
#define LT_AIRWRITER_MS   600000
#define LT_FLOW       8         // our flow quantum: 512-byte packets per SendData
#define LT_READ_MAX   (LT_FLOW * PAP_QUANTUM)   // the most one SendData can bring

// LLAP node acquisition. Appendix B has servers send 120 back-to-back ENQs,
// 200 us apart, and TailTalk sends 8; tashrouter spaces its 8 by 250 ms. A
// UART round trip sits between us and any ACK, so these are spread over a
// second, which also gives a node busy for a moment the chance to answer.
#define LT_ENQ_TRIES  20
#define LT_ENQ_GAP_MS 50

// The Option Card's status word (Apple TN NW20; bits confirmed on a real
// ImageWriter II by TailTalk). It goes on the wire as a two-byte Pascal
// string, low byte first. Ready with a black ribbon is 0x0000; a deselected
// printer reads 0x0008, out of paper 0x002A. LT_IW_BUSY is for the board
// printing someone else's job: a Mac told "busy" on its own open connection
// sits on it and never sends (bench, 2026-10-15). TailTalk never sets it.
#define LT_IW_BUSY      0x8000
#define LT_IW_RIBBON    0x0080   // colour ribbon fitted
#define LT_IW_FEEDER    0x0040
#define LT_IW_PAPER_OUT 0x0020
#define LT_IW_COVER     0x0010
#define LT_IW_OFFLINE   0x0008
#define LT_IW_JAM       0x0004
#define LT_IW_FAULT     0x0002
#define LT_IW_ACTIVE    0x0001

// PAP timers (Inside AppleTalk ch. 10).
#define LT_PAP_CONN_TIMEOUT_MS 120000   // nothing from the other end: tear down
#define LT_PAP_TICKLE_MS        60000   // half the connection timer
#define LT_PAP_SENDDATA_RETRY_MS 15000
// A workstation's own read of us (its SendData) is answered empty once it has
// sent nothing for this long, so the read does not time out; TailTalk's PAP
// server, tested on hardware, uses 2 s. Apple's AppleTalk ImageWriter 7.0.1
// never reads (bench, 2026-10-15); a LaserWriter-style client does.
#define LT_PAP_READ_IDLE_MS      2000
// An OpenConn from the Mac that holds the open connection, which has sent no
// data for this long, means it gave that one up (the Mac kept tickling a dead
// connection and retried OpenConn 18 times against "busy" on the bench). The
// old connection is closed and the new one accepted. Another Mac still gets busy.
#define LT_PAP_STALE_MS         10000
// A Mac that has sent part of a job, then nothing for this long while we were
// asking (a read out) and the printer was ready, has stopped: the bench Mac
// once sat on a finished page for 10+ minutes, tickling, never closing, and
// holding the printer from every other job. The connection is closed (normal:
// the bytes print if they end between commands). A Mac waiting out paper-out
// or off line is waiting on the status word and is never cut off.
#define LT_PAP_QUIET_MS        300000

typedef struct {               // how to answer whoever sent a datagram
    lt_addr_t addr;            // their socket
    uint8_t   via;             // LLAP destination: them, or the router that relayed it
    uint8_t   is_long;
} lt_route_t;

typedef struct {
    void *ctx;
    // One LLAP frame (dst, src, type, data), without its FCS.
    void (*send)(void *ctx, const uint8_t *frame, size_t n);
    // The node ID is ours: tell TashTalk to answer ENQ/RTS for it (and
    // remember it for next boot). 0 = answer for nothing (re-acquiring).
    void (*claim)(void *ctx, uint8_t node);
    // A line for the log and the web view.
    void (*log)(void *ctx, const char *line);
    // The printer, for a PAP job; each may be NULL (accept and discard).
    // job_begin takes the printer for a new connection: 0, or nonzero for
    // busy (another job holds it), which OpenConnReply reports. job_data gets
    // the job's bytes in order, never more than job_room last said. job_end
    // ends the job once: normal is 1 when the Mac ended it (CloseConn or EOF),
    // 0 when it was abandoned (timed out, superseded, or lt_node_pap_abort).
    int    (*job_begin)(void *ctx);
    void   (*job_data)(void *ctx, const uint8_t *d, size_t n);
    size_t (*job_room)(void *ctx);
    void   (*job_end)(void *ctx, int normal);
    // W-1082: a tuple from an NBP LkUp-Reply to lt_node_lookup (a Macintosh
    // name's answer is the name table's, not this), and the route to its
    // socket. May be NULL.
    void   (*found)(void *ctx, const nbp_tuple_t *t, const lt_route_t *r);
    // W-1088: a registration other than the node's own (lt_node_register)
    // is opened: 0 to accept, nonzero for busy. NULL: always busy. The
    // connection then feeds job_data and job_end as the node's own does, and
    // lt_node_pap_reg says which registration it is.
    int    (*pap_open)(void *ctx, int reg, uint64_t now);
    // Its SendStatus answer as a Pascal string into pstr; 0 for the node's
    // own status word. May be NULL.
    size_t (*reg_status)(void *ctx, int reg, uint8_t *pstr, size_t cap);
} lt_ops_t;

typedef uint8_t lt_name_t[NBP_NAME_MAX + 1];   // a Pascal NBP name, MacRoman

typedef struct {
    const char *object;     // NBP object: the queue name (UTF-8; non-ASCII becomes '?')
    const char *type;       // NBP type the Chooser looks up
    uint16_t    status;     // the Option Card status word to start with
    uint8_t     hint;       // the node ID to try first (last boot's), 0 = random
    uint32_t    seed;       // nonzero random seed for candidate IDs
    lt_name_t  *names;      // room for LT_SEEN_SLOTS Macintosh names (below), or
                            // NULL to learn none; the ESP keeps it in PSRAM
} lt_cfg_t;

// The link under the node. LLAP is the cable, as above. ELAP (W-1076) is
// EtherTalk through et_link.c: no LLAP node acquisition (AARP is et_link's;
// lt_node_set_address hands over the result), frames arrive as a pseudo-LLAP
// header {dst node, src node, LLAP_LONG} and a long DDP datagram and leave the
// same way, a datagram for another network is dropped, and RTMP is only noted.
enum { LT_LINK_LLAP = 0, LT_LINK_ELAP = 1 };

// The idle hold (W-1076): a connection that sends no data for idle_hold_ms
// while the status word is 0 is closed (CloseConn, the job ends normal, so
// what came prints). On the relay a Mac can be anyone with the zone name or
// an abandoned browser tab, holding the printer from an iPhone. A Mac waiting
// out paper-out or off line is waiting on the status word and is never cut
// off. 0 (the cable) leaves today's timers alone.
#define LT_IDLE_HOLD_MS 60000

typedef enum { LT_ACQUIRING = 0, LT_ONLINE } lt_phase_t;

#define LT_XO_SLOTS 4
// The nodes heard lately, for the settings page (W-1079): a node that has
// sent anything but an ENQ, and when. Full, the one quiet longest makes room.
// On ELAP a node is its network and node: every Mac picks its own network in
// the startup range. On the cable the network is left out.
#define LT_SEEN_SLOTS 16
#define LT_SEEN_MS    600000      // the page's "connected": heard in the last 10 minutes
// name: its Macintosh name while the node is unchanged, or NULL.
typedef struct { uint8_t node; uint32_t ago_s; const uint8_t *name; } lt_seen_t;

// Macintosh names. System 7 and later register the name from Sharing Setup
// with NBP as <name>:Workstation. A node given room for names (cfg->names)
// asks each node it hears for =:Workstation@*, a LkUp sent straight to that
// node's NIS: LT_NAME_DELAY_MS after first hearing it, then while unanswered
// after LT_NAME_RETRY_MS, twice that, and so on up to LT_NAME_REFRESH_MS, and
// every LT_NAME_REFRESH_MS once answered, while it stays heard. On the cable a
// Mac opens AppleTalk early in its startup and registers its name a minute or
// more later; it looks the name up first (<name>:Workstation@*, from it), and
// its next ask follows LT_NAME_DELAY_MS after the last of those. A name not
// answered for LT_NAME_TRIES asks running, or a node back after a quiet spell,
// is forgotten: the ID may be another Mac's. A Mac without a name (System 6,
// or one never named) shows its address.
#define LT_NAME_TYPE       "Workstation"
#define LT_NAME_DELAY_MS   1000
#define LT_NAME_RETRY_MS   5000
#define LT_NAME_TRIES      3
#define LT_NAME_REFRESH_MS 600000
#define LT_NAME_GAP_MS     250       // between any two asks
#define LT_XO_PKT   (ATP_HDR + 4 + 256)   // the biggest response we cache: OpenConnReply

typedef struct {               // a sent XO response, kept for duplicate requests
    uint8_t    used, src_sock;
    lt_route_t to;
    uint16_t   tid;
    uint64_t   expires;
    uint16_t   len;
    uint8_t    atp[LT_XO_PKT];
} lt_xo_t;

typedef struct {
    uint8_t    open, connid;
    uint8_t    reg;            // W-1088: the registration it was opened on (0 the node's own)
    lt_route_t ws;             // the workstation's ATP responding socket
    uint64_t   heard, tickled;
    uint16_t   tickle_tid;
    // Our read (SendData to the workstation).
    uint16_t   seq, rd_tid;
    uint8_t    rd_want;        // response packets still wanted; 0 = no read out
    uint8_t    rd_eof, done;   // done: EOF delivered, nothing more to read
    uint8_t    ended;          // job_end has been called for this connection
    uint64_t   rd_sent;
    uint16_t   rd_len[LT_FLOW];
    uint8_t    rd_buf[LT_FLOW][PAP_QUANTUM];
    // The workstation's read of us (its SendData), answered empty once it
    // has been quiet LT_PAP_READ_IDLE_MS, or with EOF once our side is done.
    uint8_t    ws_read, eof_sent;
    uint16_t   ws_read_tid, ws_read_seq;
    lt_route_t ws_read_from;
    uint16_t   ws_answered_seq;  // the last one answered, for PAP-level retransmits
    uint8_t    ws_answered_eof;
    uint64_t   wrote;            // when the workstation last sent us Data
    uint64_t   waiting;          // since when it has owed us data with the printer ready
    uint32_t   bytes;
    uint64_t   idle_since;       // the idle hold's clock: open, Data, or a nonzero status
} lt_pap_t;

typedef struct {
    uint32_t frames, for_us, lookups, replies, opens, busy, statuses, jobs, dup_suspect, echoes;
    uint32_t ws_reads, ws_answers;   // the Mac's reads of us, and our answers
} lt_stats_t;

typedef struct {
    lt_ops_t   ops;
    uint8_t    obj[NBP_NAME_MAX + 1], type[NBP_NAME_MAX + 1];
    uint16_t   status;
    lt_phase_t phase;
    uint8_t    node;           // ours when ONLINE; the candidate while ACQUIRING
    uint8_t    clash;
    uint16_t   enq_sent;
    uint32_t   candidates;     // IDs tried, including the current one
    uint64_t   enq_at;
    uint16_t   net;            // from RTMP; 0 until a router speaks
    uint32_t   rng;
    uint16_t   tid;            // next ATP TID for our requests
    lt_xo_t    xo[LT_XO_SLOTS];
    lt_pap_t   pap;
    lt_stats_t st;
    // The last LkUp logged, so a Chooser asking every second logs once.
    uint8_t    last_lkup[3 * (NBP_NAME_MAX + 1) + 2];
    uint64_t   last_lkup_at;
    uint8_t    heard[32];      // bit n: node n has sent a frame
    struct {
        uint16_t net;          // ELAP only
        uint8_t  node;         // 0: free
        uint8_t  tries;        // name asks since the last answer
        uint32_t at_s;         // last heard
        uint64_t ask_at;       // when its name is next asked
    } seen[LT_SEEN_SLOTS];
    lt_name_t *names;          // names[i] is seen[i]'s, or NULL: names off
    uint64_t   name_at;        // the last name ask, for LT_NAME_GAP_MS
    uint8_t    name_id;
    uint64_t   echo_at;        // when lt_node_echo last asked
    uint8_t    link;           // LT_LINK_*
    uint32_t   idle_hold_ms;
    uint8_t    router_logged;  // ELAP: a router has been noted once
    // W-1082: answer as the ImageWriter (NBP for obj:type, PAP on the SLS)
    // only while serve is set; a node can run for a LaserWriter alone.
    uint8_t    serve;
    uint8_t    lkup_id;        // the last lt_node_lookup's NBP id
    struct pap_client *client; // the PAP client riding this node, or NULL
    // W-1088: more names on this node, each with its own listening socket:
    // printers offered to Macs here, and the AirWriter marker. reg[0] is
    // unused (the node's own is obj:type above).
    struct { uint8_t obj[NBP_NAME_MAX + 1], type[NBP_NAME_MAX + 1], sls; } reg[LT_REG_MAX];
    // Nodes heard answering as an AirWriter (=:AirWriter@*), for ten minutes.
    struct { uint16_t net; uint8_t node; uint64_t at; } aw[16];
    uint64_t   last_now;
} lt_node_t;

void lt_node_init(lt_node_t *n, const lt_cfg_t *cfg, const lt_ops_t *ops, uint64_t now);
// The same for an ELAP node (W-1076), with its idle hold; cfg->hint is unused.
void lt_node_init_elap(lt_node_t *n, const lt_cfg_t *cfg, const lt_ops_t *ops,
                       uint32_t idle_hold_ms, uint64_t now);
// One frame as TashTalk relayed it, FCS already checked and stripped.
void lt_node_rx(lt_node_t *n, const uint8_t *f, size_t len, uint64_t now);
// Timers: enquiries while acquiring, PAP retries, tickles and timeouts.
void lt_node_tick(lt_node_t *n, uint64_t now);
// The printer gave up on the job (a stall past the limit): tell the Mac with
// CloseConn and end the job abandoned. Nothing without an open connection.
void lt_node_pap_abort(lt_node_t *n, uint64_t now);
// Send an AEP echo request to `node` (socket 4); its reply is logged. A bench
// check that frames get out and back. Does nothing until the node is online.
void lt_node_echo(lt_node_t *n, uint8_t node, uint64_t now);
// The Option Card status word, e.g. when the printer goes offline.
void lt_node_set_status(lt_node_t *n, uint16_t status);
// ELAP only: AARP made net.node ours, and the node goes online; node 0 means
// the address is gone (the link's MAC changed) and the node waits for the next.
// A change while a connection is open ends that job abandoned first.
void lt_node_set_address(lt_node_t *n, uint16_t net, uint8_t node, uint64_t now);
// The nodes heard in the last within_ms, newest first, at most max of them.
size_t lt_node_seen(const lt_node_t *n, uint64_t now, uint32_t within_ms, lt_seen_t *out, size_t max);
// The Macintosh name of net.node (net is ignored on the cable), or NULL. It
// points into the node's table: valid until the node next changes.
const uint8_t *lt_node_name(const lt_node_t *n, uint16_t net, uint8_t node);

// W-1082, the laser printers design's "AppleTalk roles": a link runs when
// the ImageWriter or a LaserWriter needs it, and answers as the ImageWriter
// (its NBP name, SendStatus and OpenConn on the SLS) only where the
// ImageWriter is shared. On at init. Turning it off leaves an open
// connection alone: abort it first (lt_node_pap_abort).
void     lt_node_set_serve(lt_node_t *n, int serve);
// W-1088: registration reg (1..LT_REG_MAX-1) as obj:type (Pascal) on listening
// socket sls; obj NULL clears it. 0, or -1 for a bad reg.
int      lt_node_register(lt_node_t *n, int reg, const uint8_t *obj, const uint8_t *type, uint8_t sls);
// Which registration the open connection is on (0 the node's own).
int      lt_node_pap_reg(const lt_node_t *n);
// 1 for a node heard answering as an AirWriter in the last ten minutes, or this one.
int      lt_node_is_airwriter(const lt_node_t *n, uint16_t net, uint8_t node);
// NBP LkUp of =:type@* to every node on the link (the AppleTalk multicast on
// EtherTalk); the answers reach ops.found. Nothing until the node is online.
void     lt_node_lookup(lt_node_t *n, const char *type, uint64_t now);
// For the PAP client (pap_client.h): an ATP packet from one of our sockets,
// and a fresh transaction ID.
void     lt_node_send_atp(lt_node_t *n, const lt_route_t *to, uint8_t src_sock, uint8_t ctl,
                          uint8_t bitmap, uint16_t tid, const uint8_t user[4],
                          const uint8_t *data, size_t len);
uint16_t lt_node_next_tid(lt_node_t *n);
// The PAP client riding this node: ATP to LT_SOCK_PAPC and LT_SOCK_PAPC_ST
// goes to it, and lt_node_tick ticks it. NULL detaches.
void     lt_node_attach(lt_node_t *n, struct pap_client *c);
