#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The settings page on this computer, in any state, with no board.

Serves the page as mkheader.py builds it (rebuilt on every load, so an edit
shows on reload) and answers its API from states.json: a base that is board
1's own answers, and one patch per state (no printer, setup mode, a failing
relay, an update error...). Nothing is sent to a board.

    python3 tools/webui/preview.py            # then open http://localhost:8079/_states
    python3 tools/webui/preview.py --port 9000

/?state=NAME opens the page in that state, at / so that the page's own
reloads keep it. In states.json a response can be {"_status": 404, "_body":
"..."} for a refusal, {"_drop": true} for no answer at all, {"_delay": s} to
answer late, and "_first": n to apply only to the first n asks. A state's
"_down_after": s stops answering s seconds after the page loads, as a board
unplugged mid-poll. A POST's "_then" is the state the board comes back in
after "_down" seconds of restarting.
"""
import argparse, copy, http.server, json, pathlib, sys, time, urllib.parse

HERE = pathlib.Path(__file__).parent
sys.path.insert(0, str(HERE))
import mkheader  # noqa: E402

STATES = HERE / "states.json"
clock = {"loaded": 0.0, "down_until": 0.0, "override": None, "seen": {}}


def merge(base, patch):
    out = copy.deepcopy(base)
    for k, v in patch.items():
        out[k] = merge(out[k], v) if isinstance(v, dict) and isinstance(out.get(k), dict) \
            and not any(x.startswith("_") for x in v) else copy.deepcopy(v)
    return out


def state(name):
    doc = json.loads(STATES.read_text())
    s = doc["states"].get(name) or {}
    return merge(doc["base"], s.get("patch", {})), s


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        pass

    def current(self):
        if clock["override"]:
            return clock["override"]
        c = self.headers.get("Cookie") or ""
        for part in c.split(";"):
            k, _, v = part.strip().partition("=")
            if k == "state":
                return urllib.parse.unquote(v)
        return "normal"

    def send(self, code, body, ctype="application/json", cookie=None):
        data = body if isinstance(body, bytes) else body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        if cookie is not None:
            self.send_header("Set-Cookie", "state=%s; Path=/" % urllib.parse.quote(cookie))
        self.end_headers()
        self.wfile.write(data)

    def drop(self):
        self.close_connection = True       # no status line: the browser sees no answer

    def answer(self, method):
        url = urllib.parse.urlparse(self.path)
        if method == "GET" and url.path == "/_states":
            doc = json.loads(STATES.read_text())
            rows = "".join('<li><a href="/?state=%s">%s</a> %s</li>' % (n, n, s.get("about", ""))
                           for n, s in doc["states"].items())
            return self.send(200, "<!doctype html><meta charset=utf-8><title>States</title>"
                             "<ul style='font:14px/1.6 system-ui'>%s</ul>" % rows, "text/html; charset=utf-8")
        if method == "GET" and url.path == "/":
            q = urllib.parse.parse_qs(url.query)
            name = q.get("state", [None])[0]
            if name is not None:            # a fresh start in that state, at /
                clock["override"] = None    # so the page's own reloads keep it
                clock["down_until"] = 0.0
                self.send_response(302)
                self.send_header("Location", "/")
                self.send_header("Set-Cookie", "state=%s; Path=/" % urllib.parse.quote(name))
                self.send_header("Content-Length", "0")
                return self.end_headers()
            clock["loaded"] = time.time()
            clock["seen"] = {}
            return self.send(200, mkheader.build_page(), "text/html; charset=utf-8")
        if not url.path.startswith(("/api/", "/update")):
            return self.send(404, "no such page", "text/plain")

        now = time.time()
        if now < clock["down_until"]:
            return self.drop()
        name = self.current()
        resp, meta = state(name)
        if meta.get("_down_after") is not None and now - clock["loaded"] > meta["_down_after"]:
            return self.drop()

        if method == "POST":
            n = int(self.headers.get("Content-Length") or 0)
            body = self.rfile.read(n).decode("utf-8", "replace")
            print("POST %s %s" % (url.path, body))
        key = "%s %s" % (method, url.path)
        r = resp.get(key)
        seen = clock["seen"][key] = clock["seen"].get(key, 0) + 1
        if isinstance(r, dict) and r.get("_first") and seen > r["_first"]:
            r = json.loads(STATES.read_text())["base"].get(key)
        if r is None and method == "POST":
            r = {"ok": True}
        if r is None:
            return self.send(404, "no such page", "text/plain")
        if isinstance(r, dict) and any(k.startswith("_") for k in r):
            if r.get("_delay"):
                time.sleep(r["_delay"])
            if r.get("_drop"):
                return self.drop()
            if "_then" in r:
                clock["down_until"] = time.time() + r.get("_down", 10)
                clock["override"] = r["_then"]     # back from the restart in this state
            body = r.get("_body", {"ok": True})
            code = r.get("_status", 200)
            if isinstance(body, str):
                return self.send(code, body, "application/json" if r.get("_json") else "text/plain")
            return self.send(code, json.dumps(body, ensure_ascii=False))
        return self.send(200, json.dumps(r, ensure_ascii=False))

    def do_GET(self):
        self.answer("GET")

    def do_POST(self):
        self.answer("POST")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=8079)
    ap.add_argument("--host", default="127.0.0.1",
                    help="0.0.0.0 to try the page on a phone on this network")
    a = ap.parse_args()
    srv = http.server.ThreadingHTTPServer((a.host, a.port), Handler)
    print("http://%s:%d/_states" % ("localhost" if a.host == "127.0.0.1" else a.host, a.port))
    srv.serve_forever()


if __name__ == "__main__":
    main()
