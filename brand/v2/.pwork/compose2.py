import cairosvg, os, re
os.chdir(os.path.dirname(os.path.abspath(__file__)))
D = "/home/ranzon/PartyUp/brand/v2/letters"

def inner(n):
    s = open(os.path.join(D, n + ".svg"), encoding="utf-8").read()
    m = re.search(r"<svg\b[^>]*>(.*)</svg>", s, re.S); return m.group(1)

def ringP(ro=194, gap=30, cx=512.0, cy=None):
    ri = ro - 72; head = ri - gap; cy = cy if cy else 300 + ro
    sx = cx - ro + 36
    return (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{ro-36}" fill="none" stroke="#F4F1EA" stroke-width="72"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{head}" fill="#FF5A2B"/>')

def compose(pbody, out):
    names = ["a","r","t","y"]
    tiles = [("p", pbody)] + [(n, inner(n)) for n in names]
    crop, gap = 520.0, 40.0
    total = len(tiles)*crop + (len(tiles)-1)*gap
    parts = []
    for i,(n,b) in enumerate(tiles):
        x = i*(crop+gap)
        parts.append(f'<svg x="{x:.1f}" y="0" width="{crop:.1f}" height="1024" viewBox="{512-crop/2:.1f} 0 {crop:.1f} 1024">{b}</svg>')
    W,H = 2600, 1240
    ox, oy = (W-total)/2, 40
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}"><rect width="{W}" height="{H}" fill="#0B0C0E"/><g transform="translate({ox:.1f},{oy:.1f})">'+"".join(parts)+'</g></svg>')
    cairosvg.svg2png(bytestring=svg.encode(), write_to=out, output_width=W, output_height=H)

compose(ringP(194,30), "party-ring.png")
compose(inner("p"), "party-literal.png")
print("ok")
