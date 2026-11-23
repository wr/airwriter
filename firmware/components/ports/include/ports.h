// SPDX-License-Identifier: GPL-3.0-or-later
//
// W-1179: the board's sockets and the job each one does. Rev A's are fixed: J2 is the printer
// port, J4 the AppleTalk port. On rev 2026.11 each socket has a printer line and a TashTalk of
// its own (J2's U10, J4's U7), so either takes either job (W-1176): the printer job holds the
// socket's PIC in reset and drives its transceiver from the ESP32; the LocalTalk job leaves the
// line to the PIC. AirWriter LC has one socket, J2, and no TashTalk. Socket 0 is J2 (port 1),
// socket 1 J4 (port 2).
//
// This is only the hardware hand-over. Who decides a socket's job, and what follows from it
// (which printer line is selected, where LocalTalk runs, the printer list), is main's.
#pragma once
#include <stdint.h>
#include "pic_prog.h"

#define PORTS_MAX 2

typedef enum { PORT_JOB_NONE = 0, PORT_JOB_PRINTER = 1, PORT_JOB_LOCALTALK = 2 } port_job_t;

typedef struct {
    int        uart;       // the printer job's UART
    int        tx, rx;     // the printer job's data out and in
    int        de;         // its transceiver's driver enable, -1: always on (rev A's J2)
    int        hski;       // the printer's HSKo arrives here (J2's: the UART's CTS)
    int        hsko_drv;   // drives the printer's HSKi
    pic_pins_t pic;        // the socket's TashTalk, mclr -1 for none
    int        lt_cts;     // TashTalk's CTS out
    uint8_t    can_print, can_lt;
} port_sock_t;

// Builds the table for this board (lc: rev 2026.11's BOARD_LC strap) and gives each socket
// its first job: jobs[i] where the socket can do it, else the one it can. The pins are handed
// over as ports_hw does, except the printer line, which serial_out takes when it starts.
void              ports_init(int lc, const port_job_t jobs[PORTS_MAX]);
int               ports_count(void);
const port_sock_t *ports_sock(int i);
// Rev 2026.11's full board: each socket takes either job.
int               ports_either(void);
port_job_t        ports_job(int i);
// The first socket doing `job`, -1 for none.
int               ports_with(port_job_t job);
// Hands socket i's pins to `job`: for the printer job its PIC goes into reset (CLK, DAT and
// CTS pulled down), its driver is enabled and serial_out takes the line; for LocalTalk the
// line is let go, the driver enable is an input and the PIC runs. The caller holds the job
// lock, has stopped whatever used the socket (LocalTalk moved or parked, no printer selected
// on it), and then starts what uses it. A no-op for a job the socket can't do.
void              ports_hw(int i, port_job_t job);
const char       *ports_job_name(port_job_t j);   // "printer", "localtalk", "none"
