# SPDX-License-Identifier: GPL-3.0-or-later
"""An IPP/1.1 Print-Job, enough to send AirWriter one URF page."""
import http.client, struct, urllib.parse


def _attr(tag, name, value):
    n, v = name.encode(), value.encode()
    return bytes([tag]) + struct.pack(">H", len(n)) + n + struct.pack(">H", len(v)) + v


def print_job(url, document, fmt="image/urf", name="labels"):
    """Sends `document` to the printer at ipp://host:port/path; returns IPP's status (0 is ok)."""
    u = urllib.parse.urlparse(url)
    req = (struct.pack(">BBHI", 1, 1, 0x0002, 1) + b"\x01"
           + _attr(0x47, "attributes-charset", "utf-8")
           + _attr(0x48, "attributes-natural-language", "en")
           + _attr(0x45, "printer-uri", url)
           + _attr(0x42, "requesting-user-name", "bench")
           + _attr(0x42, "job-name", name)
           + _attr(0x49, "document-format", fmt)
           + b"\x03" + document)
    c = http.client.HTTPConnection(u.hostname, u.port or 631, timeout=120)
    try:
        c.request("POST", u.path or "/", body=req, headers={"Content-Type": "application/ipp"})
        r = c.getresponse()
        body = r.read()
    finally:
        c.close()
    if r.status != 200 or len(body) < 4:
        raise OSError("the printer answered HTTP %d" % r.status)
    return struct.unpack(">H", body[2:4])[0]
