import cairosvg, io, os
from PIL import Image
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def svg(body):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="1024" height="1024" viewBox="0 0 1024 1024">'
            f'<rect width="1024" height="1024" fill="#0B0C0E"/>{body}</svg>')

STEM = '<path d="M387 764 L387 336" stroke="#F4F1EA" stroke-width="72" stroke-linecap="round" fill="none"/>'

def disc(cy=405, r=105, gap=38, sx=387):
    cx = sx + 36 + gap + r
    return STEM + f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#FF5A2B"/>'

# control: a real P, 72 stroke, bowl outer r=105 centered (562,405)
realP = (STEM +
         '<path d="M387 300 A105 105 0 0 1 387 510" stroke="#F4F1EA" stroke-width="72" fill="none" stroke-linecap="round"/>')

cases = [("realP", realP), ("gap0", disc(gap=0)), ("gap14", disc(gap=14)), ("gap38", disc(gap=38))]

def render(body, px):
    png = cairosvg.svg2png(bytestring=svg(body).encode(), output_width=px, output_height=px)
    im = Image.open(io.BytesIO(png)).convert("RGB")
    return im

Z = 2
tiles = []
for name, body in cases:
    big = render(body, 300)      # 300px view
    small = render(body, 96).resize((96*2,96*2), Image.NEAREST)  # pixel view
    tiles.append((name, big, small))

W = sum(t[1].width for t in tiles) + 20*(len(tiles)+1)
H = 300 + 40 + 192 + 120
canvas = Image.new("RGB", (W,H), (11,12,14))
x = 20
for name, big, small in tiles:
    canvas.paste(big, (x, 40))
    canvas.paste(small, (x, 380))
    x += 300 + 20
canvas.save("diag.png")
print("ok", W, H)
