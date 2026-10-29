#!/usr/bin/env python3
# RD の受け取り口（Mac mini で動かす）。tailscale funnel で外に出し、配った RD が使う。
#   GET  /update/latest.json     新しい版の情報（tools/service/publish.sh が置く）
#   GET  /update/<ファイル>       新しい版の zip
#   POST /log?client=&name=&version=   ログ（本文がファイルの中身）。logs/<client>/<name> に置く
# 届いたログのうち、RD のログ（1 行目が「[時刻] RD 版（…）を起動」）と Windows のダンプ（MDMP で始まる）でないものは捨てる。
# 古いもの（KEEP_DAYS 日より前）と、1 台・全体の上限を超えた分は、古いほうから自動で消す
# funnel の置き方によって頭に /rd が付いてくるので、付いていても付いていなくても受ける。
# 127.0.0.1 だけで待ち受ける（外からは funnel を通してだけ届く）
import os
import re
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.expanduser(os.environ.get("RD_SERVICE_ROOT", "~/rd-service"))
UPDATE = os.path.join(ROOT, "update")
LOGS = os.path.join(ROOT, "logs")
PORT = int(os.environ.get("RD_SERVICE_PORT", "4570"))
KEY = "rd-7f3c1a9e"  # RD の app/service.cpp と同じ（配る版に入っているので、知らない送り主を断る程度のもの）
MAX_UPLOAD = 32 << 20  # 1 個の大きさの上限
MAX_TOTAL = 4 << 30  # ログ全体の上限（超えたら古いほうから消す）
MAX_CLIENT = 256 << 20  # 1 台分の上限（超えたら、その台の古いほうから消す）
KEEP_DAYS = 60  # これより古いログは消す
MAX_PER_HOUR = 120  # 1 台が 1 時間に送れる数
LOG_HEAD = re.compile(rb"^\[\d\d:\d\d:\d\d\.\d\d\d\] RD [0-9A-Za-z.]+")

CLIENT = re.compile(r"^[0-9a-f]{16}$")
NAME = re.compile(r"^[0-9A-Za-z_.-]{1,96}$")


import threading
import time

_lock = threading.Lock()
_recent = {}  # 台 → 送ってきた時刻


def looks_like_log(name, data):
    """RD のログかダンプか（中身の頭で見る）"""
    if name.endswith(".txt"):
        return LOG_HEAD.match(data[:64]) is not None
    if name.endswith(".dmp"):
        return data[:4] == b"MDMP"
    return False


def files_of(d):
    out = []
    for root, _, fs in os.walk(d):
        for f in fs:
            if f == "versions.txt":
                continue
            p = os.path.join(root, f)
            try:
                st = os.stat(p)
                out.append((st.st_mtime, st.st_size, p))
            except OSError:
                pass
    return sorted(out)


def trim():
    """古いものと、上限を超えた分を古いほうから消す"""
    limit = time.time() - KEEP_DAYS * 86400
    for d in os.listdir(LOGS):
        cd = os.path.join(LOGS, d)
        if not os.path.isdir(cd):
            continue
        fs = files_of(cd)
        size = sum(s for _, s, _ in fs)
        for m, s, p in fs:
            if m < limit or size > MAX_CLIENT:
                try:
                    os.remove(p)
                    size -= s
                except OSError:
                    pass
    fs = files_of(LOGS)
    size = sum(s for _, s, _ in fs)
    for _, s, p in fs:
        if size <= MAX_TOTAL:
            break
        try:
            os.remove(p)
            size -= s
        except OSError:
            pass


class Handler(BaseHTTPRequestHandler):
    server_version = "rd-service"
    timeout = 30  # 何も送ってこないつなぎっぱなしは切る

    def path_parts(self):
        u = urlparse(self.path)
        p = u.path
        if p.startswith("/rd/"):
            p = p[len("/rd"):]
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
        if self.headers.get("X-RD-Key") != KEY:
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
        now = time.time()
        with _lock:
            t = [x for x in _recent.get(client, []) if now - x < 3600]
            if len(t) >= MAX_PER_HOUR:
                return self.reply(429, b"too many")
            t.append(now)
            _recent[client] = t
        data = self.rfile.read(n)
        if len(data) != n or not looks_like_log(name, data):
            return self.reply(400, b"not a log")  # 中身が RD のログでなければ置かない
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
        with _lock:
            trim()
        self.reply(200, b"ok")

    def log_message(self, fmt, *args):
        sys.stderr.write("%s %s\n" % (self.log_date_time_string(), fmt % args))


def main():
    os.makedirs(UPDATE, exist_ok=True)
    os.makedirs(LOGS, exist_ok=True)
    trim()
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    srv.daemon_threads = True
    print(f"rd-service: 127.0.0.1:{PORT} root={ROOT}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
