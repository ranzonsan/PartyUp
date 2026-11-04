import cairosvg, os, re
os.chdir(os.path.dirname(os.path.abspath(__file__)))
D = "/home/ranzon/PartyUp/brand/v2/letters"
def inner(n):
    s = open(os.path.join(D, n + ".svg"), encoding="utf-8").read()
    m = re.search(r"<svg\b[^>]*>(.*)</svg>", s, re.S); return m.group(1)
names = [n for n in "party" if os.path.exists(os.path.join(D, n + ".svg"))]
crop, gap = 520.0, 40.0
tiles = [(n, inner(n)) for n in names]
total = len(tiles)*crop + (len(tiles)-1)*gap
parts = []
for i,(n,b) in enumerate(tiles):
    x = i*(crop+gap)
    parts.append(f'<svg x="{x:.1f}" y="0" width="{crop:.1f}" height="1024" viewBox="{512-crop/2:.1f} 0 {crop:.1f} 1024">{b}</svg>')
W,H = 2600, 1240
ox, oy = (W-total)/2, 40
extra = [f'<line x1="{ox-60}" y1="{oy+800}" x2="{ox+total+60}" y2="{oy+800}" stroke="#242830"/>']
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}"><rect width="{W}" height="{H}" fill="#0B0C0E"/><g transform="translate({ox:.1f},{oy:.1f})">'+"".join(parts)+'</g>'+"".join(extra)+'</svg>')
open("party-preview.svg","w").write(svg)
cairosvg.svg2png(url="party-preview.svg", write_to="party-preview.png", output_width=W, output_height=H)
print("ok", names, total)
