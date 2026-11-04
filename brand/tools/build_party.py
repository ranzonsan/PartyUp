#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""v0.2 探索：把 "Party" 做成五个「人」——字母是身体，悬浮的圆点是脑袋。"""
from __future__ import annotations
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(os.path.dirname(ROOT), ".tools"))
import cairosvg
import build_assets as B

INK, BONE, EMBER = B.INK, B.BONE, B.EMBER
CAP, XH = 460.0, 330.0
W, BASE  = 54.0, 700.0
TOP, XTOP = BASE - CAP, BASE - XH
DESC, QS  = 200.0, 27.0

def st(d, color=BONE, w=W):
    return (f'<path d="{d}" fill="none" stroke="{color}" stroke-width="{w}" '
            f'stroke-linecap="round" stroke-linejoin="round"/>')
def dot(cx, cy, r, color=EMBER):
    return f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" fill="{color}"/>'

def L_P(x):
    r, s = 132.0, x + QS
    return (st(f"M{s} {BASE} L{s} {TOP}") + st(f"M{s} {TOP} A{r} {r} 0 0 1 {s} {TOP+2*r}"),
            2*r + W, TOP)

def L_a(x):
    r = 140.0; cx, cy = x + QS + r, BASE - r
    d = f"M{cx-r} {cy} A{r} {r} 0 1 0 {cx+r} {cy} A{r} {r} 0 1 0 {cx-r} {cy}"
    return st(d) + st(f"M{cx+r} {BASE} L{cx+r} {XTOP-40}"), 2*r + W, XTOP - 40

def L_r(x):
    s = x + QS
    return (st(f"M{s} {BASE} L{s} {XTOP}") +
            st(f"M{s} {XTOP+96} Q{s} {XTOP} {s+112} {XTOP}"), 112 + W, XTOP)

def L_t(x):
    sx = x + QS + 82
    return (st(f"M{sx} {XTOP-52} L{sx} {BASE-72} Q{sx} {BASE} {sx+78} {BASE}") +
            st(f"M{sx-84} {XTOP} L{sx+84} {XTOP}"), 168 + W, XTOP - 52)

def L_y(x):
    s = x + QS
    jx = s + 78
    return (st(f"M{s} {XTOP} L{jx} {BASE}") +
            st(f"M{s+156} {XTOP} L{jx} {BASE} L{jx} {BASE+DESC}"), 156 + W, XTOP)

LETTERS = [("P", L_P), ("a", L_a), ("r", L_r), ("t", L_t), ("y", L_y)]

def compose(gap, head_r, air, head_color=EMBER, head_dx=0.0):
    x, bodies, items = 0.0, [], []
    for name, fn in LETTERS:
        svg, w, top = fn(x)
        bodies.append(svg); items.append((x, w, top)); x += w + gap
    total = x - gap
    heads = [dot(lx + w/2 + head_dx, top - air - head_r, head_r, head_color) for lx, w, top in items]
    return "".join(bodies) + "".join(heads), total

def render(name, Wc, Hc, **kw):
    body, total = compose(**kw)
    ox = (Wc - total) / 2
    g = [f'<rect width="{Wc}" height="{Hc}" fill="{INK}"/>',
         f'<g transform="translate({ox:.1f},0)">{body}</g>',
         f'<line x1="{ox-70:.1f}" y1="{BASE}" x2="{ox+total+70:.1f}" y2="{BASE}" stroke="#232730"/>',
         f'<rect x="{ox:.1f}" y="{BASE+70}" width="{total:.1f}" height="180" fill="none" '
         f'stroke="#232730" stroke-dasharray="8 9"/>',
         f'<text x="{ox+total/2:.1f}" y="{BASE+180}" font-family="DejaVu Sans Mono" font-size="20" '
         f'fill="#3E434B" text-anchor="middle" letter-spacing="7">UP RESERVED</text>']
    s = B.svg(Wc, Hc, "".join(g))
    out = os.path.join(ROOT, "preview"); os.makedirs(out, exist_ok=True)
    open(os.path.join(out, name + ".svg"), "w").write(s)
    cairosvg.svg2png(bytestring=s.encode(), write_to=os.path.join(out, name + ".png"),
                     output_width=Wc, output_height=Hc)
    print(name, "total", round(total))

def main():
    render("party-BIG", 2600, 1240, gap=92, head_r=104, air=26)
    render("party-MID", 2600, 1240, gap=92, head_r=72, air=26)
    render("party-SMALL", 2600, 1240, gap=92, head_r=48, air=26)


def variant_group(head_r, air=26.0, head_dx=0.0, ember=True):
    body, total = compose(gap=92, head_r=head_r, air=air, head_dx=head_dx)
    return body, total

def contact_sheet():
    Wc, Hc = 2600, 1600
    CW, CH = 1300, 800
    cells = [("BIG · r104", dict(head_r=104)),
             ("MID · r72", dict(head_r=72)),
             ("SMALL · r48", dict(head_r=48)),
             ("PEEK · r72 +40", dict(head_r=72, head_dx=40))]
    g = [f'<rect width="{Wc}" height="{Hc}" fill="#0E0F11"/>']
    for i, (label, kw) in enumerate(cells):
        col, row = i % 2, i // 2
        body, total = variant_group(**kw)
        s = min((CW - 140) / total, (CH - 220) / 850.0)
        tx = col * CW + (CW - total * s) / 2
        ty = row * CH + (CH - 850 * s) / 2 + 60 - 80 * s
        g.append(f'<g transform="translate({tx:.1f},{ty:.1f}) scale({s:.4f})">'
                 f'<line x1="-80" y1="{BASE}" x2="{total+80}" y2="{BASE}" stroke="#242830"/>'
                 f'{body}</g>')
        g.append(f'<text x="{col*CW+70}" y="{row*CH+62}" font-family="DejaVu Sans Mono" '
                 f'font-size="26" fill="#6B7078">{label}</text>')
    for c in range(3):
        g.append(f'<line x1="{c*CW}" y1="0" x2="{c*CW}" y2="{Hc}" stroke="#1B1E23"/>')
    g.append(f'<line x1="0" y1="{CH}" x2="{Wc}" y2="{CH}" stroke="#1B1E23"/>')
    s = B.svg(Wc, Hc, "".join(g))
    out = os.path.join(ROOT, "preview")
    open(os.path.join(out, "party-heads.svg"), "w").write(s)
    cairosvg.svg2png(bytestring=s.encode(), write_to=os.path.join(out, "party-heads.png"),
                     output_width=Wc, output_height=Hc)
    print("sheet ok")


if __name__ == "__main__":
    main()
    contact_sheet()
