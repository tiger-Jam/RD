#!/usr/bin/env python3
# Cloudflare の受け口（tools/service/cloudflare）に届いたログを、Mac mini へ取ってくる。取ったものは受け口から消す。
# 置き場は ~/rd-service/logs/<PC の印>/。60 日より古いもの・1 台 256MB・全体 4GB を超えた分は古いほうから消す。
# 受け口の住所と管理の合言葉は ~/rd-service/cloudflare.json（{"url": ..., "admin_secret": ...}）。LaunchAgent が 10 分ごとに動かす
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

ROOT = os.path.expanduser(os.environ.get("RD_SERVICE_ROOT", "~/rd-service"))
LOGS = os.path.join(ROOT, "logs")
MAX_CLIENT = 256 << 20
MAX_TOTAL = 4 << 30
KEEP_DAYS = 60
KEY = re.compile(r"^logs/([0-9a-f]{16})/([0-9A-Za-z_.-]{1,100})$")


def call(cfg, method, path, query=None):
    url = cfg["url"].rstrip("/") + path + ("?" + urllib.parse.urlencode(query) if query else "")
    req = urllib.request.Request(url, method=method, headers={"X-RD-Admin": cfg["admin_secret"], "User-Agent": "rd-pull"})
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def pull(cfg):
    got = 0
    cursor = None
    while True:
        page = json.loads(call(cfg, "GET", "/admin/logs", {"cursor": cursor} if cursor else None))
        for o in page["objects"]:
            m = KEY.match(o["key"])
            if not m:
                continue
            client, name = m.groups()
            data = call(cfg, "GET", "/admin/object", {"key": o["key"]})
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
                fh.write(f"{os.path.basename(out)}\t{o.get('version', '')}\n")
            call(cfg, "DELETE", "/admin/object", {"key": o["key"]})
            got += 1
        cursor = page.get("cursor")
        if not cursor:
            return got


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
    for c in os.listdir(LOGS):
        cd = os.path.join(LOGS, c)
        if not os.path.isdir(cd):
            continue
        fs = files_of(cd)
        size = sum(s for _, s, _ in fs)
        for m, s, p in fs:
            if m < limit or size > MAX_CLIENT:
                os.remove(p)
                size -= s
    fs = files_of(LOGS)
    size = sum(s for _, s, _ in fs)
    for _, s, p in fs:
        if size <= MAX_TOTAL:
            break
        os.remove(p)
        size -= s


def main():
    os.makedirs(LOGS, exist_ok=True)
    with open(os.path.join(ROOT, "cloudflare.json"), encoding="utf-8") as fh:
        cfg = json.load(fh)
    try:
        n = pull(cfg)
        if n:
            print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} ログを {n} 個取った", flush=True)
    except Exception as e:  # つながらないときは次の回に取る
        print(f"{time.strftime('%Y-%m-%d %H:%M:%S')} 取れない：{e}", file=sys.stderr, flush=True)
    trim()


if __name__ == "__main__":
    main()
