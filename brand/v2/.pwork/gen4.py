import cairosvg, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
V = {
 "K340": dict(cy=340, r=105, gap=30),
 "K350": dict(cy=350, r=105, gap=30),
 "K365": dict(cy=365, r=105, gap=32),
 "K380": dict(cy=380, r=105, gap=32),
 "K350b":dict(cy=350, r=100, gap=34),
 "K405": dict(cy=405, r=105, gap=30),
}
SX = 387
def letter(v):
    cx = SX + 36 + v["gap"] + v["r"]
    return (f'<path d="M{SX} 764 L{SX} 336" stroke="#F4F1EA" stroke-width="72" '
            f'stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{v["cy"]}" r="{v["r"]}" fill="#FF5A2B"/>')
parts, W = [], 520
for i,(k,v) in enumerate(V.items()):
    col, row = i % 3, i // 3
    x, y = 20 + col*(W+20), 60 + row*(W+20)
    parts.append(f'<g transform="translate({x},{y}) scale(0.46)">{letter(v)}</g>')
    parts.append(f'<text x="{x+8}" y="{y-14}" font-family="DejaVu Sans Mono" font-size="19" fill="#8A8D93">{k} gap={v["gap"]} cy={v["cy"]}</text>')
sy = 60 + 2*(W+20)+20
for i,(k,v) in enumerate(V.items()):
    x = 20+i*150
    parts.append(f'<g transform="translate({x},{sy}) scale(0.09375)">{letter(v)}</g>')
    parts.append(f'<text x="{x+45}" y="{sy+140}" font-family="DejaVu Sans Mono" font-size="16" fill="#8A8D93">{k} 96</text>')
H = sy+160
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="1660" height="{H}" viewBox="0 0 1660 {H}">'
       f'<rect width="1660" height="{H}" fill="#0B0C0E"/>'+"".join(parts)+'</svg>')
open("grid3.svg","w").write(svg)
cairosvg.svg2png(url="grid3.svg", write_to="grid3.png", output_width=1660, background_color="#0B0C0E")
print("ok")
