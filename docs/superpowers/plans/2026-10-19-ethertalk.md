# EtherTalk Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Infinite Mac (through its zone relay) and Basilisk II (through its UDP tunnel) find
"ImageWriter II" in the Chooser and print to board 1 byte for byte, beside the cable LocalTalk
node, which does not change.

**Architecture:** The LocalTalk node (`lt_node.c`) gains an ELAP mode: no ENQ acquisition, an
address set from outside, long DDP headers, an idle-hold limit. A new pure-C EtherTalk link
(`et_link.c`) wraps one such node with AARP and 802.2 SNAP framing, and talks to the node through
a 3-byte pseudo-LLAP header so the node's frame path stays as it is. Two transports (UDP 6066 and
a TLS WebSocket) carry the link's Ethernet frames, owned by one new EtherTalk task. The spool and
print task move out of `localtalk.c` into `at_printer.c`, shared by the cable and both EtherTalk
links, one job at a time under main.c's `s_job_lock`.

**Tech Stack:** ESP-IDF v5.5.1, C (pure-C cores host-tested with `make -C tools/rip test`),
`espressif/esp_websocket_client` + `esp_crt_bundle`, lwIP sockets, Python 3 (`websockets`) for
the desktop peer.

**Spec:** `docs/superpowers/specs/2026-10-19-ethertalk-design.md` (Linear `W-1076 EtherTalk: print
from Infinite Mac and Basilisk II`). Read it first; this plan argues from it.

## Global Constraints

- Cable node behaviour, timers and log lines unchanged; `tools/rip/test/test_localtalk.c`'s
  existing tests pass without edits (new tests are added, none changed).
- `lt_cfg_t` and `lt_ops_t` only grow at the end (tests initialise them positionally).
- Pure-C cores (`lt_*.c`, `et_proto.c`, `et_link.c`, `et_transport.c`) use no FreeRTOS or
  ESP-IDF headers and build in `tools/rip`.
- New files start with `// SPDX-License-Identifier: GPL-3.0-or-later` (Python: `#`).
- Tunnel: UDP port 6066, fixed; tunnel MAC `42 32` + the board's IPv4 address, big-endian.
- Relay: `wss://infinitemac.org/zone/<zone>/websocket`; JSON exactly
  `{"type":"init","macAddress":"aa:bb:cc:dd:ee:ff"}`,
  `{"type":"send","destination":D,"packetArray":[…]}` (D `"AT"`, `"*"` or a lowercase MAC),
  in: `{"type":"receive","packetArray":[…]}`.
- No connection to infinitemac.org while the zone is empty. The zone name never appears in
  logs, `/api/status`, `/api/lt` or `/api/et`.
- AARP: 10 probes 200 ms apart; startup range net `0xFF00`–`0xFFFE`, node 1–253; probes,
  requests and DDP broadcasts to `09:00:07:FF:FF:FF`; responses unicast.
- EtherTalk nodes close a connection that sent no data for 60 000 ms while the status word is 0;
  `idle_hold_ms` 0 (the cable) keeps today's behaviour.
- Bench (AGENTS.md): never OTA while a job prints; `/api/status` `printing=false` first.
- Settings-page and panel changes are shown to Wells on board 1 before they are committed.
- Commits: why-focused messages with `Refs: W-1076`, on branch
  `wells/w-1076-ethertalk-print-from-infinite-mac-and-basilisk-ii`.
- `make -C tools/rip regress` stays byte-identical (nothing here touches the raster path).

## Review Focus

1. **The relay or Wi-Fi drops mid-job.** The job ends abandoned at once (not after timers),
   the next job resyncs the printer, the cable node never notices. Test: Task 3
   `test_link_down_mid_job`.
2. **Two EtherTalk Macs at once** (one tunnel, one zone). The second OpenConn is answered busy
   and prints after the first. Test: Task 3 `test_two_links_one_printer`.
3. **A zone name a person might type** (spaces, `/`, `?`, accents, 33+ characters, empty). Only
   1–32 of `A–Z a–z 0–9 - _ .` are accepted; anything else is a 400 and nothing is saved.
   Test: Task 4 `test_zone_name_ok`.
4. **The board's IP changes with the tunnel on** (DHCP renewal). New tunnel MAC, address map
   dropped, any job abandoned, AppleTalk address acquired again. Test: Task 3 `test_set_mac`.
5. **The board hears its own tunnel broadcasts and garbage** (an 802.3 length that lies,
   frames over 621 bytes, truncated AARP, non-AppleTalk SNAP). Dropped and counted, never
   parsed further. Tests: Task 1 `test_elap_parse_rejects`, Task 3 `test_rx_drops`.

---

### Task 1: EtherTalk packet layouts

**Files:**
- Create: `firmware/components/localtalk/include/et_proto.h`, `firmware/components/localtalk/et_proto.c`
- Create: `tools/rip/test/test_ethertalk.c`
- Modify: `tools/rip/Makefile` (a `build/test_ethertalk` rule beside `build/test_localtalk`, linking
  `et_proto.c et_link.c et_transport.c lt_proto.c lt_node.c`; Tasks 3–4 add their sources to it)
