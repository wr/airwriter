// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1179: working out what each rev 2026.11 socket has plugged into it, so either socket takes
// a printer or LocalTalk with nothing to set. Pure C, so the host tests in tools/rip run it;
// main feeds it what each socket shows, every half second, and applies what it says.
//
// What it goes by. Nothing is sent to find out: every signal is heard.
//   - A printer on a socket doing LocalTalk: its handshake (HSKo) held ready, or a clock on it
//     (a QuickDraw LaserWriter). A LocalTalk connector leaves that wire unconnected, which
//     reads as not ready. Taken only while the socket has heard no LocalTalk frame for a while,
//     so a live network is never dropped for a printer.
//   - LocalTalk on a socket doing the printer job: its frames, at 230.4 kbit/s, reach a UART
//     set for 9600 as framing errors and breaks that nobody asked for. A printer answers only
//     when asked, at the speed it is asked at. That is only a hint, so the socket takes
//     LocalTalk on trial: if TashTalk hears no frame there within PD_TRIAL_MS, the socket goes
//     back to the printer job, and the hint is ignored for PD_BACKOFF_MS. A printer that
//     shows on it mid-trial (a clock, or its handshake coming up) ends the trial at once.
//   - A socket set to a job (/api/general's port1, port2) is never changed here, and a quiet
//     socket keeps the job it has.
// What it allows. A LocalTalk socket sends AppleTalk only once it has heard LocalTalk there
// (pd_may_send): frames sent into a printer print as junk, selected or not, since a deselected
// ImageWriter keeps them for when it is selected again (board 1, 2026-11-21). Until then the
// node listens. What was heard is kept across a restart (pd_start), and dropped when the
// socket changes job or a printer shows on it.
// W-1186: each socket is worked out by itself. Both may do LocalTalk at once (two networks),
// both the printer job, or one each; nothing a socket shows changes the other.
// W-1185: rev 2026.11's port sense relaxes the listening. Each socket's sense line is low while
// a serial printer with power is plugged in, ready or not: it holds its transmitter at idle
// mark, pin 8 high or pin 5 low. It is high for an empty socket, a LocalTalk device or box
// between frames, and a printer with no power. Held high, a LocalTalk socket sends at once,
// heard or not; held low, it never sends, heard or set to LocalTalk or not, and the socket goes
// to the printer job (PD_WHY_SENSE). The handshake and the clock count as before. A board with
// no sense (rev A, the LC) passes PD_SENSE_NONE, and the listening is as it was.
#pragma once
#include <stdint.h>

#define PD_SOCKS          2
#define PD_READY_MS       2000     // a printer's handshake held ready this long
#define PD_QUIET_MS       30000    // a LocalTalk socket that heard no frame this long is quiet
#define PD_JUNK_N         3        // framing errors or breaks ...
#define PD_JUNK_MS        5000     // ... within this long: LocalTalk, perhaps
#define PD_TRIAL_MS       20000    // a trial's wait for a frame (a Chooser looks every second)
#define PD_BACKOFF_MS     600000   // no new trial on a socket for 10 min after one failed
#define PD_BOOT_MS        1000     // a socket heard before a restart sends once its line has
                                   // shown no printer this long

// W-1185: what a socket's port sense holds (pd_sense_look).
enum {
    PD_SENSE_NONE = 0,             // no sense on the socket, or no level held yet
    PD_SENSE_CLEAR,                // held high: nothing with power holds the line
    PD_SENSE_PRINTER,              // held low: a serial printer with power (or a Mac's serial port)
};
#define PD_SENSE_LOOK_MS  50       // the line is looked at this often, its edges counted between
#define PD_SENSE_HOLD_MS  40       // a level counts once it has held this long with no edge at
                                   // all: a LocalTalk frame, 21 ms at most, pulses the line
#define PD_SENSE_TX_MS    100      // and high not within this long of the socket's own node
                                   // queuing a frame (a LocalTalk box may join the pairs):
                                   // TashTalk's 2 KB of queue is 71 ms on the wire

// What a socket does and shows, this tick. Jobs are ports.h's (1 printer, 2 LocalTalk).
typedef struct {
    uint8_t job;
    uint8_t fixed;         // set to a job: never changed here
    uint8_t can_print, can_lt;
    uint8_t ready;         // the printer job's handshake line reads ready (HSKo)
    uint8_t clocked;       // a clock on it
    uint8_t frames;        // LocalTalk job: TashTalk heard a frame since the last tick
    uint8_t junk;          // printer job: framing errors and breaks since the last tick
    uint8_t silent;        // printer job: no printer has answered on it (a Mac on a direct
                           // cable may hold the handshake ready, W-1179 review)
    uint8_t sense;         // W-1185: PD_SENSE_*, as pd_sense_look holds it now
} pd_in_t;

