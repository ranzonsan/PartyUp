#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把 brand/v2/letters/*.svg 按各自墨迹宽度拼成 "Party"（共享 1024 坐标系）。

    PYTHONPATH=.tools python3 brand/tools/compose_party.py [--gap 56] [--letters p,a,r,t,y]
"""
from __future__ import annotations
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(ROOT), ".tools"))

import cairosvg  # noqa: E402
from PIL import Image  # noqa: E402
import io

INK, BONE, EMBER = "#0B0C0E", "#F4F1EA", "#FF5A2B"
LETTERS = ["p", "a", "r", "t", "y"]
LDIR = os.path.join(ROOT, "v2", "letters")
OUT = os.path.join(ROOT, "v2")
BASELINE = 800.0

NUM = re.compile(r"-?\d+(?:\.\d+)?")


def ink_bbox(svg_text: str, pad: float) -> tuple[float, float]:
    xs: list[float] = []
    ys: list[float] = []
    for m in re.finditer(r"<circle\b([^>]*)>", svg_text):
        a = m.group(1)
        cx = float(re.search(r'\bcx="([-\d.]+)"', a).group(1))
        cy = float(re.search(r'\bcy="([-\d.]+)"', a).group(1))
        r = float(re.search(r'\br="([-\d.]+)"', a).group(1))
        xs += [cx - r, cx + r]
        ys += [cy - r, cy + r]
    for m in re.finditer(r'\bd="([^"]+)"', svg_text):
        d = m.group(1)
        pts = [float(v) for v in NUM.findall(d)]
        xs += pts[0::2]
        ys += pts[1::2]
    return min(xs) - pad, max(xs) + pad


def load(name: str) -> tuple[str, float, float]:
    """返回 (内部 svg, 墨迹左边界, 右边界)；用 Pillow 实测 alpha 通道，不猜坐标。"""
    path = os.path.join(LDIR, f"{name}.svg")
    if not os.path.exists(path):
        raise SystemExit(f"缺文件: {path}")
    s = open(path, encoding="utf-8").read()
    inner = s[s.index(">", s.index("<svg")) + 1: s.rindex("</svg>")]
    inner = re.sub(r"<!--.*?-->", "", inner, flags=re.S)
    png = cairosvg.svg2png(url=path, output_width=1024, output_height=1024)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    bbox = im.split()[3].getbbox()
    if bbox is None:
        raise SystemExit(f"{name}: 空图")
    x0, x1 = float(bbox[0]), float(bbox[2])
    return inner, x0, x1


def build(letters: list[str], gap: float, ox: float = 0.0) -> tuple[str, float, list[tuple[str, float, float]]]:
    parts, total, meta = [], 0.0, []
    for i, name in enumerate(letters):
        inner, x0, x1 = load(name)
        w = x1 - x0
        parts.append(f'<svg x="{ox + total:.2f}" y="0" width="{w:.2f}" height="1024" '
                     f'viewBox="{x0:.2f} 0 {w:.2f} 1024">{inner}</svg>')
        meta.append((name, total, total + w))
        total += w + (gap if i < len(letters) - 1 else 0.0)
    return "".join(parts), total, meta


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--gap", type=float, default=56.0)
    ap.add_argument("--gaps", default="")  # 逐对字距，逗号分隔，缺省用 --gap
    ap.add_argument("--letters", default=",".join(LETTERS))
    ap.add_argument("--out", default="party")
    args = ap.parse_args()
    letters = [x.strip() for x in args.letters.split(",") if x.strip()]

    probe_total = build(letters, args.gap)[1]
    W, H = 2600, 1240
    ox = (W - probe_total) / 2
    pair_gaps = [float(x) for x in args.gaps.split(",")] if args.gaps else None
    if pair_gaps:
        # 逐对间距：把 build 拆成手动排布
        parts, total, meta = [], 0.0, []
        for i, name in enumerate(letters):
            inner, x0, x1 = load(name)
            w = x1 - x0
            parts.append(f'<svg x="{ox + total:.2f}" y="0" width="{w:.2f}" height="1024" '
                         f'viewBox="{x0:.2f} 0 {w:.2f} 1024">{inner}</svg>')
            meta.append((name, total, total + w))
            total += w + (pair_gaps[i] if i < len(pair_gaps) else 56.0)
        body = "".join(parts)
    else:
        body, total, meta = build(letters, args.gap, ox=ox)
    for n, a, b in meta:
        print(f"  {n}: {a:7.1f} … {b:7.1f}   ({b-a:.0f} wide)")
    print(f"total {total:.0f}")

    oy = 40.0
    g = [f'<rect width="{W}" height="{H}" fill="{INK}"/>',
         f'<g transform="translate(0,{oy:.1f})">{body}</g>',
         f'<line x1="{ox-70:.1f}" y1="{oy+BASELINE}" x2="{ox+total+70:.1f}" y2="{oy+BASELINE}" stroke="#242830"/>',
         f'<rect x="{ox:.1f}" y="{oy+BASELINE+250}" width="{total:.1f}" height="200" fill="none" '
         f'stroke="#242830" stroke-dasharray="8 9"/>',
         f'<text x="{ox+total/2:.1f}" y="{oy+BASELINE+365}" font-family="DejaVu Sans Mono" font-size="20" '
         f'fill="#3E434B" text-anchor="middle" letter-spacing="7">UP RESERVED</text>']
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
           + "".join(g) + "</svg>")
    os.makedirs(OUT, exist_ok=True)
    open(os.path.join(OUT, args.out + ".svg"), "w", encoding="utf-8").write(svg)
    cairosvg.svg2png(bytestring=svg.encode(), write_to=os.path.join(OUT, args.out + ".png"),
                     output_width=W, output_height=H)
    print(f"→ brand/v2/{args.out}.png")


if __name__ == "__main__":
    main()
