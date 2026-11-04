import cairosvg, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def ringP(ro, gap, cy, cx=512.0):
    ri = ro - 72
    head = ri - gap
    sx = cx - ro + 36
    body = (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{ro-36}" fill="none" stroke="#F4F1EA" stroke-width="72"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{head}" fill="#FF5A2B"/>')
    return body, dict(ro=ro, ri=ri, head=head, cy=cy, sx=sx, top=cy-ro, bottom=cy+ro)

# ro=194 -> ri 122, head 92; junction with stem (x=390) at y = cy - sqrt(ro^2-122^2)
import math
ro, ri = 194, 122
j = math.sqrt(ro*ro - ri*ri)
V = {
 "A_top300": ringP(ro,30,300+ro),          # bowl top at cap
 "B_joint300": ringP(ro,30,300+j),         # bowl meets stem at cap
 "C_mid": ringP(ro,30,300+ro*0.92),
}
parts = []
for i,(k,(b,g)) in enumerate(V.items()):
    x, y = 20 + i*(420+20), 70
    parts.append(f'<g transform="translate({x},{y}) scale(0.39)">{b}</g>')
    parts.append(f'<text x="{x+8}" y="{y-30}" font-family="DejaVu Sans Mono" font-size="15" fill="#8A8D93">{k}</text>')
    parts.append(f'<text x="{x+8}" y="{y-12}" font-family="DejaVu Sans Mono" font-size="15" fill="#8A8D93">top {g["top"]:.0f} bot {g["bottom"]:.0f} hd {g["head"]}</text>')
W = 20 + 3*(420+20)
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="480" viewBox="0 0 {W} 480">'
       f'<rect width="{W}" height="480" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
open("ring3.svg","w").write(svg)
cairosvg.svg2png(url="ring3.svg", write_to="ring3.png", output_width=W, background_color="#0B0C0E")
for k,(b,g) in V.items(): print(k, {kk: round(vv,1) if isinstance(vv,float) else vv for kk,vv in g.items()})