typedef struct {
    uint32_t ready_since;  // when the handshake last went ready (0: not ready)
    uint32_t frame_at;     // when this socket last heard a LocalTalk frame (0: never)
    uint32_t job_at;       // when it took its job
    uint32_t junk_at[PD_JUNK_N];   // the last PD_JUNK_N ticks with junk, newest first
    uint32_t backoff_until;
    uint8_t  trial;        // its LocalTalk is a trial
    uint8_t  job;          // the job last seen
    uint8_t  heard;        // LocalTalk heard on it in its present LocalTalk job
    uint8_t  heard_boot;   // ... only before the restart, so far
    uint32_t sensed_at;    // W-1185: when its sense first held a printer (0: it doesn't)
} pd_sock_t;

typedef struct {
    pd_sock_t s[PD_SOCKS];
    int       n;
    uint32_t  boot_at;
} pd_t;

typedef enum {
    PD_WHY_NONE,
    PD_WHY_HANDSHAKE,      // a printer's handshake on a LocalTalk socket
    PD_WHY_CLOCK,          // a QuickDraw LaserWriter's clock on a LocalTalk socket
    PD_WHY_TRAFFIC,        // LocalTalk traffic on a printer socket: the trial
    PD_WHY_NO_FRAME,       // the trial heard nothing: back as it was
    PD_WHY_SENSE,          // W-1185: a printer with power on a LocalTalk socket, by its sense
} pd_why_t;

typedef struct {
    int      sock;         // the socket to change
    uint8_t  job;          // to this job
    pd_why_t why;
} pd_act_t;

void        pd_init(pd_t *d, int n, uint32_t now_ms);
// Socket i's job and whether LocalTalk had been heard on it, as saved before the restart.
void        pd_start(pd_t *d, int i, uint8_t job, int heard);
// One tick. idle: no job holds the board (nothing changes mid-job, but what the sockets show
// is still taken in). Returns 1 with *act when a socket should change; the caller applies it
// (main's sock_job_set) and, if it did, calls pd_applied. Junk and frames are counts since the
// last tick; ready and clocked are levels now.
int         pd_tick(pd_t *d, const pd_in_t in[], uint32_t now_ms, int idle, pd_act_t *act);
void        pd_applied(pd_t *d, const pd_act_t *act, uint32_t now_ms);
// Whether socket i has heard LocalTalk in its present LocalTalk job, for main to save.
int         pd_heard(const pd_t *d, int i);
// Whether socket i may send AppleTalk now, after a tick. Never where its sense holds a printer.
// Otherwise: it does LocalTalk and is set to; or it shows no printer (a handshake held ready
// with no frame since, or a clock) and either its sense is held clear or it has heard
// LocalTalk. One clear by its sense, or heard only before the restart, waits PD_BOOT_MS from
// pd_init first, for the handshake and the clock to show.
int         pd_may_send(const pd_t *d, const pd_in_t in[], int i, uint32_t now_ms);
const char *pd_why_name(pd_why_t w);

// W-1185: one socket's port sense, looked at every PD_SENSE_LOOK_MS or so. A level counts once
// it has held PD_SENSE_HOLD_MS with no edge, and high only with the socket's node quiet too;
// until another does, the last one held stands, so a printer answering or a frame on the line
// changes nothing. The node's own frames leave edges like any other; a printer plugged in
// while it sends holds low regardless, and stops it (localtalk_set_gate).
typedef struct {
    uint32_t run_at;       // when the present run began
    uint8_t  run;          // its level, 1 high
    uint8_t  on;           // a run has begun
    uint8_t  held;         // PD_SENSE_*
} pd_sense_t;

// One look: the line's level now (1 high), whether it moved since the last look (an edge
// counted), and when the socket's own node last queued a frame (0: never). Returns what is
// held. A zeroed pd_sense_t holds PD_SENSE_NONE.
uint8_t     pd_sense_look(pd_sense_t *s, int level, int moved, uint32_t tx_at, uint32_t now_ms);
// Whether the cable test may send its frames out of a LocalTalk socket whose sense holds
// `sense`: never into a printer with power. A test cable from the printer socket holds it so
// too, that socket's transmitter idling at mark, and shows itself by carrying the printer
// socket's handshake across (joined).
int         pd_test_may_send(uint8_t sense, int joined);
const char *pd_sense_name(uint8_t sense);
