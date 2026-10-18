#!/usr/bin/env python3
"""Runs tests/gpu_compare with both renderers and compares the TV pictures.

usage: gpu_compare.py BUILD_BINARY OUTDIR [scale]
Writes OUTDIR/<scene>_soft.png, _gpu.png and _diff.png, prints a table."""
import os, subprocess, sys
from PIL import Image

def yuv_to_rgb(data, w=640, h=480):
    img = Image.new("RGB", (w, h))
    px = img.load()
    for y in range(h):
        row = data[y * w * 2:(y + 1) * w * 2]
        for x in range(0, w, 2):
            y0, u, y1, v = row[x * 2], row[x * 2 + 1], row[x * 2 + 2], row[x * 2 + 3]
            for k, yy in ((0, y0), (1, y1)):
                c = (yy - 16) * 298
                r = (c + 409 * (v - 128) + 128) >> 8
                g = (c - 100 * (u - 128) - 208 * (v - 128) + 128) >> 8
                b = (c + 516 * (u - 128) + 128) >> 8
                px[x + k, y] = tuple(max(0, min(255, q)) for q in (r, g, b))
    return img

def main():
    exe, out = sys.argv[1], sys.argv[2]
    scale = sys.argv[3] if len(sys.argv) > 3 else "1"
    os.makedirs(out + "/soft", exist_ok=True)
    os.makedirs(out + "/gpu", exist_ok=True)
    for mode in ("soft", "gpu"):
        r = subprocess.run([exe, mode, out + "/" + mode, scale], capture_output=True, text=True)
        if r.returncode:
            print(mode, "failed:", r.stdout, r.stderr)
            sys.exit(1)
        if mode == "gpu":
            print("\n".join(l for l in r.stdout.splitlines() if l.startswith("gpu:")))
    print(f"{'scene':18} {'max diff':>8} {'mean':>6} {'>8 off':>8}")
    bad = 0
    for name in sorted(f[:-4] for f in os.listdir(out + "/soft")):
        a = open(f"{out}/soft/{name}.yuv", "rb").read()
        b = open(f"{out}/gpu/{name}.yuv", "rb").read()
        diffs = [abs(x - y) for x, y in zip(a, b)]
        mx = max(diffs); mean = sum(diffs) / len(diffs)
        off = sum(1 for d in diffs if d > 8) / len(diffs) * 100
        print(f"{name:18} {mx:8} {mean:6.2f} {off:7.2f}%")
        ia, ib = yuv_to_rgb(a), yuv_to_rgb(b)
        ia.save(f"{out}/{name}_soft.png"); ib.save(f"{out}/{name}_gpu.png")
        d = Image.new("L", ia.size)
        pa, pb, pd = ia.load(), ib.load(), d.load()
        for y in range(ia.size[1]):
            for x in range(ia.size[0]):
                pd[x, y] = min(255, sum(abs(p - q) for p, q in zip(pa[x, y], pb[x, y])) * 4)
        d.save(f"{out}/{name}_diff.png")
        if off > 1.0: bad += 1
    print("scenes with more than 1% of bytes off by >8:", bad)

    # What the window shows in GPU mode (straight from the GPU) against the
    # software renderer's TV buffer.
    os.makedirs(out + "/present", exist_ok=True)
    r = subprocess.run([exe, "present", out + "/present", scale], capture_output=True, text=True)
    if r.returncode:
        print("present failed:", r.stdout, r.stderr); sys.exit(1)
    print(f"\n{'window picture':18} {'max diff':>8} {'mean':>6} {'>12 off':>8}")
    for name in sorted(f[:-4] for f in os.listdir(out + "/soft")):
        ref = yuv_to_rgb(open(f"{out}/soft/{name}.yuv", "rb").read())
        raw = open(f"{out}/present/{name}.rgb", "rb").read()
        img = Image.frombuffer("RGBA", (640, 480), raw, "raw", "BGRA", 0, 1).convert("RGB")
        img.save(f"{out}/{name}_window.png")
        pa, pb = ref.load(), img.load()
        diffs = [abs(p - q) for y in range(480) for x in range(640) for p, q in zip(pa[x, y], pb[x, y])]
        mx = max(diffs); mean = sum(diffs) / len(diffs)
        off = sum(1 for d in diffs if d > 12) / len(diffs) * 100
        print(f"{name:18} {mx:8} {mean:6.2f} {off:7.2f}%")

main()
