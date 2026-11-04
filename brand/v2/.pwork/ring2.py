import cairosvg, os, io
from PIL import Image
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def ringP(ro, gap, cy_top=300, cx=512.0):
    ri = ro - 72
    head = ri - gap
    cy = cy_top + ro
    sx = cx - ro + 36
    body = (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{ro-36}" fill="none" stroke="#F4F1EA" stroke-width="72"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{head}" fill="#FF5A2B"/>')
    return body, dict(ro=ro, ri=ri, head=head, cy=cy, sx=sx, width=2*ro, bottom=cy+ro)

V = {"84": ringP(186, 30), "92": ringP(194, 30), "98": ringP(200, 30)}
parts = []
for i,(k,(b,g)) in enumerate(V.items()):
    x, y = 20 + i*(420+20), 60
    parts.append(f'<g transform="translate({x},{y}) scale(0.39)">{b}</g>')
    parts.append(f'<text x="{x+8}" y="{y-14}" font-family="DejaVu Sans Mono" font-size="16" fill="#8A8D93">head {k} · bowl w{g["width"]} bottom {g["bottom"]}</text>')
W = 20 + 3*(420+20)
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="470" viewBox="0 0 {W} 470">'
       f'<rect width="{W}" height="470" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
open("ring2.svg","w").write(svg)
cairosvg.svg2png(url="ring2.svg", write_to="ring2.png", output_width=W, background_color="#0B0C0E")
for k,(b,g) in V.items(): print(k, g)
