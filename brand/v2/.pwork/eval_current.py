import cairosvg, os, re, io
from PIL import Image
os.chdir(os.path.dirname(os.path.abspath(__file__)))
D = "/home/ranzon/PartyUp/brand/v2/letters"

def inner(p):
    s = open(p, encoding="utf-8").read()
    m = re.search(r"<svg\b[^>]*>(.*)</svg>", s, re.S); return m.group(1)

pbody = inner(os.path.join(D, "p.svg"))
names = ["p","a","r","t","y"]
tiles = [(n, inner(os.path.join(D, n+".svg"))) for n in names]
crop, gap = 560.0, 60.0
total = len(tiles)*crop + (len(tiles)-1)*gap
parts = []
for i,(n,b) in enumerate(tiles):
    x = i*(crop+gap)
    parts.append(f'<svg x="{x:.1f}" y="0" width="{crop:.1f}" height="1024" viewBox="{512-crop/2:.1f} 0 {crop:.1f} 1024">{b}</svg>')
W,H = 2600, 1240
ox, oy = (W-total)/2, 40
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}"><rect width="{W}" height="{H}" fill="#0B0C0E"/><g transform="translate({ox:.1f},{oy:.1f})">'+"".join(parts)+'</g></svg>')
cairosvg.svg2png(bytestring=svg.encode(), write_to="word-current.png", output_width=W, output_height=H)

# 96px pixel test of the p alone
png = cairosvg.svg2png(bytestring=f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024"><rect width="1024" height="1024" fill="#0B0C0E"/>{pbody}</svg>'.encode(), output_width=96, output_height=96)
im = Image.open(io.BytesIO(png)).convert("RGB")
im.resize((96*4,96*4), Image.NEAREST).save("p96.png")
print("ok")
