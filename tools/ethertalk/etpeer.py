#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An EtherTalk Mac in software, for testing AirWriter without an emulator.

It speaks AppleTalk Phase 2 over the two ways the board takes it (W-1076):
Basilisk II's UDP tunnel on this network, or an Infinite Mac zone on
infinitemac.org. It takes an address with AARP, looks the printer up with NBP
as the Chooser does, and prints a file over PAP as the AppleTalk ImageWriter
driver does (OpenConn, the board's SendData answered with Data, CloseConn).

    python3 tools/ethertalk/etpeer.py tunnel lookup
    python3 tools/ethertalk/etpeer.py zone ZONE chooser LaserWriter
    python3 tools/ethertalk/etpeer.py zone ZONE print-ps "LaserWriter II NT" query.ps page.ps
    python3 tools/ethertalk/etpeer.py tunnel status "LaserWriter II NT" 120
    python3 tools/ethertalk/etpeer.py tunnel print fixtures/pap-iw701-page.bin
    python3 tools/ethertalk/etpeer.py tunnel named "Wells's iMac" 30
    python3 tools/ethertalk/etpeer.py zone ZONE lookup
    python3 tools/ethertalk/etpeer.py zone ZONE print FILE
    python3 tools/ethertalk/etpeer.py tunnel laserwriter "LaserWriter II NT" [--hex-only] [--out DIR]
                                         [--also "Office LaserWriter"] [--addr NET.NODE]
                                         [--pages N] [--no-check] ...
    python3 tools/ethertalk/etpeer.py tunnel print-sw "StyleWriter" ColorStyleWriter2400AT page.sw [--lpstyl]
    python3 tools/ethertalk/etpeer.py tunnel sw-queue "StyleWriter" ColorStyleWriter2400AT
    python3 tools/ethertalk/etpeer.py tunnel sw-rename "StyleWriter" ColorStyleWriter2400AT "New name"

`named` registers a Macintosh name, as System 7's Sharing Setup does
(<name>:Workstation), looks the printer up so the board hears it, and answers
the board's name lookups for the given seconds.

`status` asks NAME:LaserWriter for its status string once a second, as Print
Monitor does, and prints each new one: through the board's relay, a real
printer's own words.

`print-ps NAME FILE [FILE ...]` sends each file as its own job over one PAP
connection, as a Mac's LaserWriter driver does (a query, then the document,
W-1095): the job and its EOF, the printer's output read to its EOF and shown,
then the next job; CloseConn after the last. `--close-early` closes as soon as
the last job's EOF is out, before the printer's, as the SE/30's driver does.

