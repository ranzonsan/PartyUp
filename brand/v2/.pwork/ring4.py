import cairosvg, os, re
os.chdir(os.path.dirname(os.path.abspath(__file__)))
D = "/home/ranzon/PartyUp/brand/v2/letters"

def inner(n):
    s = open(os.path.join(D, n + ".svg"), encoding="utf-8").read()
    m = re.search(r"<svg\b[^>]*>(.*)</svg>", s, re.S); return m.group(1)

def ringP(ro, head, cy, cx=512.0):
    sx = cx - ro + 36
    return (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{ro-36}" fill="none" stroke="#F4F1EA" stroke-width="72"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{head}" fill="#FF5A2B"/>')

def row(bodies, out, scale=0.40, cell=None):
    n = len(bodies)
    cell = cell or int(1024*scale)+20
    W = 20 + n*(cell+10)
    H = int(1024*scale)+40
    parts = []
    for i,(k,b) in enumerate(bodies):
        x = 20 + i*(cell+10)
        parts.append(f'<g transform="translate({x},30) scale({scale})">{b}</g>')
        parts.append(f'<text x="{x+4}" y="22" font-family="DejaVu Sans Mono" font-size="14" fill="#8A8D93">{k}</text>')
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}">'
           f'<rect width="{W}" height="{H}" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
    cairosvg.svg2png(bytestring=svg.encode(), write_to=out, output_width=W)

V = [("cy434", ringP(186,84,434)), ("cy460", ringP(186,84,460)), ("cy486", ringP(186,84,486)), ("cy510", ringP(186,84,510))]
row(V, "ring4.png")
print("ok")