- Modify: `firmware/components/localtalk/CMakeLists.txt` (add the new sources as each task lands)

**Interfaces:**
- Produces (`et_proto.h`, includes `lt_proto.h`):

```c
#define ETH_ALEN        6
#define ELAP_HDR        22      // dst 6, src 6, 802.3 length 2, LLC AA AA 03, SNAP 5
#define ELAP_MIN_FRAME  60      // frames are padded to this (no FCS)
#define ELAP_MAX_FRAME  (ELAP_HDR + DDP_LONG_HDR + DDP_MAX_DATA)   // 621
#define AARP_LEN        28
enum { ET_DDP = 1, ET_AARP = 2 };
extern const uint8_t ET_BCAST[ETH_ALEN];     // ff:ff:ff:ff:ff:ff
extern const uint8_t ET_AT_MCAST[ETH_ALEN];  // 09:00:07:ff:ff:ff

typedef struct { const uint8_t *dst, *src; int proto; const uint8_t *data; size_t len; } elap_t;
size_t elap_build(uint8_t *out, size_t cap, const uint8_t dst[ETH_ALEN],
                  const uint8_t src[ETH_ALEN], int proto, const uint8_t *data, size_t len);
int    elap_parse(const uint8_t *f, size_t n, elap_t *e);

enum { AARP_REQUEST = 1, AARP_RESPONSE = 2, AARP_PROBE = 3 };
typedef struct {
    uint16_t fn;
    uint8_t  src_hw[ETH_ALEN]; uint16_t src_net; uint8_t src_node;
    uint8_t  dst_hw[ETH_ALEN]; uint16_t dst_net; uint8_t dst_node;
} aarp_t;
int    aarp_parse(const uint8_t *p, size_t n, aarp_t *a);
size_t aarp_build(uint8_t out[AARP_LEN], const aarp_t *a);
```

- [ ] **Step 1: Write the failing tests** in `test_ethertalk.c` (style of `test_localtalk.c`,
  `test_util.h`, `TEST_END`):
  - `test_elap_round_trip`: `elap_build` of 13 DDP bytes to `ET_AT_MCAST` gives 60 bytes; bytes
    12–13 = `00 15` (8 + 13); bytes 14–21 = `AA AA 03 08 00 07 80 9B`; tail zero-padded;
    `elap_parse` gives `proto == ET_DDP`, `len == 13`, the same data. Same for AARP with SNAP
    `AA AA 03 00 00 00 80 F3`.
  - `test_elap_parse_rejects`: −1 for an Ethernet II type (bytes 12–13 = `08 00`), a length
    field above `n - 14`, a length under 8, `n < ELAP_HDR`, `n > ELAP_MAX_FRAME`, and a SNAP
    header that is neither of the two.
  - `test_aarp`: a probe for `0xFF23.77` from `02 00 00 00 00 01` builds to 28 bytes starting
    `00 01 80 9B 06 04 00 03`, source AppleTalk address `00 FF 23 4D`, destination hw zeros;
    parses back equal. `aarp_parse` returns −1 for hw type ≠ 1, protocol ≠ `0x809B`, lengths
    ≠ 6/4, function 0 or 4, or `n < 28`.
- [ ] **Step 2: Run** `make -C tools/rip build/test_ethertalk && tools/rip/build/test_ethertalk`.
  Expected: build fails (no `et_proto.h`).
- [ ] **Step 3: Implement** `et_proto.c`. `elap_build` returns 0 when it does not fit `cap`
  or `len > DDP_LONG_HDR + DDP_MAX_DATA`.
- [ ] **Step 4: Run** the same command. Expected: `ok  test/test_ethertalk.c`.
- [ ] **Step 5: Commit** `et_proto.[ch]`, the test, the Makefile and CMakeLists.

### Task 2: The LocalTalk node learns ELAP mode and the idle hold

**Files:**
- Modify: `firmware/components/localtalk/include/lt_node.h`, `firmware/components/localtalk/lt_node.c`
- Test: `tools/rip/test/test_localtalk.c` (new tests only)

**Interfaces:**
- Produces:

```c
enum { LT_LINK_LLAP = 0, LT_LINK_ELAP = 1 };
// lt_cfg_t gains, at the end:  uint8_t link;  uint32_t idle_hold_ms;
// lt_node_t gains, at the end: uint8_t link;  uint32_t idle_hold_ms;  uint8_t router_logged;
// lt_pap_t gains:              uint64_t idle_since;
// ELAP only: the address is ours (from AARP). Goes ONLINE; a changed address while a PAP
// connection is open ends that job abandoned first. Logs "address %u.%u is ours".
void lt_node_set_address(lt_node_t *n, uint16_t net, uint8_t node, uint64_t now);
```

