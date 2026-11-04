import cairosvg, re, os
D = "brand/v2/letters"

def inner(name):
    s = open(os.path.join(D, name), encoding="utf-8").read()
    m = re.search(r"<svg[^>]*>(.*)</svg>", s, re.S)
    return m.group(1)

def pletter(cy=405, r=105, gap=30, sx=387):
    cx = sx + 36 + gap + r
    return (f'<path d="M{sx} 764 L{sx} 336" stroke="#F4F1EA" stroke-width="72" '
            f'stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#FF5A2B"/>')

variants = {"A": pletter(396,105,40), "B": pletter(405,105,32), "F": pletter(405,105,30)}
sib = {n: inner(n) for n in ("a.svg","t.svg","y.svg") if os.path.exists(os.path.join(D,n))}

def line(items, y, scale, gapw):
    parts, x = [], 40
    for label, body in items:
        parts.append(f'<g transform="translate({x},{y}) scale({scale})">{body}</g>')
        x += 1024*scale + gapw
    return "".join(parts), x

rows = []
y = 40
for k, body in variants.items():
    items = [("a", sib.get("a.svg","")), ("P"+k, body), ("t", sib.get("t.svg","")), ("y", sib.get("y.svg",""))]
    chunk, xend = line(items, y, 0.42, 30)
    rows.append(chunk + f'<text x="40" y="{y-10}" font-family="DejaVu Sans Mono" font-size="18" fill="#8A8D93">P-{k} · family line</text>')
    y += 1024*0.42 + 40

# small-size row at 96px
y += 10
x = 40
for k, body in variants.items():
    rows.append(f'<g transform="translate({x},{y}) scale(0.09375)">{body}</g>')
    x += 96 + 40
for n, body in sib.items():
    rows.append(f'<g transform="translate({x},{y}) scale(0.09375)">{body}</g>')
    x += 96 + 40
rows.append(f'<text x="40" y="{y-8}" font-family="DejaVu Sans Mono" font-size="18" fill="#8A8D93">96px: A/B/F then a t y</text>')

W, H = 1720, int(y + 96 + 40)
svg = f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" viewBox="0 0 {W} {H}"><rect width="{W}" height="{H}" fill="#0B0C0E"/>' + "".join(rows) + '</svg>'
os.chdir(os.path.dirname(os.path.abspath(__file__))); open("line.svg","w").write(svg)
cairosvg.svg2png(url="line.svg", write_to="line.png", output_width=W, background_color="#0B0C0E")
print("ok", W, H)
