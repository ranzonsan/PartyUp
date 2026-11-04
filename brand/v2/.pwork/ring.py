import cairosvg, os, re
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def ringP(ro=194, head=92, gap=30, cy_off=0):
    # ring outer radius ro, stroke 72 => path radius ro-36, counter radius ro-72
    cx = 512.0
    ri = ro - 72
    head = ri - gap
    cy = 300 + ro
    sx = cx - ro + 36          # stem centreline: ring outer left edge == stem left edge
    return (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{ro-36}" fill="none" stroke="#F4F1EA" stroke-width="72"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{head}" fill="#FF5A2B"/>'), dict(cx=cx, cy=cy, ro=ro, ri=ri, head=head, sx=sx)

V = {"R194_92": ringP(194, gap=30), "R200_92": ringP(200, gap=30), "R207_105": ringP(207, gap=30), "R186_84": ringP(186, gap=30)}
parts = []
for i,(k,(body, g)) in enumerate(V.items()):
    x, y = 20 + i*(430+20), 60
    parts.append(f'<g transform="translate({x},{y}) scale(0.38)">{body}</g>')
    parts.append(f'<text x="{x+8}" y="{y-14}" font-family="DejaVu Sans Mono" font-size="17" fill="#8A8D93">{k} ro={g["ro"]} ri={g["ri"]} head={g["head"]} cy={g["cy"]}</text>')
W = 20 + 4*(430+20)
H = 460
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
       f'<rect width="{W}" height="{H}" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
open("ring.svg","w").write(svg)
cairosvg.svg2png(url="ring.svg", write_to="ring.png", output_width=W, background_color="#0B0C0E")
for k,(b,g) in V.items(): print(k, g)
