#!/usr/bin/env python3
"""通用断点续传下载器：读超时短、无限重试，适合不稳定网络。

用法: fetch_url.py <url> <dest> [min_bytes]
"""
import os
import sys
import time
import urllib.request

UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}


def log(msg):
    print("[%s] %s" % (time.strftime("%H:%M:%S"), msg), flush=True)


def fetch(url, dest, min_bytes, budget=0):
    os.makedirs(os.path.dirname(dest) or ".", exist_ok=True)
    if os.path.exists(dest) and os.path.getsize(dest) >= min_bytes:
        log("cached %s (%d bytes)" % (dest, os.path.getsize(dest)))
        return 0
    tmp = dest + ".part"
    attempt = 0
    last_report = time.time()
    t_start = time.time()
    while True:
        if budget and time.time() - t_start > budget:
            log("time budget reached, partial kept (%d bytes)" %
                (os.path.getsize(tmp) if os.path.exists(tmp) else 0))
            return 2
        attempt += 1
        start = os.path.getsize(tmp) if os.path.exists(tmp) else 0
        headers = dict(UA)
        if start:
            headers["Range"] = "bytes=%d-" % start
        try:
            r = urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30)
            if getattr(r, "status", 200) == 200 and start:
                log("server ignored Range, restart from 0")
                start = 0
                mode = "wb"
            else:
                mode = "ab"
            clen = int(r.headers.get("Content-Length") or 0)
            total = start + clen if clen else 0
            with open(tmp, mode) as f:
                done = start
                while True:
                    b = r.read(1 << 20)
                    if not b:
                        break
                    f.write(b)
                    done += len(b)
                    if time.time() - last_report > 15:
                        last_report = time.time()
                        pct = ("%.1f%%" % (100.0 * done / total)) if total else "?"
                        log("  %.1f/%.1f MB (%s)" % (done / 1e6, total / 1e6, pct))
            size = os.path.getsize(tmp)
            if total and size < total:
                raise RuntimeError("incomplete %d/%d" % (size, total))
            if size < min_bytes:
                raise RuntimeError("too small %d" % size)
            os.replace(tmp, dest)
            log("done %s (%.1f MB)" % (dest, size / 1e6))
            return 0
        except Exception as e:
            log("attempt %d failed at %.1f MB: %s" % (
                attempt, (os.path.getsize(tmp) if os.path.exists(tmp) else 0) / 1e6, e))
            time.sleep(min(2 * attempt, 30))


if __name__ == "__main__":
    url, dest = sys.argv[1], sys.argv[2]
    mb = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    budget = int(sys.argv[4]) if len(sys.argv) > 4 else 0
    sys.exit(fetch(url, dest, mb, budget))
