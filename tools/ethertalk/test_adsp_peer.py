#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""etpeer.py's ADSP and its StyleWriter driver's side (W-1193), on this computer.

Two Adsp ends talk over a wire in this process: the open both ways, data and
the EOM that counts one, a window shut and opened again, attentions and their
answers, lost packets sent again, the close, and what the Mac's end refuses.
Then Mac.print_sw, sw_queue and sw_rename against a small adapter written
here: it answers the print request, calls back, and pipes to a canned printer.

    python3 -m unittest tools/ethertalk/test_adsp_peer.py
"""
import asyncio, contextlib, io, pathlib, struct, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import etpeer  # noqa: E402
from etpeer import Adsp, AdspError, AdspTimeout, AdspClosed, SwError  # noqa: E402

NAME, TYPE, SOCK = "Colour 2500", "ColorStyleWriter2400AT", 129


class Wire:
    """Two peers on one wire: each hears the other. `drop(frame)` true loses a frame."""

    def __init__(self, mac, q, peer_q):
        self.mac, self.q, self.peer_q, self.drop = mac, q, peer_q, None

    @staticmethod
    def pair():
        a, b = asyncio.Queue(), asyncio.Queue()
        return Wire(b"B2\x00\x00\x00\x02", a, b), Wire(b"B2\x00\x00\x00\x03", b, a)

    async def open(self):
        return self

    async def send(self, frame):
        if self.drop and self.drop(frame):
            return
        self.peer_q.put_nowait(frame)

    async def recv(self, timeout):
        try:
            return await asyncio.wait_for(self.q.get(), timeout)
        except asyncio.TimeoutError:
            return None

    async def close(self):
        pass


def macs():
    """Two Macs with their addresses, on one wire, each knowing where the other is."""
    wa, wb = Wire.pair()
    a, b = etpeer.Mac(wa), etpeer.Mac(wb)
    a.net, a.node, b.net, b.node = 0xFF01, 10, 0xFF01, 20
    a.online = b.online = True
    a.map[(b.net, b.node)], b.map[(a.net, a.node)] = wb.mac, wa.mac
    a.adsp_trace, b.adsp_trace = [], []
    return a, b


def adsp_of(frame):
    """A frame's ADSP packet, or None: Ethernet 14, SNAP 8, DDP 13."""
    return frame[35:] if frame[14:22] == etpeer.SNAP_DDP and frame[34] == etpeer.DDP_ADSP else None


def desc_of(frame):
    p = adsp_of(frame)
    return p[12] if p and len(p) >= 13 else None


