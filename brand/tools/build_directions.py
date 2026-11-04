#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Renders brand/preview/directions.png — the concepts considered and why one won."""
from __future__ import annotations
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(os.path.dirname(ROOT), ".tools"))
import cairosvg
import build_assets as B

INK, BONE, EMBER, FOG, LINE = B.INK, B.BONE, B.EMBER, B.FOG, B.LINE
BG, PANEL, EDGE = "#0E0F11", "#15171A", "#242830"
SANS, MONO = "Ubuntu", "DejaVu Sans Mono"

def esc(s): return str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;")
def t(x, y, s, size=14, fill=FOG, fam=SANS, anchor="start", spacing=0.0):
    return (f'<text x="{x}" y="{y}" font-family="{fam}" font-size="{size}" fill="{fill}" '
            f'text-anchor="{anchor}" letter-spacing="{spacing}">{esc(s)}</text>')

def tile(inner, bg=INK, rx=224):
    return (f'<svg x="0" y="0" width="1024" height="1024" viewBox="0 0 1024 1024">'
            f'<rect width="1024" height="1024" rx="{rx}" fill="{bg}"/>{inner}</svg>')

def body_of(svg):
    return svg[svg.index(">", svg.index("<svg")) + 1:svg.rindex("</svg>")]

# --- candidate marks -------------------------------------------------------
def m_door():
    return tile(f'<path d="M288 880 L288 470 A 224 224 0 0 1 736 470 L736 880" fill="none" '
                f'stroke="{BONE}" stroke-width="150" stroke-linecap="butt"/>'
                f'<circle cx="512" cy="672" r="100" fill="{EMBER}"/>')
def m_u():
    return tile(f'<path d="M300 300 L300 560 A 212 212 0 0 0 724 560 L724 300" fill="none" '
                f'stroke="{BONE}" stroke-width="190" stroke-linecap="butt"/>'
                f'<circle cx="512" cy="180" r="100" fill="{EMBER}"/>')
def m_huddle():
    return tile(f'<circle cx="512" cy="360" r="132" fill="{BONE}"/>'
                f'<circle cx="396" cy="592" r="132" fill="{BONE}"/>'
                f'<circle cx="628" cy="592" r="132" fill="{EMBER}"/>')
def m_chev():
    return tile(f'<path d="M256 700 L512 444 L768 700" fill="none" stroke="{BONE}" stroke-width="104" stroke-linecap="round" stroke-linejoin="round"/>'
                f'<path d="M256 452 L512 196 L768 452" fill="none" stroke="{EMBER}" stroke-width="104" stroke-linecap="round" stroke-linejoin="round"/>')
def m_keyhole():
    return tile(f'<path d="M512 216 A 196 196 0 1 0 512 608 A 196 196 0 1 0 512 216 Z'
                f'M362 596 L662 596 L584 856 Q512 884 440 856 Z" fill="{BONE}"/>')
def m_chosen():
    return B.mark_svg()

CARDS = [
    (m_chosen,   "01 — THE P",       "chosen",  "One letter, one warm field.\nReads at 16 px, scales to a billboard."),
    (m_door,     "02 — THE DOORWAY", "rejected", "A room you can drop into.\nStrong idea; reads as “n” at small sizes."),
    (m_u,        "03 — THE U",       "rejected", "The second letter, plus a presence dot.\nCharming, but ambiguous off-name."),
    (m_huddle,   "04 — THE HUDDLE",  "rejected", "Three dots, a party of friends.\nWarm; merges into a blob at 16 px."),
    (m_chev,     "05 — THE CHEVRONS", "rejected", "“Up”, pure motion.\nClear, but every other tool uses it."),
    (m_keyhole,  "06 — THE KEYHOLE", "rejected", "Access, invite code, your own server.\nReads as a flask; wrong promise."),
]

def build():
    W, H = 2400, 780
    PAD, GUT = 60, 22
    cw = (W - 2 * PAD - 2 * GUT) / 3
    ch = (H - 2 * PAD - 2 * GUT - 56) / 2
    out = [f'<rect width="{W}" height="{H}" fill="{BG}"/>']
    for i, (fn, name, verdict, note) in enumerate(CARDS):
        r, c = divmod(i, 3)
        x, y = PAD + c * (cw + GUT), PAD + 56 + r * (ch + GUT)
        out.append(f'<g transform="translate({x:.1f},{y:.1f})">'
                   f'<rect width="{cw:.1f}" height="{ch:.1f}" fill="{PANEL}"/>'
                   f'<rect x="0.5" y="0.5" width="{cw-1:.1f}" height="{ch-1:.1f}" fill="none" stroke="{EDGE}"/>')
        ms = 150
        body = body_of(fn())
        out.append(f'<g transform="translate(34,34) scale({ms/1024:.6f})">{body}</g>')
        col = EMBER if verdict == "chosen" else "#5E636B"
        out.append(t(212, 62, name, size=16, fill=BONE, fam=MONO, spacing=1.2))
        out.append(t(212, 84, verdict.upper(), size=11, fill=col, fam=MONO, spacing=1.6))
        for j, line in enumerate(note.split("\n")):
            out.append(t(212, 118 + j * 20, line, size=12.5, fill=FOG))
        out.append("</g>")
    out.append(t(PAD, 36, "DIRECTIONS CONSIDERED — ONE CHOSEN", size=13, fill=FOG, fam=MONO, spacing=1.6))
    out.append(f'<line x1="{PAD}" y1="48" x2="{W-PAD}" y2="48" stroke="{EDGE}"/>')
    out.append(t(PAD, H - 26, "the mark had to survive 16 px, one colour, and a taskbar of neighbours",
                 size=12, fill="#5E636B", fam=MONO))
    return (f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H}" '
            f'viewBox="0 0 {W} {H}">' + "".join(out) + "</svg>")

def main():
    s = build()
    os.makedirs(os.path.join(ROOT, "preview"), exist_ok=True)
    open(os.path.join(ROOT, "preview", "directions.svg"), "w").write(s)
    cairosvg.svg2png(bytestring=s.encode(), write_to=os.path.join(ROOT, "preview", "directions.png"),
                     output_width=2400, output_height=780)
    print("directions ok")

if __name__ == "__main__":
    main()
