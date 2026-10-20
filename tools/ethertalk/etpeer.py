#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""An EtherTalk Mac in software, for testing AirWriter without an emulator.

It speaks AppleTalk Phase 2 over the two ways the board takes it (W-1076):
Basilisk II's UDP tunnel on this network, or an Infinite Mac zone on
infinitemac.org. It takes an address with AARP, looks the printer up with NBP
as the Chooser does, and prints a file over PAP as the AppleTalk ImageWriter
driver does (OpenConn, the board's SendData answered with Data, CloseConn).

    python3 tools/ethertalk/etpeer.py tunnel lookup
    python3 tools/ethertalk/etpeer.py tunnel print fixtures/pap-iw701-page.bin
    python3 tools/ethertalk/etpeer.py tunnel named "Wells's iMac" 30
    python3 tools/ethertalk/etpeer.py zone ZONE lookup
    python3 tools/ethertalk/etpeer.py zone ZONE print FILE

`named` registers a Macintosh name, as System 7's Sharing Setup does
(<name>:Workstation), looks the printer up so the board hears it, and answers
the board's name lookups for the given seconds.

The file must be an ImageWriter stream: the board prints it as it is. The
tunnel binds UDP 6066, so it cannot run beside Basilisk II on the same
computer. The zone needs `pip install websockets` (a venv is fine).
"""
import asyncio, json, os, random, socket, struct, sys, time, zlib

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
        for attempt in range(30):
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


async def main(argv):
    if len(argv) < 2:
        sys.exit(__doc__)
    if argv[0] == "tunnel":
        board = os.environ.get("AIRWRITER", "10.0.1.115")
        t, rest = Tunnel(board), argv[1:]
    elif argv[0] == "zone" and len(argv) >= 3:
        t, rest = Zone(argv[1]), argv[2:]
    else:
        sys.exit(__doc__)
    await t.open()
    mac = Mac(t)
    try:
        await mac.acquire()
        if rest[0] == "lookup":
            await mac.lookup()
        elif rest[0] == "print" and len(rest) == 2:
            await mac.print_file(open(rest[1], "rb").read())
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