`laserwriter` is a fake LaserWriter (W-1082): it registers NAME:LaserWriter,
answers PAP as a PostScript LaserWriter does (the printer pulls, quantum 8;
its status; its output read back; EOF), answers the board's query job from a
LaserWriter II NT's values, and saves each job as job-N.ps in --out (rendered
to job-N.png at 300 dpi when Ghostscript is installed). Like a real one it
takes any number of jobs on a connection: after its EOF it pulls the next.
Lines on stdin fault it: `busy SECONDS`, `paper` (out of paper), `jam`, `ok`,
`error` (the next job gets a PostScript error), `drop` (it answers nothing
more), `password` (a password other than 0: the start page can't be set,
W-1099). --hex-only answers the binary probe as a Level 1 machine that cannot
take binary would. Coming up it checks no one has its name, as a LaserWriter
switched on does (W-1098: five lookups of NAME:LaserWriter@*, 2 s apart;
--no-check skips them). --addr tries the address it had first, as a printer
does; --pages is the page count it gives the query (4242), one more a job.
Started again under another name with the same --addr and as many pages or
more, it is the board's printer renamed. It prints a start page at power-on
until the board's start page job turns it off.
A tunnel fake starts itself again when this Mac's address changes.
`--also NAME` adds another fake LaserWriter, at its own AppleTalk address, on
the same tunnel (one Mac can bind the tunnel's port once); its jobs go in
DIR/NAME. Stdin's faults go to the first.
`pipe laserwriter NAME` is the same over stdin and stdout, frames each a
big-endian u16 length and the frame (test_interop.py).

`print-sw NAME TYPE FILE` prints to a StyleWriter behind Apple's StyleWriter
network adapter, or AirWriter standing in for one (W-1193), as a Mac's
StyleWriter driver does: over ADSP it asks NAME:TYPE to print (attention
000B), takes the adapter's call back on its data socket, and talks to the
printer through that connection: a reset, `?`, the setup letters, then FILE's
rects and blocks, each command one ADSP message, paced by the printer's own
`1`, `2` and `B` and the adapter's 0006. FILE is a page as `rip --printer
stylewriter --model M --out-sw FILE` writes it. `--lpstyl` does it in lpstyl's
order: attentions without Ack Request, the control connection left open, a
Forward Reset at the end. `sw-queue` asks the adapter what it is printing, and
`sw-rename` gives it a new name, as the Chooser's setup does. As the Mac's
side it is strict: an adapter that numbers a packet wrong, acknowledges the
wrong count (an EOM counts one), sends more than the window it was given or
ends an attention's answer with EOM stops it with an error that says so.

The file must be an ImageWriter stream: the board prints it as it is. The
tunnel binds UDP 6066, so it cannot run beside Basilisk II on the same
computer. The zone needs `pip install websockets` (a venv is fine).
"""
import asyncio, json, os, pathlib, random, shutil, socket, struct, subprocess, sys, time, zlib

AT_MCAST = bytes.fromhex("090007ffffff")
BCAST = b"\xff" * 6
SNAP_DDP = bytes.fromhex("aaaa03080007809b")
SNAP_AARP = bytes.fromhex("aaaa0300000080f3")
TUNNEL_PORT = 6066
NIS, AEP, SLS_TYPE, WS_TYPE = 2, 4, b"ImageWriter", b"Workstation"
DDP_NBP, DDP_ATP = 2, 3
TREQ, TRESP, TREL, XO, EOM = 0x40, 0x80, 0xC0, 0x20, 0x10
PAP_OPEN, PAP_OPEN_REPLY, PAP_SEND_DATA, PAP_DATA, PAP_TICKLE = 1, 2, 3, 4, 5
PAP_CLOSE, PAP_CLOSE_REPLY, PAP_SEND_STATUS, PAP_STATUS = 6, 7, 8, 9
MY_NIS, MY_ATP = 0xFD, 0xF4          # sockets: lookups come back here; the PAP responder
LW_TYPE, LW_SLS, LW_SESS = b"LaserWriter", 0xE0, 0xE1   # the fake LaserWriter's sockets
GS = "/opt/homebrew/bin/gs"           # by path: `gs` is a git alias in some shells
DDP_ADSP = 7
ADSP_HDR, ADSP_MAX_DATA, ADSP_VERSION = 13, 572, 0x0100
AD_CTL, AD_ACKREQ, AD_EOM, AD_ATTN = 0x80, 0x40, 0x20, 0x10      # the descriptor's flags
(AD_PROBE, AD_OPEN_REQ, AD_OPEN_ACK, AD_OPEN_REQACK, AD_OPEN_DENY, AD_CLOSE, AD_FWD_RESET,
 AD_FWD_RESET_ACK, AD_RETRANSMIT) = range(9)                     # its control codes
SW_DATA_SOCK, SW_CTL_SOCK = 0xF1, 0xF2    # sockets: the adapter calls back to the first
(SWA_BREAK, SWA_BREAK_SEEN, SWA_SERIAL, SWA_OPTIONS, SWA_FAST_CLOCK, SWA_DRAINED, SWA_FAST,
 SWA_FAST_LOWER, SWA_SET_NAME, SWA_SET_STATUS, SWA_PRINT, SWA_WITHDRAW, SWA_POSITION,
 SWA_JOB_USER) = range(1, 15)             # the adapter's attention codes, 0001 to 000E
SWA_JOB_STATUS, SWA_JOB_COUNT, SWA_END = 0x10, 0x11, 0x12
SW_REQ_LEN, SW_STATUS_PRINTING = 70, 0x0647


def mac_str(m):
    return ":".join("%02x" % b for b in m)


def ether(dst, src, snap, payload):
    f = dst + src + struct.pack(">H", len(snap) + len(payload)) + snap + payload
    return f + b"\0" * max(0, 60 - len(f))


def ddp(dst_net, dst_node, dst_sock, src_net, src_node, src_sock, typ, data):
    n = 13 + len(data)
    return struct.pack(">HHHHBBBBB", n & 0x3FF, 0, dst_net, src_net, dst_node, src_node,
                       dst_sock, src_sock, typ) + data


def atp(ctl, bitmap, tid, user, data=b""):
    return struct.pack(">BBH", ctl, bitmap, tid) + bytes(user) + data


def pstr(s):
    return bytes([len(s)]) + s


# ------------------------------------------------------------------ transports

class Tunnel:
    """Basilisk II's udptunnel: frames in UDP to port 6066; a MAC is "B2" + IPv4."""

    def __init__(self, board_ip):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.connect((board_ip, TUNNEL_PORT))           # which local address reaches it
        self.ip = s.getsockname()[0]
        s.close()
        self.mac = b"B2" + socket.inet_aton(self.ip)
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        try:
            self.sock.bind(("0.0.0.0", TUNNEL_PORT))
        except OSError as e:
            sys.exit(f"cannot bind UDP {TUNNEL_PORT} ({e}): is Basilisk II running here?")
        self.sock.setblocking(False)

    async def open(self):
        return self

    async def send(self, frame):
        dst = frame[:6]
        if dst[:2] == b"B2":
            to = socket.inet_ntoa(dst[2:6])
        elif dst in (AT_MCAST, BCAST):
            to = "255.255.255.255"
        else:
            return
        self.sock.sendto(frame, (to, TUNNEL_PORT))

    async def recv(self, timeout):
        loop = asyncio.get_running_loop()
        try:
            return await asyncio.wait_for(loop.sock_recv(self.sock, 2048), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        self.sock.close()


class Hub:
    """One transport shared by several fakes: each tap hears every frame."""

    def __init__(self, t):
        self.t, self.mac, self.qs = t, t.mac, []

    def tap(self):
        q = asyncio.Queue()
        self.qs.append(q)
        return Tap(self, q)

    async def pump(self):
        while True:
            f = await self.t.recv(0.5)
            if f:
                for q in self.qs:
                    q.put_nowait(f)


class Tap:
    def __init__(self, hub, q):
        self.hub, self.q, self.mac = hub, q, hub.mac

    async def open(self):
        return self

    async def send(self, frame):
        await self.hub.t.send(frame)

    async def recv(self, timeout):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        pass


class Zone:
    """Infinite Mac's relay: JSON over a WebSocket, one zone per URL."""

    def __init__(self, zone):
        self.url = f"wss://infinitemac.org/zone/{zone}/websocket"
        self.mac = bytes([0x0A, 0x57, 0x57]) + os.urandom(3)
        self.q = asyncio.Queue()

    async def open(self):
        import websockets
        self.ws = await websockets.connect(self.url, open_timeout=15)
        await self.ws.send(json.dumps({"type": "init", "macAddress": mac_str(self.mac)}))
        self.reader = asyncio.create_task(self._read())
        return self

    async def _read(self):
        async for m in self.ws:
            d = json.loads(m)
            if d.get("type") == "receive":
                await self.q.put(bytes(d["packetArray"]))

    async def send(self, frame):
        dst = frame[:6]
        d = "AT" if dst == AT_MCAST else "*" if dst == BCAST else mac_str(dst)
        await self.ws.send(json.dumps({"type": "send", "destination": d,
                                       "packetArray": list(frame)}))

    async def recv(self, timeout):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        try:
            await self.ws.send(json.dumps({"type": "close"}))
        except Exception:
            pass
        await self.ws.close()


class Pipe:
    """Frames over a pair of streams, each a big-endian u16 length and the
    frame: the fake LaserWriter beside tools/rip/build/lwhost (W-1082). A
    task reads frames into a queue, so a recv that times out loses nothing."""

    def __init__(self, reader, writer):
        self.reader, self.writer = reader, writer
        self.mac = b"B2\x00\x00\x00\x01"
        self.q = asyncio.Queue()

    async def open(self):
        self.task = asyncio.create_task(self._read())
        return self

    async def _read(self):
        try:
            while True:
                n = struct.unpack(">H", await self.reader.readexactly(2))[0]
                await self.q.put(await self.reader.readexactly(n))
        except (asyncio.IncompleteReadError, ConnectionError):
            await self.q.put(None)

    async def send(self, frame):
        try:
            self.writer.write(struct.pack(">H", len(frame)) + frame)
            await self.writer.drain()
        except (ConnectionError, RuntimeError):
            pass

    async def recv(self, timeout):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        self.task.cancel()


# ------------------------------------------------------------------ ADSP

class AdspError(Exception):
    """The other end broke ADSP's rules, or never answered: the text says how."""


class AdspTimeout(AdspError):
    pass


class AdspClosed(AdspError):
    pass


class SwError(Exception):
    """A StyleWriter job through the adapter could not go on: the text says why."""


def adsp_pack(conn_id, first, nxt, window, desc, data=b""):
    """An ADSP packet: the 13-byte header and what follows it."""
    return struct.pack(">HIIHB", conn_id, first & 0xFFFFFFFF, nxt & 0xFFFFFFFF,
                       max(0, min(0xFFFF, window)), desc) + bytes(data)


def adsp_unpack(p):
    """(ConnID, FirstByteSeq, NextRecvSeq, RecvWindow, descriptor, data); ValueError for a
    packet too short to hold a header or with a control code past 8."""
    if len(p) < ADSP_HDR:
        raise ValueError(f"an ADSP packet of {len(p)} bytes")
    conn_id, first, nxt, window, desc = struct.unpack(">HIIHB", p[:ADSP_HDR])
    if desc & AD_CTL and desc & 0x0F > AD_RETRANSMIT:
        raise ValueError(f"ADSP control code {desc & 0x0F}")
    return conn_id, first, nxt, window, desc, bytes(p[ADSP_HDR:])


def adsp_open_pack(conn_id, window, code, dest_id, attn_recv=0):
    """One of the four open packets, 21 bytes: the header, the version, the other end's
    ConnID (0 in a request) and the attention sequence expected first."""
    return adsp_pack(conn_id, 0, 0, window, AD_CTL | code,
                     struct.pack(">HHI", ADSP_VERSION, dest_id, attn_recv))


class Adsp:
    """One ADSP connection on one of a Mac's sockets, opened by us (open) or by the other
    end (listen, accept). It is the Mac's end, so with `strict` (the default) anything the
    other end gets wrong ends it with an AdspError: a sequence number that is not the next
    one, an acknowledgement past what was sent or of a message's bytes without its EOM,
    bytes past the window it was given, an open packet that is not 21 bytes of version
    0100 with the right ConnID, an attention's answer that carries EOM. `strict=False` is
    for a wire that loses packets: what is ahead is dropped and asked for again.

    Nothing runs by itself: packets are taken, and timers run, while some coroutine of
    this Mac waits in an Adsp call or in Mac.adsp_run."""

    open_retry, open_tries = 1.0, 5        # an Open Request again each second, 5 more
    attn_retry, attn_tries = 1.0, 10       # an attention again each second until its 0x90
    rto, rto_tries = 3.0, 5                # everything unacknowledged again after 3 s

    def __init__(self, mac, sock, window=2048, strict=True, attn_desc=AD_ATTN | AD_ACKREQ,
                 third_leg=True, need_third_leg=True):
        self.mac, self.sock, self.recv_window, self.strict = mac, sock, window, strict
        self.attn_desc = attn_desc             # 0x50 as a Mac sends it; 0x10 as lpstyl does
        self.third_leg = third_leg             # False: our open never sends its 0x82
        self.need_third_leg = need_third_leg   # accepting: open only once the 0x82 is in
        self.conn_id = mac.adsp_conn_id()
        self.state = "new"                     # listening, accepting, opening, open, closed
        self.peer, self.peer_id, self.peer_window = None, 0, 0
        self.send_seq = self.acked = self.recv_seq = 0
        self.unacked = []                      # [first, data, eom] sent and not acknowledged
        self.eom_slots = set()                 # sequence numbers our EOMs took, after data
        self.rx, self.rx_base, self.rx_eoms = bytearray(), 0, []
        self.adv_limit = 0                     # the highest sequence we said we would take
        self.attn_send = self.attn_recv = 0
        self.attn_out = None                   # [seq, packet, sent at, resends]
        self.attn_in = []                      # (code, body) the other end sent us
        self.error = None
        self.peer_closed = self.denied = self.fwd_acked = False
        self.dups = self.acks = self.eoms_in = self.heard = self.resent = 0
        self.t_open = self.t_data = 0.0
        self.n_open = self.n_rto = 0
        mac.adsp_conns.append(self)

    def __repr__(self):
        return f"<Adsp socket {self.sock} ConnID {self.conn_id:04X} {self.state}>"

    # -- small things
    def _fail(self, why):
        raise AdspError(f"ADSP socket {self.sock}, ConnID {self.conn_id:04X}: {why}")

    def _check(self):
        if self.error:
            raise self.error

    def _drop(self):
        self.state = "closed"
        if self in self.mac.adsp_conns:
            self.mac.adsp_conns.remove(self)

    def window(self):
        """Bytes we can take now: the buffer less what has not been read."""
        return max(0, self.recv_window - len(self.rx))

    def _adv(self):
        w = min(0xFFFF, self.window())
        self.adv_limit = max(self.adv_limit, self.recv_seq + w)
        return w

    def room(self):
        """Bytes the other end's window still allows us."""
        return self.peer_window - (self.send_seq - self.acked)

    async def _tx(self, payload):
        if self.mac.adsp_trace is not None:
            self.mac.adsp_trace.append(("tx", self.sock, bytes(payload)))
        await self.mac.send_ddp(self.peer[0], self.peer[1], self.peer[2], self.sock, DDP_ADSP,
                                payload)

    async def _ctl(self, code, flags=0):
        await self._tx(adsp_pack(self.conn_id, self.send_seq, self.recv_seq, self._adv(),
                                 AD_CTL | flags | code))

    async def _open_tx(self, code):
        await self._tx(adsp_open_pack(self.conn_id, self._adv(), code,
                                      0 if code == AD_OPEN_REQ else self.peer_id, self.attn_recv))

    async def _wait(self, cond, timeout):
        """True once cond() holds; False after timeout or when the connection ended first."""
        await self.mac.adsp_run(lambda: cond() or self.error or self.state == "closed", timeout)
        self._check()
        return bool(cond())

    # -- opening and closing
    async def open(self, net, node, sock, timeout=None):
        """The opener's three legs: 0x81 (again each second), their 0x83, our 0x82."""
        self.peer, self.state = (net, node, sock), "opening"
        self.t_open, self.n_open = time.monotonic(), 0
        await self._open_tx(AD_OPEN_REQ)
        limit = timeout if timeout is not None else self.open_retry * (self.open_tries + 1) + 1
        await self._wait(lambda: self.state == "open", limit)
        if self.state != "open":
            self._drop()
            raise AdspError(f"ADSP socket {self.sock}: " + (
                "the open was denied" if self.denied else
                f"no answer to the Open Request from {net}.{node} socket {sock}"))
        return self

    def listen(self):
        """Takes the next Open Request that reaches this socket."""
        self.state = "listening"
        return self

    async def accept(self, timeout=30.0):
        """Waits for the open to finish: their 0x81, our 0x83, their 0x82."""
        if self.state == "new":
            self.listen()
        if not await self._wait(lambda: self.state == "open", timeout):
            if self.state == "accepting":
                raise AdspTimeout(f"ADSP socket {self.sock}: the third open leg (0x82) never came")
            raise AdspTimeout(f"ADSP socket {self.sock}: no Open Request in {timeout} s")
        return self

    async def close(self):
        """Close Advice, once; the connection is forgotten."""
        if self.state in ("open", "accepting"):
            await self._tx(adsp_pack(self.conn_id, self.send_seq, self.recv_seq, 0,
                                     AD_CTL | AD_CLOSE))
        self._drop()

    def vanish(self):
        """Gone without a word, as a Mac switched off: nothing more is sent or answered."""
        self._drop()

    # -- the stream
    async def write(self, data, eom=True, wait=True, timeout=30.0):
        """Sends data as one message (eom) or as bytes with no end (eom=False), no faster
        than the other end's window allows; wait: until all of it is acknowledged."""
        self._check()
        if self.state != "open":
            raise AdspClosed(f"ADSP socket {self.sock}: write on a connection that is {self.state}")
        data, off = bytes(data), 0
        if not data and eom:
            await self._data_tx(b"", True)             # a bare EOM goes whatever the window
        while off < len(data):
            n = min(ADSP_MAX_DATA, self.room(), len(data) - off)
            if n > 0:
                off += n
                await self._data_tx(data[off - n:off], eom and off == len(data))
                continue
            if not await self._wait(lambda: self.room() > 0, timeout):
                if self.state == "closed":
                    raise AdspClosed(f"ADSP socket {self.sock}: closed with {len(data) - off} bytes to send")
                raise AdspTimeout(f"ADSP socket {self.sock}: the window stayed shut for {timeout} s "
                                  f"with {len(data) - off} bytes to send, and no one said it had opened")
        if wait:
            await self.flush(timeout)

    async def _data_tx(self, chunk, eom):
        first = self.send_seq
        self.unacked.append([first, chunk, eom])
        self.send_seq += len(chunk) + (1 if eom else 0)
        if eom and chunk:
            self.eom_slots.add(self.send_seq - 1)
        self.t_data, self.n_rto = time.monotonic(), 0
        await self._tx(adsp_pack(self.conn_id, first, self.recv_seq, self._adv(),
                                 AD_ACKREQ | (AD_EOM if eom else 0), chunk))

    async def flush(self, timeout=30.0):
        """Waits until everything sent is acknowledged."""
        if not await self._wait(lambda: self.acked == self.send_seq, timeout):
            if self.state == "closed":
                raise AdspClosed(f"ADSP socket {self.sock}: closed with data unacknowledged")
            raise AdspTimeout(f"ADSP socket {self.sock}: acknowledged to {self.acked} of "
                              f"{self.send_seq} (an EOM counts one)")

    async def _resend(self):
        self.t_data = time.monotonic()
        for first, chunk, eom in list(self.unacked):
            self.resent += 1
            await self._tx(adsp_pack(self.conn_id, first, self.recv_seq, self._adv(),
                                     AD_ACKREQ | (AD_EOM if eom else 0), chunk))

    def _take(self, n, eom_ok=True):
        lo, hi = self.rx_base, self.rx_base + n
        if not eom_ok and any(lo <= m <= hi for m in self.rx_eoms):
            self.error = AdspError(f"ADSP socket {self.sock}, ConnID {self.conn_id:04X}: "
                                   f"an attention's answer carries EOM")
            raise self.error
        out = bytes(self.rx[:n])
        del self.rx[:n]
        self.rx_base = hi
        self.rx_eoms = [m for m in self.rx_eoms if m > hi]
        return out

    async def _took(self, shut):
        if shut and self.window() > 0 and self.state == "open":
            await self._ctl(AD_PROBE)                  # the window is open again: say so unasked

    def _shut(self):
        return self.adv_limit - self.recv_seq <= 0

    async def read(self, n, timeout=10.0, eom_ok=True):
        """Exactly n bytes of the stream, as a Mac reads an answer."""
        if not await self._wait(lambda: len(self.rx) >= n, timeout):
            if self.state == "closed":
                raise AdspClosed(f"ADSP socket {self.sock}: closed with {len(self.rx)} of {n} bytes read")
            raise AdspTimeout(f"ADSP socket {self.sock}: {len(self.rx)} of {n} bytes in {timeout} s")
        shut = self._shut()
        out = self._take(n, eom_ok)
        await self._took(shut)
        return out

    async def read_some(self, timeout=10.0):
        """Whatever has come, at least a byte; b"" after timeout or a close."""
        try:
            await self._wait(lambda: len(self.rx) > 0, timeout)
        except AdspError:
            if not self.rx:
                raise
        shut = self._shut()
        out = self._take(len(self.rx))
        await self._took(shut)
        return out

    async def read_until(self, last, timeout=10.0, limit=64):
        """Bytes up to and with the byte `last` (a printer's `?` answer ends in 0D)."""
        if not await self._wait(lambda: last in self.rx or len(self.rx) >= limit, timeout):
            if self.state == "closed":
                raise AdspClosed(f"ADSP socket {self.sock}: closed before byte {last:02X} came")
            raise AdspTimeout(f"ADSP socket {self.sock}: no byte {last:02X} in {timeout} s")
        shut = self._shut()
        k = self.rx.find(bytes([last]))
        out = self._take(k + 1 if k >= 0 else len(self.rx))
        await self._took(shut)
        return out

    async def read_message(self, timeout=10.0):
        """The bytes up to the next EOM (a message no longer than the window)."""
        if not await self._wait(lambda: bool(self.rx_eoms), timeout):
            if self.state == "closed":
                raise AdspClosed(f"ADSP socket {self.sock}: closed before a message ended")
            raise AdspTimeout(f"ADSP socket {self.sock}: no end of message in {timeout} s")
        shut = self._shut()
        n = self.rx_eoms.pop(0) - self.rx_base
        out = bytes(self.rx[:n])
        del self.rx[:n]
        self.rx_base += n
        await self._took(shut)
        return out

    def discard(self):
        """Throws away what has come and not been read."""
        self.rx_base += len(self.rx)
        self.rx.clear()
        self.rx_eoms = []

    # -- attentions, probes, Forward Reset
    async def attention(self, code, body=b"", answer=2, timeout=10.0):
        """An attention message: sent, again each second until its 0x90, then `answer`
        bytes read off the stream (the adapter answers every attention there; 0 reads
        none). The answer must not carry EOM."""
        self._check()
        if self.state != "open":
            raise AdspClosed(f"ADSP socket {self.sock}: attention on a connection that is {self.state}")
        if self.attn_out:
            raise AdspError("one attention at a time")
        pkt = adsp_pack(self.conn_id, self.attn_send, self.attn_recv, 0, self.attn_desc,
                        struct.pack(">H", code) + bytes(body))
        self.attn_out = [self.attn_send, pkt, time.monotonic(), 0]
        await self._tx(pkt)
        if not await self._wait(lambda: self.attn_out is None, self.attn_retry * (self.attn_tries + 2)):
            raise AdspClosed(f"ADSP socket {self.sock}: closed before attention {code:04X} was acknowledged")
        if not answer:
            return b""
        try:
            return await self.read(answer, timeout, eom_ok=False)
        except AdspTimeout:
            raise AdspTimeout(f"ADSP socket {self.sock}: attention {code:04X} was acknowledged, but "
                              f"{len(self.rx)} of its {answer} answer bytes came in {timeout} s") from None

    async def next_attention(self, timeout=10.0):
        """The next attention the other end sent: (code, body), or None."""
        try:
            await self._wait(lambda: bool(self.attn_in), timeout)
        except AdspError:
            if not self.attn_in:
                raise
        return self.attn_in.pop(0) if self.attn_in else None

    async def probe(self, timeout=5.0):
        """A probe (0xC0): True when an acknowledgement came back."""
        seen = self.acks
        await self._ctl(AD_PROBE, AD_ACKREQ)
        return await self._wait(lambda: self.acks > seen, timeout)

    async def forward_reset(self, wait=False, timeout=5.0):
        """Forward Reset, once, as lpstyl sends at a job's end: what was sent and not
        acknowledged is given up. wait: for the 0x87."""
        self.unacked, self.acked, self.eom_slots = [], self.send_seq, set()
        self.fwd_acked = False
        await self._ctl(AD_FWD_RESET)
        if wait and not await self._wait(lambda: self.fwd_acked, timeout):
            raise AdspTimeout(f"ADSP socket {self.sock}: no answer to the Forward Reset")

    # -- timers
    async def tick(self, now):
        if self.error or self.state == "closed":
            return
        try:
            if self.state == "opening" and now - self.t_open >= self.open_retry:
                if self.n_open >= self.open_tries:
                    self._drop()
                    return
                self.n_open, self.t_open = self.n_open + 1, now
                await self._open_tx(AD_OPEN_REQ)
            a = self.attn_out
            if a and now - a[2] >= self.attn_retry:
                if a[3] >= self.attn_tries:
                    self._fail(f"an attention was never acknowledged ({a[3] + 1} sent)")
                a[2], a[3] = now, a[3] + 1
                await self._tx(a[1])
            if self.unacked and self.state == "open" and now - self.t_data >= self.rto:
                if self.n_rto >= self.rto_tries:
                    self._fail(f"sequence {self.acked} to {self.send_seq} never acknowledged "
                               f"({self.n_rto} resends)")
                self.n_rto += 1
                await self._resend()
        except AdspError as e:
            self.error = self.error or e

    # -- packets in
    async def on_packet(self, src, conn_id, first, nxt, window, desc, data):
        if self.error or self.state == "closed":
            return
        self.heard += 1
        try:
            await self._packet(src, conn_id, first, nxt, window, desc, data)
        except AdspError as e:
            self.error = self.error or e

    def _ack(self, nxt, window):
        """A cumulative acknowledgement and the window that came with it."""
        if nxt > self.send_seq:
            self._fail(f"an acknowledgement names {nxt}; only {self.send_seq} was sent")
        if nxt < self.acked:
            if self.strict:
                self._fail(f"an acknowledgement went back from {self.acked} to {nxt}")
            return
        if self.strict and nxt > self.acked and nxt in self.eom_slots:
            self._fail(f"an acknowledgement names {nxt}: a message's bytes without its EOM "
                       f"(an EOM counts one, so {nxt + 1})")
        if nxt > self.acked:
            self.t_data, self.n_rto = time.monotonic(), 0
        self.acked, self.peer_window = nxt, window
        self.eom_slots = {s for s in self.eom_slots if s >= nxt}
        keep = []
        for first, chunk, eom in self.unacked:
            if first + len(chunk) + (1 if eom else 0) <= nxt:
                continue
            if nxt > first:
                chunk, first = chunk[nxt - first:], nxt
            keep.append([first, chunk, eom])
        self.unacked = keep

    async def _open_packet(self, src, conn_id, first, nxt, window, code, data):
        if len(data) != 8 and self.strict:
            self._fail(f"an open packet (0x{AD_CTL | code:02X}) of {ADSP_HDR + len(data)} bytes, not 21")
        if len(data) < 8:
            return
        version, dest, attn = struct.unpack(">HHI", data[:8])
        if self.strict:
            if version != ADSP_VERSION:
                self._fail(f"an open packet of version {version:04X}, not 0100")
            if first or nxt or attn:
                self._fail(f"an open packet whose sequences are not 0 ({first}, {nxt}, {attn})")
        if code == AD_OPEN_REQ:
            if self.state == "listening":
                if dest and self.strict:
                    self._fail(f"an Open Request that names a ConnID for us ({dest:04X})")
                if not conn_id:
                    self._fail("an Open Request from ConnID 0")
                self.peer, self.peer_id, self.peer_window = src, conn_id, window
                self.state = "accepting" if self.need_third_leg else "open"
            await self._open_tx(AD_OPEN_REQACK)        # the first, or the same again
        elif code == AD_OPEN_REQACK:
            if dest != self.conn_id:
                self._fail(f"an open answer for ConnID {dest:04X}; ours is {self.conn_id:04X}")
            if self.state == "opening":
                self.peer, self.peer_id, self.peer_window, self.state = src, conn_id, window, "open"
            elif conn_id != self.peer_id:              # a second answer from a newer ConnID
                self.peer, self.peer_id = src, conn_id
            if self.third_leg:
                await self._open_tx(AD_OPEN_ACK)
        elif code == AD_OPEN_ACK:
            if dest != self.conn_id:
                self._fail(f"an Open Ack for ConnID {dest:04X}; ours is {self.conn_id:04X}")
            if self.state == "accepting":
                self.peer_window, self.state = window, "open"
            elif self.state == "opening":
                self.peer, self.peer_id, self.peer_window, self.state = src, conn_id, window, "open"
        elif code == AD_OPEN_DENY and self.state == "opening":
            self.denied = True
            self._drop()

    async def _packet(self, src, conn_id, first, nxt, window, desc, data):
        code = desc & 0x0F
        if desc & AD_CTL and not desc & AD_ATTN and AD_OPEN_REQ <= code <= AD_OPEN_DENY:
            await self._open_packet(src, conn_id, first, nxt, window, code, data)
            return
        if self.state == "accepting":
            if self.strict:
                self._fail(f"a packet (0x{desc:02X}) before the third open leg (0x82)")
            self.state = "open"
        if self.state != "open":
            return
        if desc & AD_ATTN:
            if window and self.strict:
                self._fail(f"an attention packet (0x{desc:02X}) with a window of {window}, not 0")
            if desc & AD_CTL:                          # 0x90: our attention is acknowledged
                a = self.attn_out
                if a and nxt == a[0] + 1:
                    self.attn_out, self.attn_send = None, a[0] + 1
                elif nxt > self.attn_send + (1 if a else 0) and self.strict:
                    self._fail(f"an attention acknowledgement names {nxt}; "
                               f"{self.attn_send + (1 if a else 0)} sent")
                return
            if len(data) < 2:
                self._fail("an attention message with no code")
            if first == self.attn_recv:
                self.attn_in.append((struct.unpack(">H", data[:2])[0], data[2:]))
                self.attn_recv += 1
            elif first > self.attn_recv and self.strict:
                self._fail(f"attention sequence {first}; {self.attn_recv} is next")
            await self._tx(adsp_pack(self.conn_id, self.attn_send, self.attn_recv, 0,
                                     AD_CTL | AD_ATTN))
            return
        if desc & AD_CTL:
            if code == AD_CLOSE:
                self.peer_closed = True
                self._drop()
            elif code == AD_PROBE:
                if first != self.recv_seq and self.strict:
                    self._fail(f"an acknowledgement sent from sequence {first}; we have taken "
                               f"{self.recv_seq}")
                self._ack(nxt, window)
                self.acks += 1
                if desc & AD_ACKREQ:
                    await self._ctl(AD_PROBE)
            elif code == AD_FWD_RESET:
                if self.recv_seq <= first <= self.recv_seq + self.window():
                    self.recv_seq = first
                    self.discard()
                await self._ctl(AD_FWD_RESET_ACK)
            elif code == AD_FWD_RESET_ACK:
                self.fwd_acked = True
            elif code == AD_RETRANSMIT:
                self._ack(nxt, window)
                await self._resend()
            return
        # Stream data.
        self._ack(nxt, window)
        n, eom = len(data), 1 if desc & AD_EOM else 0
        if first > self.recv_seq:
            if self.strict:
                self._fail(f"data at sequence {first}; {self.recv_seq} is next")
        elif first + n + eom <= self.recv_seq:
            if n or eom:
                self.dups += 1                         # had already: acknowledged again below
        else:
            if first + n > self.adv_limit and self.strict:
                self._fail(f"data to sequence {first + n}: {first + n - self.adv_limit} bytes past "
                           f"the window we gave (to {self.adv_limit})")
            new = data[self.recv_seq - first:][:self.window()]
            whole = len(new) == first + n - self.recv_seq
            self.rx += new
            self.recv_seq += len(new)
            if eom and whole:
                self.rx_eoms.append(self.rx_base + len(self.rx))
                self.recv_seq += 1
                self.eoms_in += 1
        if desc & AD_ACKREQ:
            await self._ctl(AD_PROBE)


# ------------------------------------------------------------------ StyleWriter streams

SW_BARE = b"dnZbsAuDNLHBFYlEP"            # the letters a StyleWriter takes on their own
SW_IDENT = {b"IJ10": "original", b"SW": "ii", b"SW3": "1200"}
SW_SUBMODEL = {1: "2400", 2: "2200", 4: "1500", 5: "2500"}


def sw_split(job):
    """A StyleWriter stream cut into its commands, [(kind, bytes)], each to go as one
    message: "rect" (R or c and four u16), "block" (G, a u16 length, the data, 00),
    "page" (0C), "query" (FF FF FF and a letter), "ident" (?), "letter" (a setup
    letter; m with its byte) and "zero" (a 00 between commands). ValueError for a
    stream that is cut short or holds anything else."""
    job, out, i = bytes(job), [], 0

    def take(kind, n):
        nonlocal i
        if i + n > len(job):
            raise ValueError(f"the stream ends inside a {kind} at byte {i}")
        out.append((kind, job[i:i + n]))
        i += n

    while i < len(job):
        b = job[i]
        if b in (0x52, 0x63):
            take("rect", 9)
        elif b == 0x47:
            if i + 3 > len(job):
                raise ValueError(f"the stream ends inside a block at byte {i}")
            n = job[i + 1] | job[i + 2] << 8
            if i + 4 + n > len(job):
                raise ValueError(f"the stream ends inside a block at byte {i}")
            if job[i + 3 + n] != 0:
                raise ValueError(f"the block at byte {i} is not followed by 00")
            take("block", 4 + n)
        elif b == 0x0C:
            take("page", 1)
        elif b == 0xFF:
            if job[i:i + 3] != b"\xff\xff\xff" or i + 4 > len(job) or job[i + 3] == 0xFF:
                raise ValueError(f"not FF FF FF and a letter at byte {i}")
            take("query", 4)
        elif b == 0x3F:
            take("ident", 1)
        elif b == 0x6D:
            take("letter", 2)
        elif b == 0x00:
            take("zero", 1)
        elif b in SW_BARE:
            take("letter", 1)
        else:
            raise ValueError(f"unknown byte {b:02X} at {i}")
    return out


def sw_request(data_sock, user):
    """The 70-byte print request: the data socket as a u32, the user's name (32 at
    most), and an empty second string."""
    name = user.encode("mac_roman") if isinstance(user, str) else bytes(user)
    return (struct.pack(">I", data_sock) + pstr(name[:32])).ljust(SW_REQ_LEN, b"\0")


def sw_status(text, ident=SW_STATUS_PRINTING):
    """Attention 000A's 257 bytes: a message ID, the words."""
    t = text.encode("mac_roman") if isinstance(text, str) else bytes(text)
    return (struct.pack(">H", ident) + pstr(t[:254])).ljust(257, b"\0")


# ------------------------------------------------------------------ the Mac

class Mac:
    def __init__(self, t):
        self.t = t
        self.net, self.node = 0xFF00 + random.randrange(0xFF), 1 + random.randrange(253)
        self.map = {}                                  # (net, node) -> MAC
        self.tid = random.randrange(0x10000)
        self.inbox = []                                # datagrams for the caller
        self.online = False
        self.name = None                               # its Macintosh name, MacRoman
        self.adsp_conns, self.adsp_trace = [], None    # its ADSP connections; a list logs packets
        self.adsp_stray = 0                            # type 7 datagrams that were no one's
        self._adsp_id, self._adsp_q = random.randrange(0xFFFF), []
        self._adsp_busy = self._adsp_reading = False
        self._adsp_evt = None

    def next_tid(self):
        self.tid = (self.tid + 1) & 0xFFFF
        return self.tid

    async def aarp(self, fn, to_mac, their_hw, their_net, their_node):
        p = struct.pack(">HHBBH", 1, 0x809B, 6, 4, fn) + self.t.mac + \
            struct.pack(">BHB", 0, self.net, self.node) + their_hw + \
            struct.pack(">BHB", 0, their_net, their_node)
        await self.t.send(ether(to_mac, self.t.mac, SNAP_AARP, p))

    async def acquire(self):
        """10 AARP probes 200 ms apart; anyone answering for the address moves us on."""
        while True:
            taken = False
            for _ in range(10):
                await self.aarp(3, AT_MCAST, b"\0" * 6, self.net, self.node)
                end = time.monotonic() + 0.2
                while time.monotonic() < end:
                    f = await self.t.recv(end - time.monotonic())
                    if f and await self.handle(f) == "taken":
                        taken = True
                if taken:
                    break
            if not taken:
                self.online = True
                print(f"address {self.net}.{self.node} ({self.net:04X}.{self.node}), MAC {mac_str(self.t.mac)}")
                return
            self.net, self.node = 0xFF00 + random.randrange(0xFF), 1 + random.randrange(253)

    async def handle(self, f):
        """One frame in: AARP answered, DDP queued; returns "taken" on a clash."""
        if len(f) < 22 or f[6:12] == self.t.mac:
            return None
        if f[:6] not in (self.t.mac, BCAST, AT_MCAST):
            return None
        n = struct.unpack(">H", f[12:14])[0]
        snap, payload = f[14:22], f[22:14 + n]
        if snap == SNAP_AARP and len(payload) >= 28:
            fn = struct.unpack(">H", payload[6:8])[0]
            src_hw = payload[8:14]
            src = struct.unpack(">H", payload[15:17])[0], payload[17]
            dst = struct.unpack(">H", payload[25:27])[0], payload[27]
            if src == (self.net, self.node) or (fn == 3 and dst == (self.net, self.node) and not self.online):
                return "taken"
            if fn != 3:
                self.map[src] = src_hw
            if self.online and fn in (1, 3) and dst == (self.net, self.node):
                await self.aarp(2, src_hw, src_hw, *src)
        elif snap == SNAP_DDP and len(payload) >= 13:
            src = struct.unpack(">H", payload[6:8])[0], payload[9]
            self.map[src] = f[6:12]                     # gleaned
            if self.name and self.online and await self.answer_lkup(payload):
                return None
            self.inbox.append(payload)
        return None

    async def answer_lkup(self, d):
        """A LkUp for our Workstation entity, answered as System 7 does."""
        if d[10] != NIS or d[12] != DDP_NBP or d[13] >> 4 != 2:
            return False
        t = d[15:]
        net, node, sock = struct.unpack(">HBB", t[:4])
        obj = t[6:6 + t[5]]
        typ = t[7 + t[5]:7 + t[5] + t[6 + t[5]]]
        if obj not in (b"=", self.name) or typ.lower() != WS_TYPE.lower():
            return False
        rep = bytes([0x31, d[14]]) + struct.pack(">HBBB", self.net, self.node, AEP, 0) + \
              pstr(self.name) + pstr(WS_TYPE) + pstr(b"*")
        await self.send_ddp(net, node, sock, NIS, DDP_NBP, rep)
        print(f"{net}.{node} asked our name: answered {self.name.decode('mac_roman')}")
        return True

    async def send_ddp(self, net, node, sock, src_sock, typ, data):
        d = ddp(net, node, sock, self.net, self.node, src_sock, typ, data)
        if node == 0xFF:
            to = AT_MCAST
        else:
            to = self.map.get((net, node))
            for _ in range(3):
                if to:
                    break
                await self.aarp(1, AT_MCAST, b"\0" * 6, net, node)
                await self.pump(0.3)
                to = self.map.get((net, node))
            if not to:
                sys.exit(f"nobody answers AARP for {net}.{node}")
        await self.t.send(ether(to, self.t.mac, SNAP_DDP, d))

    async def pump(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            f = await self.t.recv(end - time.monotonic())
            if f:
                await self.handle(f)

    async def wait_dg(self, match, seconds):
        """The first datagram match() accepts, within seconds."""
        end = time.monotonic() + seconds
        while True:
            for i, d in enumerate(self.inbox):
                if match(d):
                    return self.inbox.pop(i)
            left = end - time.monotonic()
            if left <= 0:
                return None
            f = await self.t.recv(left)
            if f:
                await self.handle(f)

    async def lookup(self):
        nbp = bytes([0x21, 0x2A]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(b"=") + pstr(SLS_TYPE) + pstr(b"*")
        for _ in range(5):
            await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
            d = await self.wait_dg(lambda d: d[12] == DDP_NBP and d[10] == MY_NIS and d[13] >> 4 == 3, 1.0)
            if d:
                t = d[15:]
                net, node, sock = struct.unpack(">HBB", t[:4])
                obj = t[5:6 + t[5]]
                print(f"found {obj[1:].decode('mac_roman')}:ImageWriter at {net}.{node} socket {sock}")
                return net, node, sock
        sys.exit("no ImageWriter answered the lookup")

    async def chooser(self, typ):
        """Every answer to =:TYPE@* in two seconds, as the Chooser lists them."""
        nbp = bytes([0x21, 0x2B]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(b"=") + pstr(typ.encode("mac_roman")) + pstr(b"*")
        await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
        seen, end = set(), time.monotonic() + 2.0
        while time.monotonic() < end:
            d = await self.wait_dg(lambda d: d[12] == DDP_NBP and d[10] == MY_NIS and d[13] >> 4 == 3,
                                   end - time.monotonic())
            if not d:
                break
            t = d[15:]
            net, node, sock = struct.unpack(">HBB", t[:4])
            obj = t[6:6 + t[5]].decode("mac_roman")
            ty = t[7 + t[5]:7 + t[5] + t[6 + t[5]]].decode("mac_roman")
            if (obj, ty, net, node, sock) not in seen:
                seen.add((obj, ty, net, node, sock))
                print(f"{obj}:{ty} at {net}.{node} socket {sock}")
        if not seen:
            print(f"nothing answered =:{typ}@*")

    async def request(self, net, node, sock, user, data=b"", xo=True, tries=5):
        """An ATP request; the first response packet's (user, data)."""
        tid = self.next_tid()
        for _ in range(tries):
            await self.send_ddp(net, node, sock, MY_ATP, DDP_ATP,
                                atp(TREQ | (XO if xo else 0), 1, tid, user, data))
            d = await self.wait_dg(lambda d: d[12] == DDP_ATP and d[10] == MY_ATP and
                                   d[13] & 0xC0 == TRESP and struct.unpack(">H", d[15:17])[0] == tid, 2.0)
            if d:
                if xo:
                    await self.send_ddp(net, node, sock, MY_ATP, DDP_ATP, atp(TREL, 0, tid, b"\0" * 4))
                return d[17:21], d[21:]
        sys.exit("no answer to an ATP request")

    async def print_file(self, data):
        net, node, sls = await self.lookup()
        user, st = await self.request(net, node, sls, [0, PAP_SEND_STATUS, 0, 0], xo=False)
        word = st[5] | st[6] << 8 if len(st) >= 7 else -1
        print(f"status word 0x{word:04x}")
        connid = random.randrange(1, 256)
        for attempt in range(150):      # five minutes, as long as a page can hold the printer
            user, rep = await self.request(net, node, sls, [connid, PAP_OPEN, 0, 0],
                                           bytes([MY_ATP, 1, 0, 0]))
            result = struct.unpack(">H", rep[2:4])[0]
            if result == 0:
                break
            print("printer busy; asking again in 2 s")
            await asyncio.sleep(2)
        else:
            sys.exit("the printer stayed busy")
        srv = rep[0]
        print(f"connection {connid} open (the board's socket {srv}, its flow quantum {rep[1]})")
        off, start = 0, time.monotonic()
        while True:
            # DDP long header: dst sock at 10, src sock 11, type 12; ATP from
            # 13: ctl, bitmap, TID (15-16), user (17-20: ConnID, function, ..).
            d = await self.wait_dg(lambda d: d[12] == DDP_ATP and d[10] == MY_ATP and
                                   d[13] & 0xC0 == TREQ and d[17] == connid and
                                   d[18] in (PAP_SEND_DATA, PAP_TICKLE), 60.0)
            if not d:
                sys.exit(f"the board stopped asking after {off} bytes")
            tid, fn = struct.unpack(">H", d[15:17])[0], d[18]
            if fn == PAP_TICKLE:
                continue
            chunk = data[off:off + 512]
            eof = off + len(chunk) >= len(data)
            src = struct.unpack(">H", d[6:8])[0], d[9]
            await self.send_ddp(src[0], src[1], d[11], MY_ATP, DDP_ATP,
                                atp(TRESP | EOM, 0, tid, [connid, PAP_DATA, 1 if eof else 0, 0], chunk))
            off += len(chunk)
            if eof:
                break
        await self.pump(0.5)
        await self.request(net, node, srv, [connid, PAP_CLOSE, 0, 0])
        secs = time.monotonic() - start
        print(f"sent {len(data)} bytes in {secs:.1f} s, crc32 {zlib.crc32(data):08x}")

    async def print_ps(self, name, jobs, close_early=False):
        """PostScript to NAME:LaserWriter as a Mac's LaserWriter driver sends it
        (W-1088, W-1095): each of jobs its own job on one connection, pulled by
        the printer and ended by EOF, the printer's output read back and shown
        to its EOF before the next; then CloseConn. close_early: CloseConn once
        the last job's EOF is out, not waiting for the printer's."""
        nbp = bytes([0x21, 0x2C]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(name.encode("mac_roman")) + pstr(LW_TYPE) + pstr(b"*")
        for _ in range(5):
            await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
            d = await self.wait_dg(lambda d: d[12] == DDP_NBP and d[10] == MY_NIS and d[13] >> 4 == 3, 1.0)
            if d:
                break
        else:
            sys.exit(f"no {name}:LaserWriter answered the lookup")
        net, node, sls = struct.unpack(">HBB", d[15:19])
        print(f"found {name}:LaserWriter at {net}.{node} socket {sls}")
        connid = random.randrange(1, 256)
        for attempt in range(60):
            user, rep = await self.request(net, node, sls, [connid, PAP_OPEN, 0, 0],
                                           bytes([MY_ATP, 8, 0, 0]))
            if struct.unpack(">H", rep[2:4])[0] == 0:
                break
            print(f"busy ({rep[5:5 + rep[4]].decode('mac_roman', 'replace')}); asking again in 2 s")
            await asyncio.sleep(2)
        else:
            sys.exit("the printer stayed busy")
        srv = rep[0]
        print(f"connection {connid} open (session socket {srv})")
        rd_seq, rd_tid, rd_at, held = 1, None, 0.0, None
        start = time.monotonic()
        for k, data in enumerate(jobs, 1):
            off, sent_eof, got_eof, out = 0, False, False, bytearray()
            t0, d0, held = time.monotonic(), held, None
            early = close_early and k == len(jobs)
            while not (sent_eof and (got_eof or early)):
                if time.monotonic() - t0 > 600:
                    sys.exit(f"gave up on job {k}: {off} bytes sent, printer EOF {got_eof}")
                if not got_eof and (rd_tid is None or time.monotonic() - rd_at > 15):
                    rd_tid = rd_tid if rd_tid is not None else self.next_tid()
                    await self.send_ddp(net, node, srv, MY_ATP, DDP_ATP,
                                        atp(TREQ | XO, 0xFF, rd_tid, [connid, PAP_SEND_DATA, rd_seq >> 8, rd_seq & 0xFF]))
                    rd_at = time.monotonic()
                d, d0 = d0, None                       # the next job's SendData, if it came early
                if not d:
                    d = await self.wait_dg(lambda d: d[12] == DDP_ATP and d[10] == MY_ATP and (
                        (d[13] & 0xC0 == TREQ and d[17] == connid and d[18] in (PAP_SEND_DATA, PAP_TICKLE)) or
                        (d[13] & 0xC0 == TRESP and struct.unpack(">H", d[15:17])[0] == rd_tid)), 2.0)
                if not d:
                    continue
                tid = struct.unpack(">H", d[15:17])[0]
                src = struct.unpack(">H", d[6:8])[0], d[9]
                if d[13] & 0xC0 == TRESP:              # the printer's words
                    out += d[21:]
                    if d[19]:
                        got_eof = True
                    if d[13] & EOM:
                        await self.send_ddp(net, node, srv, MY_ATP, DDP_ATP, atp(TREL, 0, tid, b"\0" * 4))
                        rd_tid = None
                        rd_seq = 1 if rd_seq == 0xFFFF else rd_seq + 1
                    continue
                if d[18] == PAP_TICKLE:
                    continue
                if sent_eof:                           # the printer asks for the next job
                    held = d
                    continue
                chunk = data[off:off + 512]
                eof = off + len(chunk) >= len(data)
                await self.send_ddp(src[0], src[1], d[11], MY_ATP, DDP_ATP,
                                    atp(TRESP | EOM, 0, tid, [connid, PAP_DATA, 1 if eof else 0, 0], chunk))
                off += len(chunk)
                sent_eof = eof
            secs = time.monotonic() - t0
            print(f"job {k}: sent {len(data)} bytes in {secs:.1f} s; the printer said:")
            print(out.decode("mac_roman", "replace").replace("\r", "\n").rstrip() or "(nothing)",
                  flush=True)
        await self.request(net, node, srv, [connid, PAP_CLOSE, 0, 0])
        print(f"closed after {len(jobs)} job{'' if len(jobs) == 1 else 's'}, "
              f"{time.monotonic() - start:.1f} s in all")

    async def watch_status(self, name, secs):
        """NAME:LaserWriter's status string, asked once a second as the Chooser
        and Print Monitor do, printed each time it changes."""
        nbp = bytes([0x21, 0x2D]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(name.encode("mac_roman")) + pstr(LW_TYPE) + pstr(b"*")
        for _ in range(5):
            await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
            d = await self.wait_dg(lambda d: d[12] == DDP_NBP and d[10] == MY_NIS and d[13] >> 4 == 3, 1.0)
            if d:
                break
        else:
            sys.exit(f"no {name}:LaserWriter answered the lookup")
        net, node, sls = struct.unpack(">HBB", d[15:19])
        last, start = None, time.monotonic()
        while time.monotonic() - start < secs:
            user, st = await self.request(net, node, sls, [0, PAP_SEND_STATUS, 0, 0], xo=False)
            text = st[5:5 + st[4]].decode("mac_roman", "replace") if len(st) > 4 else "(none)"
            if text != last:
                print(f"{time.strftime('%H:%M:%S')} {text}", flush=True)
                last = text
            await asyncio.sleep(1)

    # -- ADSP (W-1193)
    def adsp_conn_id(self):
        """A ConnID no connection of ours has, never 0."""
        while True:
            self._adsp_id = self._adsp_id % 0xFFFF + 1
            if all(c.conn_id != self._adsp_id for c in self.adsp_conns):
                return self._adsp_id

    def _adsp_route(self, sock, src, p):
        """The connection a type 7 datagram is for, with its fields; None for no one's."""
        try:
            f = adsp_unpack(p)
        except ValueError:
            self.adsp_stray += 1
            return None
        conn_id, desc, data = f[0], f[4], f[5]
        conns = [c for c in self.adsp_conns if c.sock == sock]
        known = [c for c in conns if c.state in ("accepting", "open") and c.peer == src
                 and c.peer_id == conn_id]
        code = desc & 0x0F if desc & AD_CTL and not desc & AD_ATTN else None
        hit = None
        if code == AD_OPEN_REQ:
            hit = known or [c for c in conns if c.state == "listening"]
        elif code in (AD_OPEN_ACK, AD_OPEN_REQACK, AD_OPEN_DENY):
            if len(data) >= 4:
                dest = struct.unpack(">H", data[2:4])[0]
                hit = [c for c in conns if c.conn_id == dest and c.state != "listening"]
            hit = hit or known or [c for c in conns if c.state == "opening"]
        else:
            hit = known
        if not hit:
            self.adsp_stray += 1
            return None
        return hit[0], f

    async def adsp_dispatch(self):
        """Type 7 datagrams out of the inbox to their connections, in order, and every
        connection's timers."""
        if not self.adsp_conns and not self._adsp_q:
            return
        keep = []
        for d in self.inbox:
            (self._adsp_q if d[12] == DDP_ADSP and d[8] in (self.node, 0xFF) else keep).append(d)
        self.inbox[:] = keep
        if self._adsp_busy:
            return
        self._adsp_busy = True
        try:
            while self._adsp_q:
                d = self._adsp_q.pop(0)
                src = struct.unpack(">H", d[6:8])[0], d[9], d[11]
                if self.adsp_trace is not None:
                    self.adsp_trace.append(("rx", d[10], bytes(d[13:])))
                r = self._adsp_route(d[10], src, d[13:])
                if r:
                    await r[0].on_packet(src, *r[1])
            now = time.monotonic()
            for c in list(self.adsp_conns):
                await c.tick(now)
        finally:
            self._adsp_busy = False

    async def adsp_run(self, until, seconds):
        """Takes frames and runs the ADSP connections until until() is true (True) or
        seconds have passed (False). Several coroutines of one Mac may wait here at once:
        one reads the wire, the others look again when it has."""
        end = time.monotonic() + seconds
        while True:
            await self.adsp_dispatch()
            if until():
                return True
            left = end - time.monotonic()
            if left <= 0:
                return False
            if self._adsp_reading:
                try:
                    await asyncio.wait_for(self._adsp_evt.wait(), min(left, 0.05))
                except asyncio.TimeoutError:
                    pass
                continue
            self._adsp_reading, self._adsp_evt = True, asyncio.Event()
            try:
                f = await self.t.recv(min(left, 0.02))
                if f:
                    await self.handle(f)
                    await self.adsp_dispatch()
            finally:
                self._adsp_reading = False
                self._adsp_evt.set()

    async def nbp_find(self, name, typ, tries=5, wait=1.0):
        """(net, node, socket) of NAME:TYPE@*, or None: the socket is the NBP answer's."""
        name = name.encode("mac_roman") if isinstance(name, str) else bytes(name)
        typ = typ.encode("mac_roman") if isinstance(typ, str) else bytes(typ)
        nbp = bytes([0x21, 0x2F]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(name) + pstr(typ) + pstr(b"*")
        hit = []

        def answered():
            for i, d in enumerate(self.inbox):
                if d[12] == DDP_NBP and d[10] == MY_NIS and d[13] >> 4 == 3 and d[14] == 0x2F:
                    hit.append(self.inbox.pop(i))
                    return True
            return False

        for _ in range(tries):
            await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
            if await self.adsp_run(answered, wait):
                return struct.unpack(">HBB", hit[0][15:19])
        return None

    async def _sw_control(self, name, nbp_type, sock, **kw):
        addr = await self.nbp_find(name, nbp_type)
        if not addr:
            raise SwError(f"no {name}:{nbp_type} answered the lookup")
        ctl = Adsp(self, sock, **kw)
        try:
            await ctl.open(*addr)
        except AdspError:
            ctl.vanish()
            raise
        return addr, ctl

    async def print_sw(self, name, nbp_type, job, order="apple", user="etpeer", vanish=None,
                       third_leg=True, attn_desc=None, setup=True, settle=2.0, busy_wait=None,
                       busy_tries=60, callback_wait=20.0, stall=60.0,
                       ctl_sock=SW_CTL_SOCK, data_sock=SW_DATA_SOCK):
        """A StyleWriter job to NAME:NBP_TYPE through Apple's network adapter, as a Mac's
        driver sends it (W-1193). job is a page stream as `rip --out-sw` writes it (rects,
        G blocks, 0C); with setup (the default) the reset, `?`, the model's setup letters,
        each page's letters and the ending letters go round it, as the driver sends them.

        order "apple": after the accept, 0012 and Close Advice on the control connection;
        at the end 0012 on the data connection, then Close Advice. order "lpstyl": the
        control connection is left as it is, attentions go without Ack Request, and the
        job ends with a Forward Reset, 00, FF FF FF I, 0012 and the close.

        To misbehave: vanish="accepted" (gone once the request is accepted), "callback"
        (the adapter's Open Request is never answered), "pipe" (gone with the pipe up and
        nothing sent), "page" (gone after the first page's first block); third_leg=False
        (our open of the control connection never sends its 0x82); attn_desc=0x10 or 0x50.

        Returns what happened: result, busy (each busy answer's queue queries), ident,
        model, pages, sent (bytes to the printer), end (the last 0012's answer), gone."""
        if order not in ("apple", "lpstyl"):
            raise ValueError("order is apple or lpstyl")
        if vanish not in (None, "accepted", "callback", "pipe", "page"):
            raise ValueError("vanish is accepted, callback, pipe or page")
        cmds = sw_split(job)
        desc = attn_desc if attn_desc is not None else (
            AD_ATTN | AD_ACKREQ if order == "apple" else AD_ATTN)
        wait = busy_wait if busy_wait is not None else (5.0 if order == "apple" else 15.0)
        req = sw_request(data_sock, user)
        out = dict(result=None, busy=[], ident=None, model=None, pages=0, sent=0, end=None,
                   gone=vanish, order=order)
        u16 = lambda b: struct.unpack(">H", b)[0]     # noqa: E731
        ctl = data = None
        start = time.monotonic()
        try:
            if vanish != "callback":                  # listening before the adapter can call
                data = Adsp(self, data_sock, attn_desc=desc).listen()
            for _ in range(busy_tries):
                addr, ctl = await self._sw_control(name, nbp_type, ctl_sock, attn_desc=desc,
                                                   third_leg=third_leg)
                res = u16(await ctl.attention(SWA_PRINT, req))
                if res == 0:
                    break
                if res == 0xFFFF:
                    await ctl.close()
                    raise SwError("the adapter refused the print request (FF FF)")
                q = dict(result=res, jobs=u16(await ctl.attention(SWA_JOB_COUNT, b"\0")),
                         job=await ctl.attention(SWA_JOB_USER, b"\0", 72),
                         status=await ctl.attention(SWA_JOB_STATUS, b"\0", 259))
                out["busy"].append(q)
                print(f"busy (result {res:04X}, {q['jobs']} job); asking again in {wait:g} s",
                      flush=True)
                if order == "lpstyl":                 # it sleeps, deaf, then ends the session
                    await asyncio.sleep(wait)
                await ctl.attention(SWA_END, b"\0")
                await ctl.close()
                if order == "apple":
                    await self.adsp_run(lambda: False, wait)
            else:
                raise SwError("the printer stayed busy")
            out["result"] = 0
            print(f"print request accepted by {addr[0]}.{addr[1]} socket {addr[2]}", flush=True)
            if vanish == "accepted":
                return out
            if order == "apple":
                await ctl.attention(SWA_END, b"\0")
                await ctl.close()
            if vanish == "callback":
                return out
            try:
                await data.accept(callback_wait)
            except AdspTimeout as e:
                raise SwError(f"the adapter never called back on socket {data_sock}: {e}") from None
            if data.peer[:2] != addr[:2]:
                raise SwError(f"the call back came from {data.peer[0]}.{data.peer[1]}, "
                              f"not the adapter at {addr[0]}.{addr[1]}")
            out["callback"] = data.peer
            out["status_answer"] = await data.attention(SWA_SET_STATUS, sw_status("Printing"))
            print(f"the adapter called back from socket {data.peer[2]}; pipe up", flush=True)
            if vanish == "pipe":
                return out
            if not await self._sw_job(data, cmds, out, setup, settle, stall, vanish):
                return out
            if order == "lpstyl":
                await data.forward_reset()
                data.discard()
                await data.write(b"\0")
                await data.write(b"\xff\xff\xffI")
            out["end"] = await data.attention(SWA_END, b"\0")
            await data.close()
            print(f"sent {out['sent']} bytes, {out['pages']} page(s), in "
                  f"{time.monotonic() - start:.1f} s", flush=True)
            return out
        finally:
            for c in (ctl, data):                     # whatever is left goes without a word
                if c:
                    c.vanish()

    async def _sw_job(self, data, cmds, out, setup, settle, stall, vanish):
        """The printer's side of the job, through the pipe: False when told to vanish."""
        async def say(b):
            await data.write(b)                       # one message, EOM, acknowledged
            out["sent"] += len(b)

        async def ask(letter, tries=3, each=5.0):
            for k in range(tries):
                await say(b"\xff\xff\xff" + letter)
                try:
                    r = (await data.read(1, each))[0]
                except AdspTimeout:
                    continue
                if k:                                 # an answer to an earlier asking may follow
                    await self.adsp_run(lambda: False, 0.5)
                    data.discard()
                return r
            raise SwError(f"the printer does not answer FF FF FF {letter.decode()}")

        async def pause(s):
            await self.adsp_run(lambda: False, s)

        # Reset, settle, `1`, then `?` and what model that is.
        await say(b"\0")
        await say(b"\xff\xff\xffI")
        await pause(settle)
        await ask(b"1", tries=4)
        for _ in range(3):
            await say(b"?")
            try:
                ident = (await data.read_until(0x0D, 5.0)).rstrip(b"\r")
                break
            except AdspTimeout:
                continue
        else:
            raise SwError("the printer does not answer ?")
        model = SW_IDENT.get(ident)
        if ident == b"CS":
            model = SW_SUBMODEL.get(await ask(b"p"), "2400")
        if not model:
            raise SwError(f"the printer answered ? with {ident!r}")
        out["ident"], out["model"] = ident, model
        print(f"the printer says {ident.decode('ascii', 'replace')}: {model}", flush=True)
        series, bdiv = model in ("2200", "2400", "2500"), 64 if model == "original" else 256
        free, cart = 1 << 30, False
        if setup:
            if model != "ii" and model != "original":
                await say(b"N" if model == "1200" else b"D")
            free = await ask(b"B") * bdiv - 2048
            if model != "original":
                h = await ask(b"H")
                if model in ("1500", "2200", "2400", "2500"):
                    if series and h not in (0x01, 0x81):
                        raise SwError(f"no cartridge (H answered {h:02X})")
                    cart = bool(h & 0x80)
            letters = {"original": b"nuA", "ii": b"unDA", "1200": b"unDA", "1500": b"m2ZA"}.get(
                model, b"m2nZA" if cart else b"m2bA")
            await say(letters)
            free -= len(letters)
        else:
            free = await ask(b"B") * bdiv - 2048

        async def clear():                            # `1` and `2` say nothing is wrong
            t0 = time.monotonic()
            while True:
                s1, s2 = await ask(b"1"), await ask(b"2")
                if not s1 & 0xC6 and not s2 & (0x7F if model in ("2400", "2500") else 0xFF):
                    return
                if time.monotonic() - t0 > stall:
                    raise SwError(f"the printer reports a fault (1 = {s1:02X}, 2 = {s2:02X})")
                await pause(0.25)

        async def drained():                          # the adapter has passed on all it holds
            t0 = time.monotonic()
            while True:
                a = await data.attention(SWA_DRAINED, b"\0")
                if a[1]:
                    return
                if time.monotonic() - t0 > 10.0:
                    raise SwError("The printer stopped taking data")
                await pause(0.1)

        async def page_bytes(b):                      # only what the printer's buffer has room for
            nonlocal free
            t0 = time.monotonic()
            while len(b) > free:
                free = await ask(b"B") * bdiv - 2048
                if len(b) <= free:
                    break
                if time.monotonic() - t0 > stall:
                    raise SwError(f"the printer's buffer stayed full ({free} free, {len(b)} to send)")
                await pause(0.1)
            await say(b)
            free -= len(b)

        mode, new_page, blocks = None, True, 0
        for i, (kind, b) in enumerate(cmds):
            if kind in ("rect", "block", "page"):
                if new_page and setup:
                    new_page, letters = False, b""
                    if series:
                        colour = cart and any(c[:1] == b"c" for k, c in self._sw_page(cmds, i)
                                              if k == "rect")
                        want = b"H" if colour else b"B"
                        if want != mode:
                            mode, letters = want, want
                    if model in ("1200", "1500", "2200", "2400", "2500"):
                        letters += b"L"
                    if letters:
                        await say(letters)
                if kind == "rect":
                    await clear()
                    await drained()
                await page_bytes(b)
                if kind == "page":
                    new_page, out["pages"] = True, out["pages"] + 1
                elif kind == "block":
                    blocks += 1
                    if vanish == "page" and blocks == 1:
                        return False
            elif kind == "query" and b[3:] not in (b"I", b"S", b"D", b"N"):
                await ask(b[3:])
            elif kind == "ident":
                await say(b)
                await data.read_until(0x0D, 5.0)
            else:
                await say(b)
        if setup:
            if out["pages"] and model != "original":  # `1` says the last page is printed
                t0 = time.monotonic()
                while not await ask(b"1") & 0x08:
                    if time.monotonic() - t0 > 2 * stall:
                        raise SwError("the printer never said the page was printed")
                    await pause(0.25)
            await say(b"EYl" if model == "1200" else b"El")
        return True

    @staticmethod
    def _sw_page(cmds, i):
        """The commands of the page that starts at cmds[i]."""
        for kind, b in cmds[i:]:
            yield kind, b
            if kind == "page":
                return

    async def sw_queue(self, name, nbp_type, request=None, ctl_sock=SW_CTL_SOCK,
                       attn_desc=AD_ATTN | AD_ACKREQ):
        """What NAME:NBP_TYPE's adapter is printing, asked on a control connection of
        its own: 0011, 000D (with a 70-byte request, zeros if none), 000E, 0010, 0012,
        close. Returns jobs, position, job (000E's 72 bytes), data_sock and user (read
        from them, the request first), status_id, status, status_raw (0010's 259), end."""
        u16 = lambda b: struct.unpack(">H", b)[0]     # noqa: E731
        _, ctl = await self._sw_control(name, nbp_type, ctl_sock, attn_desc=attn_desc)
        try:
            jobs = u16(await ctl.attention(SWA_JOB_COUNT, b"\0"))
            pos = u16(await ctl.attention(SWA_POSITION, request or bytes(SW_REQ_LEN)))
            job = await ctl.attention(SWA_JOB_USER, b"\0\0", 72)
            st = await ctl.attention(SWA_JOB_STATUS, b"\0", 259)
            end = u16(await ctl.attention(SWA_END, b"\0"))
            if ctl.rx:
                raise SwError(f"{len(ctl.rx)} bytes more than the answers asked for")
            await ctl.close()
        finally:
            ctl.vanish()
        return dict(jobs=jobs, position=pos, job=job, data_sock=job[3],
                    user=job[5:5 + min(job[4], 32)].decode("mac_roman"),
                    status_id=u16(st[:2]), status=st[3:3 + st[2]].decode("mac_roman"),
                    status_raw=st, end=end)

    async def sw_rename(self, name, nbp_type, new, ctl_sock=SW_CTL_SOCK,
                        attn_desc=AD_ATTN | AD_ACKREQ):
        """Renames NAME:NBP_TYPE as the Chooser's setup does: 0011, 0009 with the new
        name, 0012, close. Returns each answer: jobs, renamed, end."""
        u16 = lambda b: struct.unpack(">H", b)[0]     # noqa: E731
        new = new.encode("mac_roman") if isinstance(new, str) else bytes(new)
        _, ctl = await self._sw_control(name, nbp_type, ctl_sock, attn_desc=attn_desc)
        try:
            jobs = u16(await ctl.attention(SWA_JOB_COUNT, b"\0"))
            renamed = u16(await ctl.attention(SWA_SET_NAME, pstr(new)))
            end = u16(await ctl.attention(SWA_END, b"\0"))
            await ctl.close()
        finally:
            ctl.vanish()
        return dict(jobs=jobs, renamed=renamed, end=end)


# ------------------------------------------------------------------ the LaserWriter

# A LaserWriter II NT's answers to the board's query job (lw_query.h), CR line
# ends as a LaserWriter writes them, a status line among them.
QUERY_ANSWER = (b"aw:product=LaserWriter II NT\raw:version=47.0\r"
                b"%%[ status: busy; source: AppleTalk ]%%\raw:revision=1\raw:level=1\r"
                b"aw:res=300 300\raw:area=14.16 8.4 597.84 783.6\raw:size=?\r")
PS_ERROR = (b"%%[ Error: undefined; OffendingCommand: aw_x ]%%\r"
            b"%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%\r")


class Conn:
    """The one PAP connection the fake takes at a time. It carries any number
    of jobs, each ended by EOF both ways (W-1095)."""

    def __init__(self, cid, client, quantum):
        self.cid, self.client, self.quantum = cid, client, quantum
        self.seq = 1                       # our next SendData to the client
        self.pull = None                   # (tid, sent at, packets {seq: data}, eof)
        self.out = bytearray()             # our output, for the client's reads
        self.read = None                   # the client's read: (tid, (net, node), sock, want, seq)
        self.answered = {}                 # its seq -> the packets we answered with
        self.heard = self.tickled = time.monotonic()
        self.jobs = 0                      # jobs ended both ways on this connection
        self.next_job()

    def next_job(self):
        self.job = bytearray()
        self.eof = False                   # the client's EOF: the job is all here
        self.done = False                  # the job is processed: EOF after `out`
        self.error = False


class LaserWriter(Mac):
    """A fake PostScript LaserWriter on AppleTalk (W-1082): see the module doc."""

    def __init__(self, t, name, hex_only=False, out=None, busy=0, error=False, paper=False,
                 pages=4242, addr=None, password=False):
        super().__init__(t)
        if addr:                                       # the address it had: tried first
            self.net, self.node = addr
        self.pages = pages                             # its page count, one more each job
        self.lw_name = name.encode("mac_roman")[:32]
        self.hex_only, self.out_dir = hex_only, pathlib.Path(out) if out else None
        self.start_page, self.password = True, password
        self.busy_until = time.monotonic() + busy
        self.error_next, self.paused, self.dropped = error, False, False
        self.fault = None
        self.conn, self.jobs, self.xo = None, 0, {}
        if paper:
            self.command("paper")

    # -- what it says about itself
    def status(self):
        if self.fault:
            return self.fault
        if self.conn and self.conn.job:
            return "job: AirWriter; status: busy; source: AppleTalk"
        if time.monotonic() < self.busy_until:
            return "status: busy; source: AppleTalk"
        return "status: idle"

    def command(self, line):
        """A fault from the bench (stdin) or a test."""
        w = line.split()
        if not w:
            return
        if w[0] == "busy":
            self.busy_until = time.monotonic() + float(w[1] if len(w) > 1 else 30)
        elif w[0] in ("paper", "jam"):
            self.fault = ("status: PrinterError: out of paper" if w[0] == "paper"
                          else "status: PrinterError: paper jam")
            self.paused = True
        elif w[0] == "ok":
            self.fault, self.paused = None, False
        elif w[0] == "error":
            self.error_next = True
        elif w[0] == "drop":
            self.dropped = True
        elif w[0] == "password":
            self.password = True
        else:
            print(f"unknown command {line!r}: busy N, paper, jam, ok, error, drop")
            return
        print(f"fake LaserWriter: {line.strip()} -> {self.status()}")
        if self.conn and w[0] in ("paper", "jam", "ok"):
            self.conn.out += f"%%[ {self.status()} ]%%\r".encode()

    async def check_name(self, times=5, gap=2.0):
        """As a LaserWriter switched on does (the NTR, W-1098): its own name
        looked up a few times, 2 s apart, before it takes it."""
        nbp = bytes([0x21, 0x2E]) + struct.pack(">HBBB", self.net, self.node, MY_NIS, 0) + \
              pstr(self.lw_name) + pstr(b"LaserWriter") + pstr(b"*")
        for _ in range(times):
            await self.send_ddp(0, 0xFF, NIS, MY_NIS, DDP_NBP, nbp)
            await asyncio.sleep(gap)

    # -- frames
    async def serve(self):
        """Answers lookups and PAP until cancelled."""
        while True:
            f = await self.t.recv(0.05)
            if f and not self.dropped:
                await self.handle(f)
            while self.inbox:
                d = self.inbox.pop(0)
                if not self.dropped:
                    await self.dispatch(d)
            if not self.dropped:
                await self.tick()

    async def dispatch(self, d):
        if d[8] not in (self.node, 0xFF):              # another fake's, on a shared tunnel
            return
        src = struct.unpack(">H", d[6:8])[0], d[9]
        if d[12] == DDP_NBP and d[10] == NIS:
            await self.on_nbp(d)
        elif d[12] == DDP_ATP and d[10] in (LW_SLS, LW_SESS):
            await self.on_atp(d, src)

    async def on_nbp(self, d):
        if d[13] >> 4 != 2:
            return
        t = d[15:]
        net, node, sock = struct.unpack(">HBB", t[:4])
        obj = t[6:6 + t[5]]
        typ = t[7 + t[5]:7 + t[5] + t[6 + t[5]]]
        if obj not in (b"=", ) and obj.lower() != self.lw_name.lower():
            return
        if typ != b"=" and typ.lower() != LW_TYPE.lower():
            return
        rep = bytes([0x31, d[14]]) + struct.pack(">HBBB", self.net, self.node, LW_SLS, 0) + \
            pstr(self.lw_name) + pstr(LW_TYPE) + pstr(b"*")
        await self.send_ddp(net, node, sock, NIS, DDP_NBP, rep)

    async def respond(self, src, sock, our, tid, packets, xo):
        """An ATP response of (user, data) packets; kept for XO duplicates."""
        raw = [atp(TRESP | (EOM if k == len(packets) - 1 else 0), k, tid, u, b)
               for k, (u, b) in enumerate(packets)]
        for r in raw:
            await self.send_ddp(src[0], src[1], sock, our, DDP_ATP, r)
        if xo:
            self.xo[(src, sock, tid)] = (our, raw)

    async def on_atp(self, d, src):
        ctl, bitmap, tid = d[13], d[14], struct.unpack(">H", d[15:17])[0]
        user, data, sock, our = d[17:21], d[21:], d[11], d[10]
        fn = ctl & 0xC0
        key = (src, sock, tid)
        if fn == TREL:
            self.xo.pop(key, None)
            return
        if fn == TREQ and ctl & XO and key in self.xo:
            for r in self.xo[key][1]:
                await self.send_ddp(src[0], src[1], sock, self.xo[key][0], DDP_ATP, r)
            return
        if our == LW_SLS and fn == TREQ:
            if user[1] == PAP_SEND_STATUS:
                await self.respond(src, sock, our, tid, [([0, PAP_STATUS, 0, 0],
                                   b"\0" * 4 + pstr(self.status().encode()))], ctl & XO)
            elif user[1] == PAP_OPEN and len(data) >= 2:
                busy = self.conn is not None or time.monotonic() < self.busy_until
                if busy:
                    rep = bytes([LW_SESS, 8, 0xFF, 0xFF]) + pstr(b"status: busy; source: AppleTalk")
                else:
                    self.conn = Conn(user[0], (src, data[0]), max(1, min(8, data[1])))
                    rep = bytes([LW_SESS, 8, 0, 0]) + pstr(self.status().encode())
                    if self.fault:                 # a fault says so on the output too
                        self.conn.out += f"%%[ {self.fault} ]%%\r".encode()
                    print(f"fake LaserWriter: connection {user[0]} from {src[0]}.{src[1]}")
                await self.respond(src, sock, our, tid, [([user[0], PAP_OPEN_REPLY, 0, 0], rep)],
                                   ctl & XO)
            return
        c = self.conn
        if our != LW_SESS or not c or user[0] != c.cid:
            return
        c.heard = time.monotonic()
        if fn == TRESP and c.pull and tid == c.pull[0] and user[1] == PAP_DATA:
            c.pull[2][bitmap] = data
            if user[2]:
                c.pull[3][0] = True
            if ctl & EOM:
                c.pull[3][1] = bitmap
            last = c.pull[3][1]
            if last is not None and all(k in c.pull[2] for k in range(last + 1)):
                await self.pulled(c, src)
            return
        if fn != TREQ:
            return
        if user[1] == PAP_SEND_DATA:
            seq = user[2] << 8 | user[3]
            if seq and seq in c.answered:
                await self.respond(src, sock, our, tid, c.answered[seq], ctl & XO)
                return
            want = 0
            while want < 8 and bitmap >> want & 1:
                want += 1
            c.read = (tid, src, sock, max(1, want), seq, ctl & XO)
        elif user[1] == PAP_CLOSE:
            await self.respond(src, sock, our, tid, [([c.cid, PAP_CLOSE_REPLY, 0, 0], b"")], ctl & XO)
            if c.job and not c.eof:
                print(f"fake LaserWriter: connection {c.cid} closed after {len(c.job)} bytes, no EOF")
                self.save(c.job, complete=False)
            else:
                print(f"fake LaserWriter: connection {c.cid} closed after {c.jobs} "
                      f"job{'' if c.jobs == 1 else 's'}")
            self.conn = None

    async def pulled(self, c, src):
        tid, _, packets, flags = c.pull
        await self.send_ddp(src[0], src[1], c.client[1], LW_SESS, DDP_ATP, atp(TREL, 0, tid, b"\0" * 4))
        chunk = b"".join(packets[k] for k in sorted(packets))
        if not c.job and chunk and self.error_next:
            self.error_next, c.error = False, True
            c.out += PS_ERROR
        c.job += chunk
        c.pull = None
        c.seq = 1 if c.seq == 0xFFFF else c.seq + 1
        if flags[0]:
            c.eof = True
            self.finish(c)

    def finish(self, c):
        job = bytes(c.job)
        if job.startswith(b"%!PS-Adobe-3.0\n%%Title: AirWriter query"):
            intact = bytes(range(256)) in job
            probe = b"256 1" if intact and not self.hex_only else b"256 0"
            c.out += (QUERY_ANSWER + b"aw:pages=%d\r" % self.pages + b"aw:startpage="
                      + (b"true" if self.start_page else b"false")
                      + b"\raw:probe=" + probe + b"\raw:end\r")
            print(f"fake LaserWriter: query answered (binary {'ok' if probe.endswith(b'1') else 'refused'})")
        elif job.startswith(b"%!PS-Adobe-3.0\n%%Title: AirWriter start page"):
            # W-1099: exitserver with password 0, then setdostartpage, read back.
            if self.password:
                c.out += (b"%%[ Error: invalidaccess; OffendingCommand: exitserver ]%%\r"
                          b"%%[ Flushing: rest of job (to end-of-file) will be ignored ]%%\r")
                print("fake LaserWriter: start page refused (password)")
            else:
                if b"true setdostartpage" in job or b"false setdostartpage" in job:
                    self.start_page = b"true setdostartpage" in job
                c.out += (b"%%[ exitserver: permanent state may be changed ]%%\r"
                          b"aw:startpage=" + (b"true" if self.start_page else b"false")
                          + b"\raw:end\r")
                print(f"fake LaserWriter: start page {'on' if self.start_page else 'off'}")
        elif not c.error:
            self.save(job)
        c.done = True

    def save(self, job, complete=True):
        self.jobs += 1
        self.pages += 1
        print(f"fake LaserWriter: job {self.jobs}, {len(job)} bytes{'' if complete else ', cut short'}")
        if not self.out_dir:
            return
        self.out_dir.mkdir(parents=True, exist_ok=True)
        ps = self.out_dir / f"job-{self.jobs}.ps"
        ps.write_bytes(job)
        if complete and shutil.which(GS):
            png = ps.with_suffix(".png")
            r = subprocess.run([GS, "-q", "-dSAFER", "-dBATCH", "-dNOPAUSE", "-sDEVICE=pnggray",
                                "-r300", f"-sOutputFile={png}", str(ps)], capture_output=True)
            if r.returncode == 0:
                print(f"fake LaserWriter: drawn as {png}")

    async def tick(self):
        c = self.conn
        if not c:
            return
        now = time.monotonic()
        # Pull the job while there is paper; again after 2 s unanswered.
        if not c.eof and not self.paused and (not c.pull or now - c.pull[1] >= 2.0):
            if c.pull:
                tid = c.pull[0]
                c.pull = (tid, now, c.pull[2], c.pull[3])
            else:
                tid = self.next_tid()
                c.pull = (tid, now, {}, [False, None])
            await self.send_ddp(c.client[0][0], c.client[0][1], c.client[1], LW_SESS, DDP_ATP,
                                atp(TREQ | XO, 0xFF, tid, [c.cid, PAP_SEND_DATA, c.seq >> 8, c.seq & 0xFF]))
        # The client reads our output: what there is, EOF once the job is done.
        if c.read and (c.out or c.done):
            tid, src, sock, want, seq, xo = c.read
            take = bytes(c.out[:want * 512])
            del c.out[:len(take)]
            eof = 1 if c.done and not c.out else 0
            packets = [([c.cid, PAP_DATA, eof, 0], take[k:k + 512])
                       for k in range(0, max(len(take), 1), 512)]
            await self.respond(src, sock, LW_SESS, tid, packets, xo)
            if seq:
                c.answered[seq] = packets
            c.read = None
            if eof:                            # the job is over both ways: pull the next
                c.jobs += 1
                c.next_job()
        if now - c.tickled >= 30:
            await self.send_ddp(c.client[0][0], c.client[0][1], c.client[1], LW_SESS, DDP_ATP,
                                atp(TREQ, 0, self.next_tid(), [c.cid, PAP_TICKLE, 0, 0]))
            c.tickled = now
        if now - c.heard >= 120:
            print(f"fake LaserWriter: connection {c.cid} timed out")
            self.conn = None


async def follow_ip(t, board):
    """A tunnel fake's MAC is this Mac's IPv4 address: when the address moves
    under it (W-1086: the board then heard a printer that never answered),
    start again with the new one."""
    while True:
        await asyncio.sleep(10)
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.connect((board, TUNNEL_PORT))
            ip = s.getsockname()[0]
            s.close()
        except OSError:
            continue
        if ip != t.ip:
            print(f"this Mac's address moved ({t.ip} -> {ip}); starting again", flush=True)
            await t.close()
            os.execv(sys.executable, [sys.executable, "-u"] + sys.argv)


async def stdin_commands(fake):
    loop = asyncio.get_running_loop()
    while True:
        line = await loop.run_in_executor(None, sys.stdin.readline)
        if not line:
            return
        fake.command(line)


async def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[0] == "tunnel":
        board = os.environ.get("AIRWRITER", "10.0.1.115")
        t, rest = Tunnel(board), argv[1:]
    elif argv[0] == "zone" and len(argv) >= 3:
        t, rest = Zone(argv[1]), argv[2:]
    elif argv[0] == "pipe":
        loop = asyncio.get_running_loop()
        reader = asyncio.StreamReader()
        await loop.connect_read_pipe(lambda: asyncio.StreamReaderProtocol(reader), sys.stdin)
        wt, wp = await loop.connect_write_pipe(asyncio.streams.FlowControlMixin, sys.stdout)
        t, rest = Pipe(reader, asyncio.StreamWriter(wt, wp, reader, loop)), argv[1:]
    else:
        sys.exit(__doc__)
    await t.open()
    if rest[0] == "laserwriter" and len(rest) >= 2:
        opts = rest[2:]
        out = opts[opts.index("--out") + 1] if "--out" in opts else None
        also = [opts[i + 1] for i, o in enumerate(opts) if o == "--also" and i + 1 < len(opts)]
        hub = Hub(t) if also else None
        names = [rest[1]] + also
        pages = int(opts[opts.index("--pages") + 1]) if "--pages" in opts else 4242
        addr = None
        if "--addr" in opts:
            n, _, d = opts[opts.index("--addr") + 1].partition(".")
            addr = (int(n), int(d))
        fakes = []
        for k, name in enumerate(names):
            d = out if not k or not out else os.path.join(out, name)
            fakes.append(LaserWriter(hub.tap() if hub else t, name, hex_only="--hex-only" in opts,
                                     out=d, pages=pages, addr=addr if not k else None))
        try:
            tasks = [hub.pump()] if hub else []
            pump = asyncio.ensure_future(asyncio.gather(*tasks)) if tasks else None
            for name, fake in zip(names, fakes):
                await fake.acquire()
                if argv[0] != "pipe" and "--no-check" not in opts:
                    await fake.check_name()
                print(f"fake LaserWriter \"{name}\" at {fake.net}.{fake.node}, socket {LW_SLS}, "
                      f"{fake.pages} pages", file=sys.stderr if argv[0] == "pipe" else sys.stdout)
            tasks = [fake.serve() for fake in fakes]
            if argv[0] != "pipe":
                tasks.append(stdin_commands(fakes[0]))
            if argv[0] == "tunnel":
                tasks.append(follow_ip(t, os.environ.get("AIRWRITER", "10.0.1.115")))
            await asyncio.gather(*tasks)
            if pump:
                pump.cancel()
        finally:
            await t.close()
        return
    mac = Mac(t)
    try:
        await mac.acquire()
        if rest[0] == "lookup":
            await mac.lookup()
        elif rest[0] == "chooser" and len(rest) == 2:
            await mac.chooser(rest[1])
        elif rest[0] == "print" and len(rest) == 2:
            await mac.print_file(open(rest[1], "rb").read())
        elif rest[0] == "print-ps" and len([a for a in rest if a != "--close-early"]) >= 3:
            files = [a for a in rest[2:] if a != "--close-early"]
            await mac.print_ps(rest[1], [open(f, "rb").read() for f in files],
                               close_early="--close-early" in rest)
        elif rest[0] == "status" and len(rest) == 3:
            await mac.watch_status(rest[1], float(rest[2]))
        elif rest[0] == "print-sw" and len([a for a in rest if a != "--lpstyl"]) == 4:
            a = [a for a in rest if a != "--lpstyl"]
            try:
                await mac.print_sw(a[1], a[2], open(a[3], "rb").read(),
                                   order="lpstyl" if "--lpstyl" in rest else "apple")
            except (AdspError, SwError, ValueError) as e:
                sys.exit(str(e))
        elif rest[0] == "sw-queue" and len(rest) == 3:
            try:
                q = await mac.sw_queue(rest[1], rest[2])
            except (AdspError, SwError) as e:
                sys.exit(str(e))
            print(f"{q['jobs']} job(s), position {q['position']}; user {q['user']!r}, "
                  f"status {q['status_id']:04X} {q['status']!r}")
        elif rest[0] == "sw-rename" and len(rest) == 4:
            try:
                r = await mac.sw_rename(rest[1], rest[2], rest[3])
            except (AdspError, SwError) as e:
                sys.exit(str(e))
            print(f"asked: the adapter answered {r['renamed']:04X}")
        elif rest[0] == "named" and len(rest) == 3:
            mac.name = rest[1].replace("'", "\u2019").encode("mac_roman")[:31]
            await mac.lookup()
            await mac.pump(float(rest[2]))
        else:
            sys.exit(__doc__)
    finally:
        await t.close()


if __name__ == "__main__":
    asyncio.run(main(sys.argv[1:]))
