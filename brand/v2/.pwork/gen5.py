import cairosvg, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
SX = 387
def letter(cy=393, r=105, gap=40, xt=SX, xb=SX):
    # disc placed from local stem x at cy
    t = (cy - 336) / (764 - 336)
    sx = xt + (xb - xt) * t
    cx = sx + 36 + gap + r
    return (f'<path d="M{xb} 764 L{xt} 336" stroke="#F4F1EA" stroke-width="72" '
            f'stroke-linecap="round" fill="none"/><circle cx="{cx}" cy="{cy}" r="{r}" fill="#FF5A2B"/>')
V = {
  "P1": letter(393,105,40),               # head 12 above cap, straight
  "P2": letter(405,105,38),               # top flush with cap
  "P3": letter(393,105,40,xt=382),        # 5px lean at top
  "P4": letter(390,105,40),
}
parts = []
for i,(k,b) in enumerate(V.items()):
    x, y = 20 + i*(520+20), 50
    parts.append(f'<g transform="translate({x},{y}) scale(0.47)">{b}</g>')
    parts.append(f'<text x="{x+8}" y="{y-12}" font-family="DejaVu Sans Mono" font-size="18" fill="#8A8D93">{k}</text>')
W = 20+4*(520+20)
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="560" viewBox="0 0 {W} 560">'
       f'<rect width="{W}" height="560" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
open("row.svg","w").write(svg)
cairosvg.svg2png(url="row.svg", write_to="row.png", output_width=W, background_color="#0B0C0E")
print("ok", W)
