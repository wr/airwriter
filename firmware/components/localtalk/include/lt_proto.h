// SPDX-License-Identifier: GPL-3.0-or-later
//
// AppleTalk packet layouts the LocalTalk node needs: LLAP, DDP, NBP, ATP and
// PAP. Parse and build only, pure C, no state -- lt_node.c holds the state and
// the host tests in tools/rip drive both. Layouts are Inside AppleTalk, 2nd ed.
// (cited in docs/reference/README.md): LLAP ch. 1 and appendix B, DDP ch. 4, NBP ch. 7, ATP
// ch. 9, PAP ch. 10. Multi-byte fields are big-endian on the wire.
#pragma once
#include <stdint.h>
#include <stddef.h>

// ----------------------------------------------------------------- LLAP
// A frame is dst, src, type, then data. The FCS (CRC-CCITT as in HDLC, low
// byte first) follows on the wire; everything here works on frames without it.
#define LLAP_SHORT   0x01        // data: DDP with a short header
#define LLAP_LONG    0x02        // data: DDP with a long header
#define LLAP_ENQ     0x81
#define LLAP_ACK     0x82
#define LLAP_RTS     0x84
#define LLAP_CTS     0x85
#define LLAP_BCAST   0xFF
#define LLAP_HDR     3
#define LLAP_MAX     603         // the largest frame without its FCS (605 with)

// The FCS to append to frame p[0..n): low byte first on the wire.
uint16_t llap_fcs(const uint8_t *p, size_t n);
// 1 when f[0..n) -- a frame with its FCS -- checks out (residue 0xF0B8).
int llap_fcs_ok(const uint8_t *f, size_t n);

// ----------------------------------------------------------------- DDP
#define DDP_SHORT_HDR 5
#define DDP_LONG_HDR  13
#define DDP_MAX_DATA  586

enum { DDP_RTMP = 1, DDP_NBP = 2, DDP_ATP = 3, DDP_AEP = 4 };
enum { SOCK_RTMP = 1, SOCK_NIS = 2, SOCK_AEP = 4 };

typedef struct {
    uint16_t net;      // 0 on a network with no router: "this network"
    uint8_t  node;
    uint8_t  sock;
} lt_addr_t;

typedef struct {
    uint8_t        is_long, hops, type;
    uint8_t        llap_dst, llap_src;   // the LLAP header around it
    lt_addr_t      dst, src;             // short header: nets 0, nodes from LLAP
    const uint8_t *data;                 // into the frame
    size_t         len;
} ddp_t;

// 0 with *d filled for a DDP frame (LLAP header first, no FCS), or -1: not a
// DDP type, truncated, a length field that disagrees, or a bad long-header
// checksum. A frame longer than its DDP length is fine; the tail is ignored.
int ddp_parse(const uint8_t *f, size_t n, ddp_t *d);

// LLAP + DDP into out. Short header when !is_long (dst/src nets ignored).
// Returns the frame length, or 0 when it does not fit in cap or DDP_MAX_DATA.
size_t ddp_build(uint8_t *out, size_t cap, uint8_t llap_dst, uint8_t llap_src, int is_long,
                 const lt_addr_t *dst, const lt_addr_t *src, uint8_t type,
                 const uint8_t *data, size_t len);

// The long header's checksum over p[0..n) (from the destination network on).
uint16_t ddp_cksum(const uint8_t *p, size_t n);

// ----------------------------------------------------------------- NBP
enum { NBP_BRRQ = 1, NBP_LKUP = 2, NBP_LKUP_REPLY = 3, NBP_FWDREQ = 4 };
#define NBP_NAME_MAX 32
#define NBP_WILD_ALL  '='        // a whole field of "=" matches anything
#define NBP_WILD_PART 0xC5       // MacRoman approx-equals: zero or more characters

typedef struct {
    lt_addr_t addr;
    uint8_t   enumerator;
    uint8_t   obj[NBP_NAME_MAX + 1];    // Pascal strings: [0] is the length
    uint8_t   type[NBP_NAME_MAX + 1];
    uint8_t   zone[NBP_NAME_MAX + 1];
} nbp_tuple_t;

typedef struct {
    uint8_t     fn, count, id;
    nbp_tuple_t t;                      // the first tuple
} nbp_t;

// 0 with the header and first tuple, or -1 when truncated or a name is too long.
int nbp_parse(const uint8_t *d, size_t n, nbp_t *out);
// 1 when Pascal-string pattern matches name: "=" alone matches anything, one
// NBP_WILD_PART matches any run of characters, and case is folded (ASCII).
int nbp_match(const uint8_t *pattern, const uint8_t *name);
// One-tuple NBP packet. Returns its length, or 0 when it does not fit.
size_t nbp_build(uint8_t *out, size_t cap, uint8_t fn, uint8_t id, const nbp_tuple_t *t);
// A C string (UTF-8) into a Pascal NBP name (MacRoman): each character MacRoman
// has becomes its byte, any other one '?', cut at NBP_NAME_MAX.
void nbp_name(uint8_t *dst, const char *src);
// The other way, for the web page: a Pascal NBP name (MacRoman) as a C string
// in UTF-8, control characters as '?', cut at a whole character when cap is
// short. Returns the string's length.
size_t nbp_utf8(char *out, size_t cap, const uint8_t *name);

// ----------------------------------------------------------------- ATP
#define ATP_HDR       8
#define ATP_FN_MASK   0xC0
#define ATP_TREQ      0x40
#define ATP_TRESP     0x80
#define ATP_TREL      0xC0
#define ATP_XO        0x20
#define ATP_EOM       0x10
#define ATP_STS       0x08
#define ATP_TREL_MASK 0x07       // XO TReq: 0 = 30 s release timer ... 4 = 8 min
#define ATP_MAX_DATA  578

typedef struct {
    uint8_t        ctl;          // function, XO, EOM, STS, TRel timer
    uint8_t        bitmap;       // TReq: packets wanted; TResp: sequence number
    uint16_t       tid;
    uint8_t        user[4];
    const uint8_t *data;
    size_t         len;
} atp_t;

int    atp_parse(const uint8_t *d, size_t n, atp_t *a);
size_t atp_build(uint8_t *out, size_t cap, uint8_t ctl, uint8_t bitmap, uint16_t tid,
                 const uint8_t user[4], const uint8_t *data, size_t len);

// ----------------------------------------------------------------- PAP
// PAP rides in ATP's user bytes: [0] ConnID, [1] function, [2..3] the SendData
// sequence number, or [2] the EOF flag in Data.
enum {
    PAP_OPEN_CONN = 1, PAP_OPEN_CONN_REPLY, PAP_SEND_DATA, PAP_DATA, PAP_TICKLE,
    PAP_CLOSE_CONN, PAP_CLOSE_CONN_REPLY, PAP_SEND_STATUS, PAP_STATUS,
};
#define PAP_RESULT_OK   0x0000
#define PAP_RESULT_BUSY 0xFFFF
#define PAP_QUANTUM     512      // bytes per Data packet, at most
