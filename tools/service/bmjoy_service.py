#!/usr/bin/env python3
# BMjoy の受け取り口（Mac mini で動かす）。tailscale funnel で外に出し、配った BMjoy が使う。
#   GET  /update/latest.json     新しい版の情報（tools/service/publish.sh が置く）
#   GET  /update/<ファイル>       新しい版の zip
#   POST /log?client=&name=&version=   ログ（本文がファイルの中身）。logs/<client>/<name> に置く
# funnel の置き方によって頭に /bmjoy が付いてくるので、付いていても付いていなくても受ける。
# 127.0.0.1 だけで待ち受ける（外からは funnel を通してだけ届く）
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.expanduser(os.environ.get("BMJOY_SERVICE_ROOT", "~/bmjoy-service"))
UPDATE = os.path.join(ROOT, "update")
LOGS = os.path.join(ROOT, "logs")
PORT = int(os.environ.get("BMJOY_SERVICE_PORT", "4570"))
KEY = "bmjoy-7f3c1a9e"  # BMjoy の app/service.cpp と同じ（配る版に入っているので、知らない送り主を断る程度のもの）
MAX_UPLOAD = 32 << 20  # 1 個の大きさの上限
MAX_TOTAL = 4 << 30  # ログ全体の上限（超えたら受けない）

CLIENT = re.compile(r"^[0-9a-f]{16}$")
NAME = re.compile(r"^[0-9A-Za-z_.-]{1,96}$")


def logs_size():
    total = 0
    for d, _, fs in os.walk(LOGS):
        for f in fs:
            try:
                total += os.path.getsize(os.path.join(d, f))
            except OSError:
                pass
    return total


class Handler(BaseHTTPRequestHandler):
    server_version = "bmjoy-service"

    def path_parts(self):
        u = urlparse(self.path)
        p = u.path
        if p.startswith("/bmjoy/"):
            p = p[len("/bmjoy"):]
        return p, parse_qs(u.query)

    def reply(self, code, body=b"", ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_GET(self):
        p, _ = self.path_parts()
        if not p.startswith("/update/"):
            return self.reply(404, b"not found")
        name = p[len("/update/"):]
        if not NAME.match(name):
            return self.reply(404, b"not found")
        f = os.path.join(UPDATE, name)
        if not os.path.isfile(f):
            return self.reply(404, b"not found")
        ctype = "application/json" if name.endswith(".json") else "application/zip"
        size = os.path.getsize(f)
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(size))
        self.send_header("Cache-Control", "no-cache" if name.endswith(".json") else "max-age=86400")
        self.end_headers()
        with open(f, "rb") as fh:
            while True:
                b = fh.read(1 << 16)
                if not b:
                    break
                self.wfile.write(b)

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        p, q = self.path_parts()
        if p != "/log":
            return self.reply(404, b"not found")
        if self.headers.get("X-BMjoy-Key") != KEY:
            return self.reply(403, b"forbidden")
        client = (q.get("client") or [""])[0]
        name = (q.get("name") or [""])[0]
        version = (q.get("version") or [""])[0]
        if not CLIENT.match(client) or not NAME.match(name) or not re.match(r"^[0-9A-Za-z.]{0,32}$", version):
            return self.reply(400, b"bad request")
        try:
            n = int(self.headers.get("Content-Length", "-1"))
        except ValueError:
            n = -1
        if n < 0 or n > MAX_UPLOAD:
            return self.reply(413, b"too large")
        if logs_size() + n > MAX_TOTAL:
            return self.reply(507, b"full")
        data = self.rfile.read(n)
        d = os.path.join(LOGS, client)
        os.makedirs(d, exist_ok=True)
        out = os.path.join(d, name)
        i = 1
        while os.path.exists(out):
            stem, ext = os.path.splitext(name)
            out = os.path.join(d, f"{stem}.{i}{ext}")
            i += 1
        with open(out, "wb") as fh:
            fh.write(data)
        with open(os.path.join(d, "versions.txt"), "a", encoding="utf-8") as fh:
            fh.write(f"{os.path.basename(out)}\t{version}\n")
        self.reply(200, b"ok")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), fmt % args))


def main():
    os.makedirs(UPDATE, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print(f"bmjoy-service: 127.0.0.1:{PORT} root={ROOT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
