import cairosvg, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))

def L(cy, r, gap, top, bot, label):
    # stem from (botx,bot) to (topx,top); gap measured horizontally at cy against local stem x
    t = (top + bot)/2
    return dict(cy=cy, r=r, gap=gap, top=top, bot=bot, label=label)

# stem endpoints: (x_bottom, y=764) to (x_top, y=336)
V = {
 "B": dict(cy=405, r=105, gap=32, xb=387, xt=387),   # straight, top-aligned
 "F": dict(cy=405, r=105, gap=30, xb=387, xt=387),
 "G": dict(cy=405, r=105, gap=30, xb=392, xt=380),   # lean left at top (10)
 "H": dict(cy=405, r=105, gap=30, xb=382, xt=392),   # lean right at top
 "I": dict(cy=393, r=105, gap=32, xb=387, xt=387),   # head 12 above cap line
 "J": dict(cy=415, r=105, gap=30, xb=387, xt=387),   # head lower
}

def stem_x(v, y):
    # y increases downward; top y=336, bottom 764
    t = (y - 336) / (764 - 336)
    return v["xt"] + (v["xb"] - v["xt"]) * t

def letter(v):
    sx = stem_x(v, v["cy"])
    cx = sx + 36 + v["gap"] + v["r"]
    v["cx"], v["sxmid"] = cx, sx
    return (f'<path d="M{v["xb"]} 764 L{v["xt"]} 336" stroke="#F4F1EA" stroke-width="72" '
            f'stroke-linecap="round" fill="none"/>'
            f'<circle cx="{cx}" cy="{v["cy"]}" r="{v["r"]}" fill="#FF5A2B"/>')

parts, W = [], 520
for i,(k,v) in enumerate(V.items()):
    body = letter(v)
    col, row = i % 3, i // 3
    x, y = 20 + col*(W+20), 60 + row*(W+20)
    parts.append(f'<g transform="translate({x},{y}) scale(0.46)">{body}</g>')
    parts.append(f'<text x="{x+8}" y="{y-14}" font-family="DejaVu Sans Mono" font-size="19" fill="#8A8D93">{k} gap={v["gap"]} cy={v["cy"]}</text>')

sy = 60 + 2*(W+20) + 20
for i,(k,v) in enumerate(V.items()):
    x = 20 + i*150
    parts.append(f'<g transform="translate({x},{sy}) scale(0.09375)">{letter(v)}</g>')
    parts.append(f'<text x="{x+45}" y="{sy+140}" font-family="DejaVu Sans Mono" font-size="16" fill="#8A8D93">{k} 96</text>')
H = sy + 160
svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="1660" height="{H}" viewBox="0 0 1660 {H}">'
       f'<rect width="1660" height="{H}" fill="#0B0C0E"/>' + "".join(parts) + '</svg>')
open("grid2.svg","w").write(svg)
cairosvg.svg2png(url="grid2.svg", write_to="grid2.png", output_width=1660, background_color="#0B0C0E")
print("ok", {k:(round(v["cx"]),v["cy"],v["r"],v["gap"]) for k,v in V.items()})
