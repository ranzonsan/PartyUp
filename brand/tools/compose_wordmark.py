#!/usr/bin/env python3
"""自绘 P + Ubuntu 字体轮廓 → 'Party' 整词 SVG/PNG。
只取字形轮廓，不打包 ttf；输出为纯 path。
"""
import sys, argparse
sys.path.insert(0, "/home/ranzon/PartyUp/.tools")
from fontTools.ttLib import TTFont
from fontTools.pens.svgPathPen import SVGPathPen
from fontTools.pens.transformPen import TransformPen
from fontTools.misc.transform import Transform
import cairosvg

FONT = "/usr/share/fonts/truetype/ubuntu/Ubuntu[wdth,wght].ttf"
INK, BONE, EMBER = "#0B0C0E", "#F4F1EA", "#FF5A2B"
BASELINE, CAP = 800.0, 500.0
VY, VH = 60.0, 940.0

def glyph_paths(text, scale, x0):
    f = TTFont(FONT)
    gs, cmap, hmtx = f.getGlyphSet(), f.getBestCmap(), f["hmtx"]
    out, x = [], x0
    for ch in text:
        g = cmap[ord(ch)]
        pen = SVGPathPen(gs, ntos=lambda v: f"{v:.2f}")
        gs[g].draw(TransformPen(pen, Transform(scale, 0, 0, -scale, x, BASELINE)))
        d = pen.getCommands()
        if d:
            out.append(f'<path d="{d}" fill="{BONE}"/>')
        x += hmtx[g][0] * scale
    return "".join(out), x

def p_mark():
    return ('<path d="M58 336 L58 500" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            '<path d="M58 600 L58 764" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            '<circle cx="252" cy="418" r="82" fill="none" stroke="#FF5A2B" stroke-width="72"/>')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="brand/v2/wordmark")
    ap.add_argument("--gap", type=float, default=56.0)
    ap.add_argument("--px", type=int, default=2400)
    ap.add_argument("--letters", default="arty")
    a = ap.parse_args()

    scale = CAP / 693.0
    body, x_end = glyph_paths(a.letters, scale, 348.0 + a.gap)
    VW = x_end + 80.0
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="-40 {VY} {VW} {VH}" '
           f'width="{VW}" height="{VH}">'
           f'<rect x="-40" y="{VY}" width="{VW}" height="{VH}" fill="{INK}"/>'
           f'<g transform="translate(38,0)">{p_mark()}</g>{body}</svg>')
    open(f"{a.out}.svg", "w").write(svg)
    cairosvg.svg2png(bytestring=svg.encode(), write_to=f"{a.out}.png",
                     output_width=a.px, output_height=int(a.px * VH / VW))
    print(f"-> {a.out}.png  {a.px}x{int(a.px*VH/VW)}  ink≈{x_end:.0f}")

if __name__ == "__main__":
    main()