- ELAP behaviour (LLAP mode untouched):
  - `lt_node_init` with `link == LT_LINK_ELAP`: phase `LT_ACQUIRING`, `node`/`net` 0, no
    candidate, no `ops.claim` call (`claim` may be NULL in ELAP); `lt_node_tick` sends nothing
    until `lt_node_set_address`.
  - Frames arrive as `{dst node, src node, LLAP_LONG}` + a long DDP datagram (Task 3 builds
    them). After `ddp_parse`, a datagram whose `dst.net` is neither 0 nor `n->net` is dropped.
  - `on_rtmp` in ELAP logs "a router is on this network; not supported" once and never
    changes `n->net`.
  - `lt_node_echo` sends a long header in ELAP.
- Idle hold (any link with `idle_hold_ms > 0`): `idle_since` is set at OpenConn, on every Data
  packet, and on every tick while `n->status != 0`. When `now - idle_since >= idle_hold_ms`
  the node sends CloseConn and closes with `pap_close(n, "the Mac sent nothing", 1)`.

- [ ] **Step 1: Write the failing tests** (helpers beside `online()` that build pseudo-LLAP
  long frames from a Mac at `0xFF10.12`, socket `0xFD`):
  - `test_elap_init_quiet`: ELAP init, 2000 ms of ticks: `nsent == 0`, `claimed == -1`.
  - `test_elap_lookup`: after `lt_node_set_address(&n, 0xFF23, 77, t)`, an NBP LkUp
    `=:ImageWriter@*` to `{0, 0xFF, SOCK_NIS}` is answered with one `LLAP_LONG` frame whose DDP
    dst is `0xFF10.12:0xFD` and src `0xFF23.77:SOCK_NIS`, tuple socket `LT_SOCK_SLS`.
  - `test_elap_other_net`: the same LkUp addressed to net `0xFF99` node 77 gets no reply.
  - `test_elap_rtmp`: an RTMP datagram from socket 1 leaves `n.net == 0xFF23`, one log line.
  - `test_idle_hold` (ticks every 10 ms): ELAP, `idle_hold_ms = 60000`, OpenConn at `t0`, no
    data: open after the tick at `t0 + 59990`; after the tick at `t0 + 60000` a CloseConn ATP
    request has been sent, `job_eof == 1`, `job_normal == 1`, `!n.pap.open`. Second run:
    status `LT_IW_PAPER_OUT` from `t0 + 10000`, 0 again from `t0 + 100000`: open after the tick
    at `t0 + 159980`, closed by `t0 + 160000`. Third run: one Data packet at `t0 + 30000`: open
    at `t0 + 89990`, closed at `t0 + 90000`.
  - `test_cable_no_idle_hold`: the `online()` cable node (idle hold 0) after OpenConn and no
    data is still open at `t0 + 60000` and `t0 + 119990` (today's 120 s connection timer).
- [ ] **Step 2: Run** `make -C tools/rip build/test_localtalk && tools/rip/build/test_localtalk`.
  Expected: build fails (`lt_node_set_address` undeclared).
- [ ] **Step 3: Implement** in `lt_node.c` / `lt_node.h`.
- [ ] **Step 4: Run** it again. Expected: `ok  test/test_localtalk.c`, every old test included.
- [ ] **Step 5: Commit.**

### Task 3: The EtherTalk link: AARP, the address map, framing

**Files:**
- Create: `firmware/components/localtalk/include/et_link.h`, `firmware/components/localtalk/et_link.c`
- Test: `tools/rip/test/test_ethertalk.c`

**Interfaces:**
- Consumes: Task 1 (`elap_*`, `aarp_*`), Task 2 (`LT_LINK_ELAP`, `lt_node_set_address`,
  `lt_node_rx`, `lt_node_tick`, `lt_node_pap_abort`).
- Produces:

```c
#define ET_MAP_SLOTS     16
#define AARP_PROBES      10
#define AARP_PROBE_MS    200
#define AARP_REQ_TRIES   3
#define AARP_REQ_MS      250

typedef struct {
    const char *object, *type;
    uint16_t    status;
    uint8_t     mac[ETH_ALEN];
    uint16_t    hint_net;    // last boot's address; used when net is in 0xFF00..0xFFFE
    uint8_t     hint_node;   // and node in 1..253, else random
    uint32_t    seed;        // nonzero
    uint32_t    idle_hold_ms;
} et_cfg_t;

typedef struct {
    void *ctx;
    void   (*send)(void *ctx, const uint8_t *frame, size_t n);   // one padded Ethernet frame
    void   (*log)(void *ctx, const char *line);
    int    (*job_begin)(void *ctx);
    void   (*job_data)(void *ctx, const uint8_t *d, size_t n);
    size_t (*job_room)(void *ctx);
    void   (*job_end)(void *ctx, int normal);
    void   (*claimed)(void *ctx, uint16_t net, uint8_t node);    // may be NULL
} et_ops_t;

typedef struct { uint16_t net; uint8_t node; uint8_t hw[ETH_ALEN]; uint64_t at; } et_map_t;

typedef struct {
    et_ops_t  ops;
    lt_node_t node;                  // LT_LINK_ELAP; its ops are this link's thunks
    uint8_t   mac[ETH_ALEN];
    uint16_t  net; uint8_t nd;       // the candidate, then ours
    uint8_t   probes, clash;
    uint64_t  probe_at;
    uint32_t  rng;
    et_map_t  map[ET_MAP_SLOTS];
    uint8_t   pend[ELAP_MAX_FRAME];  // one datagram waiting on AARP, as the node sent it
    size_t    pend_len; uint8_t pend_tries; uint64_t pend_at;
    struct { uint32_t rx, tx, bad, own, aarp_in, aarp_out, unresolved; } st;
} et_link_t;

void et_link_init(et_link_t *l, const et_cfg_t *cfg, const et_ops_t *ops, uint64_t now);
void et_link_rx(et_link_t *l, const uint8_t *frame, size_t n, uint64_t now);
void et_link_tick(et_link_t *l, uint64_t now);
void et_link_set_mac(et_link_t *l, const uint8_t mac[ETH_ALEN], uint64_t now);
void et_link_down(et_link_t *l, uint64_t now);   // ends any job abandoned (lt_node_pap_abort)
```

- Behaviour:
  - **Acquire:** candidate from the hint, else net `0xFF00 + rnd % 0xFF`, node `1 + rnd % 253`.
    Probe to `ET_AT_MCAST` every `AARP_PROBE_MS`, `AARP_PROBES` times. A clash is any AARP
    request, response or probe naming the candidate from another MAC, or a DDP frame from the
    candidate's address with another MAC: new random candidate, probes restart. After the
    10th probe's gap with no clash: `lt_node_set_address`, `ops.claimed`.
  - **Answer:** online, an AARP request or probe for our address gets one response, unicast
    to the asker's hw, with our hw and address.
  - **Map:** learned from AARP requests and responses (never probes) and gleaned from every
    DDP frame received (src MAC ↔ DDP src net.node); full map evicts the oldest `at`.
  - **Out** (the node's thunk `send`): the frame is pseudo-LLAP + long DDP. DDP dst node
    `0xFF` → `ET_AT_MCAST`. Mapped → that hw. Else the datagram goes to `pend` (replacing any
    older one, counted `unresolved`), and an AARP request goes out, retried `AARP_REQ_TRIES`
    times `AARP_REQ_MS` apart; a response flushes `pend`; giving up drops it.
  - **In:** drop (and count) unparseable frames, frames whose src is our MAC (`own`), and
    frames whose dst is not our MAC, `ET_BCAST` or `ET_AT_MCAST`. DDP frames go to the node
    only once online, as `{dst node, src node, LLAP_LONG}` + the datagram.
  - `et_link_set_mac`: new MAC, map and `pend` cleared, any job abandoned, acquisition
    restarts (`node` back to `LT_ACQUIRING` until the new address).

- [ ] **Step 1: Write the failing tests** (fake `send` records frames; fake printer as in
  `test_localtalk.c`):
  - `test_acquire_clean`: seed 1, no hint, ticks every 10 ms to 2500 ms: exactly 10 frames
    to `ET_AT_MCAST`, each AARP probe from our MAC for one address in the startup range;
    `claimed` called once with it; `node.phase == LT_ONLINE`.
  - `test_acquire_hint_and_clash`: hint `0xFF23.77`: probes name it; an AARP response for
    `0xFF23.77` from another MAC → later probes name a different address; claimed is not
    `0xFF23.77`.
  - `test_answers_aarp`: online at `0xFF23.77` (hint, no clash); an AARP request for it from
    `B2 0A 00 01 6E 00` → one response frame to that MAC, fn 2, src hw ours, src addr
    `0xFF23.77`; a request for `0xFF23.78` → nothing.
  - `test_lookup_from_mac`: an NBP LkUp from MAC `B2 0A 00 01 6E 00`, DDP src
    `0xFF10.12:0xFD`, to `ET_AT_MCAST` / DDP `0.255:2` → the reply frame's Ethernet dst is that
    MAC (gleaned, no AARP request sent) and its DDP dst is `0xFF10.12:0xFD`.
  - `test_unresolved_then_aarp`: `lt_node_echo(&l.node, 99)` to unmapped `0xFF23.99` → an AARP
    request for it, no echo yet; an AARP response from MAC M → the echo goes to M.
  - `test_rx_drops`: our own MAC as src → `own` counted, node untouched; dst another unicast
    MAC → dropped; a truncated AARP → `bad` counted.
  - `test_pap_fixture_ethertalk`: `fixtures/pap-iw701-page.bin` sent by a Mac over the link
    exactly as `test_pap_fixture` does over the cable (OpenConn, 88-byte Data per SendData,
    CloseConn): printer bytes equal the fixture, `job_eof == 1`, `job_normal == 1`.
  - `test_link_down_mid_job`: half the fixture in, `et_link_down` → `job_eof == 1`,
    `job_normal == 0` at once, `!l.node.pap.open`.
  - `test_two_links_one_printer`: two links on one fake printer whose `job_begin` returns busy
    while a job is held; link B's OpenConn while A prints → OpenConnReply result `0xFFFF`;
    after A's CloseConn, B's next OpenConn → result `0x0000`.
  - `test_set_mac`: online with a job open; `et_link_set_mac` → job ended abandoned, map
    empty, `node.phase == LT_ACQUIRING`, the next probe's src hw is the new MAC.
- [ ] **Step 2: Run** `make -C tools/rip build/test_ethertalk && tools/rip/build/test_ethertalk`.
  Expected: build fails (`et_link.h` missing).
- [ ] **Step 3: Implement** `et_link.c` (thunks: the node's `lt_ops_t.ctx` is the link; its
  `log` and `job_*` forward to `l->ops`; `claim` is NULL).
- [ ] **Step 4: Run** it again and `make -C tools/rip test`. Expected: all `ok`.
- [ ] **Step 5: Commit.**

### Task 4: Transport rules: the tunnel's MAC and IP, the relay's JSON

**Files:**
- Create: `firmware/components/localtalk/include/et_transport.h`, `firmware/components/localtalk/et_transport.c`
- Test: `tools/rip/test/test_ethertalk.c`

**Interfaces:**
- Produces:

```c
#define ET_TUNNEL_PORT 6066
#define ET_ZONE_MAX    32
#define ET_PONG_LEN    27
void   et_tunnel_mac(uint32_t ip, uint8_t mac[ETH_ALEN]);          // ip in host order
int    et_tunnel_dest(const uint8_t dst[ETH_ALEN], uint32_t *ip);   // 1 + ip, or 0 = drop
void   et_zone_mac(const uint8_t base[ETH_ALEN], uint8_t mac[ETH_ALEN]);
int    et_zone_name_ok(const char *zone);                           // 1 or 0
size_t et_zone_init_json(char *out, size_t cap, const uint8_t mac[ETH_ALEN]);
size_t et_zone_send_json(char *out, size_t cap, const uint8_t *frame, size_t n);
int    et_zone_parse(const char *msg, size_t len, uint8_t *frame, size_t cap, size_t *n);
int    et_zone_pong(const uint8_t *frame, size_t n, const uint8_t mac[ETH_ALEN],
                    uint8_t pong[ET_PONG_LEN]);
```

- Rules: `et_tunnel_dest` gives the IP inside a `42 32 …` MAC, `0xFFFFFFFF` for `ET_BCAST` or
  `ET_AT_MCAST`, else 0. `et_zone_mac` is `B2` + `base[1..5]` (Infinite Mac sends unicast only to `B2…` MACs; this was `0A` until the bench found it). `et_zone_parse` returns 1 for
  a `"receive"` with a valid `packetArray` (every element 0–255, at most `cap`), 0 for any other
  `"type"`, −1 for anything malformed; it accepts JSON whitespace. `et_zone_pong` answers a
  26-byte frame whose bytes 14–21 are `pingPING`: dst = the ping's src, src = `mac`, length
  field `00 0D`, `pingPONG!`, the ping's 4 time bytes.

- [ ] **Step 1: Write the failing tests:**
  - `test_tunnel`: `et_tunnel_mac(0x0A000173)` = `42 32 0A 00 01 73`; dest of that MAC → 1,
    `0x0A000173`; of `ET_AT_MCAST` and `ET_BCAST` → 1, `0xFFFFFFFF`; of `0A 57 57 00 00 01` → 0.
  - `test_zone_name_ok`: 1 for `"wells-printer"`, `"a"`, `"A.b_c-9"`, 32 `x`; 0 for `""`,
    33 `x`, `"has space"`, `"a/b"`, `"a?b"`, `"café"`, `"%2F"`.
  - `test_zone_json`: init for `0A 57 57 00 00 01` is exactly
    `{"type":"init","macAddress":"0a:57:57:00:00:01"}`; a 15-byte frame to `ET_AT_MCAST` gives
    `"destination":"AT"`, to `ET_BCAST` `"*"`, to `B2 A9 D6 35 FA 4A` `"b2:a9:d6:35:fa:4a"`,
    and `packetArray` lists the 15 bytes in decimal, comma-separated, no spaces; 0 when `cap`
    is short.
  - `test_zone_parse`: `{"type":"receive","packetArray":[1,2,255]}` → 1, `n == 3`; with spaces
    after commas → 1; `{"type":"other"}` → 0; `256` in the array, a missing `]`, a
    non-number, or more bytes than `cap` → −1.
  - `test_zone_pong`: the 26-byte ping our relay probe sent (src `0A 57 57 00 00 01`) → 1 and
    the 27-byte pong above; a 26-byte frame without `pingPING` → 0.
- [ ] **Step 2: Run** the test binary. Expected: build fails.
- [ ] **Step 3: Implement** `et_transport.c` (by hand; no cJSON in the cores).
- [ ] **Step 4: Run** `make -C tools/rip test`. Expected: all `ok`.
- [ ] **Step 5: Commit.**

### Task 5: One printer for every link

**Files:**
- Create: `firmware/components/localtalk/include/at_printer.h`, `firmware/components/localtalk/at_printer.c`
- Modify: `firmware/components/localtalk/localtalk.c` (spool, print task, job capture and the
  job ops move out), `firmware/components/localtalk/include/localtalk.h`
  (`localtalk_printer_t.begin` gains the link), `firmware/main/main.c` (`lt_print_begin`,
  `lt_print_end`, `lt_status`, `lt_job`)

**Interfaces:**
- Produces:

```c
typedef enum { AT_LINK_CABLE = 0, AT_LINK_TUNNEL, AT_LINK_ZONE } at_link_t;
// localtalk_printer_t.begin becomes: int (*begin)(void *ctx, at_link_t link);
typedef struct {
    at_link_t link;
    void (*abort)(void *ctx);            // the printer gave up: lt_node_pap_abort under its lock
    void (*note)(const char *line);      // the link's log, for lt_print_end's summary line
    void *ctx;
} at_owner_t;
esp_err_t at_printer_init(const localtalk_printer_t *p);   // idempotent; first caller's p wins
int    at_job_begin(const at_owner_t *o);                  // 0, or 1 busy (draining or begin said so)
void   at_job_data(const at_owner_t *o, const uint8_t *d, size_t n);
size_t at_job_room(const at_owner_t *o);
void   at_job_end(const at_owner_t *o, int normal);
int    at_job_link(void);                                  // the link holding the printer, or -1
void   at_printer_note(const char *line);                  // to the holder's note, else dropped
const uint8_t *at_job_bytes(size_t *len);                  // the last job's capture (was localtalk_job)
void   at_printer_json(char *buf, size_t cap);             // "job":{…},"print":{…} members
```

- `localtalk.c` keeps its own `op_job_*` thunks calling `at_job_*` with its `at_owner_t`
  (`AT_LINK_CABLE`), so the node and `/api/lt`'s JSON keys are unchanged. `localtalk_job()`
  becomes a wrapper over `at_job_bytes`.
- main.c: `lt_print_begin(void *ctx, at_link_t link)` sets `s_last_job` to `"LocalTalk job"`,
  `"Basilisk II job"` or `"Infinite Mac job"`; `lt_print_end` logs with `at_printer_note`;
  `lt_status(ps_state_t st, at_link_t link)` returns `LT_IW_BUSY` while printing unless
  `at_job_link() == link`; every call site passes its link. `lt_print_*` and `lt_status` move
  out of the `CONFIG_AIRWRITER_PIC_*` guard so EtherTalk can use them on any build.

- [ ] **Step 1: Run** `make -C tools/rip test` and `idf.py build` before touching anything.
  Expected: all `ok`, a clean build (the baseline).
- [ ] **Step 2: Move the code** as above. No behaviour change for the cable.
- [ ] **Step 3: Run** `make -C tools/rip test` (unchanged tests) and, from `firmware/`,
  `. ~/esp/esp-idf/export.sh && idf.py build`. Expected: all `ok`, clean build, no new warnings.
- [ ] **Step 4: Commit.** (The cable is re-proven on paper in Task 9.)

### Task 6: The EtherTalk task: tunnel, zone relay, settings, API

**Files:**
- Create: `firmware/components/localtalk/include/ethertalk.h`, `firmware/components/localtalk/ethertalk.c`
- Create: `firmware/components/localtalk/idf_component.yml` (`espressif/esp_websocket_client`)
- Modify: `firmware/components/localtalk/CMakeLists.txt` (`REQUIRES`/`PRIV_REQUIRES`: lwip,
  esp_netif, mbedtls for the bundle, esp_websocket_client)
- Modify: `firmware/main/main.c` (start, status, IP changes, panel events, the web hooks)
- Modify: `firmware/components/webui/include/webui.h`, `firmware/components/webui/webui.c`
  (routes `GET /api/et`, `GET`+`POST /api/et/settings`; hooks `et_json`, `et_settings_get`,
  `et_settings_set` in `webui_cfg_t`, NULL answers 404)

**Interfaces:**
- Consumes: Tasks 3–5.
- Produces:

```c
typedef struct {
    const char *object, *type;
    uint16_t    tunnel_status, zone_status;
    localtalk_printer_t printer;
    void      (*on_event)(lt_event_t ev);   // LT_EV_LOOKUP, LT_EV_NEW_NODE only
} ethertalk_cfg_t;
esp_err_t ethertalk_start(const ethertalk_cfg_t *cfg);    // once, after the station has an IP
void      ethertalk_ip_changed(uint32_t ip);              // host order; from IP_EVENT_STA_GOT_IP
void      ethertalk_set_status(uint16_t tunnel_status, uint16_t zone_status);
esp_err_t ethertalk_apply(int tunnel, const char *zone);  // save to NVS "et" and apply now;
                                                          // ESP_ERR_INVALID_ARG if !et_zone_name_ok
void      ethertalk_settings(int *tunnel, char *zone, size_t cap);
void      ethertalk_json(char *buf, size_t cap);          // per link: on, state, address,
                                                          // mac, counters, last error, events;
                                                          // never the zone name
```

- NVS namespace `et`: `tunnel` u8 (default 1), `zone` string (default empty), `tnet`/`tnode`
  and `znet`/`znode` (each link's last address, written from `claimed`).
- One task, `"ethertalk"`, priority 4, owns both `et_link_t`s under its own mutex: `select()`
  on the UDP socket (bound to `INADDR_ANY:6066`, `SO_BROADCAST`) with a 20 ms timeout, then
  the zone queue, then `et_link_tick` on both, then the panel events (the `heard`/`replies`
  diff that `lt_task` does).
- Tunnel out: `et_tunnel_dest` → `sendto` (limited broadcast `255.255.255.255`). Tunnel off:
  socket closed, link down, nothing sent or answered.
- Zone: with a valid zone, an `esp_websocket_client` (`crt_bundle_attach =
  esp_crt_bundle_attach`, buffer 4096, text frames), `init` sent on every
  `WEBSOCKET_EVENT_CONNECTED`. Data events are reassembled by `payload_offset`; pings are
  answered with `et_zone_pong` from the client's task; other `receive` frames go to a queue of
  16 fixed 640-byte slots (full: dropped and counted). Out: `et_zone_send_json` and
  `esp_websocket_client_send_text` with a 200 ms timeout (fail: dropped and counted).
  Disconnect: `et_link_down`; reconnect after 1 s, doubling to 60 s, back to 1 s after a
  connection that lasted 60 s. Zone emptied: client stopped and destroyed, link down.
- main.c: start it when the station gets its first IP (not in setup-AP mode) with the queue
  name, `CONFIG_AIRWRITER_LT_NBP_TYPE`, both printer hooks; `ethertalk_ip_changed` on every
  later `IP_EVENT_STA_GOT_IP`; `ethertalk_set_status(lt_status(st, AT_LINK_TUNNEL),
  lt_status(st, AT_LINK_ZONE))` where `localtalk_set_status` is called; `on_event` is the
  existing `lt_event` path (Mac blip, activity). Both EtherTalk links use
  `idle_hold_ms = 60000`; the zone MAC is `et_zone_mac` of the station MAC.
- API: `GET /api/et` → `ethertalk_json`. `POST /api/et/settings` body
  `{"tunnel":true|false,"zone":"…"}`, either key may be left out (an empty zone turns the relay
  off); 400 `"zone: 1 to 32 of A-Z a-z 0-9 - _ ."` when `et_zone_name_ok` fails, nothing saved;
  200 with the saved settings. `GET /api/et/settings` → `{"tunnel":true,"zone":"…"}`.

- [ ] **Step 1: Write it**, then from `firmware/`: `idf.py build`. Expected: clean build.
- [ ] **Step 2: OTA board 1** (bench rules: Wells holds prints, `/api/status` `printing=false`):
  `curl -m 180 --data-binary @build/airwriter.bin http://10.0.1.115/update`; wait for
  `"trial":false`.
- [ ] **Step 3: Check the tunnel:** `GET /api/et` shows the tunnel link online at a `0xFFxx`
  address with MAC `42:32:0a:00:01:73`, and `GET /api/lt` is as before.
- [ ] **Step 4: Check the zone:** `curl -X POST -d '{"zone":"airwriter-bench-1"}'
  http://10.0.1.115/api/et/settings`; `GET /api/et` shows the zone link connected and online
  with no zone name in it; the design's relay probe (`relay_probe.py airwriter-bench-1`, in
  the session scratchpad) gets a pong from the board's zone MAC. `{"zone":"a b"}` → 400.
- [ ] **Step 5: Commit.**

### Task 7: The settings-page section

**Files:**
- Modify: `tools/webui/body.html`, `tools/webui/app.js`, `tools/webui/style.css` if needed;
  regenerate `firmware/components/webui/webui_page.h` with `tools/webui/mkheader.py` (never by hand)

**Interfaces:**
- Consumes: Task 6's `GET /api/et`, `GET`/`POST /api/et/settings`.
- The section follows the W-1072 rules in `body.html`'s comment: heading, a checkbox
  ("Print from Basilisk II on this network"), a field ("Infinite Mac zone", placeholder
  "off"), then one row: a say-line from `/api/et` (each link: listening / connected / a Mac
  seen / the last error) left, a `save` key right. The wording is a draft for Wells.

- [ ] **Step 1: Write it**, regenerate the header, `idf.py build`. Expected: clean.
- [ ] **Step 2: OTA board 1** (bench rules) and open http://10.0.1.115/ in the app's browser:
  the section renders on a desktop width and at phone width (375 px), save works, a bad zone
  shows the 400's message.
- [ ] **Step 3: Wells looks at it on board 1.** Change the copy and layout until he OKs it.
- [ ] **Step 4: Commit** (only after his OK).

### Task 8: A desktop EtherTalk peer for the bench

**Files:**
- Create: `tools/ethertalk/etpeer.py`, `tools/ethertalk/README.md`

**Interfaces:**
- `python3 tools/ethertalk/etpeer.py tunnel lookup` / `… tunnel print FILE`
  / `… zone ZONE lookup` / `… zone ZONE print FILE`. Tunnel binds UDP 6066 (it cannot run
  beside Basilisk II on the same Mac; it says so when the bind fails). Zone needs
  `websockets` (`pip install websockets` into a venv).
- It acquires a startup-range address by AARP probes, answers AARP for itself, looks up
  `=:ImageWriter@*`, prints the reply, and for `print` does SendStatus, OpenConn (its flow
  quantum 1), answers each SendData with up to 512 bytes, then CloseConn; it prints the
  file's byte count and crc32, and the board's OpenConnReply result (`busy` shows as such).

- [ ] **Step 1: Write it.**
- [ ] **Step 2: Run** `python3 tools/ethertalk/etpeer.py tunnel print fixtures/pap-iw701-page.bin`
  (a real Mac driver's ImageWriter stream, 11968 bytes). Expected: the page prints and the
  crc32 in `/api/et`'s "job printed" event equals the peer's.
- [ ] **Step 3: Same over the zone** (`zone airwriter-bench-1 print …`). Expected: equal.
- [ ] **Step 4: Commit.**

### Task 9: Bench on board 1

No new code unless a step fails (then: a failing host test first, the fix, back here).

- [ ] **Step 1: Infinite Mac.** In the app's browser,
  `https://infinitemac.org/1996/System%207.5.3?appleTalk=airwriter-bench-1`: AppleTalk
  control panel on Ethernet; Chooser → AppleTalk ImageWriter → "ImageWriter II"; print a
  page from SimpleText. Expected: it prints; `/api/lt/job` holds the job and its crc32 matches
  the event line. Note whether the driver was on the disk (the design's early check).
- [ ] **Step 2: Basilisk II on this Mac** (`~/Applications/BasiliskII/BasiliskII --config
  <bench prefs>` with `udptunnel true`, `udpport 6066`, ROM and disk image copied from a
  Macintosh Mini when one is on): the same print. If no Mini is reachable, ask Wells for the
  ROM and disk image; this step blocks the merge.
- [ ] **Step 3: Busy both ways.** An iPhone (or `ipptool` `fixtures/checker.urf`) print during
  an EtherTalk job waits and then prints crc32 `8ee84844`; an EtherTalk OpenConn during an IPP
  job is answered busy and prints after.
- [ ] **Step 4: Drop mid-job.** Stop the peer's zone print halfway (kill it, or clear the zone):
  `/api/et` shows the job abandoned at once; the next print resyncs the printer and is clean.
- [ ] **Step 5: The cable.** Wells prints a page from the SE/30 over J4 (DIP mode 2). Expected:
  as before W-1076, its crc32 in `/api/lt` events.
- [ ] **Step 6: The Macintosh Mini**, if one is on: switch its Basilisk II prefs to the tunnel by
  hand (`udptunnel true`, keep a copy of the prefs) and print. Record the result either way.
- [ ] **Step 7: Panel.** Wells watches a Chooser lookup and a job from Infinite Mac on the panel
  (the Mac blip, the Apple stripes). His OK, or changes, before merge.

### Task 10: Docs and the record

**Files:**
- Modify: `docs/SPEC.md` (new §3.13 AppleTalk: the cable node as built, EtherTalk, both
  transports, the shared printer, the idle hold, the settings and `/api/et`; the §3.1
  component table gains `localtalk`)
- Modify: `AGENTS.md` (State: EtherTalk bullet, web UI routes, `Not built` without EtherTalk
  except on the radio; Open work: W-1076 with what was benched, "no release with the zone
  relay until Wells has told Mihai Parparita", W-1077 next; Layout: `tools/ethertalk/`)
- Modify: `docs/traps.md` (anything the bench taught: own-broadcast loopback, relay quirks,
  Basilisk II prefs on macOS)
- Linear: a comment on W-1076 with the bench results; status In Review when the branch is
  ready for Wells.

- [ ] **Step 1: Write the docs.**
- [ ] **Step 2: Run** `make -C tools/rip test`, `make -C tools/rip regress`, `make -B -C tools/ipp test`,
  `idf.py build`, and `gitleaks git` on the branch. Expected: all pass, no leaks.
- [ ] **Step 3: Commit and push the branch.** Merge only on Wells's "ship it".
