#!/usr/bin/env python3
"""Silent downloader/unpacker for the portable toolchain (Windows).

Uses the bundled CPython (OpenSSL) because system schannel TLS is broken here.
Usage: setup_tools.py openssl cmake mingw sdk vc
"""
import os
import re
import sys
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
DL = os.path.join(TOOLS, "downloads")
LOG = os.path.join(TOOLS, "setup.log")
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
# 按实测速度排序的 GitHub 代理
MIRRORS = [
    "https://gh-proxy.com/",
    "https://ghfast.top/",
    "https://ghproxy.net/",
]
MIRROR = MIRRORS[0]


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    os.makedirs(TOOLS, exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def http_get(url, timeout=60):
    req = urllib.request.Request(url, headers=UA)
    return urllib.request.urlopen(req, timeout=timeout)


def probe(url):
    """Return (status, size, final_url) or raise."""
    try:
        r = http_get(url)
        size = r.headers.get("Content-Length")
        return r.status, int(size) if size else -1, r.geturl()
    except Exception as e:
        raise RuntimeError("probe failed %s: %s" % (url, e))


def download(url, name, expect_min=100 * 1024, tries=4):
    os.makedirs(DL, exist_ok=True)
    dest = os.path.join(DL, name)
    if os.path.exists(dest) and os.path.getsize(dest) >= expect_min:
        log("cached %s (%d bytes)" % (name, os.path.getsize(dest)))
        return dest
    for i in range(1, tries + 1):
        tmp = dest + ".part"
        try:
            t0 = time.time()
            with http_get(url, timeout=120) as r, open(tmp, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                done = 0
                while True:
                    chunk = r.read(1 << 20)
                    if not chunk:
                        break
                    f.write(chunk)
                    done += len(chunk)
                    if total and done % (32 << 20) < (1 << 20):
                        pct = 100.0 * done / total
                        log("  %s %5.1f%% (%.1f/%.1f MB)" % (name, pct, done / 1e6, total / 1e6))
            size = os.path.getsize(tmp)
            if size < expect_min:
                raise RuntimeError("too small: %d bytes" % size)
            os.replace(tmp, dest)
            log("downloaded %s (%.1f MB, %.1fs)" % (name, size / 1e6, time.time() - t0))
            return dest
        except Exception as e:
            log("attempt %d/%d failed for %s: %s" % (i, tries, name, e))
            if i == tries:
                raise
            time.sleep(3 * i)


def unzip(src, dest, strip_root=False):
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(src) as z:
        names = z.namelist()
        root = ""
        if strip_root:
            tops = {n.split("/")[0] for n in names if n.split("/")[0]}
            if len(tops) == 1:
                root = tops.pop() + "/"
        for n in names:
            if not n.startswith(root):
                continue
            rel = n[len(root):]
            if not rel:
                continue
            out = os.path.join(dest, rel)
            if n.endswith("/"):
                os.makedirs(out, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(n) as s, open(out, "wb") as d:
                while True:
                    b = s.read(1 << 20)
                    if not b:
                        break
                    d.write(b)
    log("unzipped %s -> %s" % (os.path.basename(src), dest))


def latest_tag(repo):
    """Resolve 'latest' tag of a GitHub repo through a mirror."""
    for m in MIRRORS:
        url = m + "https://github.com/%s/releases/latest" % repo
        try:
            with http_get(url, timeout=20) as r:
                return r.geturl()
        except Exception as e:
            log("latest_tag(%s) via %s failed: %s" % (repo, m, e))
    return ""


def get_openssl():
    dest = os.path.join(TOOLS, "openssl")
    if os.path.exists(os.path.join(dest, "include", "openssl", "evp.h")):
        log("openssl already present")
        return dest
    cands = [
        "https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.5.4.zip",
        "https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.5.3.zip",
        "https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.5.2.zip",
        "https://download.firedaemon.com/FireDaemon-OpenSSL/openssl-3.4.1.zip",
    ]
    for u in cands:
        try:
            st, size, final = probe(u)
            if st == 200 and size > 5 * 1024 * 1024:
                log("openssl source: %s (%d MB)" % (u, size // (1024 * 1024)))
                z = download(u, re.sub(r"[^A-Za-z0-9._-]", "_", os.path.basename(final)))
                unzip(z, dest, strip_root=True)
                return dest
            log("skip %s (%s, %s bytes)" % (u, st, size))
        except Exception as e:
            log("try %s -> %s" % (u, e))
    raise RuntimeError("cannot obtain openssl")


def get_cmake():
    dest = os.path.join(TOOLS, "cmake")
    if os.path.exists(os.path.join(dest, "bin", "cmake.exe")):
        log("cmake already present")
        return dest
    tag_url = latest_tag("Kitware/CMake")
    m = re.search(r"/tag/(v[\d.]+)", tag_url or "")
    tags = [m.group(1)] if m else []
    tags += ["v4.4.4", "v4.1.2", "v4.0.3", "v3.31.8"]
    for t in tags:
        asset = "cmake-%s-windows-x86_64.zip" % t.lstrip("v")
        for mirror in MIRRORS:
            u = mirror + "https://github.com/Kitware/CMake/releases/download/%s/%s" % (t, asset)
            try:
                st, size, final = probe(u)
                if st == 200 and size > 20 * 1024 * 1024:
                    log("cmake source: %s (%d MB)" % (u, size // (1024 * 1024)))
                    z = download(u, asset, expect_min=20 * 1024 * 1024)
                    unzip(z, dest, strip_root=True)
                    return dest
                log("skip %s (%s, %s)" % (u, st, size))
            except Exception as e:
                log("try %s -> %s" % (u, e))
    raise RuntimeError("cannot obtain cmake")


def get_mingw():
    dest = os.path.join(TOOLS, "mingw64")
    if os.path.exists(os.path.join(dest, "bin", "g++.exe")):
        log("mingw already present")
        return dest
    tag_url = latest_tag("brechtsanders/winlibs_mingw")
    m = re.search(r"/tag/(.+)", tag_url or "")
    tags = [m.group(1)] if m else []
    tags += ["15.2.0-12.0.0-ucrt-r2"]
    for t in tags:
        asset = "winlibs-x86_64-posix-seh-gcc-%s-mingw-w64ucrt-12.0.0-r2.zip" % t.split("-")[0]
        u = MIRROR + "https://github.com/brechtsanders/winlibs_mingw/releases/download/%s/%s" % (t, asset)
        try:
            st, size, final = probe(u)
            if st == 200 and size > 50 * 1024 * 1024:
                log("mingw source: %s (%d MB)" % (u, size // (1024 * 1024)))
                z = download(u, asset, expect_min=50 * 1024 * 1024)
                unzip(z, dest, strip_root=True)
                return dest
            log("skip %s (%s, %s)" % (u, st, size))
        except Exception as e:
            log("try %s -> %s" % (u, e))
    raise RuntimeError("cannot obtain mingw")


def get_winsdk():
    """Windows SDK headers/libs from NuGet (portable, no admin)."""
    dest = os.path.join(TOOLS, "sdk")
    if os.path.exists(os.path.join(dest, "Include")):
        log("sdk already present")
        return dest
    base = "https://globalcdn.nuget.org/packages/%s.%s.nupkg"
    # resolve newest stable version
    import json
    with http_get("https://api.nuget.org/v3-flatcontainer/%s/index.json" % "microsoft.windows.sdk.cpp") as r:
        vers = [v for v in json.loads(r.read())["versions"] if "-" not in v]
    ver = sorted(vers, key=lambda s: [int(x) for x in s.split(".") if x.isdigit()])[-1]
    log("winsdk nuget version: %s" % ver)
    pkgs = ["microsoft.windows.sdk.cpp." + ver,
            "microsoft.windows.sdk.cpp.x64." + ver,
            "microsoft.windows.sdk.cpp.desktop." + ver]
    for p in pkgs:
        u = base % (p, ver)
        try:
            st, size, _ = probe(u)
            if st != 200:
                log("skip %s (%s)" % (p, st))
                continue
            z = download(u, p + ".nupkg", expect_min=1024)
            tmp = os.path.join(DL, p)
            unzip(z, tmp)
            log("extracted %s" % p)
        except Exception as e:
            log("nuget %s -> %s" % (p, e))
    return dest


def main(argv):
    os.makedirs(TOOLS, exist_ok=True)
    want = argv[1:] or ["openssl", "cmake"]
    for w in want:
        fn = {"openssl": get_openssl, "cmake": get_cmake,
              "mingw": get_mingw, "sdk": get_winsdk}.get(w)
        if not fn:
            log("unknown component %s" % w)
            continue
        log("== %s ==" % w)
        try:
            log("ok: %s" % fn())
        except Exception as e:
            log("FAILED %s: %s" % (w, e))
            if w in ("openssl", "cmake"):
                return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