class Base(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.a, self.b = macs()
        self.tasks = []

    async def asyncTearDown(self):
        for t in self.tasks:
            t.cancel()
        await asyncio.gather(*self.tasks, return_exceptions=True)

    def bg(self, coro):
        t = asyncio.create_task(coro)
        self.tasks.append(t)
        return t

    def serve(self, mac):
        """Keeps a Mac's ADSP answering while the test waits on the other."""
        async def run():
            while True:
                await mac.adsp_run(lambda: False, 1.0)
        return self.bg(run())

    async def connected(self, **kw):
        """a's connection opened to b's listener, both Macs served."""
        lb = Adsp(self.b, 0x81, **kw.pop("b", {})).listen()
        ca = Adsp(self.a, 0xF2, **kw.pop("a", {}))
        self.serve(self.b)
        self.serve(self.a)
        await ca.open(self.b.net, self.b.node, 0x81)
        await lb.accept(2.0)
        return ca, lb


# ---------------------------------------------------------------- the codec

class Codec(unittest.TestCase):
    def test_vectors(self):
        """Brief D.1: the header vectors, both ways."""
        for hexs, fields in (
            ("12340000000000000000100081", (0x1234, 0, 0, 4096, 0x81)),
            ("56780000000000000000200082", (0x5678, 0, 0, 8192, 0x82)),
            ("ABCD0001000000020000200080", (0xABCD, 0x00010000, 0x00020000, 0x2000, 0x80)),
            ("0042000003E8000007D0080088", (0x0042, 1000, 2000, 2048, 0x88)),
            ("000A00000000000000000 6B440".replace(" ", ""), (0x000A, 0, 0, 1716, 0x40)),
            ("000A0000000000000001000090", (0x000A, 0, 1, 0, 0x90)),
        ):
            raw = bytes.fromhex(hexs)
            self.assertEqual(etpeer.adsp_unpack(raw), fields + (b"",))
            self.assertEqual(etpeer.adsp_pack(*fields), raw)
        close = etpeer.adsp_pack(0x9999, 1234567, 7654321, 0, 0x85)
        self.assertEqual(close, struct.pack(">HIIHB", 0x9999, 1234567, 7654321, 0, 0x85))
        self.assertEqual(etpeer.adsp_unpack(close)[:5], (0x9999, 1234567, 7654321, 0, 0x85))

    def test_rejects(self):
        with self.assertRaises(ValueError):
            etpeer.adsp_unpack(bytes(12) + b"\x99")       # an unknown control code
        with self.assertRaises(ValueError):
            etpeer.adsp_unpack(b"\x12\x34\x00")           # short

    def test_open_packet(self):
        """Brief D.3: the three legs are 21 bytes, version 0100, the other end's ConnID."""
        self.assertEqual(etpeer.adsp_open_pack(0x4D01, 0x0800, etpeer.AD_OPEN_REQ, 0),
                         bytes.fromhex("4D01 00000000 00000000 0800 81 0100 0000 00000000"))
        self.assertEqual(etpeer.adsp_open_pack(0xA001, 0x1000, etpeer.AD_OPEN_REQACK, 0x4D01),
                         bytes.fromhex("A001 00000000 00000000 1000 83 0100 4D01 00000000"))
        self.assertEqual(etpeer.adsp_open_pack(0x4D01, 0x0800, etpeer.AD_OPEN_ACK, 0xA001),
                         bytes.fromhex("4D01 00000000 00000000 0800 82 0100 A001 00000000"))

    def test_request_and_status(self):
        """Brief D.8's REQ, and 000A's 257 bytes."""
        req = etpeer.sw_request(0xF1, "Bob")
        self.assertEqual(req, bytes.fromhex("000000F103426F62") + bytes(62))
        st = etpeer.sw_status("Printing")
        self.assertEqual(st, bytes.fromhex("0647085072696E74696E67") + bytes(246))


class Split(unittest.TestCase):
    RECT = b"R" + struct.pack("<HHHH", 16, 0, 23, 1)
    BLOCK = b"G" + struct.pack("<H", 3) + b"\x41\x3f\x80" + b"\0"

    def test_page(self):
        job = self.RECT + self.BLOCK + b"\x0c"
        self.assertEqual(etpeer.sw_split(job),
                         [("rect", self.RECT), ("block", self.BLOCK), ("page", b"\x0c")])

    def test_letters_and_queries(self):
        job = b"\0\xff\xff\xffI?m2nZA\xff\xff\xffBL" + b"c" + bytes(8) + b"El"
        kinds = [k for k, _ in etpeer.sw_split(job)]
        self.assertEqual(kinds, ["zero", "query", "ident", "letter", "letter", "letter", "letter",
                                 "query", "letter", "rect", "letter", "letter"])
        self.assertEqual(etpeer.sw_split(job)[3], ("letter", b"m2"))
        self.assertEqual(b"".join(b for _, b in etpeer.sw_split(job)), job)

    def test_a_block_may_hold_any_byte(self):
        body = bytes(range(256)) * 3                       # ?, G, FF and 0C inside a block are data
        blk = b"G" + struct.pack("<H", len(body)) + body + b"\0"
        self.assertEqual(etpeer.sw_split(blk + b"\x0c"), [("block", blk), ("page", b"\x0c")])

    def test_bad(self):
        for bad in (b"R\x00\x00", b"G\x05\x00abc", b"G\x02\x00ab\x01", b"\xff\xffI", b"\xff\xff\xff",
                    b"\xff\xff\xff\xff", b"m", b"q", b"G"):
            with self.assertRaises(ValueError, msg=bad):
                etpeer.sw_split(bad)


# ---------------------------------------------------------------- two ends

class TwoEnds(Base):
    async def test_open_as_opener_and_listener(self):
        """The three legs, each 21 bytes; both ends open; and the same the other way."""
        ca, lb = await self.connected()
        self.assertEqual((ca.state, lb.state), ("open", "open"))
        self.assertEqual((ca.peer_id, lb.peer_id), (lb.conn_id, ca.conn_id))
        legs = [p for d, s, p in self.b.adsp_trace][:3]
        sent = [p for d, s, p in self.a.adsp_trace if d == "tx"][:2]
        self.assertEqual(sent[0], etpeer.adsp_open_pack(ca.conn_id, 2048, 1, 0))
        self.assertEqual(legs[1], etpeer.adsp_open_pack(lb.conn_id, 2048, 3, ca.conn_id))
        self.assertEqual(sent[1], etpeer.adsp_open_pack(ca.conn_id, 2048, 2, lb.conn_id))
        self.assertTrue(all(len(p) == 21 for p in legs))
        # b opens back to a, from the socket it listens on, as the adapter calls back.
        la = Adsp(self.a, 0xF1).listen()
        cb = Adsp(self.b, 0x81)
        await cb.open(self.a.net, self.a.node, 0xF1)
        await la.accept(2.0)
        self.assertEqual((cb.state, la.state, la.peer), ("open", "open", (self.b.net, self.b.node, 0x81)))
        await cb.write(b"back")
        await ca.write(b"forth")
        self.assertEqual(await la.read(4), b"back")       # two connections on b's one socket
        self.assertEqual(await lb.read(5), b"forth")

    async def test_open_request_is_sent_again(self):
        """The first Open Request is lost: it goes again a second later."""
        lost = []
        self.a.t.drop = lambda f: desc_of(f) == 0x81 and not lost and not lost.append(f)
        lb = Adsp(self.b, 0x81).listen()
        ca = Adsp(self.a, 0xF2)
        ca.open_retry = 0.1
        self.serve(self.b)
        await ca.open(self.b.net, self.b.node, 0x81)
        await lb.accept(2.0)
        self.assertEqual(len(lost), 1)

    async def test_no_listener(self):
        ca = Adsp(self.a, 0xF2)
        ca.open_retry = 0.02
        self.serve(self.b)
        with self.assertRaises(AdspError):
            await ca.open(self.b.net, self.b.node, 0x81)
        self.assertEqual(sum(1 for d, s, p in self.a.adsp_trace if p[12] == 0x81), 6)
        self.assertEqual(self.b.adsp_stray, 0)             # b has no ADSP at all: left in its inbox
        self.assertEqual(self.a.adsp_conns, [])

    async def test_no_third_leg(self):
        """An opener that never sends 0x82: a strict listener waits for it, a lenient one is open."""
        lb = Adsp(self.b, 0x81).listen()
        ca = Adsp(self.a, 0xF2, third_leg=False)
        self.serve(self.b)
        self.serve(self.a)
        await ca.open(self.b.net, self.b.node, 0x81)
        with self.assertRaises(AdspTimeout):
            await lb.accept(0.2)
        self.assertEqual(lb.state, "accepting")
        lb2 = Adsp(self.b, 0x82, need_third_leg=False).listen()
        ca2 = Adsp(self.a, 0xF3, third_leg=False)
        await ca2.open(self.b.net, self.b.node, 0x82)
        await lb2.accept(1.0)
        await ca2.write(b"ok")
        self.assertEqual(await lb2.read(2), b"ok")

    async def test_eom_counts_one(self):
        """Brief D.5: `?` as a message moves the sequence 0 to 2, and the ack names 2."""
        ca, lb = await self.connected()
        await ca.write(b"?")
        self.assertEqual((ca.send_seq, ca.acked, lb.recv_seq), (2, 2, 2))
        self.assertEqual(await lb.read_message(), b"?")
        await lb.write(b"CS\r", eom=False)                 # the printer's bytes: no EOM
        self.assertEqual((lb.send_seq, ca.recv_seq, ca.eoms_in), (3, 3, 0))
        self.assertEqual(await ca.read_until(0x0D), b"CS\r")
        await ca.write(b"\xff\xff\xff1")
        self.assertEqual(ca.send_seq, 7)
        tx = [p for d, s, p in self.a.adsp_trace if d == "tx" and not p[12] & 0x80]
        self.assertEqual(tx[0], etpeer.adsp_pack(ca.conn_id, 0, 0, 2048, 0x60, b"?"))
        self.assertEqual(tx[1], etpeer.adsp_pack(ca.conn_id, 2, 3, 2048, 0x60, b"\xff\xff\xff1"))

    async def test_long_message_and_bare_eom(self):
        """600 bytes: 572 with Ack Request, then 28 with EOM; the ack names 601 more."""
        ca, lb = await self.connected()
        body = bytes(range(256)) * 2 + bytes(88)
        await ca.write(body)
        tx = [p for d, s, p in self.a.adsp_trace if d == "tx" and not p[12] & 0x80]
        self.assertEqual([(len(p) - 13, p[12]) for p in tx], [(572, 0x40), (28, 0x60)])
        self.assertEqual((ca.send_seq, ca.acked), (601, 601))
        self.assertEqual(await lb.read_message(), body)
        await ca.write(b"")                                # a bare EOM: one sequence number, no bytes
        self.assertEqual((ca.send_seq, ca.acked, lb.recv_seq), (602, 602, 602))
        self.assertEqual(await lb.read_message(), b"")
        await ca.write(b"ab", eom=False)
        await ca.write(b"c")
        self.assertEqual(await lb.read_message(), b"abc")
        self.assertEqual(ca.send_seq, 606)

    async def test_window_shut_and_opened_again(self):
        """b takes 100 bytes at a time: a sends no more than it may, and goes on when b,
        unasked, says the window is open again. Nothing is lost or out of order."""
        ca, lb = await self.connected(b=dict(window=100))
        body = bytes((i * 7) & 0xFF for i in range(1000))
        w = self.bg(ca.write(body))
        await asyncio.sleep(0.3)
        self.assertEqual((ca.send_seq, ca.acked, ca.peer_window, ca.room()), (100, 100, 0, 0))
        self.assertFalse(w.done())
        probes = sum(1 for d, s, p in self.a.adsp_trace if d == "tx" and p[12] == 0xC0)
        self.assertEqual(probes, 0)                        # a never asks: b has to say
        got = bytearray()
        while len(got) < len(body):
            got += await lb.read(min(100, len(body) - len(got)), 2.0)
        await asyncio.wait_for(w, 2.0)
        self.assertEqual(bytes(got), body)
        self.assertEqual(ca.acked, 1001)
        self.assertIsNone(lb.error)

    async def test_window_never_opens(self):
        ca, lb = await self.connected(b=dict(window=10))
        with self.assertRaisesRegex(AdspTimeout, "window stayed shut"):
            await ca.write(bytes(30), timeout=0.3)

    async def test_attention_and_answer(self):
        """An attention is acknowledged with 0x90 and answered on the stream; attention
        sequences count messages and leave the stream's alone."""
        ca, lb = await self.connected()

        async def adapter():
            while True:
                code, body = await lb.next_attention(5.0)
                await lb.write(struct.pack(">H", code + 0x100), eom=False)

        self.bg(adapter())
        self.assertEqual(await ca.attention(0x0011, b"\0"), b"\x01\x11")
        self.assertEqual(await ca.attention(0x0009, b"\x03New"), b"\x01\x09")
        self.assertEqual((ca.attn_send, lb.attn_recv, ca.send_seq, lb.recv_seq), (2, 2, 0, 0))
        att = [p for d, s, p in self.a.adsp_trace if d == "tx" and p[12] == 0x50]
        self.assertEqual(att[0], etpeer.adsp_pack(ca.conn_id, 0, 0, 0, 0x50, b"\x00\x11\x00"))
        self.assertEqual(att[1][:13], etpeer.adsp_pack(ca.conn_id, 1, 0, 0, 0x50))
        acks = [p for d, s, p in self.a.adsp_trace if d == "rx" and p[12] == 0x90]
        self.assertEqual(acks, [etpeer.adsp_pack(lb.conn_id, 0, 1, 0, 0x90),
                                etpeer.adsp_pack(lb.conn_id, 0, 2, 0, 0x90)])
        await ca.write(b"still")                           # the stream still moves
        self.assertEqual(await lb.read(5), b"still")

    async def test_attention_without_ack_request(self):
        """lpstyl's 0x10 is acknowledged like a Mac's 0x50."""
        ca, lb = await self.connected(a=dict(attn_desc=0x10))
        self.bg(self.answer(lb, b"\0\0"))
        self.assertEqual(await ca.attention(0x0012, b"\0"), b"\0\0")
        self.assertTrue(any(p[12] == 0x10 for d, s, p in self.a.adsp_trace if d == "tx"))

    @staticmethod
    async def answer(conn, what, eom=False):
        while True:
            if await conn.next_attention(5.0):
                await conn.write(what, eom=eom)

    async def test_attention_is_sent_again(self):
        """The attention is lost once, then its 0x90 once: one delivery, one answer."""
        ca, lb = await self.connected()
        ca.attn_retry = 0.1
        lost = []
        self.a.t.drop = lambda f: desc_of(f) == 0x50 and not lost and not lost.append(f)
        self.bg(self.answer(lb, b"\xff\xff"))
        self.assertEqual(await ca.attention(0x0006, b"\0"), b"\xff\xff")
        self.a.t.drop, lost2 = None, []
        self.b.t.drop = lambda f: desc_of(f) == 0x90 and not lost2 and not lost2.append(f)
        self.assertEqual(await ca.attention(0x0006, b"\0"), b"\xff\xff")
        self.assertEqual((len(lost), len(lost2)), (1, 1))
        self.assertEqual((lb.attn_recv, ca.attn_send, lb.send_seq, len(ca.rx)), (2, 2, 4, 0))

    async def test_lost_data_is_sent_again(self):
        """A message's last packet is lost: it comes again with its EOM and its number."""
        ca, lb = await self.connected()
        ca.rto = 0.15
        lost = []
        self.a.t.drop = lambda f: desc_of(f) == 0x60 and not lost and not lost.append(f)
        body = bytes(700)
        await ca.write(body)
        self.assertEqual((len(lost), ca.acked, lb.recv_seq), (1, 701, 701))
        self.assertEqual(await lb.read_message(), body)
        again = [p for d, s, p in self.a.adsp_trace if d == "tx" and p[12] == 0x60]
        self.assertEqual(again[0][:6], again[1][:6])       # the same ConnID and FirstByteSeq
        # The ack is lost instead: the bytes come again, are not taken twice, and are acked again.
        lost2 = []
        self.a.t.drop = None
        self.b.t.drop = lambda f: desc_of(f) == 0x80 and not lost2 and not lost2.append(f)
        await ca.write(b"once")
        self.assertEqual(await lb.read_message(), b"once")
        self.assertEqual((lb.dups, len(lb.rx), ca.acked), (1, 0, 706))

    async def test_retransmit_advice(self):
        ca, lb = await self.connected()
        self.b.adsp_conns.remove(lb)                       # b goes deaf: nothing acked
        await ca.write(b"abc", wait=False)
        await self.b.send_ddp(self.a.net, self.a.node, ca.sock, lb.sock, etpeer.DDP_ADSP,
                              etpeer.adsp_pack(lb.conn_id, 0, 0, 2048, 0x88))
        await self.a.adsp_run(lambda: ca.resent, 1.0)
        self.assertEqual(ca.resent, 1)
        self.assertIsNone(ca.error)

    async def test_probe_and_close(self):
        ca, lb = await self.connected()
        self.assertTrue(await ca.probe(1.0))
        acks = [p for d, s, p in self.a.adsp_trace if d == "rx" and p[12] == 0x80]
        self.assertEqual(len(acks), 1)                     # one 0x80 and nothing more
        await ca.close()
        await self.b.adsp_run(lambda: lb.state == "closed", 1.0)
        self.assertTrue(lb.peer_closed)
        self.assertEqual((self.a.adsp_conns, self.b.adsp_conns), ([], []))
        with self.assertRaises(AdspClosed):
            await lb.read(1, 0.2)
        with self.assertRaises(AdspClosed):
            await ca.write(b"x")

    async def test_forward_reset(self):
        ca, lb = await self.connected()
        await ca.write(b"old")
        await ca.forward_reset(wait=True)
        self.assertEqual((len(lb.rx), lb.recv_seq), (0, 4))
        await ca.write(b"new")
        self.assertEqual(await lb.read_message(), b"new")

    async def test_packets_for_no_one(self):
        ca, lb = await self.connected()
        await self.b.send_ddp(self.a.net, self.a.node, ca.sock, lb.sock, etpeer.DDP_ADSP,
                              etpeer.adsp_pack(0x7777, 0, 0, 100, 0xC0))
        await self.b.send_ddp(self.a.net, self.a.node, ca.sock, lb.sock, etpeer.DDP_ADSP, b"\x01\x02\x03")
        await self.a.adsp_run(lambda: self.a.adsp_stray >= 2, 1.0)
        self.assertEqual(self.a.adsp_stray, 2)
        self.assertIsNone(ca.error)


class Strict(Base):
    """What the Mac's end will not take from an adapter."""

    async def raw(self, ca, lb, first, nxt, window, desc, data=b""):
        """A packet as if from b's end of the connection, made by hand."""
        await self.b.send_ddp(self.a.net, self.a.node, ca.sock, lb.sock, etpeer.DDP_ADSP,
                              etpeer.adsp_pack(lb.conn_id, first, nxt, window, desc, data))
        await self.a.adsp_run(lambda: ca.error, 0.5)

    async def deaf(self, **kw):
        ca, lb = await self.connected(**kw)
        self.b.adsp_conns.remove(lb)
        return ca, lb

    async def test_wrong_sequence(self):
        ca, lb = await self.deaf()
        await self.raw(ca, lb, 5, 0, 2048, 0x40, b"xy")
        with self.assertRaisesRegex(AdspError, "data at sequence 5; 0 is next"):
            await ca.read(2, 0.2)

    async def test_ack_past_what_was_sent(self):
        ca, lb = await self.deaf()
        await ca.write(b"abc", wait=False)
        await self.raw(ca, lb, 0, 9, 2048, 0x80)
        with self.assertRaisesRegex(AdspError, "names 9; only 4 was sent"):
            await ca.flush(0.2)

    async def test_ack_without_the_eom(self):
        """`abc` as a message is four sequence numbers: an ack naming 3 is wrong."""
        ca, lb = await self.deaf()
        await ca.write(b"abc", wait=False)
        await self.raw(ca, lb, 0, 3, 2048, 0x80)
        with self.assertRaisesRegex(AdspError, "without its EOM"):
            await ca.flush(0.2)

    async def test_ack_that_never_comes(self):
        ca, lb = await self.deaf()
        with self.assertRaisesRegex(AdspTimeout, "acknowledged to 0 of 4"):
            await ca.write(b"abc", timeout=0.2)

    async def test_ack_from_ahead_is_a_lost_packet_on_a_lossy_wire(self):
        """The other end's byte is lost, then it acknowledges ours: its acknowledgement is
        numbered from past the byte we never got. A strict Mac calls that wrong, as on a
        whole wire it is; a lenient one takes it, and takes the byte when it comes again."""
        ca, lb = await self.deaf()
        await ca.write(b"abc", wait=False)
        await self.raw(ca, lb, 1, 4, 2048, 0x80)
        with self.assertRaisesRegex(AdspError, "sent from sequence 1; we have taken 0"):
            await ca.flush(0.2)
        ca.vanish()
        ca, lb = await self.deaf(a=dict(strict=False))
        await ca.write(b"abc", wait=False)
        await self.raw(ca, lb, 1, 4, 2048, 0x80)
        await ca.flush(0.2)
        await self.raw(ca, lb, 0, 4, 2048, 0x40, b"z")
        self.assertEqual(await ca.read(1, 0.2), b"z")
        self.assertIsNone(ca.error)

    async def test_bytes_past_the_window(self):
        ca, lb = await self.deaf(a=dict(window=8))
        await self.raw(ca, lb, 0, 0, 2048, 0x40, bytes(9))
        with self.assertRaisesRegex(AdspError, "1 bytes past the window"):
            await ca.read(1, 0.2)

    async def test_attention_answer_with_eom(self):
        ca, lb = await self.connected()
        self.bg(TwoEnds.answer(lb, b"\0\0", eom=True))
        with self.assertRaisesRegex(AdspError, "answer carries EOM"):
            await ca.attention(0x0011, b"\0")

    async def test_attention_answer_that_never_comes(self):
        ca, lb = await self.connected()
        with self.assertRaisesRegex(AdspTimeout, "0 of its 2 answer bytes"):
            await ca.attention(0x0011, b"\0", timeout=0.2)

    async def test_attention_ack_with_a_window(self):
        ca, lb = await self.deaf()
        ca.attn_out = [0, b"", 1e18, 0]
        await self.raw(ca, lb, 0, 1, 2048, 0x90)
        self.assertRegex(str(ca.error), "attention packet .* window of 2048")

    async def test_short_open_answer(self):
        """An open answer cut to its 13-byte header is refused."""
        ca = Adsp(self.a, 0xF2)
        ca.open_retry = 5
        t = self.bg(ca.open(self.b.net, self.b.node, 0x81))
        await asyncio.sleep(0.05)
        await self.b.send_ddp(self.a.net, self.a.node, 0xF2, 0x81, etpeer.DDP_ADSP,
                              etpeer.adsp_pack(0x00A1, 0, 0, 2048, 0x83))
        with self.assertRaisesRegex(AdspError, "13 bytes, not 21"):
            await asyncio.wait_for(t, 2.0)

    async def test_open_answer_for_another_connid(self):
        ca = Adsp(self.a, 0xF2)
        ca.open_retry = 5
        t = self.bg(ca.open(self.b.net, self.b.node, 0x81))
        await asyncio.sleep(0.05)
        await self.b.send_ddp(self.a.net, self.a.node, 0xF2, 0x81, etpeer.DDP_ADSP,
                              etpeer.adsp_open_pack(0x00A1, 2048, 3, ca.conn_id ^ 0x5555))
        with self.assertRaisesRegex(AdspError, "an open answer for ConnID"):
            await asyncio.wait_for(t, 2.0)

    async def test_data_before_the_third_leg(self):
        la = Adsp(self.a, 0xF1).listen()
        t = self.bg(la.accept(2.0))
        send = lambda p: self.b.send_ddp(self.a.net, self.a.node, 0xF1, 0x81, etpeer.DDP_ADSP, p)  # noqa: E731
        await send(etpeer.adsp_open_pack(0x00A3, 2048, 1, 0))
        await send(etpeer.adsp_pack(0x00A3, 0, 0, 2048, 0x40, b"hi"))
        with self.assertRaisesRegex(AdspError, "before the third open leg"):
            await asyncio.wait_for(t, 2.0)


# ---------------------------------------------------------------- a small adapter

class Printer:
    """A canned StyleWriter: it keeps what it is sent and answers the questions."""

    def __init__(self, ident=b"CS\r", p=5):
        self.ident, self.p = ident, p
        self.got = bytearray()
        self.skip = self.ff = self.pages = 0
        self.want = None

    def feed(self, data):
        out = bytearray()
        self.got += data
        for b in data:
            if self.skip:
                self.skip -= 1
            elif self.want == "len1":
                self.len, self.want = b, "len2"
            elif self.want == "len2":
                self.skip, self.want = (self.len | b << 8) + 1, None
            elif b == 0xFF:
                self.ff += 1
            elif self.ff:
                self.ff = 0
                out += {0x31: bytes([0x08 if self.pages else 0]), 0x32: b"\x80" if self.ident == b"CS\r" else b"\0", 0x42: b"\x80",
                        0x48: b"\x81", 0x70: bytes([self.p])}.get(b, b"")
            elif b == 0x3F:
                out += self.ident
            elif b in (0x52, 0x63):
                self.skip = 8
            elif b == 0x47:
                self.want = "len1"
            elif b == 0x6D:
                self.skip = 1
            elif b == 0x0C:
                self.pages += 1
        return bytes(out)


class Adapter:
    """Apple's adapter in small, on b: one name, one job at a time, a canned printer."""

    def __init__(self, test, mac, printer=None, wait_for_close=False):
        self.t, self.mac, self.printer = test, mac, printer or Printer()
        self.name, self.busy, self.req, self.status = NAME.encode(), False, bytes(70), bytes(257)
        self.wait_for_close = wait_for_close       # call back only on 0012 or the close
        self.log, self.jobs, self.clean, self.pipe = [], 0, None, None
        self.messages, self.renamed = [], None
        test.bg(self.nbp())
        test.bg(self.listen())

    async def nbp(self):
        hit = []

        def asked():
            for i, d in enumerate(self.mac.inbox):
                if d[12] == etpeer.DDP_NBP and d[10] == etpeer.NIS and d[13] >> 4 == 2:
                    hit.append(self.mac.inbox.pop(i))
                    return True
            return False

        while True:
            if not await self.mac.adsp_run(asked, 1.0):
                continue
            d = hit.pop()
            t = d[15:]
            net, node, sock = struct.unpack(">HBB", t[:4])
            obj = t[6:6 + t[5]]
            typ = t[7 + t[5]:7 + t[5] + t[6 + t[5]]]
            if obj in (b"=", self.name) and typ == TYPE.encode():
                rep = bytes([0x31, d[14]]) + struct.pack(">HBBB", self.mac.net, self.mac.node, SOCK, 0) + \
                    etpeer.pstr(self.name) + etpeer.pstr(typ) + etpeer.pstr(b"*")
                await self.mac.send_ddp(net, node, sock, etpeer.NIS, etpeer.DDP_NBP, rep)

    async def listen(self):
        while True:
            c = Adsp(self.mac, SOCK, window=4096, need_third_leg=False).listen()
            await c.accept(3600)
            self.t.bg(self.control(c))

    async def control(self, c):
        mine, staged, called = False, None, False

        async def call():
            nonlocal called
            if mine and not called:
                called = True
                self.t.bg(self.job(c.peer[0], c.peer[1], self.req[3]))

        try:
            while True:
                a = await c.next_attention(1.0 if mine and not called and not self.wait_for_close else 3600)
                if a is None:
                    if c.state == "closed":
                        break
                    await call()                   # a second after the accept
                    continue
                code, body = a
                self.log.append(code)
                ans = b"\0\0"
                if code == etpeer.SWA_PRINT:
                    if len(body) < 5:
                        ans = b"\xff\xff"
                    elif self.busy:
                        ans = b"\0\1"
                    else:
                        self.busy, mine, self.req, self.status = True, True, body.ljust(70, b"\0"), bytes(257)
                elif code in (etpeer.SWA_JOB_COUNT, etpeer.SWA_POSITION):
                    ans = b"\0\1" if self.busy else b"\0\0"
                elif code == etpeer.SWA_JOB_USER:
                    ans = (self.req if self.busy else bytes(70)) + b"\0\0"
                elif code == etpeer.SWA_JOB_STATUS:
                    ans = (self.status if self.busy else bytes(257)) + b"\0\0"
                elif code == etpeer.SWA_SET_NAME:
                    staged = body[1:1 + body[0]]
                elif code == etpeer.SWA_END and staged:
                    self.name = self.renamed = staged
                await c.write(ans, eom=False)
                if code == etpeer.SWA_END:
                    await call()
        except AdspClosed:
            pass
        await call()

    async def job(self, net, node, sock):
        d = Adsp(self.mac, SOCK, window=4096)
        self.pipe, self.clean = d, False
        try:
            await d.open(net, node, sock)
            while d.state == "open":
                await self.mac.adsp_run(lambda: d.attn_in or d.rx or d.state != "open", 3600)
                while d.attn_in:
                    code, body = d.attn_in.pop(0)
                    self.log.append(0x1000 | code)
                    ans = b"\0\0"
                    if code == etpeer.SWA_SET_STATUS:
                        self.status = body
                    elif code == etpeer.SWA_DRAINED:
                        ans = b"\xff\xff"
                    elif code == etpeer.SWA_END:
                        ans, self.clean = b"\xff\xff", True
                    await d.write(ans, eom=False)
                while d.rx_eoms or d.rx:
                    msg = await d.read_message(0.01) if d.rx_eoms else await d.read_some(0.01)
                    self.messages.append(msg)
                    back = self.printer.feed(msg)
                    if back:
                        await d.write(back, eom=False)
        except AdspError as e:
            self.log.append(str(e))
        self.jobs += 1
        self.busy = False


def page(rows=4, blocks=3, colour=False):
    """A small page as rip --out-sw writes one: a rect, its blocks, 0C."""
    out = (b"c" if colour else b"R") + struct.pack("<HHHH", 0, 0, 7, rows - 1)
    for k in range(blocks):
        body = bytes([0x41, 0x3F + k, 0x80, 0x47, 0x0C, 0xFF])[:5]     # bytes that look like commands
        out += b"G" + struct.pack("<H", len(body)) + body + b"\0"
    return out + b"\x0c"


class PrintSw(Base):
    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.ad = Adapter(self, self.b)

    async def run_quiet(self, coro, timeout=20.0):
        with contextlib.redirect_stdout(io.StringIO()):
            return await asyncio.wait_for(coro, timeout)

    def check_job(self, out, job):
        """The printer got the reset, the questions, the setup, the page, the end, each
        command its own message."""
        got = bytes(self.ad.printer.got)
        self.assertEqual(out["model"], "2500")
        self.assertTrue(got.startswith(b"\0\xff\xff\xffI\xff\xff\xff1?\xff\xff\xffpD\xff\xff\xffB"
                                       b"\xff\xff\xffHm2nZA"), got[:40])
        cmds = [b for k, b in etpeer.sw_split(job)]
        msgs = self.ad.messages
        at = msgs.index(cmds[0])
        self.assertEqual(msgs[at - 2:at], [b"\xff\xff\xff1", b"\xff\xff\xff2"])   # asked before a band
        self.assertEqual([m for m in msgs if m in cmds], cmds)       # in order, one message each
        self.assertIn(b"El", msgs)
        self.assertEqual(sum(len(m) for m in msgs), len(got))
        self.assertEqual(out["sent"] + (5 if out["order"] == "lpstyl" else 0), len(got))

    async def test_apple_order(self):
        job = page() + page(colour=True)
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, job, user="Bob", settle=0.05))
        await asyncio.sleep(0.1)
        self.check_job(out, job)
        self.assertEqual((out["result"], out["pages"], out["end"], out["busy"]), (0, 2, b"\xff\xff", []))
        self.assertEqual(self.ad.req, etpeer.sw_request(etpeer.SW_DATA_SOCK, "Bob"))
        self.assertEqual(self.ad.status, etpeer.sw_status("Printing"))
        self.assertEqual(self.ad.log[:3], [0x000B, 0x0012, 0x100A])
        self.assertEqual(self.ad.log[-1], 0x1012)
        self.assertEqual(self.ad.log.count(0x1006), 2)               # before each band
        self.assertTrue(self.ad.clean)
        self.assertEqual((self.ad.jobs, self.ad.busy), (1, False))
        self.assertEqual(out["callback"], (self.b.net, self.b.node, SOCK))
        # The page letters: B and L for the black page, H and L for the colour one.
        msgs = self.ad.messages
        self.assertEqual(msgs[msgs.index(page()[:9]) - 3], b"BL")
        self.assertIn(b"HL", msgs)
        att = [p[12] for d, s, p in self.a.adsp_trace if d == "tx" and p[12] & 0x90 == 0x10]
        self.assertEqual(set(att), {0x50})
        closes = [p for d, s, p in self.a.adsp_trace if d == "tx" and p[12] == 0x85]
        self.assertEqual(len(closes), 2)                             # control, then data
        self.assertEqual(self.a.adsp_conns, [])

    async def test_lpstyl_order(self):
        """No 0012 and no close on the control connection, attentions as 0x10, and at
        the end Forward Reset, 00, FF FF FF I, 0012, close."""
        job = page()
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, job, order="lpstyl", settle=0.05))
        await asyncio.sleep(0.1)
        self.check_job(out, job)
        self.assertEqual(self.ad.log[:2], [0x000B, 0x100A])
        self.assertNotIn(0x0012, self.ad.log)
        self.assertEqual(self.ad.messages[-3:], [b"El", b"\0", b"\xff\xff\xffI"])
        descs = [p[12] for d, s, p in self.a.adsp_trace if d == "tx"]
        self.assertEqual({x for x in descs if x & 0x90 == 0x10}, {0x10})
        self.assertEqual(descs.count(0x86), 1)
        self.assertEqual(descs.count(0x85), 1)                       # the data connection's only
        self.assertTrue(self.ad.clean)
        self.assertIsNone(self.ad.pipe.error)

    async def test_adapter_that_waits_for_the_close(self):
        self.ad.wait_for_close = True
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05))
        self.assertEqual(out["end"], b"\xff\xff")

    async def test_busy_then_prints(self):
        """Busy: the queue is asked, the session ended, and the request made again."""
        self.ad.busy, self.ad.req, self.ad.status = True, etpeer.sw_request(0xE0, "Ann"), etpeer.sw_status("Printing")

        async def free():
            await asyncio.sleep(0.5)
            self.ad.busy = False

        self.bg(free())
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05, busy_wait=0.3))
        self.assertGreaterEqual(len(out["busy"]), 1)
        q = out["busy"][0]
        self.assertEqual((q["result"], q["jobs"], len(q["job"]), len(q["status"])), (1, 1, 72, 259))
        self.assertEqual(q["job"][:8], bytes.fromhex("000000E003416E6E"))
        self.assertEqual(q["status"][:11], bytes.fromhex("0647085072696E74696E67"))
        self.assertEqual(self.ad.log[:5], [0x000B, 0x0011, 0x000E, 0x0010, 0x0012])
        self.assertEqual((out["result"], out["pages"]), (0, 1))

    async def test_refused(self):
        self.ad.control = self.refuse
        with self.assertRaisesRegex(SwError, "refused"):
            await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05))

    async def refuse(self, c):
        await c.next_attention(5.0)
        await c.write(b"\xff\xff", eom=False)

    async def test_no_such_printer(self):
        with self.assertRaisesRegex(SwError, "answered the lookup"):
            await self.run_quiet(self.a.print_sw("Nobody", TYPE, page(), settle=0.05), 30)

    async def test_no_third_leg_and_0x10(self):
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05, third_leg=False,
                                                   attn_desc=0x10))
        self.assertEqual(out["end"], b"\xff\xff")
        descs = [p[12] for d, s, p in self.a.adsp_trace if d == "tx"]
        self.assertNotIn(0x82, descs)
        self.assertNotIn(0x50, descs)

    async def test_vanish(self):
        """Gone at each named point: nothing more is sent, not even a close."""
        for point, last in (("accepted", 0x000B), ("callback", 0x0012), ("pipe", 0x100A), ("page", None)):
            with self.subTest(point=point):
                await self.asyncTearDown()                 # a wire, two Macs and an adapter anew
                await self.asyncSetUp()
                out = await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05, vanish=point))
                n = len(self.a.adsp_trace)
                await asyncio.sleep(0.2)
                self.assertEqual((out["gone"], out["end"], self.a.adsp_conns), (point, None, []))
                self.assertEqual(len([x for x in self.a.adsp_trace[n:] if x[0] == "tx"]), 0)
                closes = [p for d, s, p in self.a.adsp_trace if d == "tx" and p[12] == 0x85]
                self.assertEqual(len(closes), 1 if point in ("callback", "pipe", "page") else 0)
                if last:
                    self.assertEqual([c for c in self.ad.log if isinstance(c, int)][-1], last)
                else:
                    blocks = [m for m in self.ad.messages if m[:1] == b"G"]
                    self.assertEqual(len(blocks), 1)
                    self.assertNotIn(b"\x0c", self.ad.messages)

    async def test_without_setup(self):
        """setup=False: the reset and `?`, then the stream as it is."""
        job = b"m2bAL" + page() + b"El"
        out = await self.run_quiet(self.a.print_sw(NAME, TYPE, job, settle=0.05, setup=False))
        got = bytes(self.ad.printer.got)
        self.assertTrue(got.endswith(page() + b"El"))
        self.assertNotIn(b"\xff\xff\xffH", self.ad.messages)
        self.assertIn(b"m2", self.ad.messages)
        self.assertEqual(out["pages"], 1)

    async def test_original_and_1200(self):
        for ident, model, setup, tail in ((b"IJ10\r", "original", b"nuA", b"El"),
                                          (b"SW3\r", "1200", b"unDA", b"EYl")):
            with self.subTest(model=model):
                self.ad.printer = Printer(ident)
                self.ad.messages.clear()
                out = await self.run_quiet(self.a.print_sw(NAME, TYPE, page(), settle=0.05))
                self.assertEqual(out["model"], model)
                self.assertIn(setup, self.ad.messages)
                self.assertEqual(self.ad.messages[-1], tail)
                self.assertNotIn(b"\xff\xff\xffp", self.ad.messages)
                self.assertEqual(b"\xff\xff\xffH" in self.ad.messages, model != "original")

    async def test_queue(self):
        q = await self.a.sw_queue(NAME, TYPE)
        self.assertEqual((q["jobs"], q["position"], q["job"], q["status_raw"], q["end"]),
                         (0, 0, bytes(72), bytes(259), 0))
        self.ad.busy, self.ad.req, self.ad.status = True, etpeer.sw_request(0xE0, "Ann"), etpeer.sw_status("Printing")
        q = await self.a.sw_queue(NAME, TYPE, request=self.ad.req)
        self.assertEqual((q["jobs"], q["position"], q["user"], q["data_sock"], q["status_id"], q["status"]),
                         (1, 1, "Ann", 0xE0, 0x0647, "Printing"))
        self.assertEqual(self.ad.log[-5:], [0x0011, 0x000D, 0x000E, 0x0010, 0x0012])
        self.assertEqual(self.a.adsp_conns, [])

    async def test_rename(self):
        """Brief D.2: 0011, 0009 with the name, 0012, Close Advice, on one connection."""
        new = "Color StyleWriter 2200"
        r = await self.a.sw_rename(NAME, TYPE, new)
        self.assertEqual(r, dict(jobs=0, renamed=0, end=0))
        self.assertEqual(self.ad.renamed, new.encode())
        tx = [p for d, s, p in self.a.adsp_trace if d == "tx"]
        cid = tx[0][:2]
        self.assertEqual([p[12] for p in tx if p[12] & 0x10 or p[12] in (0x81, 0x82, 0x85)],
                         [0x81, 0x82, 0x50, 0x50, 0x50, 0x85])
        att = [p for p in tx if p[12] == 0x50]
        self.assertEqual(att[0][:6] + att[0][10:], cid + bytes(4) + bytes.fromhex("0000 50 0011 00"))
        self.assertEqual(att[1][:6] + att[1][10:], cid + bytes.fromhex("00000001 0000 50 0009 16") + new.encode())
        self.assertEqual(att[2][:6] + att[2][10:], cid + bytes.fromhex("00000002 0000 50 0012 00"))
        self.assertIsNotNone(await self.a.nbp_find(new, TYPE, tries=1, wait=0.5))
        self.assertIsNone(await self.a.nbp_find(NAME, TYPE, tries=1, wait=0.2))


if __name__ == "__main__":
    unittest.main()
