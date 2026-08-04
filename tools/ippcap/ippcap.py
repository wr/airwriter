#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A stub IPP printer that exists only to capture what iOS sends.

It answers Get-Printer-Attributes and Validate-Job with the minimum iOS
needs, then writes any Print-Job body straight to disk. It is not a printer
and makes no attempt to be correct beyond getting the job out of the phone.

Run it, then in another terminal advertise it over Bonjour:

  dns-sd -R AirWriterCap _ipp._tcp,_universal local 6631 \\
      txtvers=1 rp=ipp/print ty="AirWriter capture" \\
      pdl=image/urf URF=V1.4,CP1,RS72-144,W8,DM1,IS1,MT1,FN3 \\
      Color=F Duplex=F

Leave both running, then print from the iPhone to "AirWriterCap".
"""
import http.server, struct, sys, pathlib, datetime

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "fixtures")
PORT = 6631

# IPP tags
TAG_OPERATION, TAG_JOB, TAG_PRINTER, TAG_END = 0x01, 0x02, 0x04, 0x03
INTEGER, BOOLEAN, ENUM = 0x21, 0x22, 0x23
TEXT, NAME, KEYWORD, URI, CHARSET, LANGUAGE, MIMETYPE = 0x41, 0x42, 0x44, 0x45, 0x47, 0x48, 0x49
RESOLUTION = 0x32


def attr(tag, name, value):
    out = bytes([tag]) + struct.pack(">H", len(name)) + name.encode()
    return out + struct.pack(">H", len(value)) + value


def txt(tag, name, s):
    return attr(tag, name, s.encode())


def num(name, v):
    return attr(INTEGER, name, struct.pack(">i", v))


def res(name, x, y):
    return attr(RESOLUTION, name, struct.pack(">iiB", x, y, 3))  # 3 = dpi


def printer_attrs(req_id, host):
    b = bytearray(struct.pack(">HHI", 0x0200, 0x0000, req_id))  # version, ok, id
    b.append(TAG_OPERATION)
    b += txt(CHARSET, "attributes-charset", "utf-8")
    b += txt(LANGUAGE, "attributes-natural-language", "en-us")
    b.append(TAG_PRINTER)
    b += txt(URI, "printer-uri-supported", f"ipp://{host}/ipp/print")
    b += txt(KEYWORD, "uri-authentication-supported", "none")
    b += txt(KEYWORD, "uri-security-supported", "none")
    b += txt(NAME, "printer-name", "AirWriter capture")
    b += attr(ENUM, "printer-state", struct.pack(">i", 3))       # idle
    b += txt(KEYWORD, "printer-state-reasons", "none")
    b += txt(KEYWORD, "ipp-versions-supported", "1.1")
    # A 1setOf is encoded as repeated values where every value after the
    # first carries a zero-length name.
    for i, op in enumerate((0x0002, 0x0004, 0x0008, 0x0009, 0x000A, 0x000B)):
        b += attr(ENUM, "operations-supported" if i == 0 else "",
                  struct.pack(">i", op))
    b += txt(CHARSET, "charset-configured", "utf-8")
    b += txt(CHARSET, "charset-supported", "utf-8")
    b += txt(LANGUAGE, "natural-language-configured", "en-us")
    b += txt(LANGUAGE, "generated-natural-language-supported", "en-us")
    b += txt(MIMETYPE, "document-format-default", "image/urf")
    b += txt(MIMETYPE, "document-format-supported", "image/urf")
    b += attr(BOOLEAN, "printer-is-accepting-jobs", b"\x01")
    b += num("queued-job-count", 0)
    b += txt(KEYWORD, "pdl-override-supported", "attempted")
    b += num("printer-up-time", 1)
    b += txt(KEYWORD, "compression-supported", "none")
    b += txt(KEYWORD, "media-default", "na_letter_8.5x11in")
    b += txt(KEYWORD, "media-supported", "na_letter_8.5x11in")
    b += res("printer-resolution-default", 144, 72)
    b += res("printer-resolution-supported", 144, 72)
    b += txt(KEYWORD, "print-color-mode-default", "monochrome")
    b += txt(KEYWORD, "print-color-mode-supported", "monochrome")
    for i, v in enumerate(("V1.4", "CP1", "RS72-144", "W8",
                           "DM1", "IS1", "MT1", "FN3")):
        b += txt(KEYWORD, "urf-supported" if i == 0 else "", v)
    b.append(TAG_END)
    return bytes(b)


def find_document(body):
    """Return the offset of the document data, walking the attribute groups.

    RFC 8010: after the 8-byte header comes a sequence of delimiter tags
    (0x01-0x05) and attributes. An attribute is a one-byte value tag, a
    2-byte big-endian name length, the name, a 2-byte big-endian value
    length, and the value. The end-of-attributes tag (0x03) terminates the
    sequence and the document follows immediately. Searching for a bare
    0x03 instead would match any such byte inside a name or a value.
    """
    pos = 8
    n = len(body)
    while pos < n:
        tag = body[pos]
        if tag == TAG_END:
            return pos + 1
        if tag <= 0x05:            # a delimiter starting a new group
            pos += 1
            continue
        pos += 1                   # value tag
        if pos + 2 > n:
            raise ValueError("truncated attribute name length")
        namelen = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2 + namelen
        if pos + 2 > n:
            raise ValueError("truncated attribute value length")
        vallen = struct.unpack(">H", body[pos:pos + 2])[0]
        pos += 2 + vallen
    raise ValueError("no end-of-attributes tag found")


def job_attrs(req_id, job_id, host):
    b = bytearray(struct.pack(">HHI", 0x0200, 0x0000, req_id))
    b.append(TAG_OPERATION)
    b += txt(CHARSET, "attributes-charset", "utf-8")
    b += txt(LANGUAGE, "attributes-natural-language", "en-us")
    b.append(TAG_JOB)
    b += num("job-id", job_id)
    b += txt(URI, "job-uri", f"ipp://{host}/ipp/print/{job_id}")
    b += attr(ENUM, "job-state", struct.pack(">i", 9))           # completed
    b += txt(KEYWORD, "job-state-reasons", "job-completed-successfully")
    b.append(TAG_END)
    return bytes(b)


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    job_id = 1

    def log_message(self, fmt, *a):
        sys.stderr.write("  " + fmt % a + "\n")

    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0))
        if self.headers.get("Transfer-Encoding", "").lower() == "chunked":
            body = bytearray()
            while True:
                size = int(self.rfile.readline().strip() or b"0", 16)
                if size == 0:
                    self.rfile.readline()
                    break
                body += self.rfile.read(size)
                self.rfile.readline()
            body = bytes(body)
        else:
            body = self.rfile.read(n)

        op = struct.unpack(">H", body[2:4])[0]
        rid = struct.unpack(">I", body[4:8])[0]
        host = self.headers.get("Host", "localhost")
        print(f"op 0x{op:04X}  {len(body)} bytes")

        if op == 0x000B:                                  # Get-Printer-Attributes
            resp = printer_attrs(rid, host)
        elif op == 0x0004:                                # Validate-Job
            resp = job_attrs(rid, Handler.job_id, host)
        elif op in (0x0002, 0x0009, 0x000A):              # Print-Job / Get-Job(s)
            if op == 0x0002:
                # The document follows the end-of-attributes tag. Walk the
                # attribute structure to find it rather than searching for
                # a bare 0x03, which can also occur inside a name or value.
                try:
                    i = find_document(body)
                except ValueError as e:
                    print(f"  malformed Print-Job request: {e}")
                    resp = struct.pack(">HHI", 0x0200, 0x0400, rid) + bytes([TAG_END])
                    self.send_response(200)
                    self.send_header("Content-Type", "application/ipp")
                    self.send_header("Content-Length", str(len(resp)))
                    self.end_headers()
                    self.wfile.write(resp)
                    return
                doc = body[i:]
                OUT.mkdir(parents=True, exist_ok=True)
                stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
                ext = "urf" if doc[:7] == b"UNIRAST" else \
                      "pwg" if doc[:4] == b"RaS2" else "bin"
                path = OUT / f"ios-{stamp}.{ext}"
                path.write_bytes(doc)
                print(f"  captured {len(doc)} bytes -> {path}")
                if ext == "urf" and len(doc) >= 44:
                    bpp, cs = doc[12], doc[13]
                    w, h, r = struct.unpack(">III", doc[24:36])
                    print(f"  URF: {w}x{h} @ {r}dpi, {bpp}bpp, colorspace {cs}")
                Handler.job_id += 1
            resp = job_attrs(rid, Handler.job_id - 1, host)
        else:
            resp = struct.pack(">HHI", 0x0200, 0x0501, rid) + bytes([TAG_END])

        self.send_response(200)
        self.send_header("Content-Type", "application/ipp")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)


if __name__ == "__main__":
    print(f"ippcap on :{PORT}, writing to {OUT}/")
    http.server.ThreadingHTTPServer(("", PORT), Handler).serve_forever()
