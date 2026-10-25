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
