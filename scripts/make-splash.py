#!/usr/bin/env python3
"""从 brand/v2/splash.svg 重新生成 assets/splash.png（Velopack 安装器启动画面）。

为什么需要它：splash 是渲染产物，源在 SVG。改了 SVG 却忘记重出 PNG，
安装器会继续显示旧图，而且不会有任何报错——所以把渲染固定成一条命令。

依赖（工作区本地安装，均在 .cache/ 下，不入库）：
  * cairosvg + Pillow：PYTHONPATH 指向 .tools
  * Noto Sans Regular / SemiBold：放在 .cache/fakehome/.fonts 并 fc-cache

用法：
  HOME=$PWD/.cache/fakehome PYTHONPATH=$PWD/.tools python3 scripts/make-splash.py

渲染策略：按 2 倍尺寸（1000x600）出图再 LANCZOS 缩到 500x300。
直接按 1 倍渲染的文字边缘在 Windows 的缩放显示下会发毛，超采样能消掉。
"""
import io
import os
import sys

try:
    import cairosvg
    from PIL import Image
except ImportError:
    sys.exit("缺少依赖：需要 cairosvg 与 Pillow（把 .tools 加进 PYTHONPATH）")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "brand", "v2", "splash.svg")
OUT = os.path.join(ROOT, "assets", "splash.png")
W, H = 500, 300
SCALE = 2


def main() -> int:
    if not os.path.exists(SRC):
        sys.exit(f"找不到源文件：{SRC}")

    with open(SRC, "rb") as f:
        svg = f.read()

    png = cairosvg.svg2png(bytestring=svg, output_width=W * SCALE, output_height=H * SCALE)
    img = Image.open(io.BytesIO(png)).convert("RGB").resize((W, H), Image.LANCZOS)
    img.save(OUT, optimize=True)

    print(f"{OUT}  {W}x{H}  {os.path.getsize(OUT)} 字节")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
