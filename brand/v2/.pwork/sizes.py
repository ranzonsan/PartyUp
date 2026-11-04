import cairosvg, io, os
from PIL import Image
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def glyph(cy=405, r=105, gap=38, sx=387):
    cx = sx + 36 + gap + r
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">'
            f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#FF5A2B"/></svg>')

sizes = [24, 32, 48, 64, 96, 128]
Z = 5
tiles = []
for s in sizes:
    png = cairosvg.svg2png(bytestring=glyph().encode(), output_width=s, output_height=s)
    im = Image.open(io.BytesIO(png)).convert("RGB")
    bg = Image.new("RGB", (s, s), (11, 12, 14)); bg.paste(im, (0,0), im if im.mode=="RGBA" else None)
    im = Image.open(io.BytesIO(png)).convert("RGBA")
    bg = Image.new("RGB", (s, s), (11, 12, 14)); bg.paste(im, (0,0), im)
    tiles.append((s, bg.resize((s*Z, s*Z), Image.NEAREST)))

W = sum(t[1].width for t in tiles) + 20*(len(tiles)+1)
H = max(t[1].height for t in tiles) + 60
canvas = Image.new("RGB", (W, H), (11,12,14))
x = 20
for s, im in tiles:
    canvas.paste(im, (x, 40)); x += im.width + 20
canvas.save("sizes.png")
print("ok", sizes, W, H)
