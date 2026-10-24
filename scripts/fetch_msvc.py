#!/usr/bin/env python3
"""Provision a portable MSVC toolchain (no admin) from the VS 2022 channel.

Downloads .base.vsix payloads, extracts the `Contents/` tree which mirrors the
Visual Studio install layout, then merges them into tools/msvc + tools/sdk.
"""
import json
import os
import re
import shutil
import sys
import time
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
DL = os.path.join(TOOLS, "downloads")
MSVC = os.path.join(TOOLS, "msvc")
SDK = os.path.join(TOOLS, "sdk")
UA = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64)"}
VSMAN_URL = ("https://download.visualstudio.microsoft.com/download/pr/"
             "c95450be-0a45-4779-b142-33f567ccfd6e/"
             "a871a5ff41acebbe5037455fd6f51fcd4907b47d8f3c9860871ea0b222a2fd45/VisualStudio.vsman")
LOG = os.path.join(TOOLS, "setup.log")


def log(msg):
    line = "[%s] %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    os.makedirs(TOOLS, exist_ok=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def download(url, name, expect_min=1024, tries=6):
    """支持断点续传的下载（网络很慢时多次运行可继续）。"""
    os.makedirs(DL, exist_ok=True)
    dest = os.path.join(DL, name)
    if os.path.exists(dest) and os.path.getsize(dest) >= expect_min:
        log("cached %s (%.1f MB)" % (name, os.path.getsize(dest) / 1e6))
        return dest
    tmp = dest + ".part"
    for i in range(1, tries + 1):
        start = os.path.getsize(tmp) if os.path.exists(tmp) else 0
        try:
            headers = dict(UA)
            if start:
                headers["Range"] = "bytes=%d-" % start
            t0 = time.time()
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as r:
                mode = "ab"
                total = 0
                if getattr(r, "status", 200) == 200 and start:
                    log("  server ignored Range, restarting %s" % name)
                    start, mode = 0, "wb"
                clen = int(r.headers.get("Content-Length") or 0)
                total = start + clen
                with open(tmp, mode) as f:
                    done = start
                    while True:
                        b = r.read(1 << 20)
                        if not b:
                            break
                        f.write(b)
                        done += len(b)
                        if done % (16 << 20) < (1 << 20):
                            log("  %s %.1f%% (%.1f/%.1f MB)" % (name, 100.0 * done / max(total, 1),
                                                                 done / 1e6, total / 1e6))
            size = os.path.getsize(tmp)
            if total and size < total:
                raise RuntimeError("incomplete %d/%d" % (size, total))
            if size < expect_min:
                raise RuntimeError("too small")
            os.replace(tmp, dest)
            log("downloaded %s (%.1f MB, %.1fs)" % (name, size / 1e6, time.time() - t0))
            return dest
        except Exception as e:
            log("retry %d/%d %s: %s" % (i, tries, name, e))
            if i == tries:
                raise
            time.sleep(2 * i)


def vsman():
    os.makedirs(DL, exist_ok=True)
    p = os.path.join(DL, "VisualStudio.vsman")
    if not os.path.exists(p):
        log("fetching VisualStudio.vsman (30MB)")
        download(VSMAN_URL, "VisualStudio.vsman", expect_min=5 * 1024 * 1024)
    return {x.get("id"): x for x in json.load(open(p, encoding="utf-8"))["packages"]}


def payload_of(pkgmap, pid):
    p = pkgmap.get(pid)
    if not p:
        raise KeyError("package not found: " + pid)
    return p["payloads"][0]


def extract_contents(vsix, dest, keep=("Contents/",)):
    """Extract selected top-level dirs, stripping 'Contents/'."""
    os.makedirs(dest, exist_ok=True)
    n = 0
    with zipfile.ZipFile(vsix) as z:
        for name in z.namelist():
            if not any(name.startswith(k) for k in keep) or name.endswith("/"):
                continue
            rel = name.split("/", 1)[1]
            out = os.path.join(dest, rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(name) as s, open(out, "wb") as d:
                shutil.copyfileobj(s, d)
            n += 1
    log("extracted %d files from %s" % (n, os.path.basename(vsix)))


def fetch_msvc():
    pk = vsman()
    plan = [
        "Microsoft.VC.14.44.17.14.Tools.HostX64.TargetX64.base",
        "Microsoft.VC.14.44.17.14.CRT.Headers.base",
        "Microsoft.VC.14.44.17.14.CRT.x64.Desktop.base",
        "Microsoft.VC.14.44.17.14.CRT.Redist.X64.base",
    ]
    for pid in plan:
        pl = payload_of(pk, pid)
        f = download(pl["url"], os.path.basename(pl["fileName"]), expect_min=512 * 1024)
        extract_contents(f, MSVC)
    log("msvc tree ready: %s" % MSVC)


def fetch_sdk():
    """Windows SDK headers/libs from NuGet (portable)."""
    import io
    os.makedirs(SDK, exist_ok=True)
    with urllib.request.urlopen(urllib.request.Request(
            "https://api.nuget.org/v3-flatcontainer/microsoft.windows.sdk.cpp/index.json",
            headers=UA), timeout=60) as r:
        vers = [v for v in json.load(r)["versions"] if "-" not in v]
    ver = sorted(vers, key=lambda s: [int(x) for x in s.split(".")])[-1]
    log("windows sdk nuget: %s" % ver)
    for pid in ("microsoft.windows.sdk.cpp.x64", "microsoft.windows.sdk.cpp"):
        url = "https://globalcdn.nuget.org/packages/%s.%s.nupkg" % (pid, ver)
        try:
            f = download(url, "%s.%s.nupkg" % (pid, ver), expect_min=1024 * 1024)
        except Exception as e:
            log("skip %s: %s" % (pid, e))
            continue
        extract_contents(f, SDK, keep=("Include/", "Lib/", "include/", "lib/", "c/"))
    log("sdk tree: %s" % SDK)


def consolidate():
    """把不同版本号的 MSVC 目录合并成一个（各 NuGet/VSIX 包的版本号略有差异）。"""
    base = os.path.join(MSVC, "VC", "Tools", "MSVC")
    if not os.path.isdir(base):
        return
    dirs = sorted([d for d in os.listdir(base) if os.path.isdir(os.path.join(base, d))], reverse=True)
    target = None
    for d in dirs:
        if os.path.exists(os.path.join(base, d, "bin", "HostX64", "x64", "cl.exe")):
            target = d
            break
    if not target:
        log("consolidate: cl.exe not found, skip")
        return
    tdir = os.path.join(base, target)
    for d in dirs:
        if d == target:
            continue
        src = os.path.join(base, d)
        shutil.copytree(src, tdir, dirs_exist_ok=True)
        shutil.rmtree(src, ignore_errors=True)
        log("merged %s -> %s" % (d, target))
    log("msvc root: %s" % tdir)


if __name__ == "__main__":
    what = sys.argv[1:] or ["msvc", "sdk"]
    for w in what:
        {"msvc": fetch_msvc, "sdk": fetch_sdk}[w]()
    if "msvc" in what:
        consolidate()
