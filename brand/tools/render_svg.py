#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""把任意 SVG 渲染成 PNG（给子代理用，不要自己再写渲染器）。

    PYTHONPATH=.tools python3 brand/tools/render_svg.py in.svg out.png [width] [bg]

bg 省略 = 保留 SVG 自身的透明背景。默认按 SVG 的 viewBox 等比放大到 width。
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))), ".tools"))

import cairosvg  # noqa: E402


def main() -> None:
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    src, dst = sys.argv[1], sys.argv[2]
    width = int(sys.argv[3]) if len(sys.argv) > 3 else 1024
    bg = sys.argv[4] if len(sys.argv) > 4 else None
    cairosvg.svg2png(url=src, write_to=dst, output_width=width, background_color=bg)
    print(f"{dst} ({width}px, bg={bg or 'transparent'})")


if __name__ == "__main__":
    main()
