#!/usr/bin/env python3
"""从定稿的 P 生成应用图标：圆角方砖 + 记号，输出 SVG/PNG 多尺寸 + 多尺寸 .ico。

几何全部由 STROKE 推出来，粗细只改一个常量：
  竖干墨迹 0..STROKE（上段 300–536 / 下段 600–800，断口 64）
  环外径固定 118（= 上段墨迹半长，保证与上段上下齐平），环中线 r = 118 - STROKE/2
  环与竖干留 GAP 的缝
"""
import sys
sys.path.insert(0, "/home/ranzon/PartyUp/.tools")
import cairosvg
from PIL import Image

INK, BONE, EMBER = "#0B0C0E", "#F4F1EA", "#FF5A2B"
STROKE = 80.0      # 笔画（56→68→80，用户要求再粗；杠随之变宽，但两段高度与缝隙不变）
GAP = 40.0         # 环与竖干的缝
S, RADIUS = 1024.0, 200.0
FILL = 0.72        # 记号高度占画布比例（60→68→72，用户逐步放大）
RING_OUTER = 118.0
# 竖干两段的**墨迹**端点（与笔画粗细无关，改粗不改这两段的起止和它们之间的缝）
UP_TOP, UP_BOT = 300.0, 536.0
LO_TOP, LO_BOT = 572.0, 800.0

def mark(stroke=None, gap=None):
    s = stroke or STROKE
    g = GAP if gap is None else gap
    h = s / 2
    r = RING_OUTER - h
    cx = s + g + RING_OUTER
    paths = (f'<path d="M{h} {UP_TOP+h} L{h} {UP_BOT-h}"/>'
             f'<path d="M{h} {LO_TOP+h} L{h} {LO_BOT-h}"/>')
    return (f'<g fill="none" stroke="{BONE}" stroke-width="{s}" stroke-linecap="round" '
            f'stroke-linejoin="round">{paths}</g>'
            f'<circle cx="{cx}" cy="418" r="{r}" fill="none" stroke="{EMBER}" stroke-width="{s}"/>',
            cx + RING_OUTER)

def tile(stroke=None, gap=None, background=True):
    body, w = mark(stroke, gap)
    k = S * FILL / 500.0
    pw, ph = w * k, S * FILL
    ox, oy = (S - pw) / 2, (S - ph) / 2
    g = (f'<g transform="translate({ox:.2f},{oy:.2f}) scale({k:.5f}) translate(0,-300)">{body}</g>')
    bg = f'<rect width="{S}" height="{S}" rx="{RADIUS}" ry="{RADIUS}" fill="{INK}"/>' if background else ""
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{S}" height="{S}" viewBox="0 0 {S} {S}">'
            f'{bg}{g}</svg>')

def main():
    svg = tile()
    open("brand/v2/icon.svg", "w").write(svg)
    cairosvg.svg2png(bytestring=svg.encode(), write_to="brand/v2/icon.png", output_width=1024, output_height=1024)
    open("brand/v2/icon-mark.svg", "w").write(tile(background=False))
    cairosvg.svg2png(bytestring=tile(background=False).encode(),
                     write_to="brand/v2/icon-mark.png", output_width=1024, output_height=1024)
    # 记号本体也单独存一份 svg（1024 画布，原始坐标）
    body, w = mark()
    open("brand/v2/letters/p.svg", "w").write(
        f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">'
        f'{body}</svg>')
    cairosvg.svg2png(bytestring=open("brand/v2/letters/p.svg").read().encode(),
                     write_to="brand/v2/letters/p.png", output_width=1024, output_height=1024,
                     background_color=INK)
    for s in [16, 24, 32, 48, 64, 128, 256, 512]:
        cairosvg.svg2png(bytestring=svg.encode(), write_to=f"brand/ico/partyup-{s}.png",
                         output_width=s, output_height=s)
    imgs = [Image.open(f"brand/ico/partyup-{s}.png").convert("RGBA") for s in [256, 128, 64, 48, 32, 24, 16]]
    imgs[0].save("brand/ico/partyup.ico", format="ICO", sizes=[(i.width, i.height) for i in imgs])

    # 验收图：16–256 缩放 + 大图
    sheet = Image.new("RGBA", (900, 260), (30, 32, 36, 255))
    x = 20
    for s in [128, 64, 48, 32, 24, 16]:
        sheet.alpha_composite(Image.open(f"brand/ico/partyup-{s}.png").convert("RGBA"), (x, 60))
        x += s + 28
    sheet.alpha_composite(Image.open("brand/v2/icon.png").convert("RGBA").resize((220, 220), Image.LANCZOS), (650, 20))
    sheet.save("brand/v2/icon-sheet.png")
    print(f"done  stroke={STROKE} gap={GAP} mark_width={w:.0f}")

if __name__ == "__main__":
    main()
