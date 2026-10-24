#!/usr/bin/env python3
"""把 Windows SDK 的 NuGet 包解包成 vcvars 风格的 Include/Lib 目录。

NuGet 内部布局：
  c/Include/<sdkver>/{ucrt,um,shared,...}/*.h   （头文件包 microsoft.windows.sdk.cpp）
  c/{ucrt,um}/x64/*.lib                        （库包 microsoft.windows.sdk.cpp.x64）

目标布局：
  tools/sdk/Include/<sdkver>/{ucrt,um,shared,...}/*.h
  tools/sdk/Lib/<ver>/{ucrt,um}/x64/*.lib
"""
import os
import shutil
import sys
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DL = os.path.join(ROOT, "tools", "downloads")
SDK = os.path.join(ROOT, "tools", "sdk")
VER = "10.0.28000.2705"


def extract(z, member, out_path):
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with z.open(member) as s, open(out_path, "wb") as d:
        shutil.copyfileobj(s, d)


def unpack_headers(path):
    n = 0
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            parts = name.split("/")
            # c/Include/<ver>/<area>/...
            if len(parts) < 5 or parts[0] != "c" or parts[1] != "Include":
                continue
            if name.endswith("/"):
                continue
            if not parts[-1].lower().endswith((".h", ".hpp", ".inl", ".idl")):
                continue
            out = os.path.join(SDK, "Include", parts[2], *parts[3:])
            extract(z, name, out)
            n += 1
    print("  headers: %d files" % n)


def unpack_libs(path):
    n = 0
    with zipfile.ZipFile(path) as z:
        for name in z.namelist():
            parts = name.split("/")
            # c/<area>/x64/*.lib
            if len(parts) < 4 or parts[0] != "c" or name.endswith("/"):
                continue
            if not parts[-1].lower().endswith(".lib"):
                continue
            out = os.path.join(SDK, "Lib", VER, parts[1], parts[2], *parts[3:])
            extract(z, name, out)
            n += 1
    print("  libs: %d files" % n)


def main():
    # 清掉早期错误嵌套
    for junk in (os.path.join(SDK, "Include", VER),):
        if os.path.isdir(junk):
            shutil.rmtree(junk, ignore_errors=True)

    libs = os.path.join(DL, "microsoft.windows.sdk.cpp.x64.%s.nupkg" % VER)
    heads = os.path.join(DL, "microsoft.windows.sdk.cpp.%s.nupkg" % VER)
    if os.path.exists(libs):
        print("unpacking libs ...")
        unpack_libs(libs)
    if os.path.exists(heads):
        print("unpacking headers ...")
        unpack_headers(heads)

    for sub in ("Include", "Lib"):
        p = os.path.join(SDK, sub)
        if os.path.isdir(p):
            print(" ", sub, sorted(os.listdir(p)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
